/*
 * mux.h — 3x 74HC4067 analog multiplexer scanner (knobs + sliders).
 *
 * One shared 4-bit select bus (MUX_SEL_PINS S0..S3) addresses all three muxes;
 * their outputs feed three separate ADC1 pins:
 *     MUX_A_OUT -> knobs  0..15
 *     MUX_B_OUT -> knobs 16..31   (only NUM_KNOBS of A+B are published)
 *     MUX_C_OUT -> sliders 0..15  (only NUM_SLIDERS published)
 *
 * Per channel: set select -> wait MUX_SETTLE_US -> analogRead each output.
 * Values are exponentially smoothed (ANALOG_SMOOTHING) via ResponsiveAnalogRead.
 * When a control moves enough to matter, the scanner posts:
 *     EV_KNOB_CHANGED   (a = knob index,   val = 0..4095 smoothed)
 *     EV_SLIDER_CHANGED (a = slider index, val = 0..4095 smoothed)
 * It never calls the synth directly — the dispatcher maps moves to params.
 *
 * Implemented in src/drivers/mux.cpp.
 * Owned by Task_MuxScan (CORE_IO), ticked every ANALOG_SCAN_INTERVAL_MS.
 */
#pragma once
#include <stdint.h>
#include "config.h"   // MUX_*, NUM_KNOBS, NUM_SLIDERS, ADC, smoothing

// Configure select pins as outputs, ADC pins/attenuation/resolution, and seed
// the smoothing filters from a first read. Call once from Task_MuxScan.
void mux_init();

// Walk all MUX_CHANNELS once, smooth each control, and post EV_KNOB_CHANGED /
// EV_SLIDER_CHANGED for any control that crossed its change threshold.
// Call once per ANALOG_SCAN_INTERVAL_MS tick. Non-blocking.
void mux_scan();

// Latest smoothed value accessors (0..4095). Index ranges:
//   knob   : 0 .. NUM_KNOBS-1
//   slider : 0 .. NUM_SLIDERS-1
// Out-of-range indices return 0. Read-only snapshot, safe on CORE_IO.
int mux_knob(uint8_t index);
int mux_slider(uint8_t index);
