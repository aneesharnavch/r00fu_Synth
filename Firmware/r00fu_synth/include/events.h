/*
 * events.h — the one and only way modules talk to each other.
 *
 * Hardware scanners NEVER call the synth/sequencer directly. They post an
 * Event to the global queue; the dispatcher (in Task_UI / modes.cpp) decides
 * what each event means in the current mode and routes it onward.
 *
 * Implemented in src/system/events.cpp.
 */
#pragma once
#include <stdint.h>

enum EventType : uint8_t {
  EV_NONE = 0,
  // --- raw hardware ---
  EV_BUTTON_PRESSED,    // a=button index (0..63)
  EV_BUTTON_RELEASED,   // a=button index
  EV_KNOB_CHANGED,      // a=knob index,  val=0..4095 (smoothed)
  EV_SLIDER_CHANGED,    // a=slider index,val=0..4095 (smoothed)
  // --- musical (post-mapping) ---
  EV_NOTE_ON,           // a=note, b=velocity, c=channel
  EV_NOTE_OFF,          // a=note, b=velocity, c=channel
  EV_CC,                // a=cc,   b=value,    c=channel
  EV_PROGRAM,           // a=program, c=channel
  EV_PITCHBEND,         // val=-8192..8191, c=channel
  // --- transport / clock ---
  EV_CLOCK_TICK,        // 24 ppqn
  EV_TRANSPORT_START,
  EV_TRANSPORT_STOP,
  EV_TRANSPORT_CONTINUE,
  // --- system ---
  EV_MODE_CHANGE,       // a=mode id
  EV_PRESET_LOAD,       // a=preset slot
  EV_PRESET_SAVE,       // a=preset slot
};

// Source tag lets the dispatcher avoid echo loops (e.g. don't re-send a USB
// note straight back out USB).
enum EventSource : uint8_t {
  SRC_LOCAL = 0,   // generated on-device (matrix/sequencer)
  SRC_DIN,         // came from DIN MIDI IN
  SRC_USB,         // came from USB MIDI IN
};

struct Event {
  EventType   type;
  EventSource src;
  uint8_t     a;     // primary  (index / note / cc / program / mode)
  uint8_t     b;     // value    (velocity / cc value)
  uint8_t     c;     // channel  (1..16) where relevant
  int16_t     val;   // wide value (analog 0..4095, pitchbend signed)
};

// ── Queue API ────────────────────────────────────────────────────────────
bool events_init();                                  // create the queue(s)
bool event_post(const Event& e);                     // task context
bool event_post_from_isr(const Event& e);            // ISR context
bool event_get(Event& out, uint32_t timeout_ms);     // consumer blocks here
uint32_t event_dropped_count();                      // diagnostics

// Convenience constructors (keep call sites readable)
static inline Event ev_button(EventType t, uint8_t idx) {
  return Event{ t, SRC_LOCAL, idx, 0, 0, 0 };
}
static inline Event ev_analog(EventType t, uint8_t idx, int16_t v) {
  return Event{ t, SRC_LOCAL, idx, 0, 0, v };
}
static inline Event ev_note(EventType t, uint8_t note, uint8_t vel, uint8_t ch, EventSource s) {
  return Event{ t, s, note, vel, ch, 0 };
}
