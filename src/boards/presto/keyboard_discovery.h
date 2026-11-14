#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
bool keyboard_discovery_busy(void);
int keyboard_discovery_start(void);
void keyboard_discovery_stop(void);
void keyboard_discovery_frame(void);
bool keyboard_discovery_event(uint8_t *packet, uint16_t size);
void keyboard_discovery_json(char *out, size_t size);
