/*
 * r00fu_synth — STUB FIRMWARE (Phase 1 + protocol)
 * ------------------------------------------------
 * Goal of this sketch: get the 8x8 button matrix scanning and emit a clean
 * USB-serial event stream that the PC GUI (gui/r00fu_config.py) consumes,
 * and accept button->function mappings back from the GUI.
 *
 * This is intentionally minimal. It is the foundation that Phase 2+ (mux,
 * MIDI, sequencer, synth) bolts onto. It does NOT make sound yet.
 *
 * Board: ESP32-S3 Dev Module
 *   - USB CDC On Boot: ENABLED   (so Serial = native USB, debug console)
 *   - PSRAM: see note below about GPIO 35/36/37
 *
 * SERIAL PROTOCOL (115200 baud, newline-terminated ASCII)
 *   Device -> PC:
 *     HELLO r00fu_synth <fwver>      sent on boot and on IDENTIFY
 *     BTN <index> <row> <col> <0|1>  button edge (1=press, 0=release)
 *     ACK MAP <index>                mapping stored
 *     LOG <text>                     human-readable debug
 *   PC -> Device:
 *     IDENTIFY                       ask device to announce itself
 *     MAP <index> <type> <ch> <val> <label...>   store a mapping
 *     DUMP                           echo all stored mappings as MAP lines
 *
 * index = row * 8 + col   (0..63)
 */

#include <Arduino.h>

#define FW_VERSION "0.1.0"

// ---- Pinout (from pinouts.txt) -------------------------------------------
static const uint8_t ROW_PINS[8] = { 4, 5, 6, 7, 15, 16, 17, 18 };
static const uint8_t COL_PINS[8] = { 8, 9, 10, 11, 12, 13, 14, 21 };

// Matrix scan: drive one ROW LOW at a time, read COLUMNS with pull-ups.
// A pressed key pulls its column LOW. Per-key diodes give N-key rollover.
// If your diodes are oriented the other way and keys read inverted/ghost,
// flip ROW/COL roles here (drive cols, read rows) — that's the only change.

static const uint32_t SCAN_INTERVAL_MS = 1;   // 1ms scan (spec)
static const uint8_t  DEBOUNCE_COUNT   = 6;   // ~6ms stable before commit

// ---- Mapping store --------------------------------------------------------
// Mirrors the GUI's JSON. Kept tiny; persisted to NVS/LittleFS in a later phase.
enum MapType : uint8_t { MAP_NONE=0, MAP_NOTE=1, MAP_CC=2, MAP_PROGRAM=3,
                         MAP_TRANSPORT=4, MAP_MODE=5 };
struct ButtonMap {
  uint8_t type;     // MapType
  uint8_t channel;  // 1..16 (MIDI) or unused
  uint8_t value;    // note#/cc#/program#/transport id/mode id
  char    label[16];
};
static ButtonMap gMap[64];

// ---- Debounce state -------------------------------------------------------
static uint8_t  stableState[64];   // last committed state
static uint8_t  candCount[64];     // consecutive reads of the candidate
static uint8_t  candState[64];     // candidate state being counted

static uint32_t lastScan = 0;

void announce() {
  Serial.printf("HELLO r00fu_synth %s\n", FW_VERSION);
}

void emitButton(uint8_t index, uint8_t row, uint8_t col, uint8_t pressed) {
  Serial.printf("BTN %u %u %u %u\n", index, row, col, pressed);
}

void scanMatrix() {
  for (uint8_t r = 0; r < 8; r++) {
    digitalWrite(ROW_PINS[r], LOW);          // activate this row
    delayMicroseconds(3);                    // let lines settle
    for (uint8_t c = 0; c < 8; c++) {
      uint8_t idx = r * 8 + c;
      uint8_t pressed = (digitalRead(COL_PINS[c]) == LOW) ? 1 : 0;

      if (pressed == stableState[idx]) {
        candCount[idx] = 0;                  // no change, reset candidate
      } else if (pressed == candState[idx]) {
        if (++candCount[idx] >= DEBOUNCE_COUNT) {
          stableState[idx] = pressed;        // commit
          candCount[idx] = 0;
          emitButton(idx, r, c, pressed);
        }
      } else {
        candState[idx] = pressed;            // new candidate
        candCount[idx] = 1;
      }
    }
    digitalWrite(ROW_PINS[r], HIGH);         // deactivate row
  }
}

void handleLine(char *line) {
  // Tokenize in-place
  char *cmd = strtok(line, " ");
  if (!cmd) return;

  if (!strcmp(cmd, "IDENTIFY")) {
    announce();
  } else if (!strcmp(cmd, "MAP")) {
    char *sIdx = strtok(NULL, " ");
    char *sTyp = strtok(NULL, " ");
    char *sCh  = strtok(NULL, " ");
    char *sVal = strtok(NULL, " ");
    char *sLbl = strtok(NULL, "\n");          // rest of line = label
    if (!sIdx || !sTyp || !sCh || !sVal) return;
    int idx = atoi(sIdx);
    if (idx < 0 || idx > 63) return;
    gMap[idx].type    = (uint8_t)atoi(sTyp);
    gMap[idx].channel = (uint8_t)atoi(sCh);
    gMap[idx].value   = (uint8_t)atoi(sVal);
    if (sLbl) { strncpy(gMap[idx].label, sLbl, 15); gMap[idx].label[15] = 0; }
    else      { gMap[idx].label[0] = 0; }
    Serial.printf("ACK MAP %d\n", idx);
  } else if (!strcmp(cmd, "DUMP")) {
    for (int i = 0; i < 64; i++) {
      Serial.printf("MAP %d %u %u %u %s\n", i, gMap[i].type,
                    gMap[i].channel, gMap[i].value,
                    gMap[i].label[0] ? gMap[i].label : "-");
    }
  }
}

void pollSerial() {
  static char buf[96];
  static uint8_t len = 0;
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\n' || ch == '\r') {
      if (len) { buf[len] = 0; handleLine(buf); len = 0; }
    } else if (len < sizeof(buf) - 1) {
      buf[len++] = ch;
    }
  }
}

void setup() {
  Serial.begin(115200);

  for (uint8_t r = 0; r < 8; r++) {
    pinMode(ROW_PINS[r], OUTPUT);
    digitalWrite(ROW_PINS[r], HIGH);         // idle high (inactive)
  }
  for (uint8_t c = 0; c < 8; c++) {
    pinMode(COL_PINS[c], INPUT_PULLUP);
  }

  memset(stableState, 0, sizeof(stableState));
  memset(candCount, 0, sizeof(candCount));
  memset(candState, 0, sizeof(candState));
  memset(gMap, 0, sizeof(gMap));

  delay(300);                                // let USB-CDC enumerate
  announce();
}

void loop() {
  uint32_t now = millis();
  if (now - lastScan >= SCAN_INTERVAL_MS) {
    lastScan = now;
    scanMatrix();
  }
  pollSerial();
}
