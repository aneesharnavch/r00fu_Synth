/*
 * midi_din.h — 5-pin DIN MIDI I/O on UART1 (FortySevenEffects MIDI Library).
 *
 * MIDI IN  (MIDI_RX_PIN, via opto) is parsed and turned into Events tagged
 * SRC_DIN, so the dispatcher can route external notes/CC/clock without echo
 * loops:
 *     0x90/0x80 -> EV_NOTE_ON / EV_NOTE_OFF
 *     0xB0      -> EV_CC
 *     0xC0      -> EV_PROGRAM
 *     0xE0      -> EV_PITCHBEND
 *     0xF8      -> EV_CLOCK_TICK
 *     0xFA/0xFB/0xFC -> EV_TRANSPORT_START / _CONTINUE / _STOP
 *
 * MIDI OUT (MIDI_TX_PIN) is driven by the send helpers below, called from the
 * dispatcher when a local/sequenced musical event should leave the device.
 *
 * Implemented in src/drivers/midi_din.cpp.
 * Polled by Task_MIDI (CORE_IO). The send helpers are task-context only.
 */
#pragma once
#include <stdint.h>
#include "config.h"   // MIDI_TX_PIN/MIDI_RX_PIN/MIDI_BAUD/MIDI_UART_NUM

// Bind the MIDI library to UART1 on the configured pins at 31250 baud,
// THRU off (we route in software). Call once from Task_MIDI.
void midi_din_init();

// Drain the UART RX, parse complete messages, and post SRC_DIN Events.
// Call as often as possible in the MIDI task loop. Non-blocking.
void midi_din_poll();

// ── Send helpers (serialize onto DIN OUT). channel is 1..16. ───────────────
void midi_din_send_note_on (uint8_t note, uint8_t vel, uint8_t channel);
void midi_din_send_note_off(uint8_t note, uint8_t vel, uint8_t channel);
void midi_din_send_cc      (uint8_t cc,   uint8_t value, uint8_t channel);
void midi_din_send_program (uint8_t program, uint8_t channel);
void midi_din_send_pitchbend(int16_t bend /*-8192..8191*/, uint8_t channel);

// ── Transport / clock (channel-less real-time bytes) ───────────────────────
void midi_din_send_clock();        // 0xF8, 24 ppqn
void midi_din_send_start();        // 0xFA
void midi_din_send_continue();     // 0xFB
void midi_din_send_stop();         // 0xFC
