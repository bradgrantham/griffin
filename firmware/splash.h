#pragma once
#include <cstdint>

// 1bpp splash image, packed MSB-left, 16 pixels per uint16_t.
// Caller guarantees SPLASH_WIDTH is a multiple of 16.
inline constexpr int SPLASH_WIDTH  = 64;
inline constexpr int SPLASH_HEIGHT = 64;
inline constexpr int SPLASH_WORDS  = (SPLASH_WIDTH / 16) * SPLASH_HEIGHT;

extern const uint16_t splash_bitmap[SPLASH_WORDS];

// Blit the splash into the top-left of a 1bpp linear framebuffer.
// `pitch_bytes` is the framebuffer row stride in bytes.
void splash_blit_topleft(uint8_t* fb, unsigned pitch_bytes);

// How many text rows the splash occupies, for a given font cell height.
// Used by the textport console to home the cursor below the splash.
constexpr unsigned splash_rows_for_font_height(unsigned font_h)
{
    return (static_cast<unsigned>(SPLASH_HEIGHT) + font_h - 1) / font_h;
}
