/*
 * filter.h — Chamberlin state-variable filter (LP/HP/BP/Notch).
 *
 * One float SVF per voice. Cutoff in Hz, resonance 0..1. Per-sample process();
 * the selected mode picks which internal output is returned. No allocation, no
 * locks. Self-contained SVF math (AcidBox/ML pattern). CORE_AUDIO only.
 */
#pragma once
#include <stdint.h>
#include "config.h"   // SAMPLE_RATE

enum FilterMode : uint8_t {
  FILT_LOWPASS = 0,
  FILT_HIGHPASS,
  FILT_BANDPASS,
  FILT_NOTCH,
};

class Filter {
public:
  Filter();

  void  setMode(FilterMode m);
  void  setCutoff(float hz);        // clamped to a stable range vs SAMPLE_RATE
  void  setResonance(float r0to1);  // 0 = none, ~1 = self-oscillation edge
  void  reset();                    // clear delay state

  float process(float in);          // one sample in -> one sample out

private:
  FilterMode _mode;
  float _f;      // frequency coefficient derived from cutoff
  float _q;      // damping derived from resonance
  float _low;    // integrator state
  float _band;   // integrator state
};
