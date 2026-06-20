/*
 * midi_map.h — per-button function mapping.
 * MUST stay in lock-step with gui/r00fu_config.py (TYPES dict) and the
 * BTN/MAP serial protocol in config_protocol.cpp.
 */
#pragma once
#include <stdint.h>
#include "config.h"

enum MapType : uint8_t {
  MAP_NONE      = 0,
  MAP_NOTE      = 1,   // value=note,    channel used
  MAP_CC        = 2,   // value=cc num,  channel used
  MAP_PROGRAM   = 3,   // value=program, channel used
  MAP_TRANSPORT = 4,   // value=0 Play,1 Stop,2 Record,3 Continue,4 TapTempo
  MAP_MODE      = 5,   // value=0 Drum,1 StepSeq,2 Keyboard,3 DAW,4 Performance
};

struct ButtonMap {
  uint8_t type;      // MapType
  uint8_t channel;   // 1..16
  uint8_t value;     // note/cc/program/transport-id/mode-id
  char    label[16];
};

// Global table, indexed 0..63 (== btn_index(row,col)). Defined in midi_map.cpp.
extern ButtonMap g_button_map[NUM_BUTTONS];

void midi_map_init();                       // load defaults / from flash
void midi_map_set(uint8_t idx, uint8_t type, uint8_t ch, uint8_t val, const char* label);
const ButtonMap& midi_map_get(uint8_t idx);
bool midi_map_save();                        // -> LittleFS
bool midi_map_load();                        // <- LittleFS
