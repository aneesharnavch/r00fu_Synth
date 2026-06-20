/*
 * settings.h — global, preset-independent device preferences.
 *
 * Stored as a single JSON document at SETTINGS_FILE (config.h) on LittleFS.
 * Holds things that persist across presets: default mode, master tuning, MIDI
 * channel/clock routing, brightness, last preset, etc. CORE_IO only.
 *
 * Implemented in src/storage/settings.cpp.
 */
#pragma once
#include <stdint.h>
#include "config.h"

// In-RAM mirror of the persisted prefs. Read freely on CORE_IO; mutate via the
// fields then call settings_save() to persist.
struct Settings {
  uint8_t startup_mode;     // ui Mode id (0..4) entered on boot
  uint8_t midi_channel;     // default channel 1..16 for local notes
  uint8_t send_clock;       // 0/1 emit MIDI clock when playing
  uint8_t recv_clock;       // 0/1 slave to incoming MIDI clock
  uint8_t led_brightness;   // 0..255 status LED
  uint8_t last_preset;      // slot auto-loaded on boot (0xFF = none)
  float   master_tune;      // A4 reference, default 440.0 Hz
};

extern Settings g_settings;   // defined in settings.cpp

// Load from SETTINGS_FILE into g_settings, applying defaults for any missing
// keys (and writing the file if it doesn't exist). Call once after presets_init.
void settings_init();

// Persist g_settings to SETTINGS_FILE. Returns false on I/O error.
bool settings_save();

// Re-read SETTINGS_FILE into g_settings (discarding unsaved changes).
bool settings_load();
