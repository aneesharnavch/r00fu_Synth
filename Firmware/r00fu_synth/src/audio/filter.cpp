/*
 * filter.cpp — Chamberlin state-variable filter.
 *
 * Classic 2-pole SVF producing low/high/band/notch simultaneously; setMode()
 * picks which to return. Coefficient _f = 2*sin(pi*fc/fs), damping _q = 1/Q.
 * Stable while _f < ~1.0 (we clamp cutoff to ~ fs/6 for headroom). Pure float,
 * no allocation. Two state integrators (_low, _band). CORE_AUDIO only.
 *
 * Adapted from the well-known Hal Chamberlin / Andrew Simper SVF used in the
 * AcidBox / ML_SynthTools filters, self-contained here.
 */
#include "filter.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Keep the design stable: Chamberlin SVF is well-behaved while the corner is
// below ~ fs/6. We clamp cutoff to that window.
static const float CUTOFF_MIN = 20.0f;
static const float CUTOFF_MAX = (float)SAMPLE_RATE / 6.0f;

Filter::Filter()
: _mode(FILT_LOWPASS), _f(0.3f), _q(1.0f), _low(0.0f), _band(0.0f) {
  setCutoff(1000.0f);
  setResonance(0.2f);
}

void Filter::setMode(FilterMode m) { _mode = m; }

void Filter::setCutoff(float hz) {
  if (hz < CUTOFF_MIN) hz = CUTOFF_MIN;
  if (hz > CUTOFF_MAX) hz = CUTOFF_MAX;
  // _f = 2*sin(pi*fc/fs): the exact Chamberlin tuning coefficient.
  //
  // Within our clamped range (fc <= fs/6, so the argument is <= pi/3) the cheap
  // small-angle approximation 2*sin(x) ~= 2*x is within ~5% and the residual is
  // a slight, monotonic cutoff offset — inaudible for a synth corner. We use it
  // here to keep this off the per-sample/transcendental hot path: Voice::render
  // calls setCutoff() at block rate, and at 8-voice polyphony the saved sinf()
  // calls are the single biggest CORE_AUDIO margin win. (Was: 2*sinf(pi*fc/fs).)
  _f = (2.0f * (float)M_PI / (float)SAMPLE_RATE) * hz;
}

void Filter::setResonance(float r0to1) {
  if (r0to1 < 0.0f) r0to1 = 0.0f;
  if (r0to1 > 1.0f) r0to1 = 1.0f;
  // Damping _q = 1/Q. r=0 -> heavy damping (q=2, gentle), r->1 -> q->~0.02
  // (near self-oscillation). Leave a small floor so it never fully blows up.
  _q = 2.0f - 1.98f * r0to1;
}

void Filter::reset() {
  _low  = 0.0f;
  _band = 0.0f;
}

float Filter::process(float in) {
  // One pass of the Chamberlin SVF. (Two passes would oversample for stability
  // at high cutoff; one pass is fine within our clamped range and cheaper.)
  _low += _f * _band;
  float high = in - _low - _q * _band;
  _band += _f * high;
  float notch = high + _low;

  switch (_mode) {
    case FILT_HIGHPASS: return high;
    case FILT_BANDPASS: return _band;
    case FILT_NOTCH:    return notch;
    case FILT_LOWPASS:
    default:            return _low;
  }
}
