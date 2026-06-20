/*
 * adsr.cpp — linear-segment ADSR envelope.
 *
 * Per-sample, no allocation. Attack ramps 0->1, Decay ramps 1->sustain, Sustain
 * holds, Release ramps current level->0. Rates are precomputed per-sample
 * increments from the segment times so process() is just add/compare. A tiny
 * floor on times prevents divide-by-zero and gives near-instant segments.
 * CORE_AUDIO only.
 */
#include "adsr.h"

// Shortest meaningful segment: ~1 sample. Used as a time floor.
static const float MIN_TIME = 1.0f / (float)SAMPLE_RATE;

ADSR::ADSR()
: _stage(IDLE), _level(0.0f),
  _aRate(1.0f), _dRate(1.0f), _rRate(1.0f), _sustain(0.8f) {
  setAttack(0.005f);
  setDecay(0.1f);
  setRelease(0.2f);
}

// Each "rate" is the per-sample delta to traverse the full 0..1 span in `sec`
// seconds. Decay/Release scale their actual spans by the current distance.
void ADSR::setAttack(float seconds) {
  if (seconds < MIN_TIME) seconds = MIN_TIME;
  _aRate = 1.0f / (seconds * (float)SAMPLE_RATE);
}
void ADSR::setDecay(float seconds) {
  if (seconds < MIN_TIME) seconds = MIN_TIME;
  _dRate = 1.0f / (seconds * (float)SAMPLE_RATE);
}
void ADSR::setSustain(float level0to1) {
  if (level0to1 < 0.0f) level0to1 = 0.0f;
  if (level0to1 > 1.0f) level0to1 = 1.0f;
  _sustain = level0to1;
}
void ADSR::setRelease(float seconds) {
  if (seconds < MIN_TIME) seconds = MIN_TIME;
  _rRate = 1.0f / (seconds * (float)SAMPLE_RATE);
}

void ADSR::gate(bool on) {
  if (on) {
    _stage = ATTACK;          // restart from current level (legato-friendly)
  } else {
    if (_stage != IDLE) _stage = RELEASE;
  }
}

void ADSR::reset() {
  _stage = IDLE;
  _level = 0.0f;
}

float ADSR::process() {
  switch (_stage) {
    case ATTACK:
      _level += _aRate;
      if (_level >= 1.0f) {
        _level = 1.0f;
        _stage = DECAY;
      }
      break;

    case DECAY:
      // Move toward sustain at the decay rate (rate is span-per-sample).
      _level -= _dRate;
      if (_level <= _sustain) {
        _level = _sustain;
        _stage = SUSTAIN;
      }
      break;

    case SUSTAIN:
      _level = _sustain;
      // Edge case: if a note sustains at 0 it never goes idle on its own; the
      // amp voice treats SUSTAIN as still-active, which is correct (held key).
      break;

    case RELEASE:
      _level -= _rRate;
      if (_level <= 0.0f) {
        _level = 0.0f;
        _stage = IDLE;
      }
      break;

    case IDLE:
    default:
      _level = 0.0f;
      break;
  }
  return _level;
}
