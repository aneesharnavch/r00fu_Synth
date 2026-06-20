/*
 * oscillator.h — single band-limited-ish wavetable/phase oscillator.
 *
 * Pure float DSP, no allocation, no locks. One phase accumulator advanced per
 * sample at a frequency set by the voice. Self-contained (adapted from the
 * ML_SynthTools / AcidBox phase-osc pattern) so it builds without that lib.
 *
 * Used only on CORE_AUDIO inside Voice (voice.h).
 */
#pragma once
#include <stdint.h>
#include "config.h"   // SAMPLE_RATE

enum OscWave : uint8_t {
  OSC_SAW = 0,
  OSC_SQUARE,
  OSC_TRIANGLE,
  OSC_SINE,
  OSC_NOISE,
};

class Oscillator {
public:
  Oscillator();

  void  setWave(OscWave w);        // select waveform
  void  setFreq(float hz);         // fundamental in Hz (recomputes phase incr)
  void  setPulseWidth(float pw);   // 0..1, square/pulse duty (default 0.5)
  void  reset(float phase = 0.0f); // restart phase 0..1 (e.g. on noteOn)

  float process();                 // next sample, nominally -1..+1

private:
  OscWave _wave;
  float   _phase;       // 0..1
  float   _inc;         // phase increment per sample = hz / SAMPLE_RATE
  float   _pw;          // pulse width 0..1
  uint32_t _rng;        // xorshift state for OSC_NOISE
};
