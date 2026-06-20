/*
 * config.h — r00fu_synth hardware configuration (LOCKED PIN MAP)
 * Board: ESP32-S3-DevKitC-1  (the big dual-USB-C dev module)
 *
 * ┌─────────────────────────────────────────────────────────────────────┐
 * │ PSRAM NOTE — READ THIS                                               │
 * │ Mux select S0/S1/S2 reuse GPIO 35/36/37, which on this board are the │
 * │ OCTAL-PSRAM bus. We therefore ship with PSRAM DISABLED so the wiring │
 * │ you already have works unchanged (-DBOARD_HAS_PSRAM=0 in ini).       │
 * │ To enable PSRAM later: move S0->39, S1->47, S2->48 (3 jumpers) and   │
 * │ set the relocated pins below + enable OPI PSRAM in platformio.ini.   │
 * └─────────────────────────────────────────────────────────────────────┘
 */
#pragma once
#include <stdint.h>

// ───────────────────────── Button matrix (8x8) ──────────────────────────
#define MATRIX_ROWS 8
#define MATRIX_COLS 8
#define NUM_BUTTONS (MATRIX_ROWS * MATRIX_COLS)   // 64
// Drive one ROW low at a time, read COLUMNS with pull-ups. Per-key diodes
// give N-key rollover. Flip these two arrays if your diodes are reversed.
static const int ROW_PINS[MATRIX_ROWS] = { 4, 5, 6, 7, 15, 16, 17, 18 };
static const int COL_PINS[MATRIX_COLS] = { 8, 9, 10, 11, 12, 13, 14, 21 };

#define MATRIX_SCAN_INTERVAL_MS 1     // 1 ms scan (spec)
#define MATRIX_DEBOUNCE_COUNT   6     // ~6 ms stable before commit

// ───────────────────── Analog multiplexers (3x 74HC4067) ────────────────
#define MUX_COUNT     3
#define MUX_CHANNELS  16
// Shared 4-bit select bus (S0..S3). GPIO38 is free; 35/36/37 are free only
// because PSRAM is disabled (see banner above).
static const int MUX_SEL_PINS[4] = { 35, 36, 37, 38 };   // S0,S1,S2,S3
#define MUX_A_OUT 1   // ADC1_CH0  -> knobs  0..15
#define MUX_B_OUT 2   // ADC1_CH1  -> knobs 16..31
#define MUX_C_OUT 3   // ADC1_CH2  -> sliders 0..15
#define MUX_SETTLE_US 3                // settle time after channel switch
#define ANALOG_SCAN_INTERVAL_MS 5      // 5 ms (spec)
#define ANALOG_SMOOTHING 0.10f         // filtered = 0.9*prev + 0.1*cur

#define NUM_KNOBS    25                // first 25 of mux A+B (0..31 read)
#define NUM_SLIDERS  15                // first 15 of mux C
#define ADC_RESOLUTION_BITS 12         // 0..4095

// ───────────────────────── I2S DAC (PCM5102A) ───────────────────────────
#define I2S_BCK_PIN   40
#define I2S_LRCK_PIN  41
#define I2S_DOUT_PIN  42
#define SAMPLE_RATE   44100
#define I2S_BITS      16
#define AUDIO_BLOCK_SIZE 64            // frames per audio render block
#define NUM_VOICES    8                // 8-voice polyphony (spec)

// ───────────────────────────── DIN MIDI ─────────────────────────────────
// On UART1 via the GPIO matrix. These are U0TX/U0RX physically and are also
// wired to the on-board USB-UART bridge; use the *native USB* port for data
// so the bridge stays idle. See SETUP.md "Two USB ports".
#define MIDI_TX_PIN   43              // DIN OUT
#define MIDI_RX_PIN   44              // DIN IN  (from 6N138/H11L1 opto)
#define MIDI_BAUD     31250
#define MIDI_UART_NUM 1

// ───────────────────────── Status RGB LED ───────────────────────────────
#define STATUS_LED_PIN 48            // on-board WS2812 on DevKitC-1
#define STATUS_LED_COUNT 1

// ───────────────────────── FreeRTOS task layout ─────────────────────────
#define CORE_IO    0                 // scanners, MIDI, UI, sequencer
#define CORE_AUDIO 1                 // audio only, highest priority
#define PRIO_AUDIO      (configMAX_PRIORITIES - 1)
#define PRIO_BUTTONSCAN 5
#define PRIO_MUXSCAN    4
#define PRIO_MIDI       6
#define PRIO_SEQUENCER  5
#define PRIO_UI         3

// ───────────────────────── Storage / presets ────────────────────────────
#define PRESET_DIR "/presets"
#define SETTINGS_FILE "/settings.json"

// Helper: row,col -> linear button index used everywhere (and by the GUI)
static inline uint8_t btn_index(uint8_t row, uint8_t col) { return row * MATRIX_COLS + col; }
