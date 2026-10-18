// The Presto's start-up (board.h: board_init).
#include "pico/stdlib.h"
#include "board.h"

void board_init(void) {
    // The piezo's MOSFET: driven off (until the sound takes it, if it's the audio out). Left
    // floating it picks up the WiFi chip's activity and buzzes.
    gpio_init(BOARD_PIEZO_PIN);
    gpio_set_dir(BOARD_PIEZO_PIN, GPIO_OUT);
    gpio_put(BOARD_PIEZO_PIN, 0);
}
