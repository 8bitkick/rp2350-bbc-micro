#pragma once
/* Real SDK event accessors, mocked radio operations. */
#include "btstack_defines.h"
#include "btstack_event.h"
int gap_inquiry_start(uint8_t length);
int gap_inquiry_stop(void);
