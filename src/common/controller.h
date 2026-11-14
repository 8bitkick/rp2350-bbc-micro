#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void controller_keyboard_map_set(const uint8_t map[64]);
void controller_keyboard_map_get(uint8_t map[64]);
void controller_reset(void);
void controller_bluetooth_keyboard(const uint8_t usages[32]);
void controller_frame(void);
void controller_input(bool connected, const int16_t axes[4], unsigned directions, const int32_t buttons[8], bool menu);
int controller_config(const char *text);
void controller_get_config(int32_t out[19]);
void controller_json(char *out,size_t size);
int16_t controller_normalize(int32_t value,int32_t minimum,int32_t maximum);
void machine_controller(unsigned port,uint16_t x,uint16_t y,unsigned directions,bool fire);


// UI captures mapped directions/fire without delivering them to the emulator.
bool controller_menu_state(unsigned *directions, bool *fire);
void controller_menu_capture(bool capture);

#ifdef __cplusplus
}
#endif
