// The Presto's start-up (board.h: board_init).
#include "pico/stdlib.h"
#include "board.h"

void board_init(void) {
    // The piezo's MOSFET: driven off. Left floating it picks up the WiFi chip's activity and buzzes.
    gpio_init(BOARD_AUDIO_PWM_PIN);
    gpio_set_dir(BOARD_AUDIO_PWM_PIN, GPIO_OUT);
    gpio_put(BOARD_AUDIO_PWM_PIN, 0);
}
