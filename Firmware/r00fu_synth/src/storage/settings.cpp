/*
 * settings.cpp — global, preset-independent device preferences (settings.h).
 *
 * Persists the Settings struct as a single JSON document at SETTINGS_FILE
 * (config.h) on LittleFS. On first boot (no file) it writes out the defaults so
 * subsequent loads are stable. CORE_IO only.
 *
 * Assumes LittleFS is already mounted by presets_init() (called first at
 * startup, per settings.h's "Call once after presets_init").
 */
#include "settings.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

// In-RAM mirror, declared extern in settings.h.
Settings g_settings;

// Factory defaults. Mirrors the field comments in settings.h.
static void apply_defaults(Settings& s) {
  s.startup_mode   = 0;        // MODE_DRUM
  s.midi_channel   = 1;        // local notes default to channel 1
  s.send_clock     = 1;        // emit MIDI clock when playing
  s.recv_clock     = 0;        // not slaved by default
  s.led_brightness = 64;       // moderate status-LED brightness
  s.last_preset    = 0xFF;     // none auto-loaded
  s.master_tune    = 440.0f;   // A4 = 440 Hz
}

void settings_init() {
  // Seed RAM with defaults so even a failed load leaves valid values.
  apply_defaults(g_settings);

  if (LittleFS.exists(SETTINGS_FILE)) {
    settings_load();           // overlay persisted values onto the defaults
  } else {
    settings_save();           // first boot: write the defaults out
  }
}

bool settings_save() {
  JsonDocument doc;
  doc["version"]        = 1;
  doc["startup_mode"]   = g_settings.startup_mode;
  doc["midi_channel"]   = g_settings.midi_channel;
  doc["send_clock"]     = g_settings.send_clock;
  doc["recv_clock"]     = g_settings.recv_clock;
  doc["led_brightness"] = g_settings.led_brightness;
  doc["last_preset"]    = g_settings.last_preset;
  doc["master_tune"]    = g_settings.master_tune;

  File f = LittleFS.open(SETTINGS_FILE, "w");
  if (!f) return false;
  bool ok = (serializeJson(doc, f) > 0);
  f.close();
  return ok;
}

bool settings_load() {
  if (!LittleFS.exists(SETTINGS_FILE)) return false;

  File f = LittleFS.open(SETTINGS_FILE, "r");
  if (!f) return false;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;

  // Each key falls back to the value already in g_settings (defaults), so a
  // partial/older file just leaves unknown fields untouched. The "| x" default
  // operand keeps any newly added field sane on an old preferences file.
  g_settings.startup_mode   = doc["startup_mode"]   | g_settings.startup_mode;
  g_settings.midi_channel   = doc["midi_channel"]   | g_settings.midi_channel;
  g_settings.send_clock     = doc["send_clock"]     | g_settings.send_clock;
  g_settings.recv_clock     = doc["recv_clock"]     | g_settings.recv_clock;
  g_settings.led_brightness = doc["led_brightness"] | g_settings.led_brightness;
  g_settings.last_preset    = doc["last_preset"]    | g_settings.last_preset;
  g_settings.master_tune    = doc["master_tune"]    | g_settings.master_tune;
  return true;
}
