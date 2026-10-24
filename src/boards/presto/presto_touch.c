// The Presto's touch screen (an FT6236 on I2C1: SDA GP30, SCL GP31, interrupt GP32, address 0x48),
// for its own screens (presto_screen.h), never the machine's:
//   a tap on a screen that's up: its button (presto_net.c, net_screen_tap)
//   a finger held still in the top-left corner for 3 seconds: the web panel's address
//   (net_show_address)
//   a finger held on the screen as it starts: WiFi setup (presto_net.c, touch_held_us)
// The touch reports panel coordinates, (0, 0) at the top left. It's read only while touched (the
// controller pulls its interrupt low), so it costs nothing otherwise.
#include <stdlib.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "presto_touch.h"
#include "presto_net.h"

#define TOUCH_I2C i2c1
#define TOUCH_SDA 30
#define TOUCH_SCL 31
#define TOUCH_INT 32
#define TOUCH_ADDR 0x48

#define STILL_PX 6              // moved less than this: still
#define CORNER_PX 64            // a finger held still this near the top-left corner
#define CARD_US 3000000         // for this long: the web panel's address
#define PRESS_PX 40             // a button press may move this far

static bool down, moved, done, for_screen;
static int last_x, last_y, start_x, start_y;
static uint32_t start_us;
static int errors;

void touch_init(void) {
    i2c_init(TOUCH_I2C, 400000);
    gpio_set_function(TOUCH_SDA, GPIO_FUNC_I2C);
    gpio_set_function(TOUCH_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(TOUCH_SDA);
    gpio_pull_up(TOUCH_SCL);
    gpio_init(TOUCH_INT);
    gpio_set_dir(TOUCH_INT, GPIO_IN);
    gpio_pull_up(TOUCH_INT);
}

uint32_t touch_held_us(void) {
    return down ? time_us_32() - start_us : 0;
}

void touch_poll(void) {
    // the controller pulls its interrupt low while touched; read it then, and once after
    if (gpio_get(TOUCH_INT) && !down) return;
    if (errors > 20) return;  // not answering: leave it alone
    uint8_t reg = 0, buf[15];
    if (i2c_write_timeout_us(TOUCH_I2C, TOUCH_ADDR, &reg, 1, true, 2000) != 1 ||
        i2c_read_timeout_us(TOUCH_I2C, TOUCH_ADDR, buf, sizeof buf, false, 4000) != (int) sizeof buf) {
        errors++;
        return;
    }
    errors = 0;
    int touches = buf[2] & 0x0f;
    int event = buf[3] >> 6;   // 0 down, 1 up, 2 contact
    int x = ((buf[3] & 0x0f) << 8) | buf[4], y = ((buf[5] & 0x0f) << 8) | buf[6];
    uint32_t now = time_us_32();
    bool touching = touches > 0 && event != 1 && event != 3;
    if (touching && !down) {
        down = true;
        moved = done = false;
        start_x = last_x = x;
        start_y = last_y = y;
        start_us = now;
        for_screen = net_screen_up();
        return;
    }
    if (touching) {
        if (abs(x - start_x) > STILL_PX || abs(y - start_y) > STILL_PX) moved = true;
        last_x = x;
        last_y = y;
        // held still in the top-left corner: the web panel's address on screen
        if (!for_screen && !done && !moved && touches == 1 && start_x < CORNER_PX && start_y < CORNER_PX &&
            now - start_us > CARD_US) {
            done = true;    // (the rest of this touch is the card's)
            net_show_address(20);
        }
        return;
    }
    if (!down) return;
    // lifted
    down = false;
    if (for_screen && abs(last_x - start_x) < PRESS_PX && abs(last_y - start_y) < PRESS_PX && net_screen_up())
        net_screen_tap(start_x, start_y);
}
