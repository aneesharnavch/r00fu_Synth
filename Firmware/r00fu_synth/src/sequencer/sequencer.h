/*
 * sequencer.h — 8-track x 64-step step sequencer with transport + clock.
 *
 * Lives on CORE_IO (Task_Sequencer). It is driven by a 24-ppqn clock — either
 * internal (derived from tempo) or external (EV_CLOCK_TICK from MIDI IN). On a
 * step boundary it does NOT call the synth/DSP directly: it posts EV_NOTE_ON/
 * EV_NOTE_OFF (and the dispatcher routes them to synth + MIDI OUT) and/or calls
 * the MIDI OUT send helpers for external gear. This keeps the HARD RULE intact.
 *
 * Per-step parameter locks let a step override synth params for its duration;
 * a lock is applied as an EV_CC / synth_set_param at step time.
 *
 * Implemented in src/sequencer/sequencer.cpp.
 */
#pragma once
#include <stdint.h>
#include "config.h"

#define SEQ_TRACKS         8
#define SEQ_STEPS          64
#define SEQ_PLOCKS_PER_STEP 4    // up to 4 param locks per step
#define SEQ_PPQN           24    // clock resolution
#define SEQ_TICKS_PER_STEP 6     // 24 ppqn / 6 = 16th-note steps
#define SEQ_PATTERNS       8     // pattern bank slots (RAM-resident)

// One parameter lock: when active, set CC `cc` to `value` as the step fires.
struct ParamLock {
  uint8_t active;   // 0/1
  uint8_t cc;       // CC number (mapped to a SynthParam by the dispatcher)
  uint8_t value;    // 0..127
};

// Per-step data (mirrors instructions.txt Step, with fixed-size plock array).
struct Step {
  uint8_t   active;       // 0/1 step on
  uint8_t   note;         // MIDI note
  uint8_t   velocity;     // 0..127
  uint8_t   probability;  // 0..100 (% chance to fire)
  uint8_t   length;       // gate length in ticks (1..)
  ParamLock plocks[SEQ_PLOCKS_PER_STEP];
};

// Transport state for the UI / status LED.
enum SeqState : uint8_t { SEQ_STOPPED = 0, SEQ_PLAYING, SEQ_PAUSED };

void seq_init();    // zero all tracks, default tempo/swing, STOPPED.

// Advance by ONE 24-ppqn tick. Call from the clock source (internal timer in
// Task_Sequencer, or once per EV_CLOCK_TICK when slaved). Fires due steps via
// event_post(). Non-blocking.
void seq_tick();

// ── Transport ──────────────────────────────────────────────────────────────
void seq_start();      // from step 0, posts EV_TRANSPORT_START + MIDI start
void seq_stop();       // all-notes-off, posts EV_TRANSPORT_STOP
void seq_continue();   // resume from current position
SeqState seq_state();

// ── Tempo / feel ───────────────────────────────────────────────────────────
void  seq_set_tempo(float bpm);     // internal-clock BPM
float seq_tempo();
void  seq_set_swing(uint8_t pct);   // 0..75 (%) swing on even steps
void  seq_set_external_clock(bool slaved);  // true = follow EV_CLOCK_TICK
bool  seq_is_external();            // true while slaved to external clock

// Swing amount accessor (0..75 %), for preset save and UI feedback.
uint8_t seq_swing();

// ── Pattern editing (called by the UI dispatcher in StepSeq mode) ──────────
void        seq_set_step(uint8_t track, uint8_t step, const Step& s);
const Step& seq_get_step(uint8_t track, uint8_t step);
void        seq_toggle_step(uint8_t track, uint8_t step);
void        seq_clear_track(uint8_t track);
void        seq_set_track_mute(uint8_t track, bool mute);
uint8_t     seq_current_step();     // playhead column 0..63 (for grid feedback)

// Per-track MIDI channel (1..16). Defaults to track+1 (track 0 -> ch 1, ...).
// Used as Event.c on the EV_NOTE_ON/OFF and EV_CC the sequencer posts.
void        seq_set_track_channel(uint8_t track, uint8_t channel);
uint8_t     seq_track_channel(uint8_t track);

// ── Pattern select ─────────────────────────────────────────────────────────
// Up to SEQ_PATTERNS RAM-resident patterns; switching takes effect at the next
// step-0 boundary while playing (so the change is musically quantized), or
// immediately when stopped. All edit/transport calls act on the active pattern.
void        seq_select_pattern(uint8_t pattern);  // queue/select pattern slot
uint8_t     seq_current_pattern();                // currently-playing slot
uint8_t     seq_pending_pattern();                // queued slot (==current if none)
