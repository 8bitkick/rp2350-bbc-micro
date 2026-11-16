#pragma once
#include <stdbool.h>
#include <stdint.h>
// First released level arms the button. A held button at startup does nothing.
typedef struct { bool initialized, raw, stable, armed; uint32_t changed_ms; } boot_button_t;
static inline bool boot_button_press(boot_button_t *b, bool down, uint32_t now) {
    if (!b->initialized) {
        b->initialized = true; b->raw = b->stable = down;
        b->armed = !down; b->changed_ms = now;
        return false;
    }
    if (down != b->raw) { b->raw = down; b->changed_ms = now; }
    if (b->stable == b->raw || (uint32_t)(now - b->changed_ms) < 40) return false;
    b->stable = b->raw;
    if (!b->stable) { b->armed = true; return false; }
    if (!b->armed) return false;
    b->armed = false;
    return true;
}
