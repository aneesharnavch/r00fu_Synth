/*
 * main.cpp — boot + system wiring for r00fu_synth.
 *
 * Arduino entry points. setup() runs once on CORE_IO (the Arduino loopTask core)
 * and performs the whole bring-up in dependency order, then hands control to the
 * FreeRTOS tasks created by tasks_start(). loop() is intentionally empty: every
 * recurring job lives in a pinned task (see system/tasks.cpp), so the Arduino
 * loopTask has nothing left to do.
 *
 * INIT ORDER (each step depends on the ones before it):
 *   1. Serial          — native USB-CDC up first so early logs/HELLO reach the GUI.
 *   2. events_init     — the global queue must exist before ANY producer/consumer.
 *   3. presets_init    — mounts LittleFS (format-on-first-boot); storage for 4/5.
 *   4. settings_init   — loads global prefs (needs LittleFS); used by ui_init etc.
 *   5. midi_map_init   — factory map + overlay the GUI's saved assignments.
 *   6. i2s_audio_init  — install the I2S DAC channel (DMA ring; the audio pacer).
 *   7. synth_init      — voices + effects + the core0->core1 command queue.
 *   8. mux/matrix init — analog + button scanners' pin setup.
 *   9. midi/usb/cfg    — DIN + USB-MIDI interfaces and the GUI config link.
 *  10. seq_init        — clear patterns, default tempo, STOPPED.
 *  11. ui_init         — dispatcher state + status LED + startup mode.
 *  12. auto-load       — if g_settings.last_preset is pinned, presets_load() it.
 *  13. tasks_start     — spawn & pin every task; the device is now live.
 *
 * (presets_init() is called exactly once, at step 3; step 12 is the optional
 * last-preset auto-load, not a second mount.)
 *
 * Note on init vs. tasks: the scanners/MIDI/audio modules ALSO call their own
 * init at the top of their task (see tasks.cpp) so pin/DMA ownership lands on the
 * right core. Those inits are idempotent, so calling them here too is harmless
 * and keeps this boot sequence a single readable source of truth.
 */
#include <Arduino.h>

#include "config.h"
#include "events.h"
#include "midi_map.h"

#include "drivers/button_matrix.h"
#include "drivers/mux.h"
#include "drivers/midi_din.h"
#include "drivers/usb_midi.h"
#include "drivers/config_protocol.h"
#include "drivers/pcm5102.h"
#include "audio/synth.h"
#include "sequencer/sequencer.h"
#include "ui/modes.h"
#include "storage/presets.h"
#include "storage/settings.h"
#include "system/tasks.h"

void setup() {
  // 1) Native USB-CDC console (115200 to match the GUI + monitor_speed). A short
  //    settle lets the host enumerate the composite device before we greet it.
  Serial.begin(115200);
  delay(300);

  // 2) Global event bus — everything else may post to it from here on.
  events_init();

  // 3) Filesystem + persisted state. presets_init() mounts LittleFS (formatting
  //    on first boot); settings/midi_map then read their JSON off it.
  const bool fs_ok = presets_init();
  settings_init();
  midi_map_init();
  if (!fs_ok) cfg_log("LittleFS mount failed (presets/settings volatile)");

  // 4) Audio engine. The DAC channel and the synth (voices/effects + the RT-safe
  //    command queue) are created before producers exist; Task_Audio re-affirms
  //    them on CORE_AUDIO (idempotent) so DMA/DSP ownership lands on core 1.
  i2s_audio_init();
  synth_init();

  // 5) Input scanners' pin setup (button matrix + analog muxes).
  mux_init();
  matrix_init();

  // 6) MIDI I/O + the GUI config protocol over USB-CDC.
  midi_din_init();
  usb_midi_init();
  cfg_init();

  // 7) Sequencer (clear patterns, default tempo, STOPPED).
  seq_init();

  // 8) UI dispatcher + status LED + startup mode (reads g_settings).
  ui_init();

  // 9) Auto-load the last preset if the user pinned one (0xFF = none).
  if (g_settings.last_preset != 0xFF && presets_exists(g_settings.last_preset)) {
    if (presets_load(g_settings.last_preset)) cfg_log("auto-loaded last preset");
  }

  // 10) Spawn & pin every task. After this the device is fully live; control
  //     never returns to a meaningful loop().
  tasks_start();

  cfg_log("r00fu_synth boot complete");
}

void loop() {
  // Everything runs in pinned FreeRTOS tasks (system/tasks.cpp). Nothing to do
  // here — yield generously so the Arduino loopTask doesn't hog CORE_IO.
  vTaskDelay(pdMS_TO_TICKS(1000));
}
