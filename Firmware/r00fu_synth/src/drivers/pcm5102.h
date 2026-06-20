/*
 * pcm5102.h — PCM5102A I2S stereo DAC output (the only thing that paces audio).
 *
 * Configures the ESP32-S3 I2S peripheral in master TX mode at SAMPLE_RATE,
 * I2S_BITS, 2 channels on BCK/LRCK/DOUT (config.h). i2s_write_block() is the
 * single blocking call in the entire audio path: it blocks ONLY on the DMA
 * ring having room, which is exactly what clocks the render loop. No malloc,
 * no locks, no other blocking allowed around it (HARD RULE).
 *
 * Implemented in src/drivers/pcm5102.cpp. Used exclusively by Task_Audio
 * (CORE_AUDIO, PRIO_AUDIO).
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "config.h"   // I2S_* pins, SAMPLE_RATE, I2S_BITS, AUDIO_BLOCK_SIZE

// Install and start the I2S TX channel + DMA ring. Call once at the top of
// Task_Audio, before the render loop. Returns false if the driver failed to
// install (caller should surface an error and not enter the loop).
bool i2s_audio_init();

// Push one interleaved L/R block (frames*2 int16 samples) to the DAC. Blocks
// only until the DMA ring accepts the bytes — this is the audio clock. Returns
// the number of FRAMES actually written (== frames on success). Call once per
// rendered block from Task_Audio. Never call from CORE_IO.
size_t i2s_write_block(const int16_t* interleavedLR, size_t frames);
