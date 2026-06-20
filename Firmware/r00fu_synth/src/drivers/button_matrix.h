/*
 * button_matrix.h — 8x8 key matrix scanner (driver layer).
 *
 * Drives one ROW low at a time and reads the COLUMNS (pull-ups), exactly like
 * the prototype sketch. Per-key diodes give N-key rollover. Software debounce
 * (MATRIX_DEBOUNCE_COUNT stable reads) gates every edge.
 *
 * This driver NEVER touches the synth/sequencer/UI directly. Every committed
 * edge becomes an Event posted to the global queue:
 *     press   -> EV_BUTTON_PRESSED  (a = button index 0..63)
 *     release -> EV_BUTTON_RELEASED (a = button index 0..63)
 * The dispatcher (ui/modes.cpp) interprets that index per current mode.
 *
 * Implemented in src/drivers/button_matrix.cpp.
 * Owned by Task_ButtonScan (CORE_IO), ticked every MATRIX_SCAN_INTERVAL_MS.
 */
#pragma once
#include <stdint.h>
#include "config.h"   // ROW_PINS/COL_PINS, NUM_BUTTONS, debounce/scan timing

// One-time pin setup: rows OUTPUT (idle HIGH), columns INPUT_PULLUP, clear
// the debounce state. Call once from Task_ButtonScan before the scan loop.
void matrix_init();

// Scan the whole 8x8 grid once (all rows). For every key whose debounced
// state changes, posts EV_BUTTON_PRESSED / EV_BUTTON_RELEASED via event_post().
// Call once per MATRIX_SCAN_INTERVAL_MS tick. Non-blocking, no allocation.
void matrix_scan();

// Diagnostic / UI-feedback accessor: debounced state of one key (1=held).
// Reads the committed snapshot only; safe to call from CORE_IO contexts.
bool matrix_pressed(uint8_t index);
