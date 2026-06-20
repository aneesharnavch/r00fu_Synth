/*
 * config_protocol.cpp — GUI configuration protocol over USB-CDC Serial.
 *
 * Speaks the exact ASCII line protocol the PC GUI (gui/r00fu_config.py) and the
 * prototype sketch already use, at 115200 baud, newline-terminated:
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
 * MAP lines write straight into the shared g_button_map via midi_map_set().
 * This rides on the native USB-CDC `Serial` port, which is a separate interface
 * from USB-MIDI on the same TinyUSB composite device — the two never mix.
 *
 * Polled by Task_UI (CORE_IO). Matches src/drivers/config_protocol.h exactly.
 */
#include <Arduino.h>
#include <string.h>
#include <stdlib.h>

#include "config_protocol.h"
#include "config.h"
#include "midi_map.h"

// ── Line assembly state (mirrors the prototype's pollSerial) ───────────────
static char    s_buf[96];
static uint8_t s_len = 0;

// ── Emit helpers ───────────────────────────────────────────────────────────
static void announce() {
  Serial.printf("HELLO r00fu_synth %s\n", CFG_FW_VERSION);
}

void cfg_init() {
  // Serial (USB-CDC) is begun centrally with the TinyUSB device; we just send
  // the greeting so a GUI already listening sees us on boot.
  announce();
}

// ── Line handlers ──────────────────────────────────────────────────────────
// IDENTIFY / MAP / DUMP, tokenized in-place exactly like the prototype.
static void handle_line(char* line) {
  char* cmd = strtok(line, " ");
  if (!cmd) return;

  if (!strcmp(cmd, "IDENTIFY")) {
    announce();

  } else if (!strcmp(cmd, "MAP")) {
    char* sIdx = strtok(NULL, " ");
    char* sTyp = strtok(NULL, " ");
    char* sCh  = strtok(NULL, " ");
    char* sVal = strtok(NULL, " ");
    char* sLbl = strtok(NULL, "\n");      // rest of line = label (may be NULL)
    if (!sIdx || !sTyp || !sCh || !sVal) return;

    int idx = atoi(sIdx);
    if (idx < 0 || idx >= NUM_BUTTONS) return;

    uint8_t type = (uint8_t)atoi(sTyp);
    uint8_t ch   = (uint8_t)atoi(sCh);
    uint8_t val  = (uint8_t)atoi(sVal);

    // The GUI sends "-" for an empty label; treat that as no label.
    const char* label = (sLbl && strcmp(sLbl, "-") != 0) ? sLbl : "";

    // Write straight into the shared table; the device acts on it standalone.
    midi_map_set((uint8_t)idx, type, ch, val, label);

    Serial.printf("ACK MAP %d\n", idx);

  } else if (!strcmp(cmd, "DUMP")) {
    // Echo every stored mapping back as a MAP line the GUI can re-import.
    for (uint8_t i = 0; i < NUM_BUTTONS; i++) {
      const ButtonMap& m = g_button_map[i];
      Serial.printf("MAP %u %u %u %u %s\n", i, m.type, m.channel, m.value,
                    (m.label[0]) ? m.label : "-");
    }
  }
  // Unknown commands are ignored (forward-compatible with the GUI).
}

void cfg_poll() {
  // Drain whatever the host has sent; assemble newline-terminated lines.
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\n' || ch == '\r') {
      if (s_len) { s_buf[s_len] = 0; handle_line(s_buf); s_len = 0; }
    } else if (s_len < sizeof(s_buf) - 1) {
      s_buf[s_len++] = ch;
    }
    // Over-long lines silently overflow-truncate (drop excess until newline).
  }
}

// ── Outgoing event lines ───────────────────────────────────────────────────
void cfg_emit_button(uint8_t idx, uint8_t state) {
  // Derive row/col from the linear index so the GUI's grid lights up.
  uint8_t row = idx / MATRIX_COLS;
  uint8_t col = idx % MATRIX_COLS;
  Serial.printf("BTN %u %u %u %u\n", idx, row, col, (state ? 1 : 0));
}

void cfg_log(const char* text) {
  if (!text) return;
  Serial.printf("LOG %s\n", text);
}
