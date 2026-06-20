/*
 * sequencer.cpp — 8-track x 64-step step sequencer with transport + clock.
 *
 * Runs on CORE_IO inside Task_Sequencer. It is purely an EVENT PRODUCER: on a
 * step boundary it posts EV_NOTE_ON now and schedules an EV_NOTE_OFF for later
 * (and EV_CC for any active param locks). The UI dispatcher (modes.cpp) is what
 * actually routes those events to the synth queue and/or MIDI OUT — so the HARD
 * RULE "scanners/sequencer never touch the DSP directly" is preserved here.
 *
 * Clock model: everything is measured in 24-ppqn ticks.
 *   - Internal clock: Task_Sequencer derives a tick period from the BPM (see
 *     seq_tick_period_us) and calls seq_tick() on a timer. We don't own that
 *     timer; we just expose seq_tick() and the period.
 *   - External clock: when slaved, the MIDI-IN parser posts EV_CLOCK_TICK and
 *     the dispatcher calls seq_tick() once per tick. Either way seq_tick() is
 *     the single advance primitive.
 *
 * Threading — SINGLE OWNER: all sequencer state is mutated from exactly ONE
 * task, Task_UI (the dispatcher), so no locks are required. Task_Sequencer's
 * internal timer no longer calls seq_tick() directly; instead it POSTS an
 * EV_CLOCK_TICK (SRC_LOCAL) and the dispatcher calls seq_tick() when it drains
 * that event — the same path external MIDI clock already takes. Edits
 * (seq_toggle_step/...), transport and seq_tick() therefore all run serialized
 * on Task_UI, eliminating the prior Task_Sequencer-vs-Task_UI data race on
 * s_patterns/s_active. The clock-source split (seq_is_external()) keeps the two
 * clock origins from double-advancing. We never block, malloc, or call DSP.
 */
#include "sequencer.h"
#include "events.h"

#include <string.h>   // memset / memcpy
#include <stdlib.h>   // rand / RAND_MAX

// ─────────────────────────────── State ──────────────────────────────────────

// A pattern is the full 8x64 grid. We keep a small RAM bank so "pattern select"
// is instant (presets.cpp is responsible for persisting banks to LittleFS).
struct Pattern {
  Step steps[SEQ_TRACKS][SEQ_STEPS];
};

static Pattern  s_patterns[SEQ_PATTERNS];
static uint8_t  s_pattern_cur     = 0;   // pattern currently playing
static uint8_t  s_pattern_pending = 0;   // pattern queued (applied at step 0)

static bool     s_track_mute[SEQ_TRACKS];
static uint8_t  s_track_chan[SEQ_TRACKS];   // MIDI channel 1..16 per track

// Transport / playhead.
static SeqState s_state    = SEQ_STOPPED;
static uint8_t  s_step     = 0;    // current step column 0..63
static uint8_t  s_tick_in_step = 0; // 0..SEQ_TICKS_PER_STEP-1 within the step

// Tempo / feel.
static float    s_bpm      = 120.0f;
static uint8_t  s_swing    = 0;     // 0..75 (%) delay applied to odd steps
static bool     s_external = false; // true = ignore internal timing, follow ticks

// ── Active-gate bookkeeping ─────────────────────────────────────────────────
// Instead of posting EV_NOTE_OFF "in the future" (we have no scheduler that can
// hold an event), we remember every note we turned on and count down its gate
// in ticks. seq_tick() decrements and fires the EV_NOTE_OFF when it hits zero.
// Bound the table generously: one note per track can be re-triggered before the
// previous tail ends, so allow a few overlaps per track.
#define SEQ_MAX_ACTIVE (SEQ_TRACKS * 4)

struct ActiveNote {
  uint8_t used;
  uint8_t note;
  uint8_t velocity;
  uint8_t channel;
  uint16_t ticks_left;   // EV_NOTE_OFF fires when this reaches 0
};
static ActiveNote s_active[SEQ_MAX_ACTIVE];

// Swing scheduling: when an odd step is delayed, we hold its firing until the
// swing offset (in ticks) has elapsed. -1 = nothing pending.
static int8_t   s_swing_pending_step = -1;
static uint8_t  s_swing_delay_ticks  = 0;
static uint8_t  s_swing_elapsed      = 0;

// ─────────────────────────── small helpers ──────────────────────────────────

static inline bool valid_track(uint8_t t) { return t < SEQ_TRACKS; }
static inline bool valid_step (uint8_t s) { return s < SEQ_STEPS;  }

// The grid the public edit/playback calls operate on (the playing pattern).
static inline Step* grid() { return &s_patterns[s_pattern_cur].steps[0][0]; }
static inline Step& step_at(uint8_t track, uint8_t st) {
  return s_patterns[s_pattern_cur].steps[track][st];
}

// Roll a 0..99 number; fire when it's below the step's probability (100 = always).
static inline bool prob_hit(uint8_t probability) {
  if (probability >= 100) return true;
  if (probability == 0)   return false;
  return (uint32_t)(rand() % 100) < probability;
}

// Register a sounding note so its EV_NOTE_OFF can be scheduled by gate length.
static void track_active_note(uint8_t note, uint8_t vel, uint8_t channel, uint16_t ticks) {
  if (ticks == 0) ticks = 1;            // never a zero-length gate
  for (int i = 0; i < SEQ_MAX_ACTIVE; ++i) {
    if (!s_active[i].used) {
      s_active[i] = ActiveNote{ 1, note, vel, channel, ticks };
      return;
    }
  }
  // Table full (pathological): drop the tail tracking. The note still played;
  // worst case it relies on the next seq_stop() all-notes-off to release.
}

// Post EV_NOTE_OFF for every currently-sounding note and clear the table.
static void release_all_active() {
  for (int i = 0; i < SEQ_MAX_ACTIVE; ++i) {
    if (s_active[i].used) {
      event_post(ev_note(EV_NOTE_OFF, s_active[i].note, 0, s_active[i].channel, SRC_LOCAL));
      s_active[i].used = 0;
    }
  }
}

// Count down every active gate by one tick and fire NOTE_OFF on expiry.
static void advance_active_gates() {
  for (int i = 0; i < SEQ_MAX_ACTIVE; ++i) {
    if (!s_active[i].used) continue;
    if (--s_active[i].ticks_left == 0) {
      event_post(ev_note(EV_NOTE_OFF, s_active[i].note, 0, s_active[i].channel, SRC_LOCAL));
      s_active[i].used = 0;
    }
  }
}

// Fire one step column across all 8 tracks: NOTE_ON + param-lock CCs, and queue
// the matching NOTE_OFF via gate-length bookkeeping. Respects mute + probability.
static void fire_step(uint8_t st) {
  for (uint8_t t = 0; t < SEQ_TRACKS; ++t) {
    if (s_track_mute[t]) continue;
    const Step& sp = step_at(t, st);
    if (!sp.active) continue;
    if (!prob_hit(sp.probability)) continue;

    const uint8_t ch = s_track_chan[t];

    // Apply this step's param locks first (as EV_CC) so the synth is set up
    // before the note sounds. The dispatcher maps cc -> SynthParam.
    for (uint8_t p = 0; p < SEQ_PLOCKS_PER_STEP; ++p) {
      const ParamLock& pl = sp.plocks[p];
      if (pl.active) {
        Event e{ EV_CC, SRC_LOCAL, pl.cc, pl.value, ch, 0 };
        event_post(e);
      }
    }

    // Trigger the note and schedule its release after `length` ticks.
    event_post(ev_note(EV_NOTE_ON, sp.note, sp.velocity, ch, SRC_LOCAL));
    track_active_note(sp.note, sp.velocity, ch, sp.length);
  }
}

// Swing offset in ticks for the upcoming odd step. pct is 0..75; 50% swing on a
// 6-tick step delays the odd step by ~ (pct/100 * step) ticks. We clamp so the
// offset never reaches the next step boundary.
static uint8_t swing_offset_ticks() {
  if (s_swing == 0) return 0;
  // ticks = round(swing% * ticks_per_step / 100), capped at TICKS_PER_STEP-1.
  uint16_t off = ((uint16_t)s_swing * SEQ_TICKS_PER_STEP) / 100;
  if (off >= SEQ_TICKS_PER_STEP) off = SEQ_TICKS_PER_STEP - 1;
  return (uint8_t)off;
}

// Apply a queued pattern change. Only happens at a step-0 boundary while playing
// (musical quantization) or immediately when stopped.
static void apply_pending_pattern() {
  if (s_pattern_pending != s_pattern_cur) {
    s_pattern_cur = s_pattern_pending;
  }
}

// ─────────────────────────────── lifecycle ──────────────────────────────────

void seq_init() {
  memset(s_patterns, 0, sizeof(s_patterns));
  memset(s_active,   0, sizeof(s_active));

  for (uint8_t t = 0; t < SEQ_TRACKS; ++t) {
    s_track_mute[t] = false;
    s_track_chan[t] = (uint8_t)(t + 1);   // track 0 -> ch 1, ... track 7 -> ch 8
  }

  s_pattern_cur      = 0;
  s_pattern_pending  = 0;
  s_state            = SEQ_STOPPED;
  s_step             = 0;
  s_tick_in_step     = 0;
  s_bpm              = 120.0f;
  s_swing            = 0;
  s_external         = false;

  s_swing_pending_step = -1;
  s_swing_delay_ticks  = 0;
  s_swing_elapsed      = 0;
}

// ──────────────────────────────── clock ─────────────────────────────────────

void seq_tick() {
  if (s_state != SEQ_PLAYING) return;

  // 1) Release any gates that expire on this tick (do this first so a NOTE_OFF
  //    for the previous step lands before a same-pitch NOTE_ON on this step).
  advance_active_gates();

  // 2) Handle a pending swung step: an odd step whose firing we delayed.
  if (s_swing_pending_step >= 0) {
    if (++s_swing_elapsed >= s_swing_delay_ticks) {
      fire_step((uint8_t)s_swing_pending_step);
      s_swing_pending_step = -1;
    }
  }

  // 3) Are we on a step boundary?
  if (s_tick_in_step == 0) {
    // Quantized pattern switch happens exactly at the top of the pattern.
    if (s_step == 0) apply_pending_pattern();

    // Even steps fire immediately; odd steps may be delayed for swing feel.
    const bool odd = (s_step & 1) != 0;
    const uint8_t off = odd ? swing_offset_ticks() : 0;
    if (off == 0) {
      fire_step(s_step);
    } else {
      s_swing_pending_step = (int8_t)s_step;
      s_swing_delay_ticks  = off;
      s_swing_elapsed      = 0;
    }
  }

  // 4) Advance the sub-step tick counter and roll over to the next step.
  if (++s_tick_in_step >= SEQ_TICKS_PER_STEP) {
    s_tick_in_step = 0;
    if (++s_step >= SEQ_STEPS) s_step = 0;
  }
}

// ─────────────────────────────── transport ──────────────────────────────────

void seq_start() {
  // Rewind to the top and clear any lingering tails, then announce start.
  release_all_active();
  apply_pending_pattern();        // honor a queued pattern immediately on (re)start
  s_step               = 0;
  s_tick_in_step       = 0;
  s_swing_pending_step = -1;
  s_state              = SEQ_PLAYING;

  event_post(Event{ EV_TRANSPORT_START, SRC_LOCAL, 0, 0, 0, 0 });
  // Note: actual DIN/USB MIDI Start bytes are emitted by the dispatcher when it
  // sees EV_TRANSPORT_START (keeps the "events only" rule intact).
}

void seq_stop() {
  release_all_active();           // all-notes-off for everything we triggered
  s_swing_pending_step = -1;
  s_state              = SEQ_STOPPED;
  event_post(Event{ EV_TRANSPORT_STOP, SRC_LOCAL, 0, 0, 0, 0 });
}

void seq_continue() {
  // Resume from the current playhead without rewinding.
  if (s_state == SEQ_PLAYING) return;
  s_state = SEQ_PLAYING;
  event_post(Event{ EV_TRANSPORT_CONTINUE, SRC_LOCAL, 0, 0, 0, 0 });
}

SeqState seq_state() { return s_state; }

// ─────────────────────────────── tempo / feel ───────────────────────────────

void seq_set_tempo(float bpm) {
  // Clamp to a sane musical range; Task_Sequencer reads seq_tempo() to set its
  // internal-clock timer period.
  if (bpm < 20.0f)  bpm = 20.0f;
  if (bpm > 300.0f) bpm = 300.0f;
  s_bpm = bpm;
}

float seq_tempo() { return s_bpm; }

void seq_set_swing(uint8_t pct) {
  if (pct > 75) pct = 75;         // header contract: 0..75 %
  s_swing = pct;
}

uint8_t seq_swing() { return s_swing; }

void seq_set_external_clock(bool slaved) {
  s_external = slaved;
}

// Read by Task_Sequencer to suppress its internal tick while slaved (so the
// external EV_CLOCK_TICK is the sole advance source — no double-clocking).
bool seq_is_external() { return s_external; }

// ─────────────────────────────── pattern edit ───────────────────────────────

void seq_set_step(uint8_t track, uint8_t step, const Step& s) {
  if (!valid_track(track) || !valid_step(step)) return;
  step_at(track, step) = s;
}

const Step& seq_get_step(uint8_t track, uint8_t step) {
  static const Step kEmpty = {};
  if (!valid_track(track) || !valid_step(step)) return kEmpty;
  return step_at(track, step);
}

void seq_toggle_step(uint8_t track, uint8_t step) {
  if (!valid_track(track) || !valid_step(step)) return;
  Step& sp = step_at(track, step);
  sp.active = sp.active ? 0 : 1;
  // Give a freshly-enabled empty step musically-useful defaults so a bare grid
  // tap produces sound without the GUI having to fill every field.
  if (sp.active && sp.velocity == 0) {
    if (sp.note == 0)        sp.note = 60;    // middle C
    sp.velocity            = 100;
    if (sp.length == 0)      sp.length = SEQ_TICKS_PER_STEP;  // one step gate
    if (sp.probability == 0) sp.probability = 100;            // always fire
  }
}

void seq_clear_track(uint8_t track) {
  if (!valid_track(track)) return;
  memset(&s_patterns[s_pattern_cur].steps[track][0], 0, sizeof(Step) * SEQ_STEPS);
}

void seq_set_track_mute(uint8_t track, bool mute) {
  if (!valid_track(track)) return;
  // On mute, release any of this track's sounding notes so it goes silent now.
  if (mute) {
    for (int i = 0; i < SEQ_MAX_ACTIVE; ++i) {
      if (s_active[i].used && s_active[i].channel == s_track_chan[track]) {
        event_post(ev_note(EV_NOTE_OFF, s_active[i].note, 0, s_active[i].channel, SRC_LOCAL));
        s_active[i].used = 0;
      }
    }
  }
  s_track_mute[track] = mute;
}

uint8_t seq_current_step() { return s_step; }

// ───────────────────────────── per-track channel ────────────────────────────

void seq_set_track_channel(uint8_t track, uint8_t channel) {
  if (!valid_track(track)) return;
  if (channel < 1)  channel = 1;
  if (channel > 16) channel = 16;
  s_track_chan[track] = channel;
}

uint8_t seq_track_channel(uint8_t track) {
  if (!valid_track(track)) return 1;
  return s_track_chan[track];
}

// ─────────────────────────────── pattern select ─────────────────────────────

void seq_select_pattern(uint8_t pattern) {
  if (pattern >= SEQ_PATTERNS) return;
  s_pattern_pending = pattern;
  // If we're stopped there's no step-0 boundary coming, so switch immediately.
  if (s_state != SEQ_PLAYING) apply_pending_pattern();
}

uint8_t seq_current_pattern() { return s_pattern_cur; }
uint8_t seq_pending_pattern() { return s_pattern_pending; }
