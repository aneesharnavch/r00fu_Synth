/*
 * usb_midi.h — USB-MIDI device endpoint (Adafruit TinyUSB, composite w/ CDC).
 *
 * Mirrors midi_din.h so the dispatcher can treat both ports uniformly. Incoming
 * USB-MIDI is parsed and posted as Events tagged SRC_USB (the src tag stops the
 * dispatcher from echoing a USB note straight back out USB). This is the port a
 * DAW (Ableton/FL/Reaper/Bitwig) sees in DAW-controller mode.
 *
 * The USB stack is brought up centrally (TinyUSB composite = CDC + MIDI) so
 * Serial (config_protocol) and this share one device. Implemented in
 * src/drivers/usb_midi.cpp. Polled by Task_MIDI (CORE_IO).
 */
#pragma once
#include <stdint.h>
#include "config.h"

// Register the USB-MIDI interface on the TinyUSB device and start it.
// Call once from Task_MIDI (after the shared USB device is begun). Idempotent.
void usb_midi_init();

// Pump the TinyUSB MIDI RX, parse messages, post SRC_USB Events. Non-blocking.
void usb_midi_poll();

// True once the host has enumerated and the MIDI interface is ready. Send
// helpers no-op until this is true.
bool usb_midi_mounted();

// ── Send helpers (cable 0). channel is 1..16. ──────────────────────────────
void usb_midi_send_note_on (uint8_t note, uint8_t vel, uint8_t channel);
void usb_midi_send_note_off(uint8_t note, uint8_t vel, uint8_t channel);
void usb_midi_send_cc      (uint8_t cc,   uint8_t value, uint8_t channel);
void usb_midi_send_program (uint8_t program, uint8_t channel);
void usb_midi_send_pitchbend(int16_t bend /*-8192..8191*/, uint8_t channel);

// ── Transport / clock ──────────────────────────────────────────────────────
void usb_midi_send_clock();        // 0xF8
void usb_midi_send_start();        // 0xFA
void usb_midi_send_continue();     // 0xFB
void usb_midi_send_stop();         // 0xFC
