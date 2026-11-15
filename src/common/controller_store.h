#pragma once
#include <stdbool.h>
#include <stdint.h>
void controller_store_init(void);
bool controller_store_bond(uint8_t address[6],uint8_t key[16],uint8_t *type);
void controller_store_set_bond(const uint8_t address[6],const uint8_t key[16],uint8_t type);
void controller_store_forget(void);
bool controller_store_get_config(unsigned machine,int32_t config[19]);
void controller_store_set_config(unsigned machine,const int32_t config[19]);
bool controller_store_flush(void);
bool controller_store_pending(void);

bool controller_store_get_preferences(unsigned machine,uint8_t out[5]);
void controller_store_set_preferences(unsigned machine,const uint8_t values[5]);

bool keyboard_store_bond(uint8_t address[6],uint8_t key[16],uint8_t *type);
void keyboard_store_set_bond(const uint8_t address[6],const uint8_t key[16],uint8_t type);
void keyboard_store_forget(void);

#define KEYBOARD_MAP_BYTES 64
void keyboard_store_get_map(unsigned machine,uint8_t out[KEYBOARD_MAP_BYTES]);
void keyboard_store_set_map(unsigned machine,const uint8_t map[KEYBOARD_MAP_BYTES]);
