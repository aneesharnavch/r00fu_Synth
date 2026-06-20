/*
 * config_protocol.h — GUI configuration protocol over USB-CDC Serial.
 *
 * Speaks the exact line protocol the PC GUI (gui/r00fu_config.py) and the
 * prototype sketch already use, at 115200 baud, newline-terminated ASCII:
 *
 *   Device -> PC:
 *     HELLO r00fu_synth <fwver>        on boot and on IDENTIFY
 *     BTN <index> <row> <col> <0|1>    button edge (1=press, 0=release)
 *     ACK MAP <index>                  a mapping was stored
 *     MAP <index> <type> <ch> <val> <label>   one row of a DUMP
 *     LOG <text>                       human-readable debug
 *   PC -> Device:
 *     IDENTIFY                         announce yourself
 *     MAP <index> <type> <ch> <val> <label...>  store a mapping
 *     DUMP                             echo all mappings back as MAP lines
 *
 * MAP lines write straight into the shared g_button_map (midi_map.h), so the
 * device acts on the GUI's assignment standalone. This runs alongside USB-MIDI
 * on the same TinyUSB composite device (CDC + MIDI).
 *
 * Implemented in src/drivers/config_protocol.cpp. Polled by Task_UI (CORE_IO).
 */
#pragma once
#include <stdint.h>
#include "config.h"
#include "midi_map.h"   // protocol reads/writes g_button_map

#define CFG_FW_VERSION "0.2.0"   // reported in the HELLO line

// Bring up the CDC link state and send the initial HELLO. Call once from
// Task_UI after Serial/USB is up.
void cfg_init();

// Read available CDC bytes, assemble lines, and execute IDENTIFY / MAP / DUMP.
// Non-blocking; safe to call every UI tick.
void cfg_poll();

// Emit a BTN edge line for the GUI's live grid. Called by the dispatcher when
// it observes EV_BUTTON_PRESSED/RELEASED (state: 1=press, 0=release). row/col
// are derived from idx internally.
void cfg_emit_button(uint8_t idx, uint8_t state);

// Optional structured log line: "LOG <text>". Cheap, drop if CDC not connected.
void cfg_log(const char* text);
