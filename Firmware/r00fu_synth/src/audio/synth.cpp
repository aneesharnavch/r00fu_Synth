/*
 * synth.cpp — polyphonic engine + the core0->core1 RT-safe handoff.
 *
 * Producers (CORE_IO): synth_note_on/off/cc/... package a SynthCmd and xQueueSend
 * with 0 ticks (never block; drop+count on full). Consumer (CORE_AUDIO):
 * synth_render() drains the queue with zero-timeout receives BEFORE any DSP,
 * applies each command to voice/effect state, then mixes NUM_VOICES voices to
 * mono, widens to stereo, runs the effects chain, and converts to int16. No
 * malloc/lock/Serial in the render path; the queue is the only sync primitive.
 *
 * Voice stealing: prefer a free (idle) voice; else steal the oldest-started one.
 * Single-timbre engine (channel ignored), per the header.
 */
#include "synth.h"
#include "voice.h"
#include "effects.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include <math.h>
#include <string.h>

// ── Engine state (CORE_AUDIO-owned; only touched in synth_render) ───────────
static Voice      s_voices[NUM_VOICES];
static uint32_t   s_voiceAge[NUM_VOICES];   // monotonically increasing start tag
static uint32_t   s_ageCounter = 0;         // bumps on every note-on

static Delay      s_delay;
static Reverb     s_reverb;
static Chorus     s_chorus;
static Bitcrusher s_crush;

static float      s_masterVol = 0.8f;

// Shared patch params applied to all voices (single-timbre). Cached so a held
// note picks up edits and a freshly-stolen voice inherits the current patch.
struct Patch {
  OscWave    waveA, waveB;
  float      mixA, mixB, noiseLvl;     // derived from SP_OSC_MIX + SP_NOISE_LEVEL
  float      detune;                   // semitones
  FilterMode filtMode;
  float      cutoff, res, filtEnvAmt;
  float      ampA, ampD, ampS, ampR;
  float      lfoRate, lfoPitch, lfoCutoff;
};
static Patch s_patch;

// Raw 0..1 store for the two halves of the osc mix knob so we can recombine.
static float s_oscMix = 0.5f;   // 0=all A, 1=all B
static float s_noiseRaw = 0.0f;

// Global pitch-bend multiplier shared by all voices (single-timbre engine).
// 1.0 = no bend; ±8192 maps to ±SYNTH_BEND_RANGE semitones.
static float s_bend = 1.0f;
#define SYNTH_BEND_RANGE 2.0f   // ± semitones at full bend (standard default)

// ── Command queue (the core0->core1 boundary) ───────────────────────────────
static QueueHandle_t s_cmdQ = nullptr;
static volatile uint32_t s_dropped = 0;

// Max commands applied per render block (the rest wait for the next block). Big
// enough to absorb a sequencer step's worth of note/CC fan-out plus some knob
// motion, but bounded so a producer burst can't stall the audio block.
#define SYNTH_CMDS_PER_BLOCK 24

// ── Helpers: scale a 0..4095 knob value to a musical range ──────────────────
static inline float knob01(int16_t v) {
  if (v < 0) v = 0; if (v > 4095) v = 4095;
  return (float)v * (1.0f / 4095.0f);
}
// Exponential map for time/frequency-ish params (more resolution at the bottom).
static inline float knobExp(int16_t v, float lo, float hi) {
  float t = knob01(v);
  return lo * powf(hi / lo, t);
}
static inline float knobLin(int16_t v, float lo, float hi) {
  return lo + (hi - lo) * knob01(v);
}

// Recompute the A/B/noise mix levels from the stored knob positions.
static void recomputeOscMix() {
  // Equal-power-ish crossfade between A and B, then noise added on top.
  s_patch.mixA = (1.0f - s_oscMix);
  s_patch.mixB = s_oscMix;
  s_patch.noiseLvl = s_noiseRaw;
}

// Push the whole cached patch into one voice (used on note-on/steal).
static void applyPatchToVoice(Voice& v) {
  v.setOscWave(0, s_patch.waveA);
  v.setOscWave(1, s_patch.waveB);
  v.setOscMix(s_patch.mixA, s_patch.mixB, s_patch.noiseLvl);
  v.setDetune(s_patch.detune);
  v.setAmpADSR(s_patch.ampA, s_patch.ampD, s_patch.ampS, s_patch.ampR);
  v.setFilter(s_patch.filtMode, s_patch.cutoff, s_patch.res);
  v.setFilterEnvAmount(s_patch.filtEnvAmt);
  v.setLFO(s_patch.lfoRate, s_patch.lfoPitch, s_patch.lfoCutoff);
  v.setPitchBend(s_bend);                 // inherit current global bend
}

// Apply a single live param to ALL voices (so held notes hear the edit).
static void applyParamToAllVoices() {
  for (int i = 0; i < NUM_VOICES; ++i) applyPatchToVoice(s_voices[i]);
}

// Lazy patch-apply: param/CC/program commands just SET the cached patch and mark
// it dirty; we re-push the whole patch to all voices at most ONCE per block (at
// the top of render) instead of NUM_VOICES*~8 setters per command. This keeps a
// fast knob-sweep / param-lock storm from spiking per-block command cost.
static bool s_patchDirty = false;

// ── Voice allocation ────────────────────────────────────────────────────────
static int findVoiceFor(uint8_t note) {
  // Retrigger same-note first (avoids stacking duplicates).
  for (int i = 0; i < NUM_VOICES; ++i) {
    if (s_voices[i].active() && s_voices[i].note() == note) return i;
  }
  // Free voice?
  for (int i = 0; i < NUM_VOICES; ++i) {
    if (!s_voices[i].active()) return i;
  }
  // Steal the oldest.
  int oldest = 0;
  uint32_t bestAge = 0xFFFFFFFFu;
  for (int i = 0; i < NUM_VOICES; ++i) {
    if (s_voiceAge[i] < bestAge) { bestAge = s_voiceAge[i]; oldest = i; }
  }
  s_voices[oldest].kill();
  return oldest;
}

// ── Command application (CORE_AUDIO context, inside render drain) ────────────
static void applyCmd(const SynthCmd& c) {
  switch (c.type) {
    case SCMD_NOTE_ON: {
      if (c.b == 0) {                 // velocity 0 == note off (MIDI convention)
        for (int i = 0; i < NUM_VOICES; ++i)
          if (s_voices[i].active() && s_voices[i].note() == c.a)
            s_voices[i].noteOff();
        break;
      }
      int v = findVoiceFor(c.a);
      applyPatchToVoice(s_voices[v]);     // inherit current patch
      s_voices[v].noteOn(c.a, c.b);
      s_voiceAge[v] = ++s_ageCounter;
      break;
    }
    case SCMD_NOTE_OFF: {
      for (int i = 0; i < NUM_VOICES; ++i)
        if (s_voices[i].active() && s_voices[i].note() == c.a)
          s_voices[i].noteOff();
      break;
    }
    case SCMD_ALL_NOTES_OFF: {
      for (int i = 0; i < NUM_VOICES; ++i) s_voices[i].kill();
      break;
    }
    case SCMD_PITCHBEND: {
      // Single-timbre global bend: map -8192..+8191 to ±SYNTH_BEND_RANGE
      // semitones, convert to a frequency multiplier, and push it to every voice
      // (held notes bend live; new/stolen voices inherit it via applyPatchToVoice).
      float semis = ((float)c.val / 8192.0f) * SYNTH_BEND_RANGE;
      s_bend = exp2f(semis / 12.0f);
      for (int i = 0; i < NUM_VOICES; ++i) s_voices[i].setPitchBend(s_bend);
      break;
    }
    case SCMD_PROGRAM: {
      // Programs could select preset patches; the preset system owns that. Here
      // we just re-push the current patch (no-op placeholder, no DSP glitch).
      s_patchDirty = true;
      break;
    }
    case SCMD_CC: {
      // Map a few standard CCs to params for convenience (single-timbre).
      switch (c.a) {
        case 1:   /* mod wheel -> LFO pitch depth */
          s_patch.lfoPitch = knobLin((int16_t)(c.b * 32), 0.0f, 0.5f);
          s_patchDirty = true; break;
        case 7:   s_masterVol = (float)c.b / 127.0f; break;       // volume
        case 74:  /* brightness -> cutoff */
          s_patch.cutoff = knobExp((int16_t)(c.b * 32), 80.0f, 8000.0f);
          s_patchDirty = true; break;
        case 71:  /* resonance */
          s_patch.res = (float)c.b / 127.0f;
          s_patchDirty = true; break;
        case 123: for (int i = 0; i < NUM_VOICES; ++i) s_voices[i].kill(); break;
        default: break;
      }
      break;
    }
    case SCMD_SET_PARAM: {
      const int16_t v = c.val;
      switch ((SynthParam)c.a) {
        case SP_OSC_A_WAVE:
          s_patch.waveA = (OscWave)(knob01(v) * 4.999f); s_patchDirty = true; break;
        case SP_OSC_B_WAVE:
          s_patch.waveB = (OscWave)(knob01(v) * 4.999f); s_patchDirty = true; break;
        case SP_OSC_MIX:
          s_oscMix = knob01(v); recomputeOscMix(); s_patchDirty = true; break;
        case SP_NOISE_LEVEL:
          s_noiseRaw = knob01(v); recomputeOscMix(); s_patchDirty = true; break;
        case SP_DETUNE:
          s_patch.detune = knobLin(v, -12.0f, 12.0f); s_patchDirty = true; break;
        case SP_FILTER_CUTOFF:
          s_patch.cutoff = knobExp(v, 80.0f, 8000.0f); s_patchDirty = true; break;
        case SP_FILTER_RES:
          s_patch.res = knob01(v); s_patchDirty = true; break;
        case SP_FILTER_ENV_AMT:
          s_patch.filtEnvAmt = knobLin(v, 0.0f, 6000.0f); s_patchDirty = true; break;
        case SP_AMP_ATTACK:
          s_patch.ampA = knobExp(v, 0.001f, 4.0f); s_patchDirty = true; break;
        case SP_AMP_DECAY:
          s_patch.ampD = knobExp(v, 0.001f, 4.0f); s_patchDirty = true; break;
        case SP_AMP_SUSTAIN:
          s_patch.ampS = knob01(v); s_patchDirty = true; break;
        case SP_AMP_RELEASE:
          s_patch.ampR = knobExp(v, 0.001f, 6.0f); s_patchDirty = true; break;
        case SP_LFO_RATE:
          s_patch.lfoRate = knobExp(v, 0.05f, 20.0f); s_patchDirty = true; break;
        case SP_LFO_PITCH_DEPTH:
          s_patch.lfoPitch = knobLin(v, 0.0f, 12.0f); s_patchDirty = true; break;
        case SP_LFO_CUTOFF_DEPTH:
          s_patch.lfoCutoff = knobLin(v, 0.0f, 4000.0f); s_patchDirty = true; break;
        case SP_MASTER_VOLUME:
          s_masterVol = knob01(v); break;
        default: break;
      }
      break;
    }
    default: break;
  }
}

// ── Init ────────────────────────────────────────────────────────────────────
bool synth_init() {
  // Idempotent: if the command queue already exists we've been initialized
  // (e.g. called once from setup() and once at the top of Task_Audio). Don't
  // re-create the queue (that would orphan producers holding the old handle).
  if (s_cmdQ != nullptr) return true;

  // Default patch.
  s_patch.waveA = OSC_SAW;   s_patch.waveB = OSC_SAW;
  s_oscMix = 0.4f;           s_noiseRaw = 0.0f;
  s_patch.detune = 0.08f;    // tiny natural detune
  s_patch.filtMode = FILT_LOWPASS;
  s_patch.cutoff = 2000.0f;  s_patch.res = 0.2f;  s_patch.filtEnvAmt = 2500.0f;
  s_patch.ampA = 0.005f; s_patch.ampD = 0.15f; s_patch.ampS = 0.7f; s_patch.ampR = 0.3f;
  s_patch.lfoRate = 4.0f; s_patch.lfoPitch = 0.0f; s_patch.lfoCutoff = 0.0f;
  recomputeOscMix();

  for (int i = 0; i < NUM_VOICES; ++i) {
    s_voiceAge[i] = 0;
    applyPatchToVoice(s_voices[i]);
  }

  // Effects defaults — modest, mostly dry.
  s_delay.setTime(0.30f);   s_delay.setFeedback(0.30f); s_delay.setMix(0.18f);
  s_reverb.setRoomSize(0.5f); s_reverb.setDamping(0.5f); s_reverb.setMix(0.15f);
  s_chorus.setRate(0.8f);   s_chorus.setDepth(0.4f);    s_chorus.setMix(0.0f);
  s_crush.setBits(16.0f);   s_crush.setRateReduction(1.0f); s_crush.setMix(0.0f);

  // The command queue: fixed length, statically-sized item, no dynamic growth.
  s_cmdQ = xQueueCreate(SYNTH_CMD_QUEUE_LEN, sizeof(SynthCmd));
  return s_cmdQ != nullptr;
}

// ── Render (CORE_AUDIO) ─────────────────────────────────────────────────────
void synth_render(int16_t* outLR, size_t frames) {
  // 1) DRAIN the command queue first — non-blocking, zero timeout. This is the
  //    only place DSP state mutates. The drain is CAPPED per block so a burst of
  //    producer commands (knob-scan storm / many param-locks) can't push the
  //    per-block apply cost arbitrarily high before any DSP runs; leftover
  //    commands wait for the next block (1-block latency on non-musical params is
  //    inaudible). Note-on/off are cheap, so the cap is comfortably above a
  //    realistic per-block musical load.
  if (s_cmdQ) {
    SynthCmd c;
    int budget = SYNTH_CMDS_PER_BLOCK;
    while (budget-- > 0 && xQueueReceive(s_cmdQ, &c, 0) == pdTRUE) {
      applyCmd(c);
    }
    // Apply accumulated patch edits to all voices exactly ONCE here, rather than
    // re-pushing the whole patch on every individual SET_PARAM/CC/PROGRAM.
    if (s_patchDirty) {
      applyParamToAllVoices();
      s_patchDirty = false;
    }
  }

  // 2) Mix voices into a mono accumulator (stack buffer, fixed AUDIO_BLOCK_SIZE).
  if (frames > AUDIO_BLOCK_SIZE) frames = AUDIO_BLOCK_SIZE;
  float mono[AUDIO_BLOCK_SIZE];
  for (size_t i = 0; i < frames; ++i) mono[i] = 0.0f;

  for (int v = 0; v < NUM_VOICES; ++v) {
    s_voices[v].render(mono, frames);    // ADDS into mono
  }

  // 3) Widen mono -> interleaved stereo float, apply master volume, with a soft
  //    headroom scale (8 voices summed need attenuation before the limiter).
  float stereo[AUDIO_BLOCK_SIZE * 2];
  const float voiceScale = 0.30f;        // pre-FX headroom for summed voices
  for (size_t i = 0; i < frames; ++i) {
    float s = mono[i] * voiceScale;
    stereo[i * 2 + 0] = s;
    stereo[i * 2 + 1] = s;
  }

  // 4) Effects chain (in place): chorus -> delay -> reverb -> bitcrush.
  s_chorus.process(stereo, frames);
  s_delay.process(stereo, frames);
  s_reverb.process(stereo, frames);
  s_crush.process(stereo, frames);

  // 5) Master volume + soft clip + int16 convert.
  for (size_t i = 0; i < frames; ++i) {
    for (int ch = 0; ch < 2; ++ch) {
      float s = stereo[i * 2 + ch] * s_masterVol;
      // Cubic soft clip: linear for |s|<=1, smoothly saturates toward ±1 above.
      // Continuous and C1 at the ±1 boundary (no discontinuity/click).
      if (s >  1.5f)      s =  1.0f;
      else if (s < -1.5f) s = -1.0f;
      else                s = s - (s * s * s) * (1.0f / 6.75f);  // x - x^3/6.75
      int32_t q = (int32_t)lrintf(s * 32767.0f);
      if (q >  32767) q =  32767;
      if (q < -32768) q = -32768;
      outLR[i * 2 + ch] = (int16_t)q;
    }
  }
}

// ── RT-safe producers (CORE_IO) ─────────────────────────────────────────────
static inline bool pushCmd(const SynthCmd& c) {
  if (!s_cmdQ) return false;
  if (xQueueSend(s_cmdQ, &c, 0) != pdTRUE) {   // 0 ticks: never block CORE_IO
    s_dropped++;
    return false;
  }
  return true;
}

bool synth_note_on(uint8_t note, uint8_t velocity, uint8_t channel) {
  (void)channel;
  SynthCmd c{ SCMD_NOTE_ON, note, velocity, 0, 0 };
  return pushCmd(c);
}
bool synth_note_off(uint8_t note, uint8_t velocity, uint8_t channel) {
  (void)channel; (void)velocity;
  SynthCmd c{ SCMD_NOTE_OFF, note, 0, 0, 0 };
  return pushCmd(c);
}
bool synth_cc(uint8_t cc, uint8_t value, uint8_t channel) {
  (void)channel;
  SynthCmd c{ SCMD_CC, cc, value, 0, 0 };
  return pushCmd(c);
}
bool synth_pitchbend(int16_t bend, uint8_t channel) {
  (void)channel;
  SynthCmd c{ SCMD_PITCHBEND, 0, 0, 0, bend };
  return pushCmd(c);
}
bool synth_program(uint8_t program, uint8_t channel) {
  (void)channel;
  SynthCmd c{ SCMD_PROGRAM, program, 0, 0, 0 };
  return pushCmd(c);
}
bool synth_all_notes_off() {
  SynthCmd c{ SCMD_ALL_NOTES_OFF, 0, 0, 0, 0 };
  return pushCmd(c);
}
bool synth_set_param(SynthParam param, int16_t value) {
  SynthCmd c{ SCMD_SET_PARAM, (uint8_t)param, 0, 0, value };
  return pushCmd(c);
}

uint32_t synth_cmd_dropped() { return s_dropped; }
