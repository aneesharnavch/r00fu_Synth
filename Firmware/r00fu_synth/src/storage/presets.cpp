/*
 * presets.cpp — preset bundles on LittleFS (implements presets.h).
 *
 * A preset captures everything that makes a "sound + groove":
 *   - synth params   (the SynthParam continuous controls, 0..4095 domain)
 *   - mixer          (per-track levels + master)
 *   - sequencer      (tempo, swing, all 8x64 steps with note/vel/prob/len/plocks)
 *   - the per-button MIDI map (delegated to midi_map.cpp)
 * stored as JSON at PRESET_DIR/<slot>.json. Gather/apply run on CORE_IO only.
 *
 * ── Why a param mirror? ──────────────────────────────────────────────────────
 * synth.h's command queue is one-way (core0 -> core1); there is no getter for
 * live DSP state and we must NOT read voice memory from CORE_IO. So this module
 * keeps an authoritative CORE_IO-side mirror of the values it owns (synth params
 * + mixer). The dispatcher should route knob/slider param changes through
 * presets_note_param()/presets_set_mixer() so the mirror stays current; saving
 * serializes the mirror, loading writes the mirror AND pushes it to the synth
 * via the RT-safe synth_set_param() queue. Sequencer state is read/written
 * through the seq_* API (which is CORE_IO-owned already), so it needs no mirror.
 */
#include "presets.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <string.h>
#include <stdio.h>

#include "midi_map.h"
#include "../audio/synth.h"
#include "../sequencer/sequencer.h"

// Mixer geometry: one level per sequencer track plus a master fader. Kept here
// (not in a header) because the mixer has no dedicated module yet; the value
// domain matches the analog scanners (0..4095) for a 1:1 slider mapping.
#define PRESET_MIXER_TRACKS SEQ_TRACKS
#define MIXER_LEVEL_DEFAULT 3000   // ~0.73 of full scale

// ── CORE_IO-side authoritative mirror of preset-owned, write-only state ──────
static int16_t s_synth_params[SP_PARAM_COUNT];
static int16_t s_mixer_track[PRESET_MIXER_TRACKS];
static int16_t s_mixer_master;
static bool    s_mirror_ready = false;

// Reasonable musical starting point for the synth-param mirror (0..4095 raw,
// scaled in-engine). These are only the snapshot defaults; the engine has its
// own boot defaults until a preset is loaded.
static void mirror_defaults() {
  for (int i = 0; i < SP_PARAM_COUNT; ++i) s_synth_params[i] = 2048;  // mid
  s_synth_params[SP_OSC_MIX]       = 2048;
  s_synth_params[SP_NOISE_LEVEL]   = 0;
  s_synth_params[SP_DETUNE]        = 256;
  s_synth_params[SP_FILTER_CUTOFF] = 3600;   // fairly open
  s_synth_params[SP_FILTER_RES]    = 600;
  s_synth_params[SP_FILTER_ENV_AMT]= 1800;
  s_synth_params[SP_AMP_ATTACK]    = 60;
  s_synth_params[SP_AMP_DECAY]     = 1200;
  s_synth_params[SP_AMP_SUSTAIN]   = 2600;
  s_synth_params[SP_AMP_RELEASE]   = 1400;
  s_synth_params[SP_LFO_RATE]      = 800;
  s_synth_params[SP_LFO_PITCH_DEPTH]  = 0;
  s_synth_params[SP_LFO_CUTOFF_DEPTH] = 0;
  s_synth_params[SP_MASTER_VOLUME] = 3200;

  for (int t = 0; t < PRESET_MIXER_TRACKS; ++t) s_mixer_track[t] = MIXER_LEVEL_DEFAULT;
  s_mixer_master = MIXER_LEVEL_DEFAULT;
  s_mirror_ready = true;
}

// Compose PRESET_DIR/<slot>.json into caller's buffer.
static void slot_path(uint8_t slot, char* out, size_t n) {
  snprintf(out, n, "%s/%u.json", PRESET_DIR, (unsigned)slot);
}

// ── init ────────────────────────────────────────────────────────────────────
bool presets_init() {
  // Mount LittleFS, formatting on first boot if the partition is blank. The
  // `true` arg to begin() = format-on-fail (Arduino-ESP32 LittleFS signature).
  if (!LittleFS.begin(true)) {
    return false;   // mount + format both failed -> storage unusable
  }

  // Ensure the preset directory exists (mkdir is a no-op if already present).
  if (!LittleFS.exists(PRESET_DIR)) {
    LittleFS.mkdir(PRESET_DIR);
  }

  if (!s_mirror_ready) mirror_defaults();
  return true;
}

// ── mirror updates (call these from the dispatcher so saves stay accurate) ───
// Update a synth param in the mirror AND forward it to the engine. Returns the
// synth queue result (false if dropped).
bool presets_note_param(SynthParam p, int16_t value) {
  if ((uint8_t)p < SP_PARAM_COUNT) s_synth_params[p] = value;
  return synth_set_param(p, value);
}

// Update a mixer level in the mirror. track==0xFF addresses the master fader.
void presets_set_mixer(uint8_t track, int16_t value) {
  if (track == 0xFF)                       s_mixer_master = value;
  else if (track < PRESET_MIXER_TRACKS)    s_mixer_track[track] = value;
}

// ── save ────────────────────────────────────────────────────────────────────
bool presets_save(uint8_t slot) {
  if (slot >= PRESET_SLOT_COUNT) return false;
  if (!s_mirror_ready) mirror_defaults();

  JsonDocument doc;
  doc["version"] = 1;
  doc["device"]  = "r00fu_synth";
  doc["slot"]    = slot;

  // --- synth params (mirror) ---
  JsonArray sp = doc["synth"].to<JsonArray>();
  for (int i = 0; i < SP_PARAM_COUNT; ++i) sp.add(s_synth_params[i]);

  // --- mixer ---
  JsonObject mix = doc["mixer"].to<JsonObject>();
  mix["master"] = s_mixer_master;
  JsonArray tr = mix["tracks"].to<JsonArray>();
  for (int t = 0; t < PRESET_MIXER_TRACKS; ++t) tr.add(s_mixer_track[t]);

  // --- sequencer transport feel ---
  JsonObject seq = doc["sequencer"].to<JsonObject>();
  seq["tempo"] = seq_tempo();
  seq["swing"] = seq_swing();

  // --- patterns: 8 tracks x 64 steps, sparse (only active steps written) ---
  JsonArray pat = seq["patterns"].to<JsonArray>();
  for (uint8_t t = 0; t < SEQ_TRACKS; ++t) {
    JsonArray track = pat.add<JsonArray>();
    for (uint8_t st = 0; st < SEQ_STEPS; ++st) {
      const Step& s = seq_get_step(t, st);
      if (!s.active) continue;            // skip empties to keep files small
      JsonObject so = track.add<JsonObject>();
      so["i"] = st;
      so["n"] = s.note;
      so["v"] = s.velocity;
      so["p"] = s.probability;
      so["l"] = s.length;
      // Param-locks (only the active ones).
      JsonArray pl = so["pl"].to<JsonArray>();
      for (uint8_t k = 0; k < SEQ_PLOCKS_PER_STEP; ++k) {
        if (!s.plocks[k].active) continue;
        JsonObject lo = pl.add<JsonObject>();
        lo["cc"] = s.plocks[k].cc;
        lo["v"]  = s.plocks[k].value;
      }
    }
  }

  // --- per-button MIDI map (same shape as midi_map.cpp / the GUI) ---
  JsonArray buttons = doc["buttons"].to<JsonArray>();
  for (uint8_t i = 0; i < NUM_BUTTONS; ++i) {
    const ButtonMap& m = midi_map_get(i);
    JsonObject b = buttons.add<JsonObject>();
    b["index"]   = i;
    b["type"]    = m.type;          // numeric id here (device-internal preset)
    b["channel"] = m.channel;
    b["value"]   = m.value;
    b["label"]   = m.label;
  }

  char path[48];
  slot_path(slot, path, sizeof(path));
  File f = LittleFS.open(path, "w");
  if (!f) return false;
  bool ok = (serializeJson(doc, f) > 0);
  f.close();
  return ok;
}

// ── load ────────────────────────────────────────────────────────────────────
bool presets_load(uint8_t slot) {
  if (slot >= PRESET_SLOT_COUNT) return false;
  if (!s_mirror_ready) mirror_defaults();

  char path[48];
  slot_path(slot, path, sizeof(path));
  if (!LittleFS.exists(path)) return false;

  File f = LittleFS.open(path, "r");
  if (!f) return false;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;

  // --- synth params: write mirror, then push each to the engine (RT-safe) ---
  JsonArrayConst sp = doc["synth"].as<JsonArrayConst>();
  if (!sp.isNull()) {
    int i = 0;
    for (JsonVariantConst v : sp) {
      if (i >= SP_PARAM_COUNT) break;
      s_synth_params[i] = v.as<int16_t>();
      synth_set_param((SynthParam)i, s_synth_params[i]);   // enqueue to core1
      ++i;
    }
  }

  // --- mixer ---
  JsonObjectConst mix = doc["mixer"].as<JsonObjectConst>();
  if (!mix.isNull()) {
    s_mixer_master = mix["master"] | s_mixer_master;
    JsonArrayConst tr = mix["tracks"].as<JsonArrayConst>();
    int t = 0;
    for (JsonVariantConst v : tr) {
      if (t >= PRESET_MIXER_TRACKS) break;
      s_mixer_track[t++] = v.as<int16_t>();
    }
  }

  // --- sequencer transport + patterns ---
  JsonObjectConst seq = doc["sequencer"].as<JsonObjectConst>();
  if (!seq.isNull()) {
    if (seq["tempo"].is<float>()) seq_set_tempo(seq["tempo"].as<float>());
    if (seq["swing"].is<int>())   seq_set_swing((uint8_t)seq["swing"].as<int>());

    JsonArrayConst pat = seq["patterns"].as<JsonArrayConst>();
    if (!pat.isNull()) {
      uint8_t t = 0;
      for (JsonVariantConst trackv : pat) {
        if (t >= SEQ_TRACKS) break;
        JsonArrayConst track = trackv.as<JsonArrayConst>();
        seq_clear_track(t);                 // start from empty, then fill
        for (JsonObjectConst so : track) {
          uint8_t st = so["i"] | 0xFF;
          if (st >= SEQ_STEPS) continue;
          Step s;
          memset(&s, 0, sizeof(s));
          s.active      = 1;
          s.note        = so["n"] | 60;
          s.velocity    = so["v"] | 100;
          s.probability = so["p"] | 100;
          s.length      = so["l"] | 1;
          JsonArrayConst pl = so["pl"].as<JsonArrayConst>();
          uint8_t k = 0;
          for (JsonObjectConst lo : pl) {
            if (k >= SEQ_PLOCKS_PER_STEP) break;
            s.plocks[k].active = 1;
            s.plocks[k].cc     = lo["cc"] | 0;
            s.plocks[k].value  = lo["v"]  | 0;
            ++k;
          }
          seq_set_step(t, st, s);
        }
        ++t;
      }
    }
  }

  // --- per-button MIDI map (device-internal numeric type ids) ---
  JsonArrayConst buttons = doc["buttons"].as<JsonArrayConst>();
  if (!buttons.isNull()) {
    for (JsonObjectConst b : buttons) {
      uint8_t idx = b["index"] | 0xFF;
      if (idx >= NUM_BUTTONS) continue;
      midi_map_set(idx,
                   b["type"]    | (uint8_t)MAP_NONE,
                   b["channel"] | (uint8_t)1,
                   b["value"]   | (uint8_t)0,
                   b["label"]   | "");
    }
  }

  return true;
}

// ── existence check ──────────────────────────────────────────────────────────
bool presets_exists(uint8_t slot) {
  if (slot >= PRESET_SLOT_COUNT) return false;
  char path[48];
  slot_path(slot, path, sizeof(path));
  return LittleFS.exists(path);
}
