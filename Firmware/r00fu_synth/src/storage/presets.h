/*
 * presets.h — preset save/load (LittleFS + ArduinoJson).
 *
 * A preset bundles everything that makes a "sound + pattern": sequencer
 * patterns, mixer levels, synth params, effects settings, and the per-button
 * MIDI map. Stored as JSON at PRESET_DIR/<slot>.json (config.h). Snapshots are
 * gathered/applied on CORE_IO only (never touches DSP directly — synth params
 * are pushed via synth_set_param, which uses the RT-safe queue).
 *
 * Implemented in src/storage/presets.cpp.
 */
#pragma once
#include <stdint.h>
#include "config.h"
#include "../audio/synth.h"   // SynthParam (presets_note_param)

#define PRESET_SLOT_COUNT 16    // /presets/0.json .. /presets/15.json

// Mount LittleFS (format on first boot if needed) and ensure PRESET_DIR exists.
// Call once at startup before any save/load. Returns false on mount failure.
bool presets_init();

// ── Live-edit mirror updates (call from the dispatcher) ──────────────────────
// presets_save() serializes a CORE_IO-side mirror of the synth params / mixer
// (we can't read DSP state back across the core boundary). The dispatcher MUST
// route knob/slider parameter edits through these so the mirror reflects the
// user's live tweaks; otherwise every saved preset stores stale defaults.
//
// presets_note_param() updates the mirror AND forwards the value to the engine
// via the RT-safe synth_set_param() queue (so callers replace a direct
// synth_set_param() call with this). Returns the synth-queue result.
bool presets_note_param(SynthParam p, int16_t value);

// Update a mixer level in the mirror. track==0xFF addresses the master fader.
void presets_set_mixer(uint8_t track, int16_t value);

// Serialize the current device state to PRESET_DIR/<slot>.json. Returns false
// on I/O or out-of-range slot. Safe on CORE_IO.
bool presets_save(uint8_t slot);

// Load PRESET_DIR/<slot>.json and apply it (synth via synth_set_param queue,
// sequencer via seq_set_step, map via midi_map_set). Returns false if the slot
// file is missing/corrupt. Safe on CORE_IO.
bool presets_load(uint8_t slot);

// True if PRESET_DIR/<slot>.json exists (for UI slot browsing).
bool presets_exists(uint8_t slot);
