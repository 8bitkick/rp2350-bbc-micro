#pragma once
#include "btstack.h"
bool keyboard_host_event(uint8_t *packet,uint16_t size);
bool keyboard_host_save_key(bd_addr_t address,link_key_t key,link_key_type_t type);
bool keyboard_host_delete_key(bd_addr_t address);
void keyboard_host_init(void);
void keyboard_host_frame(bool radio_available,bool deliver_input);
bool keyboard_host_busy(void);
int keyboard_host_command(const char *command,bool available);
void keyboard_host_json(char *out,size_t size);

const char *keyboard_host_boot_status(void);
