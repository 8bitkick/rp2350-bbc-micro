// The front end every machine shares (frontend.h): the USB channel, the network, queued keys.
//
// The USB channel (tools/presto.py speaks it):
//   0x1a 0x00 <SDL scancode>   key down
//   0x1a 0x01 <SDL scancode>   key up
//   !<command> [args]          a command of the command layer (api.c), its text reply:
//                              status, stats, keys, turbo, overlay, boot, break, eject, discs,
//                              sd, restore, net, wifi, ambient, program, command
//   !api <command> [args]      the same, its JSON reply (as the HTTP API's): "API <code> <json>"
//   !shot [ms]                 SHOT <w> <h>, then the panel's rows as RGB565 (then hold the
//                              machine still ms, for a camera to take the same frame)
//   !put <name> <size>         receive a disc image and write it to the SD card
//   !temp, !bootsel            the chip's temperature; reboot to the USB bootloader
//   !audio, !beep              (SOUND) the sound's state, test sounds
// and the first spellings, still taken: !disc (eject), !disc restore (restore), !disc <name> (sd),
// !perf (overlay), !leds (ambient), !wifi <ssid> <password>. Anything else goes to the machine
// (machine_usb_command: the Model B's crtc, state, peek, ...), then to a program built in
// alongside (machine_command_hook).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/adc.h"
#include "hardware/watchdog.h"

#include "frontend.h"
#include "api.h"
#include "machine_if.h"
#include "machine_hook.h"
#include "presto_boot.h"
#include "presto_lcd.h"
#include "presto_sd.h"
#if BOARD_HAS_NET
#include "net_if.h"
#endif
#if WITH_SOUND
#include "presto_audio.h"
#endif

// In hundredths of a degree (RP2350 datasheet: 27C at 0.706V, -1.721mV/C).
int frontend_temp_centi(void) {
    static bool s_adc_ready;
    if (!s_adc_ready) {
        adc_init();
        adc_set_temp_sensor_enabled(true);
        s_adc_ready = true;
    }
    adc_select_input(ADC_TEMPERATURE_CHANNEL_NUM);
    uint32_t sum = 0;
    for (int i = 0; i < 64; i++) sum += adc_read();
    int32_t uv = (int32_t) (((uint64_t) sum * 3300000u) / (64 * 4096));
    return 2700 - (int) (((int64_t) (uv - 706000) * 100) / 1721);
}

// ---- The USB channel

static void binary_out(const void *data, uint32_t len) {
    stdio_set_translate_crlf(&stdio_usb, false);
    (void) fwrite(data, 1, len, stdout);
    (void) fflush(stdout);
    stdio_set_translate_crlf(&stdio_usb, true);
}

// the panel's rows as shown, RGB565, big-endian
static void shot(const char *args) {
    static uint16_t s_out[PRESTO_LCD_WIDTH];
    unsigned hold_ms = 0;
    (void) sscanf(args, "%u", &hold_ms);
    printf("SHOT %d %d\n", PRESTO_LCD_WIDTH, PRESTO_LCD_HEIGHT);
    fflush(stdout);
    for (int y = 0; y < PRESTO_LCD_HEIGHT; y++) {
        const uint16_t *src = presto_lcd_row(y, s_out);
        for (int x = 0; x < PRESTO_LCD_WIDTH; x++) s_out[x] = __builtin_bswap16(src[x]);
        binary_out(s_out, PRESTO_LCD_WIDTH * sizeof(uint16_t));
    }
    if (hold_ms > 10000) hold_ms = 10000;
    while (hold_ms > 0) {
        unsigned n = hold_ms > 100 ? 100 : hold_ms;
        sleep_ms(n);
        hold_ms -= n;
        watchdog_update();
    }
}

static bool is(const char *cmd, const char *name) {
    size_t n = strlen(name);
    return !strncmp(cmd, name, n) && (cmd[n] == 0 || cmd[n] == ' ');
}

static const char *after(const char *cmd, const char *name) {
    const char *a = cmd + strlen(name);
    while (*a == ' ') a++;
    return a;
}

static void text_call(const char *name, const char *args) {
    api_req_t r = {.args = args, .json = false};
    api_call(name, &r);
}

static void usb_command(char *cmd) {
    if (is(cmd, "api")) {
        // "!api <name> [args]": the JSON reply, as over HTTP
        static char out[2048];
        char name[24];
        const char *a = after(cmd, "api");
        size_t n = strcspn(a, " ");
        if (n >= sizeof name) n = sizeof name - 1;
        memcpy(name, a, n);
        name[n] = 0;
        api_req_t r = {.args = after(a, name), .json = true, .out = out, .size = sizeof out};
        if (!api_call(name, &r)) printf("API 404 {\"error\":\"no such command\"}\n");
        else printf("API %d %s\n", r.code, out);
        return;
    }
    if (is(cmd, "shot")) {
        shot(after(cmd, "shot"));
    } else if (is(cmd, "bootsel")) {
        printf("BOOTSEL\n");
        fflush(stdout);
        sleep_ms(100);
        reset_usb_boot(0, 0);
    } else if (is(cmd, "put")) {
        presto_sd_put(after(cmd, "put"));
    } else if (is(cmd, "temp")) {
        int centi = frontend_temp_centi();
        printf("TEMP %d.%02dC\n", centi / 100, centi % 100);
#if WITH_SOUND
    } else if (is(cmd, "audio")) {
        presto_audio_command(after(cmd, "audio"));
    } else if (is(cmd, "beep")) {
        presto_audio_test(after(cmd, "beep"));
#endif
    // the first spellings
    } else if (!strcmp(cmd, "disc")) {
        text_call("eject", "");
    } else if (!strcmp(cmd, "disc restore")) {
        text_call("restore", "");
    } else if (is(cmd, "disc")) {
        const char *name = after(cmd, "disc");
        if (!presto_sd_load_disc(name)) printf("DISC error: %s not loaded\n", name);
    } else if (!strcmp(cmd, "perf")) {
        text_call("overlay", "");
    } else if (is(cmd, "leds")) {
        text_call("ambient", after(cmd, "leds"));
    } else {
        char name[24];
        size_t n = strcspn(cmd, " ");
        if (n < sizeof name) {
            memcpy(name, cmd, n);
            name[n] = 0;
            api_req_t r = {.args = after(cmd, name), .json = false};
            if (api_call(name, &r)) return;
        }
        if (machine_usb_command(cmd)) return;
        char reply[256] = "";
        if (machine_command_hook(cmd, reply, sizeof reply)) {
            if (reply[0]) printf("%s\n", reply);
            return;
        }
        printf("ERR unknown command '%s'\n", cmd);
    }
}

static void usb_poll(void) {
#if !FRONTEND_NO_USB_COMMANDS
    static char s_cmd[128];
    static int s_cmd_len = -1;  // -1 when not in a command
    static int s_key_state;     // 1 after 0x1a, 2 / 3 awaiting a scancode
    int c;
    while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
        if (s_key_state == 1) {
            s_key_state = (c == 0) ? 2 : ((c == 1) ? 3 : 0);
        } else if (s_key_state >= 2) {
            machine_key_sdl((uint32_t) c, s_key_state == 2);
            s_key_state = 0;
        } else if (c == 0x1a) {
            s_key_state = 1;
        } else if (c == '!' && s_cmd_len < 0) {
            // (not within a command: a WiFi password may hold a '!')
            s_cmd_len = 0;
        } else if (s_cmd_len >= 0) {
            if (c == '\n' || c == '\r') {
                s_cmd[s_cmd_len] = 0;
                s_cmd_len = -1;
                usb_command(s_cmd);
            } else if (s_cmd_len < (int) sizeof s_cmd - 1) {
                s_cmd[s_cmd_len++] = (char) c;
            }
        }
    }
#endif
}

// ---- Each frame and in between

void frontend_init(void) {
#if BOARD_HAS_NET
    net_init();
#endif
}

void frontend_poll(void) {
    usb_poll();
#if BOARD_HAS_NET
    net_poll();
#endif
}

void frontend_frame(void) {
    static unsigned frames;
    if (frames < 100 && ++frames == 100) presto_boot_ok();     // running: disarm the boot guard (presto_boot.c)
    usb_poll();
    api_frame();
#if BOARD_HAS_NET
    net_frame();
#endif
}
