/*
 * mux.cpp — 3x 74HC4067 analog multiplexer scanner (knobs + sliders).
 *
 * Three muxes share one 4-bit select bus (MUX_SEL_PINS S0..S3). Each mux output
 * feeds its own ADC1 pin (MUX_A_OUT / B_OUT / C_OUT). For every channel 0..15
 * we drive the select bits, wait MUX_SETTLE_US for the analog mux + RC to
 * settle, then analogRead all three outputs in turn.
 *
 *   mux A ch0..15  -> knobs   0..15
 *   mux B ch0..15  -> knobs  16..31   (only first NUM_KNOBS of A+B published)
 *   mux C ch0..15  -> sliders 0..15   (only first NUM_SLIDERS published)
 *
 * Each raw read is fed through a per-control ResponsiveAnalogRead which gives
 * us exponential smoothing + a built-in "did this move enough to matter?" test
 * (hasChanged). On a meaningful change we event_post() EV_KNOB_CHANGED /
 * EV_SLIDER_CHANGED. We never touch the synth — the dispatcher maps moves.
 *
 * Owned by Task_MuxScan (CORE_IO), ticked every ANALOG_SCAN_INTERVAL_MS.
 */
#include "drivers/mux.h"
#include "events.h"
#include <Arduino.h>
#include <ResponsiveAnalogRead.h>

// ── Channel layout ─────────────────────────────────────────────────────────
// Total smoothed controls = the two knob banks (A,B) + one slider bank (C),
// 16 channels each. We allocate the full 3*16 and only publish the first
// NUM_KNOBS / NUM_SLIDERS; the rest are read (so the filters stay warm) but
// never emit events.
#define KNOB_RAW_COUNT   (2 * MUX_CHANNELS)   // mux A + mux B = 32 raw knobs
#define SLIDER_RAW_COUNT (1 * MUX_CHANNELS)   // mux C         = 16 raw sliders

// The three ADC output pins, indexed [0]=A, [1]=B, [2]=C — kept in an array so
// the inner read loop stays branch-free.
static const int kMuxOut[MUX_COUNT] = { MUX_A_OUT, MUX_B_OUT, MUX_C_OUT };

// One smoother per raw control. ResponsiveAnalogRead is used in "manual" mode:
// we read the ADC ourselves (one pin is shared across 16 channels) and push the
// raw value in via update(int). The pin arg is therefore unused (0).
//   - sleepEnable = true  : settle to a stable value and stop dithering at rest
//   - snapMultiplier from ANALOG_SMOOTHING : higher = snappier, lower = smoother
static ResponsiveAnalogRead g_knob[KNOB_RAW_COUNT];
static ResponsiveAnalogRead g_slider[SLIDER_RAW_COUNT];

// Latest smoothed snapshots (0..4095) for the public accessors. Written only on
// CORE_IO by mux_scan(); read by mux_knob()/mux_slider() on the same core, so
// no locking is needed.
static int g_knobVal[KNOB_RAW_COUNT];
static int g_sliderVal[SLIDER_RAW_COUNT];

// First few scans after boot are discarded: GPIO3 (MUX_C_OUT) is a strapping
// pin and the select lines / mux outputs need a moment to settle, so the very
// first reads can be garbage. We seed the filters during warm-up but suppress
// event posting until the controls are trustworthy.
#define MUX_WARMUP_SCANS 4
static uint8_t g_warmup = MUX_WARMUP_SCANS;

// ── select bus ─────────────────────────────────────────────────────────────
// Drive S0..S3 to address channel `ch` (0..15) on all three muxes at once.
static inline void mux_select(uint8_t ch) {
  digitalWrite(MUX_SEL_PINS[0], (ch >> 0) & 0x01);
  digitalWrite(MUX_SEL_PINS[1], (ch >> 1) & 0x01);
  digitalWrite(MUX_SEL_PINS[2], (ch >> 2) & 0x01);
  digitalWrite(MUX_SEL_PINS[3], (ch >> 3) & 0x01);
}

// ── init ───────────────────────────────────────────────────────────────────
void mux_init() {
  // Select pins are plain digital outputs; park them low.
  for (int i = 0; i < 4; ++i) {
    pinMode(MUX_SEL_PINS[i], OUTPUT);
    digitalWrite(MUX_SEL_PINS[i], LOW);
  }

  // ADC config: 12-bit and 11 dB attenuation so the full 0..3.3 V pot/slider
  // travel maps onto the 0..4095 range.
  analogReadResolution(ADC_RESOLUTION_BITS);
  for (int m = 0; m < MUX_COUNT; ++m) {
    pinMode(kMuxOut[m], INPUT);
    analogSetPinAttenuation(kMuxOut[m], ADC_11db);
  }

  // Configure every smoother. snapMultiplier == ANALOG_SMOOTHING (0.10) gives
  // the spec's "0.9*prev + 0.1*cur" feel. enableEdgeSnap keeps the extremes
  // (0 / 4095) reachable without the filter parking just short of them.
  for (int i = 0; i < KNOB_RAW_COUNT; ++i) {
    g_knob[i] = ResponsiveAnalogRead(0, true, ANALOG_SMOOTHING);
    g_knob[i].setAnalogResolution(1 << ADC_RESOLUTION_BITS);  // 4096
    g_knob[i].enableEdgeSnap();
    g_knobVal[i] = 0;
  }
  for (int i = 0; i < SLIDER_RAW_COUNT; ++i) {
    g_slider[i] = ResponsiveAnalogRead(0, true, ANALOG_SMOOTHING);
    g_slider[i].setAnalogResolution(1 << ADC_RESOLUTION_BITS);
    g_slider[i].enableEdgeSnap();
    g_sliderVal[i] = 0;
  }

  // Seed the filters from one full read so the first published values aren't a
  // jump from zero. We still burn MUX_WARMUP_SCANS scans before emitting events.
  for (uint8_t ch = 0; ch < MUX_CHANNELS; ++ch) {
    mux_select(ch);
    delayMicroseconds(MUX_SETTLE_US);
    int a = analogRead(MUX_A_OUT);
    int b = analogRead(MUX_B_OUT);
    int c = analogRead(MUX_C_OUT);
    g_knob[ch].update(a);
    g_knob[MUX_CHANNELS + ch].update(b);
    g_slider[ch].update(c);
  }
  g_warmup = MUX_WARMUP_SCANS;
}

// ── one scan pass ──────────────────────────────────────────────────────────
void mux_scan() {
  const bool emit = (g_warmup == 0);   // suppress events during warm-up

  for (uint8_t ch = 0; ch < MUX_CHANNELS; ++ch) {
    mux_select(ch);
    delayMicroseconds(MUX_SETTLE_US);  // let mux + RC settle before sampling

    // Read all three banks for this channel.
    const int raw[MUX_COUNT] = {
      analogRead(MUX_A_OUT),   // knob bank A
      analogRead(MUX_B_OUT),   // knob bank B
      analogRead(MUX_C_OUT),   // slider bank C
    };

    // --- knob bank A (knobs 0..15) and B (knobs 16..31) ---
    for (int bank = 0; bank < 2; ++bank) {
      const uint8_t k = (uint8_t)(bank * MUX_CHANNELS + ch);  // raw knob index
      g_knob[k].update(raw[bank]);
      const int v = g_knob[k].getValue();
      g_knobVal[k] = v;
      // Publish only mapped knobs, and only once warmed up + actually moved.
      if (emit && k < NUM_KNOBS && g_knob[k].hasChanged()) {
        event_post(ev_analog(EV_KNOB_CHANGED, k, (int16_t)v));
      }
    }

    // --- slider bank C (sliders 0..15) ---
    g_slider[ch].update(raw[2]);
    const int sv = g_slider[ch].getValue();
    g_sliderVal[ch] = sv;
    if (emit && ch < NUM_SLIDERS && g_slider[ch].hasChanged()) {
      event_post(ev_analog(EV_SLIDER_CHANGED, ch, (int16_t)sv));
    }
  }

  if (g_warmup) --g_warmup;  // count down toward the first real emit
}

// ── accessors ──────────────────────────────────────────────────────────────
int mux_knob(uint8_t index) {
  if (index >= NUM_KNOBS) return 0;       // out-of-range -> 0 per contract
  return g_knobVal[index];
}

int mux_slider(uint8_t index) {
  if (index >= NUM_SLIDERS) return 0;
  return g_sliderVal[index];
}
