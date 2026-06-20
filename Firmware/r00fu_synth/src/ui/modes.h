/*
 * modes.h — the UI dispatcher: the ONLY place raw events become musical action.
 *
 * Hardware scanners post raw Events (EV_BUTTON_*, EV_KNOB/SLIDER_CHANGED) and
 * MIDI/sequencer post musical/transport Events. ui_dispatch() consumes each
 * Event from the queue and, depending on the current Mode and the per-button
 * map (midi_map.h), routes it onward:
 *   - musical actions reach the synth via the RT-safe synth_* queue helpers,
 *   - and/or leave via midi_din_send_* / usb_midi_send_* (respecting Event.src
 *     to avoid echo loops),
 *   - sequencer edits via seq_* calls, presets via presets_*, etc.
 * It also drives the on-board status LED and emits BTN lines to the GUI
 * (cfg_emit_button). Runs in Task_UI (CORE_IO). Never calls DSP directly.
 *
 * Implemented in src/ui/modes.cpp.
 */
#pragma once
#include <stdint.h>
#include "config.h"
#include "events.h"
#include "midi_map.h"

// The five product modes (ids match midi_map.h MAP_MODE values AND the GUI's
// MODES list: 0 Drum, 1 StepSeq, 2 Keyboard, 3 DAW, 4 Performance).
enum Mode : uint8_t {
  MODE_DRUM = 0,
  MODE_STEPSEQ,
  MODE_KEYBOARD,
  MODE_DAW,
  MODE_PERFORMANCE,
  MODE_COUNT,
};

// Set up dispatcher state, NeoPixel status LED, and select the startup mode
// (from g_settings). Call once from Task_UI before the dispatch loop.
void ui_init();

// Handle exactly one Event (already pulled from the queue by Task_UI). Routes
// per current mode + button map. Non-blocking aside from the RT-safe sends.
void ui_dispatch(const Event& e);

// Current-mode get/set. ui_set_mode posts EV_MODE_CHANGE and reconfigures the
// grid meaning + LED; callers (a MAP_MODE button) use it via the dispatcher.
Mode ui_mode();
void ui_set_mode(Mode m);
