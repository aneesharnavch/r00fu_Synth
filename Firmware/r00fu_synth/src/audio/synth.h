/*
 * synth.h — polyphonic synth engine + the core0 -> core1 command handoff.
 *
 * ┌──────────────────────────────────────────────────────────────────────────┐
 * │ RT-SAFE HANDOFF (the heart of the dual-core split)                        │
 * │                                                                           │
 * │ Musical actions originate on CORE_IO (dispatcher in modes.cpp, the        │
 * │ sequencer, MIDI IN). They must NOT touch DSP state directly. Instead each │
 * │ synth_note_on/off/cc/pitchbend/program/set_param call PACKAGES a small    │
 * │ SynthCmd and pushes it onto a single fixed-size FreeRTOS queue            │
 * │ (g_synth_cmd_q, length SYNTH_CMD_QUEUE_LEN, no dynamic alloc).            │
 * │                                                                           │
 * │ The audio task (CORE_AUDIO) calls synth_render() once per block. At the   │
 * │ TOP of synth_render(), BEFORE any DSP, it DRAINS the queue with a         │
 * │ zero-timeout xQueueReceive loop and applies each command to voice state.  │
 * │ That is the ONLY place commands mutate DSP. The audio path therefore      │
 * │ never blocks (queue receive is non-blocking), never mallocs, never takes  │
 * │ a lock — the queue is the lock-free boundary. Producers use the           │
 * │ non-blocking xQueueSend (0 ticks); on overflow the command is dropped and │
 * │ counted (synth_cmd_dropped) rather than stalling CORE_IO.                 │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 * Engine: NUM_VOICES Voice instances (voice.h) with last-note/oldest voice
 * stealing, summed to mono, widened to stereo, then run through the effects
 * chain (effects.h) before i2s_write_block(). All under Task_Audio.
 *
 * Implemented in src/audio/synth.cpp.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "config.h"   // NUM_VOICES, AUDIO_BLOCK_SIZE, SAMPLE_RATE

// ── Command set crossing core0 -> core1 ────────────────────────────────────
enum SynthCmdType : uint8_t {
  SCMD_NOTE_ON = 0,   // a=note, b=velocity, (ch implicit single-timbre)
  SCMD_NOTE_OFF,      // a=note
  SCMD_CC,            // a=cc number, b=value 0..127
  SCMD_PITCHBEND,     // val=-8192..8191
  SCMD_PROGRAM,       // a=program 0..127
  SCMD_SET_PARAM,     // a=SynthParam id, val=raw 0..4095 (knob) or scaled
  SCMD_ALL_NOTES_OFF, // panic
};

// Continuous parameters addressable via synth_set_param / SCMD_SET_PARAM.
// Values arrive as 0..4095 (knob/slider domain) and are scaled inside the
// engine to musical ranges.
enum SynthParam : uint8_t {
  SP_OSC_A_WAVE = 0,
  SP_OSC_B_WAVE,
  SP_OSC_MIX,          // A<->B balance
  SP_NOISE_LEVEL,
  SP_DETUNE,
  SP_FILTER_CUTOFF,
  SP_FILTER_RES,
  SP_FILTER_ENV_AMT,
  SP_AMP_ATTACK, SP_AMP_DECAY, SP_AMP_SUSTAIN, SP_AMP_RELEASE,
  SP_LFO_RATE, SP_LFO_PITCH_DEPTH, SP_LFO_CUTOFF_DEPTH,
  SP_MASTER_VOLUME,
  SP_PARAM_COUNT,
};

// Packed command pushed through the queue. 8 bytes, trivially copyable.
struct SynthCmd {
  SynthCmdType type;
  uint8_t      a;     // note / cc / program / param id
  uint8_t      b;     // velocity / cc value
  uint8_t      _pad;
  int16_t      val;   // pitchbend / scaled param
};

#define SYNTH_CMD_QUEUE_LEN 64   // depth of the core0->core1 ring

// Create voices, effects, and the command queue. Call once from Task_Audio
// (CORE_AUDIO) before the render loop. Returns false on queue alloc failure.
bool synth_init();

// Render exactly `frames` interleaved L/R int16 samples into outLR
// (outLR length = frames*2). DRAINS the command queue first (see banner),
// then mixes voices + effects. Called by Task_Audio, never from CORE_IO.
void synth_render(int16_t* outLR, size_t frames);

// ── RT-safe producers (call from CORE_IO). Each enqueues one SynthCmd. ──────
// Return false if the command was dropped (queue full). channel is accepted
// for API symmetry but the built-in engine is single-timbre (channel ignored).
bool synth_note_on (uint8_t note, uint8_t velocity, uint8_t channel);
bool synth_note_off(uint8_t note, uint8_t velocity, uint8_t channel);
bool synth_cc      (uint8_t cc, uint8_t value, uint8_t channel);
bool synth_pitchbend(int16_t bend /*-8192..8191*/, uint8_t channel);
bool synth_program (uint8_t program, uint8_t channel);
bool synth_all_notes_off();

// Set a continuous parameter (0..4095 raw, scaled in-engine). Enqueues
// SCMD_SET_PARAM. False if dropped.
bool synth_set_param(SynthParam param, int16_t value);

// Diagnostics: commands dropped on full queue since boot.
uint32_t synth_cmd_dropped();
