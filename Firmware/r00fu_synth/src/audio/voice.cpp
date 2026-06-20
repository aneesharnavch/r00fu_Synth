/*
 * voice.cpp — one polyphonic synth voice.
 *
 * Path: OscA + OscB + Noise  ->  amp ADSR  ->  SVF (modulated by filter ADSR +
 * LFO)  -> mono float, ADDED into the synth's accumulator. A single cheap sine
 * LFO modulates pitch and/or cutoff. Pure float, no allocation, no locks.
 * CORE_AUDIO only. Self-contained DSP (ML_SynthTools/AcidBox style).
 */
#include "voice.h"
#include <math.h>

static const float TWO_PI = 6.28318530717958647692f;

// MIDI note -> Hz. note 69 (A4) = 440 Hz. exp2f keeps it branchless.
static inline float noteToHz(uint8_t note) {
  return 440.0f * exp2f(((float)note - 69.0f) / 12.0f);
}

Voice::Voice()
: _note(0), _vel(0), _baseFreq(440.0f), _bend(1.0f),
  _aLvl(0.6f), _bLvl(0.4f), _nLvl(0.0f),
  _detuneB(0.0f), _filtEnvAmt(0.0f), _baseCutoff(1200.0f),
  _lfoPhase(0.0f), _lfoInc(0.0f), _lfoPitchDepth(0.0f), _lfoCutoffDepth(0.0f) {
  _oscA.setWave(OSC_SAW);
  _oscB.setWave(OSC_SAW);
  _noise.setWave(OSC_NOISE);
  _ampEnv.setAttack(0.005f); _ampEnv.setDecay(0.12f);
  _ampEnv.setSustain(0.8f);  _ampEnv.setRelease(0.25f);
  _filtEnv.setAttack(0.005f); _filtEnv.setDecay(0.15f);
  _filtEnv.setSustain(0.4f);  _filtEnv.setRelease(0.3f);
  _filter.setMode(FILT_LOWPASS);
  _filter.setCutoff(_baseCutoff);
  _filter.setResonance(0.2f);
  setLFO(4.0f, 0.0f, 0.0f);
}

// Re-apply oscillator frequencies from the current base note, detune and global
// pitch bend. Called whenever any of those change so held notes track edits.
void Voice::retune() {
  _oscA.setFreq(_baseFreq * _bend);
  _oscB.setFreq(_baseFreq * exp2f(_detuneB / 12.0f) * _bend);
}

void Voice::noteOn(uint8_t note, uint8_t velocity) {
  _note = note;
  _vel  = velocity;
  _baseFreq = noteToHz(note);
  // Set oscillator frequencies (OscB detuned, both scaled by current bend).
  retune();
  _noise.setFreq(0.0f);            // noise ignores freq
  // Phase reset for a consistent attack transient (and to avoid clicks from
  // stealing). Keep OscB slightly offset so detune doesn't phase-cancel.
  _oscA.reset(0.0f);
  _oscB.reset(0.25f);
  _ampEnv.gate(true);
  _filtEnv.gate(true);
}

void Voice::noteOff() {
  _ampEnv.gate(false);
  _filtEnv.gate(false);
}

void Voice::kill() {
  _ampEnv.reset();
  _filtEnv.reset();
  _filter.reset();
  _note = 0;
}

bool Voice::active() const {
  // The amp envelope governs audibility; a voice is reclaimable once it idles.
  return _ampEnv.isActive();
}

void Voice::render(float* out, size_t frames) {
  if (!_ampEnv.isActive()) return;   // nothing to add

  // Velocity scales amplitude (0..1).
  const float velGain = (float)_vel * (1.0f / 127.0f);

  // Is the LFO doing anything? When both depths are zero we skip the per-sample
  // sinf() entirely (the common no-modulation case) — a real CORE_AUDIO win.
  const bool lfoActive = (_lfoPitchDepth != 0.0f) || (_lfoCutoffDepth != 0.0f);

  // ── Filter cutoff is retuned at BLOCK rate, not per sample ────────────────
  // The old code called _filter.setCutoff() (a transcendental) every sample for
  // every voice. Cutoff is driven by the filter envelope + LFO, both of which
  // move far slower than fs, so sampling them once per block (here, at the LFO
  // phase the block starts on) is inaudible and removes ~512 sinf()/block at
  // 8-voice polyphony. We sample the LFO once for this purpose without advancing
  // its phase prematurely (the per-sample loop owns the phase).
  {
    const float lfo0 = lfoActive ? sinf(TWO_PI * _lfoPhase) : 0.0f;
    const float fenv = _filtEnv.level();   // envelope level without advancing it
    float cutoff = _baseCutoff
                 + fenv * _filtEnvAmt
                 + lfo0 * _lfoCutoffDepth;
    _filter.setCutoff(cutoff);
  }

  for (size_t i = 0; i < frames; ++i) {
    // ── LFO (one cheap sine) — only when it actually modulates something ──
    float lfo = 0.0f;
    if (lfoActive) {
      lfo = sinf(TWO_PI * _lfoPhase);
      _lfoPhase += _lfoInc;
      if (_lfoPhase >= 1.0f) _lfoPhase -= 1.0f;

      // Pitch modulation: ±_lfoPitchDepth semitones. Only retune when there is
      // depth (avoids per-sample exp2f cost for the common no-vibrato case).
      // Fold in the global pitch-bend multiplier so vibrato rides on top of bend.
      if (_lfoPitchDepth != 0.0f) {
        float vib = exp2f((lfo * _lfoPitchDepth) / 12.0f);
        _oscA.setFreq(_baseFreq * _bend * vib);
        _oscB.setFreq(_baseFreq * exp2f(_detuneB / 12.0f) * _bend * vib);
      }
    }

    // ── Oscillators + noise mix ──────────────────────────────────────────
    float sig = _oscA.process() * _aLvl
              + _oscB.process() * _bLvl
              + _noise.process() * _nLvl;

    // ── Filter (coefficient set once per block above) ────────────────────
    // Still advance the filter envelope per sample so its state stays in sync
    // (release/decay timing must not depend on block size); the cutoff coeff it
    // would produce is just not re-applied until the next block.
    _filtEnv.process();
    sig = _filter.process(sig);

    // ── Amp envelope + velocity ──────────────────────────────────────────
    float amp = _ampEnv.process();
    out[i] += sig * amp * velGain;

    // Reclaim mid-block: if the amp env finished, stop adding (rest is silence).
    if (!_ampEnv.isActive()) {
      _note = 0;
      break;
    }
  }
}

// ── Parameter setters ──────────────────────────────────────────────────────
void Voice::setOscWave(uint8_t which, OscWave w) {
  if (which == 0) _oscA.setWave(w);
  else            _oscB.setWave(w);
}

void Voice::setOscMix(float aLevel, float bLevel, float noiseLevel) {
  _aLvl = aLevel;
  _bLvl = bLevel;
  _nLvl = noiseLevel;
}

void Voice::setDetune(float semitonesB) {
  _detuneB = semitonesB;
  // Re-apply immediately so a knob twist is heard on held notes (bend-aware).
  retune();
}

void Voice::setPitchBend(float ratio) {
  if (ratio <= 0.0f) ratio = 1.0f;
  _bend = ratio;
  // Retune held notes now; the LFO-pitch path also reads _bend per sample.
  retune();
}

void Voice::setAmpADSR(float a, float d, float s, float r) {
  _ampEnv.setAttack(a);
  _ampEnv.setDecay(d);
  _ampEnv.setSustain(s);
  _ampEnv.setRelease(r);
}

void Voice::setFilter(FilterMode m, float cutoffHz, float res) {
  _filter.setMode(m);
  _baseCutoff = cutoffHz;
  _filter.setResonance(res);
}

void Voice::setFilterEnvAmount(float amount) {
  _filtEnvAmt = amount;
}

void Voice::setLFO(float rateHz, float pitchDepth, float cutoffDepth) {
  if (rateHz < 0.0f) rateHz = 0.0f;
  _lfoInc         = rateHz / (float)SAMPLE_RATE;
  _lfoPitchDepth  = pitchDepth;    // semitones
  _lfoCutoffDepth = cutoffDepth;   // Hz
}
