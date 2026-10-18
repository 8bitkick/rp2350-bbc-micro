#ifndef APP_H
#define APP_H

#include "machine_hook.h"

#ifdef __cplusplus
extern "C" {
#endif

// Between frames, on core 0: the frame count on, and the program's frame hook (machine_hook.h)
// with what the machine has filled in (version, model, RAM, key, analogue; resets as it resets).
void app_frame(struct machine_hook_io *io);

#ifdef __cplusplus
}
#endif

#endif
