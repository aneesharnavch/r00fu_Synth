/*
 * tasks.h — FreeRTOS task creation + the system wiring entry point.
 *
 * tasks_start() owns ALL timing. It initializes the shared infrastructure
 * (events queue, USB device, LittleFS, midi map, settings, synth command
 * queue) and creates each task pinned to the core and priority declared in
 * config.h. Every module exposes small init/step functions; the tasks here are
 * thin loops that call them on the right cadence — no module owns its own task.
 *
 *   CORE_IO (0):
 *     Task_ButtonScan  PRIO_BUTTONSCAN  matrix_init()/matrix_scan() @1ms
 *     Task_MuxScan     PRIO_MUXSCAN     mux_init()/mux_scan()       @5ms
 *     Task_MIDI        PRIO_MIDI        midi_din_*/usb_midi_* poll
 *     Task_Sequencer   PRIO_SEQUENCER   seq_init()/seq_tick() on clock
 *     Task_UI          PRIO_UI          ui_dispatch(event_get(...)) + cfg_poll()
 *   CORE_AUDIO (1):
 *     Task_Audio       PRIO_AUDIO       synth_init(); loop synth_render() ->
 *                                       i2s_write_block() (paced by I2S)
 *
 * Implemented in src/system/tasks.cpp. Called once from app_main()/setup().
 */
#pragma once

// Initialize shared infrastructure and spawn all tasks pinned per config.h.
// Returns after the tasks are created (they run forever). Call once at boot.
void tasks_start();
