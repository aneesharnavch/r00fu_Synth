/*
 * tasks.cpp — FreeRTOS task bodies + the spawn entry point (implements tasks.h).
 *
 * Every module exposes thin init/step functions; the tasks here are the only
 * things that own timing and core placement. We pin each task to the core and
 * run it at the priority declared in config.h (CORE_IO=0, CORE_AUDIO=1), keeping
 * the dual-core split explicit and matching the header's table exactly:
 *
 *   CORE_IO (0):
 *     Task_ButtonScan  PRIO_BUTTONSCAN  matrix_init()  / matrix_scan() @ 1 ms
 *     Task_MuxScan     PRIO_MUXSCAN     mux_init()     / mux_scan()    @ 5 ms
 *     Task_MIDI        PRIO_MIDI        midi/usb poll + send drain (tight loop)
 *     Task_Sequencer   PRIO_SEQUENCER   seq_tick() on the internal tempo clock
 *     Task_UI          PRIO_UI          ui_dispatch(event_get(...)) + cfg_poll()
 *   CORE_AUDIO (1):
 *     Task_Audio       PRIO_AUDIO       i2s+synth init, then render -> i2s loop
 *
 * HARD RULE compliance:
 *   - Task_Audio is the only thing on CORE_AUDIO and runs at the highest prio.
 *     Its loop NEVER blocks except inside i2s_write_block() (the DMA pacer); no
 *     malloc, no locks, no Serial. The I2S channel + synth command queue are
 *     created from this task so the DMA + voices live on core 1.
 *   - The scanners post Events only; they never call the synth/sequencer.
 *
 * Periodic tasks use vTaskDelayUntil so cadence doesn't drift with work time.
 */
#include "system/tasks.h"

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "events.h"

// Driver / engine step functions owned by these tasks.
#include "drivers/button_matrix.h"
#include "drivers/mux.h"
#include "drivers/midi_din.h"
#include "drivers/usb_midi.h"
#include "drivers/config_protocol.h"
#include "drivers/pcm5102.h"
#include "audio/synth.h"
#include "sequencer/sequencer.h"
#include "ui/modes.h"

// ── Task handles (kept for completeness / future introspection) ─────────────
static TaskHandle_t s_hButtonScan = nullptr;
static TaskHandle_t s_hMuxScan    = nullptr;
static TaskHandle_t s_hMIDI       = nullptr;
static TaskHandle_t s_hSequencer  = nullptr;
static TaskHandle_t s_hUI         = nullptr;
static TaskHandle_t s_hAudio      = nullptr;

// Stack depths (in words). Generous for the I/O tasks (printf-style logging on
// the UI/config path); the audio task gets the largest because the render path
// holds AUDIO_BLOCK_SIZE float buffers on its stack.
#define STACK_BUTTONSCAN 2048
#define STACK_MUXSCAN    3072   // ResponsiveAnalogRead + analogRead machinery
#define STACK_MIDI       4096   // MIDI library + TinyUSB callbacks
#define STACK_SEQUENCER  3072
#define STACK_UI         4096   // dispatcher + NeoPixel + cfg printf
#define STACK_AUDIO      6144   // synth/effects float blocks + FX delay math

// ───────────────────────────── CORE_IO tasks ────────────────────────────────

// Task_ButtonScan — 8x8 matrix scan at MATRIX_SCAN_INTERVAL_MS (1 ms), fixed
// cadence via vTaskDelayUntil. matrix_scan() posts EV_BUTTON_* Events only.
static void Task_ButtonScan(void*) {
  matrix_init();
  TickType_t last = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(MATRIX_SCAN_INTERVAL_MS);
  for (;;) {
    matrix_scan();
    vTaskDelayUntil(&last, period);
  }
}

// Task_MuxScan — knob/slider scan at ANALOG_SCAN_INTERVAL_MS (5 ms). mux_scan()
// posts EV_KNOB_CHANGED / EV_SLIDER_CHANGED only.
static void Task_MuxScan(void*) {
  mux_init();
  TickType_t last = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(ANALOG_SCAN_INTERVAL_MS);
  for (;;) {
    mux_scan();
    vTaskDelayUntil(&last, period);
  }
}

// Task_MIDI — drain DIN + USB MIDI IN as fast as we can; both poll functions are
// non-blocking and post SRC_DIN / SRC_USB Events. A 1 ms tick keeps latency low
// without busy-spinning the core. The MIDI interfaces are brought up here so
// their underlying UART / TinyUSB objects are touched from a single task.
static void Task_MIDI(void*) {
  midi_din_init();
  usb_midi_init();
  TickType_t last = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(1);
  for (;;) {
    midi_din_poll();
    usb_midi_poll();
    vTaskDelayUntil(&last, period);
  }
}

// Task_Sequencer — internal clock SOURCE only. We derive the 24-ppqn tick period
// from the current tempo every loop (so tempo changes take effect smoothly) and,
// while playing AND not slaved, POST an EV_CLOCK_TICK (SRC_LOCAL). We do NOT call
// seq_tick() here: the dispatcher (Task_UI) owns all sequencer state, so it is
// the single task that advances the sequencer (on both local and external clock
// ticks). This removes the Task_Sequencer-vs-Task_UI data race and the prior
// double-advance when slaved. Posting SRC_LOCAL ticks also gives the dispatcher
// a hook to emit outgoing MIDI clock as master. seq_init() runs once up front.
static void Task_Sequencer(void*) {
  seq_init();
  TickType_t last = xTaskGetTickCount();
  for (;;) {
    // Period of one 24-ppqn tick at the current BPM:
    //   us/tick = 60e6 / (BPM * PPQN). Recomputed each iteration.
    float bpm = seq_tempo();
    if (bpm < 1.0f) bpm = 120.0f;
    const double us_per_tick = 60000000.0 / ((double)bpm * (double)SEQ_PPQN);
    TickType_t period = pdMS_TO_TICKS((uint32_t)(us_per_tick / 1000.0));
    if (period < 1) period = 1;   // never a zero-tick delay

    // Drive the internal clock only when we are the clock master (not slaved)
    // and actually playing. When slaved, the external EV_CLOCK_TICK is the sole
    // advance source, so we stay quiet here and just pace the loop off tempo.
    if (!seq_is_external() && seq_state() == SEQ_PLAYING) {
      event_post(Event{ EV_CLOCK_TICK, SRC_LOCAL, 0, 0, 0, 0 });
    }
    vTaskDelayUntil(&last, period);
  }
}

// Task_UI — the dispatcher. Blocks on event_get() (parks the core when idle),
// routes each Event via ui_dispatch(), and services the GUI config protocol.
// ui_init() and cfg_init() run once before the loop. We use a short event_get
// timeout so cfg_poll() still runs promptly even when no Events are flowing.
static void Task_UI(void*) {
  ui_init();
  cfg_init();
  for (;;) {
    Event e;
    if (event_get(e, 5 /*ms*/)) {
      ui_dispatch(e);
    }
    // Always pump the config link (IDENTIFY / MAP / DUMP from the GUI).
    cfg_poll();
  }
}

// ──────────────────────────── CORE_AUDIO task ───────────────────────────────

// Task_Audio — the ONLY thing on CORE_AUDIO, highest priority. It installs the
// I2S channel and the synth (command queue + voices) from this core so the DMA
// ring and DSP state are owned by core 1, then runs the render loop forever.
// i2s_write_block() is the single blocking call; it paces the loop to real time.
// If init fails we park the task (no audio) rather than spinning a broken loop.
static void Task_Audio(void*) {
  if (!i2s_audio_init() || !synth_init()) {
    // Surface the failure once and idle; the rest of the device still runs.
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
  }

  static int16_t block[AUDIO_BLOCK_SIZE * 2];   // interleaved L/R, task-static
  for (;;) {
    synth_render(block, AUDIO_BLOCK_SIZE);       // drains cmd queue, mixes voices
    i2s_write_block(block, AUDIO_BLOCK_SIZE);    // blocks on DMA room == clock
  }
}

// ───────────────────────────── spawn entry ──────────────────────────────────
void tasks_start() {
  // CORE_IO (0): scanners, MIDI, sequencer, UI/dispatcher.
  xTaskCreatePinnedToCore(Task_ButtonScan, "btnscan", STACK_BUTTONSCAN, nullptr,
                          PRIO_BUTTONSCAN, &s_hButtonScan, CORE_IO);
  xTaskCreatePinnedToCore(Task_MuxScan, "muxscan", STACK_MUXSCAN, nullptr,
                          PRIO_MUXSCAN, &s_hMuxScan, CORE_IO);
  xTaskCreatePinnedToCore(Task_MIDI, "midi", STACK_MIDI, nullptr,
                          PRIO_MIDI, &s_hMIDI, CORE_IO);
  xTaskCreatePinnedToCore(Task_Sequencer, "seq", STACK_SEQUENCER, nullptr,
                          PRIO_SEQUENCER, &s_hSequencer, CORE_IO);
  xTaskCreatePinnedToCore(Task_UI, "ui", STACK_UI, nullptr,
                          PRIO_UI, &s_hUI, CORE_IO);

  // CORE_AUDIO (1): audio only, highest priority.
  xTaskCreatePinnedToCore(Task_Audio, "audio", STACK_AUDIO, nullptr,
                          PRIO_AUDIO, &s_hAudio, CORE_AUDIO);
}
