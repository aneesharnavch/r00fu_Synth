/*
 * voice.h — one polyphonic synth voice.
 *
 * Signal path:  OscA + OscB + Noise  -> (amp ADSR) -> Filter (filter ADSR/LFO
 * modulated)  -> mono float out. The LFO can modulate pitch and/or cutoff.
 * The synth owns NUM_VOICES of these and renders/sums them. Pure float, no
 * allocation, no locks. CORE_AUDIO only.
 *
 * render() ADDS this voice's contribution into a mono accumulator buffer so the
 * synth can mix all voices in one pass before the stereo/effects stage.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "config.h"
#include "oscillator.h"
#include "adsr.h"
#include "filter.h"

class Voice {
public:
  Voice();

  // Trigger / release. noteOn sets pitch from MIDI note, opens both ADSRs.
  void noteOn(uint8_t note, uint8_t velocity);
  void noteOff();                 // begins amp/filter release
  void kill();                    // immediate silence (voice steal)

  bool  active() const;           // amp envelope still sounding?
  uint8_t note() const { return _note; }

  // Render `frames` samples, ADDING into mono accumulator `out`. No clear.
  void render(float* out, size_t frames);

  // ── Per-voice parameters (set by synth from CC/knob mapping) ─────────────
  void setOscWave(uint8_t which /*0=A,1=B*/, OscWave w);
  void setOscMix(float aLevel, float bLevel, float noiseLevel);
  void setDetune(float semitonesB);            // OscB detune vs OscA
  void setAmpADSR (float a, float d, float s, float r);
  void setFilter(FilterMode m, float cutoffHz, float res);
  void setFilterEnvAmount(float amount);       // env -> cutoff depth
  void setLFO(float rateHz, float pitchDepth, float cutoffDepth);
  void setPitchBend(float ratio);              // global bend multiplier (1.0=none)

private:
  void retune();       // re-apply osc freqs from base * detune * bend

  Oscillator _oscA, _oscB, _noise;
  ADSR       _ampEnv, _filtEnv;
  Filter     _filter;

  uint8_t _note;       // current MIDI note (0 = none)
  uint8_t _vel;        // 0..127
  float   _baseFreq;   // Hz from _note
  float   _bend;       // pitch-bend multiplier (1.0 = no bend)
  float   _aLvl, _bLvl, _nLvl;   // osc mix
  float   _detuneB;    // semitones
  float   _filtEnvAmt; // env->cutoff depth
  float   _baseCutoff; // Hz before modulation

  // LFO (one per voice; cheap sine accumulator)
  float   _lfoPhase, _lfoInc, _lfoPitchDepth, _lfoCutoffDepth;
};
