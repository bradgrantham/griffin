// clut_prepare.cpp — reference image production for the CLUT experiment
//
// Build: see the Makefile in this directory.
// Usage: clut_prepare <in.ppm|in.png> <out.png>
//        clut_prepare --synth <out.png>
//
// The first form scales the source to cover 640x480 with the aspect ratio
// preserved, centre-crops it, and writes 8-bit sRGB with no gamma games: a box
// filter over the exact fractional source rectangle of every destination pixel,
// which for the reference photographs is always a downscale and therefore a
// proper area average.  The result is the one reference both microham_encode
// and clut_encode are fed, so every number in the sweep is against the same
// pixels.
//
// --synth writes the gate image of DESIGN.md §6: 640x480, at most 16 distinct
// colours on any line, every colour exactly representable in R4G4B4, and enough
// single-pixel detail that a mode which blurs or drifts cannot pass.

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image.h"
#include "stb/stb_image_write.h"

#include "clut_format.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{

using Clut::HEIGHT;
using Clut::OUT_WIDTH;

// Overlap of [a,b) with the unit cell [i,i+1), for the box filter.
double overlap(double a, double b, int i)
{
    const double lo = (a > static_cast<double>(i)) ? a : static_cast<double>(i);
    const double hi = (b < static_cast<double>(i + 1)) ? b : static_cast<double>(i + 1);
    return (hi > lo) ? (hi - lo) : 0.0;
}

void box_resample(const unsigned char *src, int src_w, int src_h, double crop_x, double crop_y,
                  double crop_w, double crop_h, std::vector<unsigned char> &dst)
{
    dst.assign(static_cast<size_t>(OUT_WIDTH) * HEIGHT * 3, 0);

    for (int dy = 0; dy < HEIGHT; dy++)
    {
        const double y0 = crop_y + crop_h * static_cast<double>(dy) / HEIGHT;
        const double y1 = crop_y + crop_h * static_cast<double>(dy + 1) / HEIGHT;
        const int    iy0 = static_cast<int>(std::floor(y0));
        const int    iy1 = static_cast<int>(std::ceil(y1));

        for (int dx = 0; dx < OUT_WIDTH; dx++)
        {
            const double x0  = crop_x + crop_w * static_cast<double>(dx) / OUT_WIDTH;
            const double x1  = crop_x + crop_w * static_cast<double>(dx + 1) / OUT_WIDTH;
            const int    ix0 = static_cast<int>(std::floor(x0));
            const int    ix1 = static_cast<int>(std::ceil(x1));

            double sum[3] = { 0.0, 0.0, 0.0 };
            double weight = 0.0;
            for (int sy = iy0; sy < iy1; sy++)
            {
                if (sy < 0 || sy >= src_h)
                {
                    continue;
                }
                const double wy = overlap(y0, y1, sy);
                if (wy <= 0.0)
                {
                    continue;
                }
                for (int sx = ix0; sx < ix1; sx++)
                {
                    if (sx < 0 || sx >= src_w)
                    {
                        continue;
                    }
                    const double w = wy * overlap(x0, x1, sx);
                    if (w <= 0.0)
                    {
                        continue;
                    }
                    const size_t base = (static_cast<size_t>(sy) * src_w + sx) * 3;
                    for (int i = 0; i < 3; i++)
                    {
                        sum[i] += w * src[base + i];
                    }
                    weight += w;
                }
            }

            const size_t out_base = (static_cast<size_t>(dy) * OUT_WIDTH + dx) * 3;
            for (int i = 0; i < 3; i++)
            {
                const double v = (weight > 0.0) ? (sum[i] / weight) : 0.0;
                dst[out_base + i] = static_cast<unsigned char>(Clut::clamp_8(
                    static_cast<int>(std::lround(v))));
            }
        }
    }
}

uint32_t next_random(uint32_t &state)
{
    state = state * 1664525u + 1013904223u;
    return state >> 8;
}

void make_synthetic(std::vector<unsigned char> &dst)
{
    dst.assign(static_cast<size_t>(OUT_WIDTH) * HEIGHT * 3, 0);

    for (int y = 0; y < HEIGHT; y++)
    {
        uint32_t          state = 0x1234u + static_cast<uint32_t>(y) * 2654435761u;
        Clut::Rgb444      palette[Clut::CLUT_SIZE];
        for (int i = 0; i < Clut::CLUT_SIZE; i++)
        {
            palette[i] = Clut::rgb444(static_cast<int>(next_random(state) % 16),
                                      static_cast<int>(next_random(state) % 16),
                                      static_cast<int>(next_random(state) % 16));
        }

        int x = 0;
        while (x < OUT_WIDTH)
        {
            // Runs from a single pixel up to 40, so cell-based and doubled modes
            // both have something they cannot represent.
            int run = 1 + static_cast<int>(next_random(state) % 40);
            if (x + run > OUT_WIDTH)
            {
                run = OUT_WIDTH - x;
            }
            const Clut::Rgb444 c = palette[next_random(state) % Clut::CLUT_SIZE];
            for (int i = 0; i < run; i++)
            {
                const size_t base = (static_cast<size_t>(y) * OUT_WIDTH + x + i) * 3;
                dst[base + 0] = Clut::nibble_to_8(Clut::rgb444_r(c));
                dst[base + 1] = Clut::nibble_to_8(Clut::rgb444_g(c));
                dst[base + 2] = Clut::nibble_to_8(Clut::rgb444_b(c));
            }
            x += run;
        }
    }
}

}  // namespace

int main(int argc, char **argv)
{
    std::vector<unsigned char> out;

    if (argc == 3 && std::strcmp(argv[1], "--synth") == 0)
    {
        make_synthetic(out);
        if (stbi_write_png(argv[2], OUT_WIDTH, HEIGHT, 3, out.data(), OUT_WIDTH * 3) == 0)
        {
            std::fprintf(stderr, "%s: cannot write %s\n", argv[0], argv[2]);
            return 1;
        }
        std::printf("%s: synthetic gate image, 640x480, <=16 colours per line\n", argv[2]);
        return 0;
    }

    if (argc != 3)
    {
        std::fprintf(stderr, "Usage: %s <in.ppm|in.png> <out.png>\n       %s --synth <out.png>\n",
                     argv[0], argv[0]);
        return 1;
    }

    int            width  = 0;
    int            height = 0;
    int            comps  = 0;
    unsigned char *image  = stbi_load(argv[1], &width, &height, &comps, 3);
    if (image == nullptr)
    {
        std::fprintf(stderr, "%s: cannot read %s (%s)\n", argv[0], argv[1], stbi_failure_reason());
        return 1;
    }

    // Cover 640x480: crop the source to 4:3 about its centre, then area-average.
    double crop_w = width;
    double crop_h = height;
    if (static_cast<double>(width) * HEIGHT > static_cast<double>(height) * OUT_WIDTH)
    {
        crop_w = static_cast<double>(height) * OUT_WIDTH / HEIGHT;
    }
    else
    {
        crop_h = static_cast<double>(width) * HEIGHT / OUT_WIDTH;
    }
    const double crop_x = (static_cast<double>(width) - crop_w) / 2.0;
    const double crop_y = (static_cast<double>(height) - crop_h) / 2.0;

    box_resample(image, width, height, crop_x, crop_y, crop_w, crop_h, out);
    stbi_image_free(image);

    if (stbi_write_png(argv[2], OUT_WIDTH, HEIGHT, 3, out.data(), OUT_WIDTH * 3) == 0)
    {
        std::fprintf(stderr, "%s: cannot write %s\n", argv[0], argv[2]);
        return 1;
    }
    std::printf("%s: %dx%d -> 640x480 (crop %.0fx%.0f at %.0f,%.0f)\n", argv[2], width, height,
                crop_w, crop_h, crop_x, crop_y);
    return 0;
}
