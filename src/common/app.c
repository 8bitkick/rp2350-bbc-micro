// A program built in alongside the machine (machine_hook.h): what's common to the machines.
//
// The hooks are weak here, doing nothing unless a build supplies them (-DAPP_SOURCES=...,
// first called MACHINE_EXTRA_SOURCES). Each machine fills in a machine_hook_io once (its model,
// its RAM, a key and an analogue function) and calls app_frame with it between frames; the
// command layer passes on commands it doesn't know (api.c: command; frontend.c: the USB
// channel's) and shows machine_program_json as the web page's program panel.
#include <stddef.h>

#include "app.h"

__attribute__((weak)) void machine_frame_hook(const struct machine_hook_io *io) { (void) io; }
__attribute__((weak)) bool machine_command_hook(const char *cmd, char *reply, uint32_t reply_size) {
    (void) cmd; (void) reply; (void) reply_size;
    return false;
}
__attribute__((weak)) const char *machine_program_json(void) { return NULL; }
__attribute__((weak)) const struct machine_boot_image *machine_boot_image(void) { return NULL; }

void app_frame(struct machine_hook_io *io) {
    io->frame++;
    machine_frame_hook(io);
}
