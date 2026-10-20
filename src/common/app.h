#ifndef APP_H
#define APP_H

#include "machine_hook.h"

#ifdef __cplusplus
extern "C" {
#endif

// The app run alongside the machine (app.c, app_api.h).

// At power on, before the machine's boot image and disc: find the app (built in, or in the app
// slot) and check it for this machine (enum machine_model). The machine's built-in disc in
// *disc, *disc_size (NULL, 0: none): an app that carries its own puts that there instead.
void app_load(uint32_t model, const uint8_t **disc, uint32_t *disc_size);
// Between frames, on core 0: the frame count on, and the app's frame (its init, the first time)
// with what the machine has filled in (version, model, RAM, key, analogue; resets as it resets).
void app_frame(struct machine_hook_io *io);
// a command for the app (true: taken, its answer in reply); its panel (NULL: none); its loading
// screen (NULL: none)
bool app_command(const char *cmd, char *reply, uint32_t reply_size);
const char *app_program_json(void);
const struct machine_boot_image *app_boot_image(void);
// what it is: {"state": none|loaded|running|refused|off|stopped, "why", "name", "version",
// "machine", "from": slot|built in} and, full, "disc", "ram" and "flash" ([used, size]) and its
// "log"; a line of text ("APP running elite-agent ..."); the app slot's bytes in use
void app_json(char *out, uint32_t size, bool full);
void app_text(char *out, uint32_t size);
uint32_t app_flash_used(void);
// Installing an app into the slot, as it arrives (POST /api/app; "!app install <size>" over USB:
// app_usb_install): begin with its size (false: refused, why), its bytes in order (false: more
// than it said, or the flash failed), then end (false: it isn't a good app, why; the slot is left
// empty either way) or abort (the upload went). The app running stops at begin; after a good end
// the board restarts into the new one. app_remove erases it.
bool app_install_begin(uint32_t size, char *why, uint32_t why_size);
bool app_install_data(const uint8_t *p, uint32_t n);
bool app_install_end(char *why, uint32_t why_size);
void app_install_abort(const char *why);
void app_usb_install(const char *args);
bool app_remove(void);
// let an app turned off by the app guard run again, from the next boot
bool app_turn_on(void);

#ifdef __cplusplus
}
#endif

#endif
