/*
 * adsr.h — linear/exponential ADSR envelope generator.
 *
 * Per-sample float envelope, no allocation. Times are in seconds; sustain is a
 * 0..1 level. gate(true) starts Attack, gate(false) starts Release. isActive()
 * lets the voice allocator reclaim a voice once the envelope has fully decayed.
 *
 * Used only on CORE_AUDIO inside Voice (voice.h).
 */
#pragma once
#include <stdint.h>
#include "config.h"   // SAMPLE_RATE

class ADSR {
public:
  enum Stage : uint8_t { IDLE = 0, ATTACK, DECAY, SUSTAIN, RELEASE };

  ADSR();

  void  setAttack (float seconds);
  void  setDecay  (float seconds);
  void  setSustain(float level0to1);
  void  setRelease(float seconds);

  void  gate(bool on);   // true = note on (->ATTACK), false = note off (->RELEASE)
  void  reset();         // hard reset to IDLE, level 0 (voice steal)

  float process();       // next envelope sample 0..1 (advances the envelope)
  float level()    const { return _level; }   // current level, WITHOUT advancing
  bool  isActive() const { return _stage != IDLE; }
  Stage stage()    const { return _stage; }

private:
  Stage _stage;
  float _level;          // current output 0..1
  float _aRate, _dRate, _rRate;   // per-sample increments
  float _sustain;        // sustain level 0..1
};
