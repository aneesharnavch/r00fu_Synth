/*
 * oscillator.cpp — phase-accumulator oscillator (saw/square/tri/sine/noise).
 *
 * Self-contained, pure float, branch-on-waveform. Naive (not fully band-limited)
 * waveforms with a light polyBLEP-style correction on the discontinuous edges
 * (saw/square) to tame the worst aliasing without pulling in a wavetable. This
 * matches the ML_SynthTools/AcidBox "cheap phase osc" approach. CORE_AUDIO only.
 */
#include "oscillator.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
static const float TWO_PI = 6.28318530717958647692f;

Oscillator::Oscillator()
: _wave(OSC_SAW), _phase(0.0f), _inc(0.0f), _pw(0.5f), _rng(0x1234abcdu) {}

void Oscillator::setWave(OscWave w) { _wave = w; }

void Oscillator::setFreq(float hz) {
  // Phase increment per sample (cycles/sample). Clamp to keep below Nyquist and
  // non-negative; the engine feeds positive Hz but be defensive.
  if (hz < 0.0f) hz = 0.0f;
  _inc = hz / (float)SAMPLE_RATE;
  if (_inc > 0.5f) _inc = 0.5f;      // never exceed Nyquist
}

void Oscillator::setPulseWidth(float pw) {
  // Keep away from 0/1 so the square always has both edges.
  if (pw < 0.01f) pw = 0.01f;
  if (pw > 0.99f) pw = 0.99f;
  _pw = pw;
}

void Oscillator::reset(float phase) {
  if (phase < 0.0f) phase = 0.0f;
  if (phase >= 1.0f) phase = 0.0f;
  _phase = phase;
}

// polyBLEP residual for a single discontinuity, normalized phase distance t to
// the edge (in [0,1) cycles) and increment dt. Removes the step's aliasing.
static inline float polyBlep(float t, float dt) {
  if (t < dt) {                // just after the edge
    t /= dt;
    return t + t - t * t - 1.0f;
  } else if (t > 1.0f - dt) {  // just before the next edge
    t = (t - 1.0f) / dt;
    return t * t + t + t + 1.0f;
  }
  return 0.0f;
}

float Oscillator::process() {
  float out;

  switch (_wave) {
    case OSC_SAW: {
      // Naive ramp -1..+1, minus polyBLEP at the wrap.
      out = 2.0f * _phase - 1.0f;
      out -= polyBlep(_phase, _inc);
      break;
    }
    case OSC_SQUARE: {
      // ±1 split at pulse width, polyBLEP on both rising and falling edges.
      out = (_phase < _pw) ? 1.0f : -1.0f;
      out += polyBlep(_phase, _inc);                       // rising edge @ 0
      float t2 = _phase - _pw;                             // falling edge @ pw
      if (t2 < 0.0f) t2 += 1.0f;
      out -= polyBlep(t2, _inc);
      break;
    }
    case OSC_TRIANGLE: {
      // Derived from a triangle directly; integrating the square is cleaner but
      // the naive triangle aliases far less than saw/square, so keep it direct.
      float tri = (_phase < 0.5f) ? (4.0f * _phase - 1.0f)
                                  : (3.0f - 4.0f * _phase);
      out = tri;
      break;
    }
    case OSC_SINE: {
      out = sinf(TWO_PI * _phase);
      break;
    }
    case OSC_NOISE: {
      // xorshift32 white noise, scaled to ~-1..+1. Phase is irrelevant but we
      // still advance it so freq sweeps don't desync if the wave is changed.
      _rng ^= _rng << 13;
      _rng ^= _rng >> 17;
      _rng ^= _rng << 5;
      out = ((float)(int32_t)_rng) * (1.0f / 2147483648.0f);
      // Noise advances no phase edge; return early to skip wrap bookkeeping for
      // clarity, but still increment phase below for waveform-switch safety.
      break;
    }
    default:
      out = 0.0f;
      break;
  }

  // Advance and wrap phase (cheap, no fmod).
  _phase += _inc;
  if (_phase >= 1.0f) _phase -= 1.0f;

  return out;
}
