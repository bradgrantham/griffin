
#pragma once

#include "splash_write.h"
#include "splash.h"

void write_splash(uint32_t address) {
    uint16_t *src = splash_bitmap;
    volatile uint16_t * const dst = static_cast<uint16_t *>(address);
    int count = SPLASH_WORDS;

    while (count--) {
        *dst = *src++;
    }
}

