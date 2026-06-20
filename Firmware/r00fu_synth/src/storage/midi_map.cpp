/*
 * midi_map.cpp — per-button function map (implements midi_map.h).
 *
 * Owns the shared g_button_map[NUM_BUTTONS] that the GUI, the config protocol
 * and the UI dispatcher all read. Provides sensible defaults (a drum layout on
 * the bottom rows + a chromatic keyboard on the upper rows), accessor helpers,
 * and JSON save/load to LittleFS that matches gui/r00fu_config.py's shape:
 *
 *   { "version":1, "device":"r00fu_synth",
 *     "buttons":[ {index,row,col,type,channel,value,label}, ... ] }
 *
 * The "type" field is serialized as the GUI's string name ("none","note",...)
 * and parsed back to the MapType id, so files round-trip between the GUI and
 * the device unchanged. CORE_IO only — never touched from the audio task.
 */
#include "midi_map.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <string.h>

// The one shared table everyone references (declared extern in midi_map.h).
ButtonMap g_button_map[NUM_BUTTONS];

// File the map persists to on LittleFS. Kept separate from settings/presets so
// the GUI's standalone mapping survives a preset change.
#define MIDI_MAP_FILE "/midi_map.json"

// ── type-name <-> id table (MUST match gui TYPES dict / MapType enum) ────────
static const char* const kTypeNames[] = {
  "none",       // MAP_NONE      = 0
  "note",       // MAP_NOTE      = 1
  "cc",         // MAP_CC        = 2
  "program",    // MAP_PROGRAM   = 3
  "transport",  // MAP_TRANSPORT = 4
  "mode",       // MAP_MODE      = 5
};
static const uint8_t kTypeCount = sizeof(kTypeNames) / sizeof(kTypeNames[0]);

// id -> "name" (returns "none" for anything out of range).
static const char* type_to_name(uint8_t type) {
  return (type < kTypeCount) ? kTypeNames[type] : kTypeNames[MAP_NONE];
}

// "name" -> id (returns MAP_NONE for anything unrecognized).
static uint8_t name_to_type(const char* name) {
  if (name) {
    for (uint8_t i = 0; i < kTypeCount; ++i) {
      if (strcmp(name, kTypeNames[i]) == 0) return i;
    }
  }
  return MAP_NONE;
}

// Copy a C-string into a fixed ButtonMap::label, always NUL-terminated.
static void copy_label(char* dst, const char* src) {
  const size_t cap = sizeof(g_button_map[0].label);   // 16
  if (!src) { dst[0] = '\0'; return; }
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = '\0';
}

// ── defaults ────────────────────────────────────────────────────────────────
// Lay out a usable factory map on the 8x8 grid:
//   * bottom two rows (6,7) = a 16-pad drum kit on channel 10 (GM drums),
//     General-MIDI percussion notes starting at 36 (Bass Drum 1).
//   * top six rows (0..5) = a chromatic keyboard on channel 1, ascending so the
//     low-left key is the lowest note and it climbs left->right, bottom->top of
//     that block (48 keys from C2 = note 36 upward stops just below the drums).
// Everything is plain MIDI, so the same map doubles as a DAW control surface.
static void load_factory_defaults() {
  // GM drum note for each of the 16 pads (kick, snare, hats, toms, cymbals…).
  static const uint8_t kDrumNotes[16] = {
    36, 38, 42, 46,   // kick, snare, closed hat, open hat
    37, 39, 40, 41,   // side stick, clap, e.snare, low floor tom
    43, 45, 47, 48,   // hi floor tom, low tom, low-mid tom, hi-mid tom
    49, 51, 50, 57,   // crash, ride, hi tom, crash 2
  };

  for (uint8_t row = 0; row < MATRIX_ROWS; ++row) {
    for (uint8_t col = 0; col < MATRIX_COLS; ++col) {
      uint8_t idx = btn_index(row, col);
      ButtonMap& m = g_button_map[idx];

      if (row >= 6) {
        // Drum pads: rows 6,7 -> pad 0..15 (row 6 = lower pads, row 7 = upper).
        uint8_t pad = (uint8_t)((row - 6) * MATRIX_COLS + col);
        m.type    = MAP_NOTE;
        m.channel = 10;                 // GM percussion channel
        m.value   = kDrumNotes[pad];
        m.label[0] = '\0';              // GUI shows note name when label empty
      } else {
        // Chromatic keyboard: 48 keys over rows 0..5. Linearize so it ascends
        // left->right then up a row. Start at C2 (36).
        uint8_t key = (uint8_t)(row * MATRIX_COLS + col);
        int note = 36 + key;            // 36..83
        if (note > 127) note = 127;
        m.type    = MAP_NOTE;
        m.channel = 1;
        m.value   = (uint8_t)note;
        m.label[0] = '\0';
      }
    }
  }
}

// ── public API ──────────────────────────────────────────────────────────────
void midi_map_init() {
  // Start from a known-good factory map, then overlay anything persisted.
  load_factory_defaults();
  // If a saved map exists, it wins (user's GUI assignments). Missing/corrupt
  // file just leaves the factory defaults in place.
  midi_map_load();
}

void midi_map_set(uint8_t idx, uint8_t type, uint8_t ch, uint8_t val,
                  const char* label) {
  if (idx >= NUM_BUTTONS) return;
  ButtonMap& m = g_button_map[idx];
  m.type    = (type < kTypeCount) ? type : MAP_NONE;
  m.channel = (ch >= 1 && ch <= 16) ? ch : 1;
  m.value   = val;
  // A bare "-" from the MAP serial line means "no label" (see config protocol).
  if (label && strcmp(label, "-") == 0) copy_label(m.label, "");
  else                                  copy_label(m.label, label);
}

const ButtonMap& midi_map_get(uint8_t idx) {
  // Clamp out-of-range to index 0 so callers never read past the array.
  return g_button_map[idx < NUM_BUTTONS ? idx : 0];
}

bool midi_map_save() {
  // Build the document in the GUI's exact shape.
  JsonDocument doc;
  doc["version"] = 1;
  doc["device"]  = "r00fu_synth";

  JsonArray buttons = doc["buttons"].to<JsonArray>();
  for (uint8_t i = 0; i < NUM_BUTTONS; ++i) {
    const ButtonMap& m = g_button_map[i];
    JsonObject b = buttons.add<JsonObject>();
    b["index"]   = i;
    b["row"]     = i / MATRIX_COLS;
    b["col"]     = i % MATRIX_COLS;
    b["type"]    = type_to_name(m.type);
    b["channel"] = m.channel;
    b["value"]   = m.value;
    b["label"]   = m.label;
  }

  File f = LittleFS.open(MIDI_MAP_FILE, "w");
  if (!f) return false;
  bool ok = (serializeJson(doc, f) > 0);
  f.close();
  return ok;
}

bool midi_map_load() {
  if (!LittleFS.exists(MIDI_MAP_FILE)) return false;

  File f = LittleFS.open(MIDI_MAP_FILE, "r");
  if (!f) return false;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;

  JsonArrayConst buttons = doc["buttons"].as<JsonArrayConst>();
  if (buttons.isNull()) return false;

  // Apply each entry by its explicit "index" (file order is not assumed). Any
  // button not present in the file keeps whatever it had (factory default).
  for (JsonObjectConst b : buttons) {
    uint8_t idx = b["index"] | 0xFF;
    if (idx >= NUM_BUTTONS) continue;
    uint8_t type = name_to_type(b["type"] | "none");
    uint8_t ch   = b["channel"] | 1;
    uint8_t val  = b["value"]   | 0;
    const char* label = b["label"] | "";
    midi_map_set(idx, type, ch, val, label);
  }
  return true;
}
