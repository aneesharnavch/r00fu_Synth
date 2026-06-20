/*
 * button_matrix.cpp — 8x8 key matrix scanner (driver layer).
 *
 * Drives one ROW low at a time and reads the COLUMNS (INPUT_PULLUP), exactly
 * like the proven prototype sketch. A pressed key pulls its column LOW. Per-key
 * diodes give N-key rollover. Software debounce (MATRIX_DEBOUNCE_COUNT stable
 * reads of the same candidate) gates every edge before it is committed.
 *
 * This driver NEVER touches the synth/sequencer/UI directly. Every committed
 * edge becomes an Event posted to the global queue via event_post():
 *     press   -> EV_BUTTON_PRESSED  (a = button index 0..63)
 *     release -> EV_BUTTON_RELEASED (a = button index 0..63)
 * The dispatcher (ui/modes.cpp) interprets that index per the current mode.
 *
 * Owned by Task_ButtonScan (CORE_IO); matrix_scan() is ticked every
 * MATRIX_SCAN_INTERVAL_MS (1 ms). Non-blocking, no allocation, no locks.
 */
#include "button_matrix.h"

#include <Arduino.h>
#include <string.h>      // memset

#include "events.h"      // Event, ev_button(), event_post()

// ── Debounce state (one slot per key, indexed by btn_index(row,col)) ────────
// Mirrors the prototype's three-array integrator:
//   stableState — last committed/debounced state (1 = held)
//   candState   — the candidate state currently being counted toward a commit
//   candCount   — number of consecutive scans that have agreed with candState
static uint8_t s_stableState[NUM_BUTTONS];
static uint8_t s_candState[NUM_BUTTONS];
static uint8_t s_candCount[NUM_BUTTONS];

// One-time pin setup + state clear. Call once from Task_ButtonScan before the
// scan loop. Rows are OUTPUT idle HIGH (inactive); columns are INPUT_PULLUP.
void matrix_init() {
  for (uint8_t r = 0; r < MATRIX_ROWS; r++) {
    pinMode(ROW_PINS[r], OUTPUT);
    digitalWrite(ROW_PINS[r], HIGH);         // idle high = row inactive
  }
  for (uint8_t c = 0; c < MATRIX_COLS; c++) {
    pinMode(COL_PINS[c], INPUT_PULLUP);      // released col floats HIGH
  }

  memset(s_stableState, 0, sizeof(s_stableState));
  memset(s_candState,   0, sizeof(s_candState));
  memset(s_candCount,   0, sizeof(s_candCount));
}

// Scan the whole 8x8 grid once (all rows). For every key whose debounced state
// changes, post EV_BUTTON_PRESSED / EV_BUTTON_RELEASED. Non-blocking.
void matrix_scan() {
  for (uint8_t r = 0; r < MATRIX_ROWS; r++) {
    digitalWrite(ROW_PINS[r], LOW);          // activate this row
    delayMicroseconds(3);                    // let the column lines settle

    for (uint8_t c = 0; c < MATRIX_COLS; c++) {
      const uint8_t idx     = btn_index(r, c);
      const uint8_t pressed = (digitalRead(COL_PINS[c]) == LOW) ? 1 : 0;

      if (pressed == s_stableState[idx]) {
        // Reads match the committed state: nothing pending, drop any candidate.
        s_candCount[idx] = 0;
      } else if (pressed == s_candState[idx]) {
        // Same candidate as last scan: advance the stability counter.
        if (++s_candCount[idx] >= MATRIX_DEBOUNCE_COUNT) {
          s_stableState[idx] = pressed;      // commit the new debounced state
          s_candCount[idx]   = 0;
          event_post(ev_button(pressed ? EV_BUTTON_PRESSED
                                       : EV_BUTTON_RELEASED, idx));
        }
      } else {
        // A different reading than the committed state — start a new candidate.
        s_candState[idx] = pressed;
        s_candCount[idx] = 1;
      }
    }

    digitalWrite(ROW_PINS[r], HIGH);         // deactivate row before next
  }
}

// Diagnostic / UI-feedback accessor: committed debounced state of one key.
bool matrix_pressed(uint8_t index) {
  if (index >= NUM_BUTTONS) return false;
  return s_stableState[index] != 0;
}
