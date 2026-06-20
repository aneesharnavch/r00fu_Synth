/*
 * midi_din.cpp — 5-pin DIN MIDI I/O on UART1 (FortySevenEffects MIDI Library).
 *
 * RX: HardwareSerial(MIDI_UART_NUM) bound to the MIDI library; midi_din_poll()
 *     reads complete messages and posts Events tagged SRC_DIN onto the global
 *     event queue (the dispatcher routes them — scanners/drivers never call the
 *     synth/sequencer directly, per events.h).
 * TX: the send helpers serialize standard MIDI onto MIDI_TX_PIN.
 *
 * Polled by Task_MIDI (CORE_IO). Send helpers are task-context only.
 * Matches src/drivers/midi_din.h exactly.
 */
#include <Arduino.h>
#include <HardwareSerial.h>
#include <MIDI.h>            // FortySevenEffects MIDI Library

#include "midi_din.h"
#include "events.h"

// ── UART + MIDI instance ───────────────────────────────────────────────────
// Dedicated HardwareSerial on the configured UART. We pass the pins to begin()
// so the GPIO matrix routes MIDI_RX_PIN/MIDI_TX_PIN to this UART regardless of
// the default pin assignment for UART1.
static HardwareSerial s_din_uart(MIDI_UART_NUM);

// Bind the MIDI library to that serial port. MIDI_CREATE_INSTANCE expands to a
// midi::MidiInterface<...> named s_midi over a SerialMIDI transport on the UART.
MIDI_CREATE_INSTANCE(HardwareSerial, s_din_uart, s_midi);

// ── Helpers ────────────────────────────────────────────────────────────────
// The MIDI library reports channels as 1..16 already, matching our Event 'c'.
// Clamp the caller's channel to a legal 1..16 for the send path.
static inline uint8_t clamp_channel(uint8_t channel) {
  if (channel < 1)  return 1;
  if (channel > 16) return 16;
  return channel;
}

// Build and post a musical Event with an explicit source tag. CC/program/
// pitchbend/clock/transport are not covered by the events.h convenience
// constructors, so we assemble the struct directly here.
static inline void post_din(EventType t, uint8_t a, uint8_t b, uint8_t c, int16_t val) {
  Event e{ t, SRC_DIN, a, b, c, val };
  event_post(e);
}

// ── Public API ─────────────────────────────────────────────────────────────
void midi_din_init() {
  // The MIDI library's begin() internally calls s_din_uart.begin(31250) with
  // this UART's *default* pins — so run it first, then re-begin the UART with
  // our explicit MIDI_RX_PIN/MIDI_TX_PIN (same baud) so the GPIO matrix routes
  // the MIDI signals to the wired pins. Order matters: the pin assignment wins.
  s_midi.begin(MIDI_CHANNEL_OMNI);
  s_din_uart.begin(MIDI_BAUD, SERIAL_8N1, MIDI_RX_PIN, MIDI_TX_PIN);

  // THRU off — routing is done in software (dispatcher) to avoid echo loops.
  s_midi.turnThruOff();
}

void midi_din_poll() {
  // read() returns true once per fully-parsed incoming message. Drain whatever
  // arrived this tick; never blocks (it only consumes already-buffered bytes).
  while (s_midi.read()) {
    const midi::MidiType  type = s_midi.getType();
    const uint8_t         d1   = s_midi.getData1();
    const uint8_t         d2   = s_midi.getData2();
    const uint8_t         ch   = s_midi.getChannel();   // 1..16 (0 for realtime)

    switch (type) {
      case midi::NoteOn:
        // Running-status / zero-velocity NoteOn is a NoteOff by convention.
        if (d2 == 0) post_din(EV_NOTE_OFF, d1, 0,  ch, 0);
        else         post_din(EV_NOTE_ON,  d1, d2, ch, 0);
        break;

      case midi::NoteOff:
        post_din(EV_NOTE_OFF, d1, d2, ch, 0);
        break;

      case midi::ControlChange:
        post_din(EV_CC, d1, d2, ch, 0);
        break;

      case midi::ProgramChange:
        post_din(EV_PROGRAM, d1, 0, ch, 0);
        break;

      case midi::PitchBend: {
        // MIDI sends 14-bit unsigned 0..16383 (center 8192); Event.val is
        // signed -8192..8191. getData1()=LSB, getData2()=MSB.
        int16_t bend = (int16_t)(((uint16_t)d2 << 7) | (uint16_t)d1) - 8192;
        Event e{ EV_PITCHBEND, SRC_DIN, 0, 0, ch, bend };
        event_post(e);
        break;
      }

      case midi::Clock:
        post_din(EV_CLOCK_TICK, 0, 0, 0, 0);
        break;

      case midi::Start:
        post_din(EV_TRANSPORT_START, 0, 0, 0, 0);
        break;

      case midi::Continue:
        post_din(EV_TRANSPORT_CONTINUE, 0, 0, 0, 0);
        break;

      case midi::Stop:
        post_din(EV_TRANSPORT_STOP, 0, 0, 0, 0);
        break;

      default:
        // ActiveSensing, SystemExclusive, etc. — ignored for now.
        break;
    }
  }
}

// ── Send helpers ───────────────────────────────────────────────────────────
void midi_din_send_note_on(uint8_t note, uint8_t vel, uint8_t channel) {
  s_midi.sendNoteOn(note, vel, clamp_channel(channel));
}

void midi_din_send_note_off(uint8_t note, uint8_t vel, uint8_t channel) {
  s_midi.sendNoteOff(note, vel, clamp_channel(channel));
}

void midi_din_send_cc(uint8_t cc, uint8_t value, uint8_t channel) {
  s_midi.sendControlChange(cc, value, clamp_channel(channel));
}

void midi_din_send_program(uint8_t program, uint8_t channel) {
  s_midi.sendProgramChange(program, clamp_channel(channel));
}

void midi_din_send_pitchbend(int16_t bend /*-8192..8191*/, uint8_t channel) {
  // The library's sendPitchBend(int, ch) takes a signed -8192..8191 value.
  s_midi.sendPitchBend((int)bend, clamp_channel(channel));
}

// ── Transport / clock (channel-less real-time bytes) ───────────────────────
void midi_din_send_clock()    { s_midi.sendRealTime(midi::Clock);    }
void midi_din_send_start()    { s_midi.sendRealTime(midi::Start);    }
void midi_din_send_continue() { s_midi.sendRealTime(midi::Continue); }
void midi_din_send_stop()     { s_midi.sendRealTime(midi::Stop);     }
