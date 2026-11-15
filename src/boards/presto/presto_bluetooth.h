#pragma once
#include <stddef.h>
#include <stdbool.h>
void presto_bluetooth_start(void);
void presto_bluetooth_json(char *out, size_t size);
int presto_bluetooth_command(const char *command);

void presto_bluetooth_frame(void);
void presto_bluetooth_boot_poll(void);
const char *presto_bluetooth_boot_status(void);

void presto_bluetooth_prepare_restart(void);
bool presto_bluetooth_restart_done(void);

const char *presto_keyboard_boot_status(void);
