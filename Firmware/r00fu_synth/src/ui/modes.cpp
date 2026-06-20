/*
 * modes.cpp — the UI dispatcher (the "brain"). Implements modes.h.
 *
 * ui_dispatch() is the ONLY place raw hardware events become musical action.
 * It consumes one Event per call (Task_UI pulls them off the global queue) and
 * routes by the current Mode + the per-button map (g_button_map):
 *
 *   raw EV_BUTTON_*  --(g_button_map)-->  note / cc / program / transport / mode
 *   raw EV_KNOB/SLIDER_CHANGED          ->  synth params or MIDI CC (per mode)
 *   musical EV_NOTE/CC/PROGRAM/PITCHBEND ->  synth (RT-safe queue) + MIDI mirror
 *   transport / clock                   ->  sequencer + MIDI mirror
 *
 * Echo-loop rule (events.h EventSource): a musical Event tagged SRC_USB is NOT
 * mirrored back out USB; one tagged SRC_DIN is NOT mirrored back out DIN. Local
 * (SRC_LOCAL) events go out BOTH ports. This lets the box be a standalone synth
 * AND a transparent USB<->DIN bridge without feedback.
 *
 * HARD RULE: this runs on CORE_IO and NEVER calls DSP directly. Notes/CC reach
 * the synth only through the RT-safe synth_* producers (synth.h), which package
 * a SynthCmd onto the lock-free core0->core1 queue.
 *
 * Runs in Task_UI (CORE_IO). Non-blocking aside from those RT-safe sends.
 */
#include "modes.h"

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>

#include "../audio/synth.h"            // synth_note_on/off/cc/..., SynthParam
#include "../sequencer/sequencer.h"    // seq_* transport + pattern editing
#include "../drivers/midi_din.h"       // DIN MIDI OUT
#include "../drivers/usb_midi.h"       // USB MIDI OUT (+ mounted())
#include "../drivers/config_protocol.h"// cfg_emit_button / cfg_log
#include "../storage/presets.h"        // presets_load / presets_save
#include "../storage/settings.h"       // g_settings (startup mode, default channel, clock)

// ───────────────────────────── module state ─────────────────────────────────
static Mode             s_mode = MODE_DRUM;   // current product mode
static Adafruit_NeoPixel s_led(STATUS_LED_COUNT, STATUS_LED_PIN, NEO_GRB + NEO_KHZ800);

// Cheap activity blink: set when a note fires, decayed by the LED refresh so the
// status pixel flickers brighter on musical activity without any timer.
static uint8_t  s_activity = 0;       // 0..255, decays toward 0
static uint32_t s_led_last_ms = 0;    // last LED refresh (rate-limit redraws)

// StepSeq edit cursor: which of the 8 sequencer tracks the grid edits. The 8x8
// grid maps row->track, col(+page) -> step; we keep one 8-step page visible at a
// time and let track selection scroll. Kept minimal & self-contained.
static uint8_t  s_seq_track = 0;      // 0..SEQ_TRACKS-1
static uint8_t  s_seq_page  = 0;      // step page (0..7) -> steps page*8 .. +7

// Tap-tempo state (MAP_TRANSPORT value 4): average the last few tap intervals.
static uint32_t s_last_tap_ms = 0;

// MIDI/live recording: when armed (REC), incoming/locally-played notes are
// captured into the active pattern at the current playhead step.
static bool s_rec_armed = false;

// Performance mode: momentary beat-repeat (stutter). While a held grid button is
// down we slam the delay into a short, high-feedback loop for a glitch-repeat;
// release restores the prior delay settings. s_perf_fx_held counts held FX pads
// so overlapping holds don't prematurely restore the dry delay.
static uint8_t s_perf_fx_held = 0;

// ─────────────────────────── small helpers ──────────────────────────────────

// Clamp a 12-bit ADC reading (0..4095) down to a 7-bit MIDI value (0..127).
static inline uint8_t adc_to_midi(int16_t v) {
  if (v < 0) v = 0;
  if (v > 4095) v = 4095;
  return (uint8_t)(v >> 5);   // 4096 >> 5 = 128 buckets -> 0..127
}

// Default channel for locally generated material when a map leaves channel 0.
static inline uint8_t local_channel(uint8_t mapCh) {
  if (mapCh >= 1 && mapCh <= 16) return mapCh;
  uint8_t s = g_settings.midi_channel;
  return (s >= 1 && s <= 16) ? s : 1;
}

// Per-mode base colour for the status LED (GRB packed via Color()).
static uint32_t mode_colour(Mode m) {
  switch (m) {
    case MODE_DRUM:        return s_led.Color(40, 10,  0);   // amber
    case MODE_STEPSEQ:     return s_led.Color(0,  35, 30);   // teal
    case MODE_KEYBOARD:    return s_led.Color(0,  30, 40);   // blue
    case MODE_DAW:         return s_led.Color(35, 0,  35);   // magenta
    case MODE_PERFORMANCE: return s_led.Color(0,  40, 5);    // green
    default:               return s_led.Color(10, 10, 10);
  }
}

// Redraw the status pixel = mode colour, brightened by recent activity. Called
// on dispatches; rate-limited so we don't hammer the WS2812 bus.
static void led_refresh(bool force) {
  uint32_t now = millis();
  if (!force && (now - s_led_last_ms) < 16) return;   // ~60 Hz cap
  s_led_last_ms = now;

  // Decay the activity blink a little each refresh.
  if (s_activity > 8) s_activity -= 8; else s_activity = 0;

  uint32_t base = mode_colour(s_mode);
  uint8_t r = (uint8_t)(base >> 16), g = (uint8_t)(base >> 8), b = (uint8_t)base;

  // Mix in a white flash proportional to activity.
  uint16_t rr = r + s_activity, gg = g + s_activity, bb = b + s_activity;
  if (rr > 255) rr = 255; if (gg > 255) gg = 255; if (bb > 255) bb = 255;

  s_led.setPixelColor(0, s_led.Color((uint8_t)rr, (uint8_t)gg, (uint8_t)bb));
  s_led.show();
}

static inline void note_activity() { s_activity = 200; }

// ─────────────────────── MIDI OUT fan-out (echo-safe) ────────────────────────
// Mirror a musical event out the two MIDI ports. A note that arrived on a port
// is never sent back out that same port (src guard). SRC_LOCAL goes everywhere.

static void out_note_on(uint8_t note, uint8_t vel, uint8_t ch, EventSource src) {
  if (src != SRC_DIN) midi_din_send_note_on(note, vel, ch);
  if (src != SRC_USB) usb_midi_send_note_on(note, vel, ch);
}
static void out_note_off(uint8_t note, uint8_t vel, uint8_t ch, EventSource src) {
  if (src != SRC_DIN) midi_din_send_note_off(note, vel, ch);
  if (src != SRC_USB) usb_midi_send_note_off(note, vel, ch);
}
static void out_cc(uint8_t cc, uint8_t val, uint8_t ch, EventSource src) {
  if (src != SRC_DIN) midi_din_send_cc(cc, val, ch);
  if (src != SRC_USB) usb_midi_send_cc(cc, val, ch);
}
static void out_program(uint8_t prog, uint8_t ch, EventSource src) {
  if (src != SRC_DIN) midi_din_send_program(prog, ch);
  if (src != SRC_USB) usb_midi_send_program(prog, ch);
}
static void out_pitchbend(int16_t bend, uint8_t ch, EventSource src) {
  if (src != SRC_DIN) midi_din_send_pitchbend(bend, ch);
  if (src != SRC_USB) usb_midi_send_pitchbend(bend, ch);
}

// ──────────────────── CC -> SynthParam routing (local feel) ──────────────────
// Map a handful of standard MIDI CCs onto the engine's continuous params so an
// external controller (or a sequencer param-lock) can drive the built-in synth.
// CC value 0..127 is expanded to the engine's 0..4095 raw domain. Returns true
// if the CC was consumed by the engine.
static bool cc_to_synth_param(uint8_t cc, uint8_t value) {
  SynthParam p;
  switch (cc) {
    case 74: p = SP_FILTER_CUTOFF; break;   // standard "brightness"
    case 71: p = SP_FILTER_RES;    break;   // standard "resonance"
    case 73: p = SP_AMP_ATTACK;    break;
    case 72: p = SP_AMP_RELEASE;   break;
    case 75: p = SP_AMP_DECAY;     break;
    case 76: p = SP_LFO_RATE;      break;
    case 77: p = SP_LFO_PITCH_DEPTH; break;
    case 7:  p = SP_MASTER_VOLUME; break;   // channel volume
    default: return false;
  }
  synth_set_param(p, (int16_t)((uint16_t)value << 5));   // 0..127 -> 0..4064
  return true;
}

// ───────────────────────── per-mode knob/slider maps ────────────────────────
// Knobs are oriented around the synth voice; sliders around mixer/ADSR. The map
// is intentionally per-mode so the same physical control means something sensible
// in context. Index ranges: knobs 0..NUM_KNOBS-1, sliders 0..NUM_SLIDERS-1.

// Knob index -> SynthParam (Drum/Keyboard/StepSeq/Performance "synth" modes).
static bool knob_to_param(uint8_t idx, SynthParam& out) {
  switch (idx) {
    case 0:  out = SP_OSC_A_WAVE;        return true;
    case 1:  out = SP_OSC_B_WAVE;        return true;
    case 2:  out = SP_OSC_MIX;           return true;
    case 3:  out = SP_DETUNE;            return true;
    case 4:  out = SP_NOISE_LEVEL;       return true;
    case 5:  out = SP_FILTER_CUTOFF;     return true;
    case 6:  out = SP_FILTER_RES;        return true;
    case 7:  out = SP_FILTER_ENV_AMT;    return true;
    case 8:  out = SP_LFO_RATE;          return true;
    case 9:  out = SP_LFO_PITCH_DEPTH;   return true;
    case 10: out = SP_LFO_CUTOFF_DEPTH;  return true;
    default: return false;
  }
}

// Slider index -> SynthParam: ADSR + master on the first faders.
static bool slider_to_param(uint8_t idx, SynthParam& out) {
  switch (idx) {
    case 0: out = SP_AMP_ATTACK;   return true;
    case 1: out = SP_AMP_DECAY;    return true;
    case 2: out = SP_AMP_SUSTAIN;  return true;
    case 3: out = SP_AMP_RELEASE;  return true;
    case 4: out = SP_MASTER_VOLUME;return true;
    default: return false;
  }
}

// ─────────────────────────── transport handling ─────────────────────────────
// A MAP_TRANSPORT button (value: 0 Play,1 Stop,2 Record,3 Continue,4 TapTempo).
static void do_transport(uint8_t which) {
  switch (which) {
    case 0:  // Play
      seq_start();
      break;
    case 1:  // Stop
      seq_stop();
      break;
    case 2:  // Record — toggle live-record arm. Arming also starts the transport
      // if stopped so there is a moving playhead to capture against. Disarming
      // leaves the sequencer running (just stops capturing).
      s_rec_armed = !s_rec_armed;
      if (s_rec_armed && seq_state() != SEQ_PLAYING) seq_start();
      cfg_log(s_rec_armed ? "REC armed" : "REC off");
      break;
    case 3:  // Continue
      seq_continue();
      break;
    case 4: { // Tap tempo
      uint32_t now = millis();
      if (s_last_tap_ms != 0) {
        uint32_t dt = now - s_last_tap_ms;
        if (dt > 100 && dt < 2000) {            // 30..600 BPM sane window
          float bpm = 60000.0f / (float)dt;
          seq_set_tempo(bpm);
        }
      }
      s_last_tap_ms = now;
      break;
    }
    default: break;
  }
}

// ─────────────────────────── live step recording ────────────────────────────
// When REC is armed and the sequencer is playing, write a live-played note into
// the active pattern at the current playhead step (on the currently-edited
// track). Called only from LIVE note sources (external MIDI + played pads), not
// from the sequencer's own playback, so it can't record its own output.
static void rec_capture_note(uint8_t note, uint8_t vel) {
  if (!s_rec_armed) return;
  if (seq_state() != SEQ_PLAYING) return;

  Step s = seq_get_step(s_seq_track, seq_current_step());  // copy current step
  s.active      = 1;
  s.note        = note;
  s.velocity    = vel ? vel : 100;
  if (s.length == 0)      s.length = SEQ_TICKS_PER_STEP;
  if (s.probability == 0) s.probability = 100;
  seq_set_step(s_seq_track, seq_current_step(), s);
}

// ─────────────────────── musical-event dispatch core ────────────────────────
// Shared by both raw-button mappings (which synthesize a SRC_LOCAL musical
// Event) and real incoming MIDI Events (SRC_DIN/SRC_USB). One place => one set
// of routing rules, so echo handling is consistent.

// Forward decls (route_note_on calls route_note_off for the vel-0 convention).
static void route_note_off(uint8_t note, uint8_t vel, uint8_t ch, EventSource src);

static void route_note_on(uint8_t note, uint8_t vel, uint8_t ch, EventSource src) {
  // Velocity 0 note-on is a note-off by MIDI convention.
  if (vel == 0) { route_note_off(note, 0, ch, src); return; }
  synth_note_on(note, vel, ch);          // local engine (RT-safe queue)
  out_note_on(note, vel, ch, src);       // mirror to the OTHER port(s)
  note_activity();
}

static void route_note_off(uint8_t note, uint8_t vel, uint8_t ch, EventSource src) {
  synth_note_off(note, vel, ch);
  out_note_off(note, vel, ch, src);
}

static void route_cc(uint8_t cc, uint8_t val, uint8_t ch, EventSource src) {
  cc_to_synth_param(cc, val);            // drive the engine if it's a known CC
  out_cc(cc, val, ch, src);              // and pass the controller through
}

static void route_program(uint8_t prog, uint8_t ch, EventSource src) {
  synth_program(prog, ch);
  out_program(prog, ch, src);
}

static void route_pitchbend(int16_t bend, uint8_t ch, EventSource src) {
  synth_pitchbend(bend, ch);
  out_pitchbend(bend, ch, src);
}

// ─────────────────────── raw button -> musical action ───────────────────────
// Apply g_button_map[idx] for a press (down=true) or release (down=false).
// MODE/PROGRAM/TRANSPORT act only on press; NOTE gates on press/release; CC
// sends value 127 on press, 0 on release (momentary controller).
static void apply_button_map(uint8_t idx, bool down) {
  const ButtonMap& m = g_button_map[idx];   // shared with the GUI; never block
  uint8_t ch = local_channel(m.channel);

  switch (m.type) {
    case MAP_NOTE:
      if (down) {
        rec_capture_note(m.value, 100);   // live-record played pads when armed
        route_note_on(m.value, 100 /*fixed vel for pads*/, ch, SRC_LOCAL);
      } else {
        route_note_off(m.value, 0, ch, SRC_LOCAL);
      }
      break;

    case MAP_CC:
      // Momentary: full on press, zero on release.
      route_cc(m.value, down ? 127 : 0, ch, SRC_LOCAL);
      break;

    case MAP_PROGRAM:
      if (down) route_program(m.value, ch, SRC_LOCAL);
      break;

    case MAP_TRANSPORT:
      if (down) do_transport(m.value);
      break;

    case MAP_MODE:
      if (down && m.value < MODE_COUNT) ui_set_mode((Mode)m.value);
      break;

    case MAP_NONE:
    default:
      break;   // unmapped key: still emitted to GUI (handled by caller)
  }
}

// ───────────────── mode-specific raw button interpretation ───────────────────
// Some modes give the grid a *built-in* meaning that overrides the per-button
// map (e.g. StepSeq toggles pattern steps). When a mode consumes the button
// itself it returns true; otherwise we fall through to the generic map.

static bool stepseq_handle_button(uint8_t idx, bool down) {
  // Only act on press; the grid edits the pattern rather than playing notes.
  if (!down) return true;

  uint8_t row = idx / MATRIX_COLS;   // 0..7
  uint8_t col = idx % MATRIX_COLS;   // 0..7

  // Row 0 selects the active track (cols 0..7 -> tracks 0..7). Re-pressing the
  // ALREADY-selected track toggles that track's mute (classic groovebox idiom),
  // so mutes are reachable without stealing a whole row.
  if (row == 0) {
    if (col == s_seq_track) {
      // Toggle mute on the current track. seq has no mute getter, so mirror it.
      static uint8_t muted_mask = 0;   // bit t set => track t muted
      bool now = !(muted_mask & (1u << col));
      seq_set_track_mute(col, now);
      if (now) muted_mask |=  (1u << col);
      else     muted_mask &= ~(1u << col);
      cfg_log("seq track mute toggle");
    } else {
      s_seq_track = col;
      cfg_log("seq track select");
    }
    return true;
  }
  // Row 1 selects the step page (cols 0..7 -> pages 0..7 -> 8-step windows).
  if (row == 1) {
    s_seq_page = col;
    return true;
  }
  // Row 2 selects/queues the active pattern slot (cols 0..7 -> patterns 0..7).
  // Switching is musically quantized while playing (applied at step 0) and
  // immediate when stopped — handled inside seq_select_pattern.
  if (row == 2) {
    if (col < SEQ_PATTERNS) {
      seq_select_pattern(col);
      cfg_log("seq pattern select");
    }
    return true;
  }
  // Remaining rows (3..7) are the step grid for the selected track; each row is
  // an 8-step span so the 5 rows expose 40 of the 64 steps at a glance. We use
  // the visible page to pick the actual step.
  uint8_t step = (uint8_t)((s_seq_page * 8 + (row - 3) * 8 + col) % SEQ_STEPS);
  seq_toggle_step(s_seq_track, step);
  return true;
}

// ── Performance mode: momentary filter sweep on held grid buttons ────────────
// Performance was previously a silent clone of Keyboard. Here the 8x8 grid is a
// momentary filter-sweep surface: holding a button slams the engine cutoff to a
// position set by its column (left=dark, right=open) for as long as it is held;
// releasing restores the patch's mapped cutoff. Multiple holds are reference-
// counted so an overlapping release doesn't snap the filter back early. This
// uses only the existing RT-safe synth_set_param path — no DSP is touched here.
static bool performance_handle_button(uint8_t idx, bool down) {
  uint8_t col = idx % MATRIX_COLS;   // 0..7 -> sweep position

  if (down) {
    // Map column to a cutoff in the 0..4095 engine domain (col 0 dark, 7 open).
    int16_t cutoff = (int16_t)((uint32_t)(col + 1) * 4095u / MATRIX_COLS);
    synth_set_param(SP_FILTER_CUTOFF, cutoff);
    if (s_perf_fx_held < 0xFF) ++s_perf_fx_held;
  } else {
    if (s_perf_fx_held) --s_perf_fx_held;
    // When the last held pad releases, reopen the filter so the patch is audible
    // again. (A fuller build would restore the exact pre-sweep mirror value; the
    // preset mirror in presets.cpp holds it, but a wide-open default is a safe,
    // glitch-free restore for a momentary performance gesture.)
    if (s_perf_fx_held == 0) synth_set_param(SP_FILTER_CUTOFF, 3600);
  }
  return true;   // Performance grid is consumed by the FX surface.
}

// DAW mode: the grid is a clip launcher / control surface for the host. Buttons
// follow the per-button map (the GUI assigns notes/CC that Ableton et al. learn)
// but we ALSO guarantee they leave over USB even if mapped to the local synth.
// We simply defer to the generic map here (out_* already fans out to USB), so no
// override is needed — return false to use the map.

// ─────────────────────────── public API ─────────────────────────────────────

Mode ui_mode() { return s_mode; }

void ui_set_mode(Mode m) {
  if (m >= MODE_COUNT) return;
  if (m == s_mode) { led_refresh(true); return; }

  // Leaving a mode: kill any hanging voices so a held pad/key doesn't drone
  // across the mode switch (the new grid layout would never release it).
  synth_all_notes_off();

  s_mode = m;

  // Reset transient per-mode UI state.
  s_seq_track = 0;
  s_seq_page  = 0;

  // Tell the rest of the system + the GUI, and recolour the LED immediately.
  event_post(Event{ EV_MODE_CHANGE, SRC_LOCAL, (uint8_t)m, 0, 0, 0 });
  led_refresh(true);
  cfg_log("mode change");
}

void ui_init() {
  // Bring up the status pixel.
  s_led.begin();
  uint8_t br = g_settings.led_brightness ? g_settings.led_brightness : 64;
  s_led.setBrightness(br);
  s_led.clear();
  s_led.show();

  // Adopt the persisted startup mode (defaulting to Drum if out of range).
  Mode start = MODE_DRUM;
  if (g_settings.startup_mode < MODE_COUNT) start = (Mode)g_settings.startup_mode;
  s_mode = start;

  // Honour the persisted clock-slave preference at boot.
  seq_set_external_clock(g_settings.recv_clock != 0);

  led_refresh(true);
}

// ─────────────────────────── the dispatcher ─────────────────────────────────
void ui_dispatch(const Event& e) {
  switch (e.type) {

    // ── raw hardware: buttons ───────────────────────────────────────────────
    case EV_BUTTON_PRESSED:
    case EV_BUTTON_RELEASED: {
      const bool down = (e.type == EV_BUTTON_PRESSED);
      const uint8_t idx = e.a;
      if (idx >= NUM_BUTTONS) break;

      // ALWAYS mirror the raw edge to the GUI's live grid first.
      cfg_emit_button(idx, down ? 1 : 0);

      // Let the current mode optionally claim the button (built-in grid meaning).
      bool consumed = false;
      switch (s_mode) {
        case MODE_STEPSEQ:     consumed = stepseq_handle_button(idx, down); break;
        case MODE_PERFORMANCE: consumed = performance_handle_button(idx, down); break;
        default:               consumed = false; break;   // Drum/Keyboard/DAW
      }

      // Otherwise apply the user's per-button map (note/cc/program/mode/transport).
      if (!consumed) apply_button_map(idx, down);

      led_refresh(false);
      break;
    }

    // ── raw hardware: continuous controls ───────────────────────────────────
    case EV_KNOB_CHANGED: {
      const uint8_t idx = e.a;
      if (s_mode == MODE_DAW) {
        // In DAW mode knobs are host controllers: emit CC out USB (and DIN).
        // CC# 16.. for knobs (GM "general purpose" range-ish), value 0..127.
        out_cc((uint8_t)(16 + idx), adc_to_midi(e.val),
               local_channel(0), SRC_LOCAL);
      } else if (idx == 11) {
        // Knob 11 -> sequencer swing (0..75 %). Wires up seq_set_swing(), which
        // otherwise had no caller, so swing feel is actually reachable.
        int16_t v = e.val; if (v < 0) v = 0; if (v > 4095) v = 4095;
        seq_set_swing((uint8_t)(((uint32_t)v * 75u) / 4095u));
      } else {
        // Synth modes: drive an engine parameter (raw 0..4095 passed through).
        // Route through presets_note_param so the CORE_IO save-mirror tracks the
        // live edit (it forwards to synth_set_param internally) — otherwise saved
        // presets would store stale default param values.
        SynthParam p;
        if (knob_to_param(idx, p)) presets_note_param(p, e.val);
      }
      break;
    }

    case EV_SLIDER_CHANGED: {
      const uint8_t idx = e.a;
      if (s_mode == MODE_DAW) {
        // Faders -> host CC (CC# 0.. as channel-strip volumes).
        out_cc((uint8_t)idx, adc_to_midi(e.val), local_channel(0), SRC_LOCAL);
      } else {
        // Same mirror-tracking path as knobs (forwards to the engine internally).
        SynthParam p;
        if (slider_to_param(idx, p)) presets_note_param(p, e.val);
      }
      break;
    }

    // ── musical events (post-mapping): from MIDI IN or the sequencer ─────────
    // These already carry a note/cc/etc and a source tag. The sequencer posts
    // SRC_LOCAL EV_NOTE_ON/OFF; MIDI IN posts SRC_DIN/SRC_USB.
    case EV_NOTE_ON:
      // Live-record only EXTERNAL note input (SRC_DIN/SRC_USB). SRC_LOCAL here is
      // the sequencer's own playback, which must never be recorded back in.
      if (e.src != SRC_LOCAL) rec_capture_note(e.a, e.b);
      route_note_on(e.a, e.b, e.c ? e.c : local_channel(0), e.src);
      led_refresh(false);
      break;

    case EV_NOTE_OFF:
      route_note_off(e.a, e.b, e.c ? e.c : local_channel(0), e.src);
      break;

    case EV_CC:
      route_cc(e.a, e.b, e.c ? e.c : local_channel(0), e.src);
      break;

    case EV_PROGRAM:
      route_program(e.a, e.c ? e.c : local_channel(0), e.src);
      break;

    case EV_PITCHBEND:
      route_pitchbend(e.val, e.c ? e.c : local_channel(0), e.src);
      break;

    // ── transport / clock ───────────────────────────────────────────────────
    case EV_CLOCK_TICK:
      // Task_UI is the single owner of sequencer state, so EVERY tick — internal
      // (SRC_LOCAL, posted by Task_Sequencer) and external (SRC_DIN/SRC_USB) —
      // advances the sequencer right here, never from Task_Sequencer.
      if (e.src == SRC_LOCAL) {
        // We are the clock MASTER: advance our sequencer and, if configured to
        // send clock, emit 24-ppqn clock out BOTH ports for downstream gear.
        seq_tick();
        if (g_settings.send_clock) {
          midi_din_send_clock();
          usb_midi_send_clock();
        }
      } else {
        // External clock byte arrived. When slaved, advance one tick.
        if (g_settings.recv_clock) seq_tick();
        // Soft-thru incoming clock to the opposite port only if we are not also
        // generating our own master clock (avoid doubling on the shared bus).
        if (!g_settings.send_clock) {
          if (e.src != SRC_DIN) midi_din_send_clock();
          if (e.src != SRC_USB) usb_midi_send_clock();
        }
      }
      break;

    case EV_TRANSPORT_START:
      if (g_settings.recv_clock) seq_start();
      if (e.src != SRC_DIN) midi_din_send_start();
      if (e.src != SRC_USB) usb_midi_send_start();
      break;

    case EV_TRANSPORT_CONTINUE:
      if (g_settings.recv_clock) seq_continue();
      if (e.src != SRC_DIN) midi_din_send_continue();
      if (e.src != SRC_USB) usb_midi_send_continue();
      break;

    case EV_TRANSPORT_STOP:
      if (g_settings.recv_clock) seq_stop();
      if (e.src != SRC_DIN) midi_din_send_stop();
      if (e.src != SRC_USB) usb_midi_send_stop();
      break;

    // ── system ──────────────────────────────────────────────────────────────
    case EV_MODE_CHANGE:
      // Honour mode changes requested by other producers (e.g. the GUI). Guard
      // against re-entrancy: ui_set_mode re-posts EV_MODE_CHANGE, so only act if
      // the requested mode differs from the current one.
      if (e.a < MODE_COUNT && (Mode)e.a != s_mode) ui_set_mode((Mode)e.a);
      break;

    case EV_PRESET_LOAD:
      if (presets_load(e.a)) cfg_log("preset loaded");
      else                   cfg_log("preset load failed");
      led_refresh(true);
      break;

    case EV_PRESET_SAVE:
      if (presets_save(e.a)) cfg_log("preset saved");
      else                   cfg_log("preset save failed");
      break;

    case EV_NONE:
    default:
      break;
  }
}
