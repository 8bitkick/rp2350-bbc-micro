#pragma once
// Power-on only, before emulation and HTTP uploads start.
void boot_banner_show(void);

#include <stdbool.h>
void boot_status_frame(void);
bool boot_status_visible(void);
