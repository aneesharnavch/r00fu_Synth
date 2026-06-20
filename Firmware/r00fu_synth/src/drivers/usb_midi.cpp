/*
 * usb_midi.cpp — USB-MIDI device endpoint (Adafruit TinyUSB, composite w/ CDC).
 *
 * Adafruit_USBD_MIDI provides a raw USB-MIDI jack on the shared TinyUSB device
 * (the same device that exposes Serial/CDC for config_protocol — they coexist
 * as a composite). We wrap that jack in the FortySevenEffects MIDI Library so
 * parsing/serialization is identical to the DIN path; incoming messages are
 * posted as Events tagged SRC_USB (the tag stops the dispatcher echoing a USB
 * note straight back out USB).
 *
 * Polled by Task_MIDI (CORE_IO). Send helpers no-op until usb_midi_mounted().
 * Matches src/drivers/usb_midi.h exactly.
 */
#include <Arduino.h>
#include <Adafruit_TinyUSB.h>   // Adafruit_USBD_MIDI, USBDevice
#include <MIDI.h>               // FortySevenEffects MIDI Library

#include "usb_midi.h"
#include "events.h"

// ── USB-MIDI jack + MIDI instance ──────────────────────────────────────────
// One virtual cable (the default). Adafruit_USBD_MIDI satisfies the MIDI
// library's transport interface, so MIDI_CREATE_INSTANCE binds directly to it.
static Adafruit_USBD_MIDI s_usb_jack;
MIDI_CREATE_INSTANCE(Adafruit_USBD_MIDI, s_usb_jack, s_umidi);

static bool s_started = false;   // usb_midi_init() (MIDI lib begin) ran once

// ── EARLY interface registration (critical for enumeration) ──────────────────
// With -DARDUINO_USB_MODE=0 + -DARDUINO_USB_CDC_ON_BOOT=1 the arduino-esp32 core
// calls USBDevice.begin() from its OWN pre-setup init (initArduino, before
// setup() runs). TinyUSB interfaces MUST be added to the device BEFORE it is
// begun, or the host enumerates the composite descriptor without our MIDI jack
// and it never appears. usb_midi_init() runs from Task_MIDI — far too late.
//
// So we register the jack from a global C++ constructor, which the runtime runs
// during static-init, BEFORE initArduino()/USBDevice.begin(). The jack's
// begin() here only allocates the USB interface/endpoints and string descriptor
// (it does NOT need the FortySevenEffects MIDI layer, which we still bring up in
// usb_midi_init()). If a particular core revision happens to have already begun
// USB by static-init time, we force a re-enumeration (detach/attach) so the host
// re-reads the now-complete descriptor.
namespace {
struct UsbMidiEarlyRegister {
  UsbMidiEarlyRegister() {
    s_usb_jack.setStringDescriptor("r00fu_synth MIDI");
    s_usb_jack.begin();                 // add the MIDI interface to the device

    // Belt-and-suspenders: if USB was already started, re-enumerate so the host
    // picks up the freshly-added interface.
    if (TinyUSBDevice.mounted()) {
      TinyUSBDevice.detach();
      delay(10);
      TinyUSBDevice.attach();
    }
  }
};
// File-scope instance => its constructor runs at static-init, before setup().
UsbMidiEarlyRegister s_usb_midi_early_register;
}  // namespace

// ── Helpers ────────────────────────────────────────────────────────────────
static inline uint8_t clamp_channel(uint8_t channel) {
  if (channel < 1)  return 1;
  if (channel > 16) return 16;
  return channel;
}

static inline void post_usb(EventType t, uint8_t a, uint8_t b, uint8_t c, int16_t val) {
  Event e{ t, SRC_USB, a, b, c, val };
  event_post(e);
}

// ── Public API ─────────────────────────────────────────────────────────────
void usb_midi_init() {
  if (s_started) return;   // idempotent

  // NOTE: the USB-MIDI *interface* (s_usb_jack) is registered far earlier, from
  // the UsbMidiEarlyRegister static constructor above, so it is present in the
  // composite descriptor BEFORE the core's USBDevice.begin() runs. Here we only
  // bring up the FortySevenEffects MIDI parsing layer on top of that jack
  // (OMNI in; THRU off — software routing only). Doing the jack registration
  // here (from Task_MIDI) would be too late to enumerate.
  s_umidi.begin(MIDI_CHANNEL_OMNI);
  s_umidi.turnThruOff();

  s_started = true;
}

bool usb_midi_mounted() {
  // Ready once the host has enumerated the composite device.
  return s_started && USBDevice.mounted();
}

void usb_midi_poll() {
  if (!s_started) return;

  while (s_umidi.read()) {
    const midi::MidiType type = s_umidi.getType();
    const uint8_t        d1   = s_umidi.getData1();
    const uint8_t        d2   = s_umidi.getData2();
    const uint8_t        ch   = s_umidi.getChannel();

    switch (type) {
      case midi::NoteOn:
        if (d2 == 0) post_usb(EV_NOTE_OFF, d1, 0,  ch, 0);
        else         post_usb(EV_NOTE_ON,  d1, d2, ch, 0);
        break;

      case midi::NoteOff:
        post_usb(EV_NOTE_OFF, d1, d2, ch, 0);
        break;

      case midi::ControlChange:
        post_usb(EV_CC, d1, d2, ch, 0);
        break;

      case midi::ProgramChange:
        post_usb(EV_PROGRAM, d1, 0, ch, 0);
        break;

      case midi::PitchBend: {
        int16_t bend = (int16_t)(((uint16_t)d2 << 7) | (uint16_t)d1) - 8192;
        Event e{ EV_PITCHBEND, SRC_USB, 0, 0, ch, bend };
        event_post(e);
        break;
      }

      case midi::Clock:    post_usb(EV_CLOCK_TICK,        0, 0, 0, 0); break;
      case midi::Start:    post_usb(EV_TRANSPORT_START,   0, 0, 0, 0); break;
      case midi::Continue: post_usb(EV_TRANSPORT_CONTINUE,0, 0, 0, 0); break;
      case midi::Stop:     post_usb(EV_TRANSPORT_STOP,    0, 0, 0, 0); break;

      default:
        break;
    }
  }
}

// ── Send helpers (cable 0). No-op until mounted. ───────────────────────────
void usb_midi_send_note_on(uint8_t note, uint8_t vel, uint8_t channel) {
  if (!usb_midi_mounted()) return;
  s_umidi.sendNoteOn(note, vel, clamp_channel(channel));
}

void usb_midi_send_note_off(uint8_t note, uint8_t vel, uint8_t channel) {
  if (!usb_midi_mounted()) return;
  s_umidi.sendNoteOff(note, vel, clamp_channel(channel));
}

void usb_midi_send_cc(uint8_t cc, uint8_t value, uint8_t channel) {
  if (!usb_midi_mounted()) return;
  s_umidi.sendControlChange(cc, value, clamp_channel(channel));
}

void usb_midi_send_program(uint8_t program, uint8_t channel) {
  if (!usb_midi_mounted()) return;
  s_umidi.sendProgramChange(program, clamp_channel(channel));
}

void usb_midi_send_pitchbend(int16_t bend /*-8192..8191*/, uint8_t channel) {
  if (!usb_midi_mounted()) return;
  s_umidi.sendPitchBend((int)bend, clamp_channel(channel));
}

// ── Transport / clock ──────────────────────────────────────────────────────
void usb_midi_send_clock()    { if (usb_midi_mounted()) s_umidi.sendRealTime(midi::Clock);    }
void usb_midi_send_start()    { if (usb_midi_mounted()) s_umidi.sendRealTime(midi::Start);    }
void usb_midi_send_continue() { if (usb_midi_mounted()) s_umidi.sendRealTime(midi::Continue); }
void usb_midi_send_stop()     { if (usb_midi_mounted()) s_umidi.sendRealTime(midi::Stop);     }
