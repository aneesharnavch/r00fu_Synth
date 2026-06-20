/*
 * pcm5102.cpp — PCM5102A I2S stereo DAC output (the audio pacer).
 *
 * Uses the ESP-IDF 5 "new" I2S STD driver (driver/i2s_std.h), which is what
 * arduino-esp32 3.x (the core paired with this PlatformIO platform) ships.
 * One TX channel in master mode, Philips standard format, 16-bit/2-channel at
 * SAMPLE_RATE, on the BCK/LRCK/DOUT pins from config.h. No MCLK pin is wired —
 * the PCM5102A runs in software/PLL mode (SCK tied low on the board), so we set
 * the MCLK GPIO to "unused".
 *
 * i2s_write_block() is the ONLY blocking call in the audio path. It blocks the
 * caller (Task_Audio) inside i2s_channel_write() until the DMA ring has room,
 * which is precisely what paces the render loop to real time. Strictly speaking
 * i2s_channel_write() does take the channel's internal FreeRTOS mutex before
 * waiting on the DMA queue, but that mutex is UNCONTENDED: Task_Audio is the sole
 * owner/user of s_tx_chan and no other task/core ever calls any i2s_channel_* API
 * at runtime, so there is no priority inversion. Keep that invariant — never call
 * i2s_channel_*() from another task. No malloc, no Serial anywhere in here.
 *
 * See pcm5102.h for the contract.
 */
#include "pcm5102.h"

#include "driver/i2s_std.h"
#include "driver/gpio.h"

// ── Module-private handle to the TX channel ────────────────────────────────
// Single static handle; the audio task is the sole owner/user. portMAX_DELAY
// on the write is what turns the DMA ring into the audio clock.
static i2s_chan_handle_t s_tx_chan = nullptr;

bool i2s_audio_init() {
  if (s_tx_chan != nullptr) {
    return true;   // already installed (idempotent)
  }

  // 1) Allocate a TX channel in master role. Default DMA descriptor count/size
  //    give enough buffering to absorb scheduling jitter while keeping latency
  //    low; tune dma_frame_num to AUDIO_BLOCK_SIZE so one write == a few DMA
  //    buffers.
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num  = 6;                 // number of DMA descriptors
  chan_cfg.dma_frame_num = AUDIO_BLOCK_SIZE;  // frames per DMA buffer
  chan_cfg.auto_clear    = true;              // zero-fill on underrun (no glitch loop)

  if (i2s_new_channel(&chan_cfg, &s_tx_chan, nullptr) != ESP_OK) {
    s_tx_chan = nullptr;
    return false;
  }

  // 2) Standard (Philips I2S) mode: SAMPLE_RATE, 16-bit slots, stereo.
  i2s_std_config_t std_cfg = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                    I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,                 // PCM5102A SCK tied low on board
      .bclk = (gpio_num_t)I2S_BCK_PIN,
      .ws   = (gpio_num_t)I2S_LRCK_PIN,
      .dout = (gpio_num_t)I2S_DOUT_PIN,
      .din  = I2S_GPIO_UNUSED,                 // TX only
      .invert_flags = {
        .mclk_inv = false,
        .bclk_inv = false,
        .ws_inv   = false,
      },
    },
  };

  if (i2s_channel_init_std_mode(s_tx_chan, &std_cfg) != ESP_OK) {
    i2s_del_channel(s_tx_chan);
    s_tx_chan = nullptr;
    return false;
  }

  // 3) Enable the channel: clocks start, DMA begins consuming (auto_clear
  //    keeps it silent until we feed it).
  if (i2s_channel_enable(s_tx_chan) != ESP_OK) {
    i2s_del_channel(s_tx_chan);
    s_tx_chan = nullptr;
    return false;
  }

  return true;
}

size_t i2s_write_block(const int16_t* interleavedLR, size_t frames) {
  if (s_tx_chan == nullptr || interleavedLR == nullptr || frames == 0) {
    return 0;
  }

  // bytes = frames * 2 channels * 2 bytes/sample. Block (portMAX_DELAY) until
  // the DMA ring accepts everything — THIS is the audio clock. We never time
  // out: if we did, real-time pacing would be lost.
  const size_t bytes = frames * 2u * sizeof(int16_t);
  size_t written = 0;
  esp_err_t err = i2s_channel_write(s_tx_chan, interleavedLR, bytes,
                                    &written, portMAX_DELAY);
  if (err != ESP_OK) {
    return 0;
  }
  return written / (2u * sizeof(int16_t));   // bytes -> frames
}
