/*
 * effects.cpp — master stereo effects (in-place float blocks).
 *
 * Delay, Reverb (compact Schroeder: 4 combs + 2 allpass per channel), Chorus
 * (modulated short delay), Bitcrusher. All buffers are FIXED-size members (no
 * allocation in process()). Reverb's internal lines live as file-scope-sized
 * arrays inside this .cpp per the header's note. Pure float, no locks, no Serial.
 * CORE_AUDIO only.
 *
 * Schroeder/comb structure adapted from the public-domain Freeverb topology,
 * self-contained.
 */
#include "effects.h"
#include <math.h>
#include <string.h>

static const float TWO_PI = 6.28318530717958647692f;

static inline float clamp01(float v) {
  if (v < 0.0f) return 0.0f;
  if (v > 1.0f) return 1.0f;
  return v;
}

// ════════════════════════════════════════════════════════════════════════
//  Delay
// ════════════════════════════════════════════════════════════════════════
Delay::Delay()
: _write(0), _delaySamples(SAMPLE_RATE / 4), _fb(0.3f), _mix(0.25f) {
  memset(_buf, 0, sizeof(_buf));
}

void Delay::setTime(float seconds) {
  if (seconds < 0.0f) seconds = 0.0f;
  size_t s = (size_t)(seconds * (float)SAMPLE_RATE);
  if (s < 1) s = 1;
  if (s >= FX_DELAY_MAX_SAMPLES) s = FX_DELAY_MAX_SAMPLES - 1;
  _delaySamples = s;
}

void Delay::setFeedback(float fb0to1) { _fb = clamp01(fb0to1); }
void Delay::setMix(float wet0to1)     { _mix = clamp01(wet0to1); }

void Delay::reset() {
  memset(_buf, 0, sizeof(_buf));
  _write = 0;
}

void Delay::process(float* stereo, size_t frames) {
  for (size_t i = 0; i < frames; ++i) {
    // Read index trails the write head by _delaySamples (wrapped).
    size_t r = _write + FX_DELAY_MAX_SAMPLES - _delaySamples;
    if (r >= FX_DELAY_MAX_SAMPLES) r -= FX_DELAY_MAX_SAMPLES;

    for (int ch = 0; ch < 2; ++ch) {
      float in  = stereo[i * 2 + ch];
      float dly = _buf[ch][r];
      // Write input + feedback of the delayed sample.
      _buf[ch][_write] = in + dly * _fb;
      // Dry + wet.
      stereo[i * 2 + ch] = in * (1.0f - _mix) + dly * _mix;
    }

    if (++_write >= FX_DELAY_MAX_SAMPLES) _write = 0;
  }
}

// ════════════════════════════════════════════════════════════════════════
//  Reverb — compact Freeverb-style (4 combs + 2 allpass per channel)
//  Buffer lengths (in samples @ ~44.1k) are the classic Freeverb primes, with
//  a small stereo spread on the right channel.
// ════════════════════════════════════════════════════════════════════════
#define RV_NCOMB    4
#define RV_NALLPASS 2

static const int RV_COMB_LEN[RV_NCOMB]    = { 1557, 1617, 1491, 1422 };
static const int RV_AP_LEN[RV_NALLPASS]   = { 556, 441 };
static const int RV_STEREO_SPREAD         = 23;   // R channel offset
#define RV_COMB_MAX 1700
#define RV_AP_MAX   600

// Per-channel state lives here (file scope, fixed size — header asked for this).
struct ReverbState {
  float comb[2][RV_NCOMB][RV_COMB_MAX];
  int   combIdx[2][RV_NCOMB];
  float combFilt[2][RV_NCOMB];           // one-pole damping state
  float ap[2][RV_NALLPASS][RV_AP_MAX];
  int   apIdx[2][RV_NALLPASS];
};
// One global reverb tank (there is exactly one master Reverb instance).
static ReverbState s_rv;

Reverb::Reverb()
: _room(0.5f), _damp(0.5f), _mix(0.2f) {
  memset(&s_rv, 0, sizeof(s_rv));
}

void Reverb::setRoomSize(float r0to1) { _room = clamp01(r0to1); }
void Reverb::setDamping(float d0to1)  { _damp = clamp01(d0to1); }
void Reverb::setMix(float wet0to1)    { _mix  = clamp01(wet0to1); }

void Reverb::reset() {
  memset(&s_rv, 0, sizeof(s_rv));
}

void Reverb::process(float* stereo, size_t frames) {
  // Map room/damping to comb feedback and damping coefficients (Freeverb scale).
  const float feedback = 0.28f + _room * 0.70f;   // ~0.28..0.98
  const float damp     = _damp * 0.4f;            // gentle one-pole

  for (size_t i = 0; i < frames; ++i) {
    for (int ch = 0; ch < 2; ++ch) {
      float in = stereo[i * 2 + ch];
      float acc = 0.0f;

      // Parallel comb filters with damped feedback.
      for (int c = 0; c < RV_NCOMB; ++c) {
        int len = RV_COMB_LEN[c] + (ch ? RV_STEREO_SPREAD : 0);
        int idx = s_rv.combIdx[ch][c];
        float y = s_rv.comb[ch][c][idx];
        acc += y;
        // One-pole lowpass in the feedback path (damping).
        float filt = y * (1.0f - damp) + s_rv.combFilt[ch][c] * damp;
        s_rv.combFilt[ch][c] = filt;
        s_rv.comb[ch][c][idx] = in + filt * feedback;
        if (++idx >= len) idx = 0;
        s_rv.combIdx[ch][c] = idx;
      }
      acc *= (1.0f / RV_NCOMB);

      // Series allpass diffusers.
      for (int a = 0; a < RV_NALLPASS; ++a) {
        int len = RV_AP_LEN[a];
        int idx = s_rv.apIdx[ch][a];
        float buf = s_rv.ap[ch][a][idx];
        const float g = 0.5f;
        float y = -acc + buf;
        s_rv.ap[ch][a][idx] = acc + buf * g;
        acc = y;
        if (++idx >= len) idx = 0;
        s_rv.apIdx[ch][a] = idx;
      }

      stereo[i * 2 + ch] = in * (1.0f - _mix) + acc * _mix;
    }
  }
}

// ════════════════════════════════════════════════════════════════════════
//  Chorus — modulated short delay (one shared line per channel)
// ════════════════════════════════════════════════════════════════════════
#define CH_MAX 2048                      // ~46 ms @ 44.1k
static float s_chBuf[2][CH_MAX];
static int   s_chWrite = 0;

Chorus::Chorus()
: _rate(0.8f), _depth(0.5f), _mix(0.4f), _lfoPhase(0.0f) {
  memset(s_chBuf, 0, sizeof(s_chBuf));
  s_chWrite = 0;
}

void Chorus::setRate(float hz)      { _rate  = (hz < 0.0f) ? 0.0f : hz; }
void Chorus::setDepth(float d0to1)  { _depth = clamp01(d0to1); }
void Chorus::setMix(float wet0to1)  { _mix   = clamp01(wet0to1); }

void Chorus::reset() {
  memset(s_chBuf, 0, sizeof(s_chBuf));
  s_chWrite = 0;
  _lfoPhase = 0.0f;
}

void Chorus::process(float* stereo, size_t frames) {
  const float inc = _rate / (float)SAMPLE_RATE;
  // Base delay ~12 ms, modulation depth up to ~8 ms.
  const float baseDelay = 0.012f * (float)SAMPLE_RATE;
  const float modDepth  = 0.008f * (float)SAMPLE_RATE * _depth;

  for (size_t i = 0; i < frames; ++i) {
    // Two LFO phases 90° apart give a wider stereo image.
    float lfoL = 0.5f * (1.0f + sinf(TWO_PI * _lfoPhase));
    float lfoR = 0.5f * (1.0f + sinf(TWO_PI * _lfoPhase + 1.5707963f));
    _lfoPhase += inc;
    if (_lfoPhase >= 1.0f) _lfoPhase -= 1.0f;

    for (int ch = 0; ch < 2; ++ch) {
      float in = stereo[i * 2 + ch];
      s_chBuf[ch][s_chWrite] = in;

      float d = baseDelay + modDepth * (ch ? lfoR : lfoL);
      // Fractional read with linear interpolation.
      float rpos = (float)s_chWrite - d;
      while (rpos < 0.0f) rpos += CH_MAX;
      int   r0 = (int)rpos;
      float fr = rpos - (float)r0;
      int   r1 = r0 + 1; if (r1 >= CH_MAX) r1 -= CH_MAX;
      float wet = s_chBuf[ch][r0] * (1.0f - fr) + s_chBuf[ch][r1] * fr;

      stereo[i * 2 + ch] = in * (1.0f - _mix) + wet * _mix;
    }

    if (++s_chWrite >= CH_MAX) s_chWrite = 0;
  }
}

// ════════════════════════════════════════════════════════════════════════
//  Bitcrusher — bit-depth + sample-rate reduction
// ════════════════════════════════════════════════════════════════════════
Bitcrusher::Bitcrusher()
: _bits(16.0f), _reduction(1.0f), _mix(1.0f),
  _holdL(0.0f), _holdR(0.0f), _counter(0.0f) {}

void Bitcrusher::setBits(float bits1to16) {
  if (bits1to16 < 1.0f)  bits1to16 = 1.0f;
  if (bits1to16 > 16.0f) bits1to16 = 16.0f;
  _bits = bits1to16;
}
void Bitcrusher::setRateReduction(float factor1up) {
  if (factor1up < 1.0f) factor1up = 1.0f;
  _reduction = factor1up;
}
void Bitcrusher::setMix(float wet0to1) { _mix = clamp01(wet0to1); }

void Bitcrusher::process(float* stereo, size_t frames) {
  // Quantization step: 2^bits levels over the -1..+1 range.
  float levels = powf(2.0f, _bits);
  float step   = 2.0f / levels;

  for (size_t i = 0; i < frames; ++i) {
    // Sample-and-hold for rate reduction: refresh the held sample every
    // _reduction frames.
    _counter += 1.0f;
    if (_counter >= _reduction) {
      _counter -= _reduction;
      float l = stereo[i * 2 + 0];
      float r = stereo[i * 2 + 1];
      // Quantize to the reduced bit depth.
      _holdL = floorf(l / step + 0.5f) * step;
      _holdR = floorf(r / step + 0.5f) * step;
    }
    stereo[i * 2 + 0] = stereo[i * 2 + 0] * (1.0f - _mix) + _holdL * _mix;
    stereo[i * 2 + 1] = stereo[i * 2 + 1] * (1.0f - _mix) + _holdR * _mix;
  }
}
