// griffin_preview.cpp — Read Griffin 1bpp bitmap + palette, render to PNG
//
// Build: g++ -std=c++23 -O2 -o griffin_preview griffin_preview.cpp
// Usage: griffin_preview <bitmap_file> <palette_file> <output.png>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

constexpr int WIDTH = 640;
constexpr int HEIGHT = 480;

struct RGB8 { uint8_t r, g, b; };

static RGB8 r3g3b2_to_rgb(uint8_t c) {
    unsigned r3 = (c >> 5) & 7;
    unsigned g3 = (c >> 2) & 7;
    unsigned b2 = c & 3;
    return {
        static_cast<uint8_t>(r3 * 255 / 7),
        static_cast<uint8_t>(g3 * 255 / 7),
        static_cast<uint8_t>(b2 * 255 / 3),
    };
}

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "Usage: %s <bitmap> <palette> <output.png>\n", argv[0]);
        return 1;
    }

    // Read bitmap
    std::vector<uint8_t> bitmap(WIDTH / 8 * HEIGHT);
    {
        std::ifstream f(argv[1], std::ios::binary);
        f.read(reinterpret_cast<char*>(bitmap.data()), bitmap.size());
        if (!f) { std::fprintf(stderr, "Failed to read bitmap\n"); return 1; }
    }

    // Read palette
    std::vector<uint8_t> palette(HEIGHT * 2);
    {
        std::ifstream f(argv[2], std::ios::binary);
        f.read(reinterpret_cast<char*>(palette.data()), palette.size());
        if (!f) { std::fprintf(stderr, "Failed to read palette\n"); return 1; }
    }

    // Render
    std::vector<uint8_t> pixels(WIDTH * HEIGHT * 3);
    for (int y = 0; y < HEIGHT; ++y) {
        RGB8 colA = r3g3b2_to_rgb(palette[y * 2 + 0]);
        RGB8 colB = r3g3b2_to_rgb(palette[y * 2 + 1]);

        for (int x = 0; x < WIDTH; ++x) {
            int byte_idx = y * (WIDTH / 8) + (x / 8);
            bool bit = (bitmap[byte_idx] >> (7 - (x % 8))) & 1;
            const RGB8& c = bit ? colA : colB;

            int px = (y * WIDTH + x) * 3;
            pixels[px + 0] = c.r;
            pixels[px + 1] = c.g;
            pixels[px + 2] = c.b;
        }
    }

    stbi_write_png(argv[3], WIDTH, HEIGHT, 3, pixels.data(), WIDTH * 3);
    std::fprintf(stderr, "Wrote %s\n", argv[3]);
    return 0;
}
