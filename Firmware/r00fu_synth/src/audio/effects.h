/*
 * effects.h — master stereo effects chain (process in place).
 *
 * Each processor takes an interleaved stereo float block (frames*2) and
 * rewrites it in place. No allocation in process() — delay/reverb buffers are
 * fixed-size members sized at construction. CORE_AUDIO only; run after voice
 * mixing, before int16 conversion + i2s_write_block().
 *
 * ── Internal-RAM budget (PSRAM DISABLED is the default; see config.h) ────────
 * The S3 has 512 KB SRAM but ~150-200 KB is consumed by FreeRTOS, TinyUSB,
 * LittleFS, ArduinoJson and the six task stacks before the app's own statics.
 * The big static FX lines (delay/reverb/chorus) therefore have to stay small or
 * they blow the budget at boot. The delay line is sized off whether PSRAM is on:
 *   - PSRAM OFF: FX_DELAY_MAX_SAMPLES = 4096 (~93 ms/ch -> 2*4096*4 = 32 KB).
 *   - PSRAM ON : FX_DELAY_MAX_SAMPLES = 16384 (~0.37 s/ch -> 128 KB) — fine in
 *                external RAM once the relocated mux pins free the OPI bus.
 * Reverb (~64 KB) + chorus (16 KB) are unconditional but modest; keeping the
 * delay small is what brings the total back under the internal-RAM ceiling.
 *
 * Implemented in src/audio/effects.cpp.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "config.h"   // SAMPLE_RATE, AUDIO_BLOCK_SIZE

// Fixed delay-line budget (samples per channel). With PSRAM disabled (default)
// the 2-channel float buffer must fit in internal SRAM alongside everything
// else, so we cap it low; with PSRAM enabled we can afford the full ~0.37 s.
#if defined(BOARD_HAS_PSRAM) && BOARD_HAS_PSRAM
#define FX_DELAY_MAX_SAMPLES 16384    // ~0.37 s/ch (128 KB) — external PSRAM
#else
#define FX_DELAY_MAX_SAMPLES 4096     // ~93 ms/ch (32 KB)   — internal SRAM
#endif

// ── Stereo delay ───────────────────────────────────────────────────────────
class Delay {
public:
  Delay();
  void setTime(float seconds);     // clamped to FX_DELAY_MAX_SAMPLES
  void setFeedback(float fb0to1);
  void setMix(float wet0to1);
  void reset();
  void process(float* stereo, size_t frames);   // in place
private:
  float  _buf[2][FX_DELAY_MAX_SAMPLES];
  size_t _write;
  size_t _delaySamples;
  float  _fb, _mix;
};

// ── Reverb (compact Schroeder/FDN-style) ───────────────────────────────────
class Reverb {
public:
  Reverb();
  void setRoomSize(float r0to1);
  void setDamping(float d0to1);
  void setMix(float wet0to1);
  void reset();
  void process(float* stereo, size_t frames);   // in place
private:
  float _room, _damp, _mix;
  // comb/allpass state sized in the .cpp (fixed arrays there)
};

// ── Chorus (modulated short delay) ─────────────────────────────────────────
class Chorus {
public:
  Chorus();
  void setRate(float hz);
  void setDepth(float d0to1);
  void setMix(float wet0to1);
  void reset();
  void process(float* stereo, size_t frames);   // in place
private:
  float _rate, _depth, _mix, _lfoPhase;
};

// ── Bitcrusher (bit-depth + sample-rate reduction) ─────────────────────────
class Bitcrusher {
public:
  Bitcrusher();
  void setBits(float bits1to16);
  void setRateReduction(float factor1up);   // 1=none, N=hold N samples
  void setMix(float wet0to1);
  void process(float* stereo, size_t frames);   // in place
private:
  float _bits, _reduction, _mix;
  float _holdL, _holdR;
  float _counter;
};
