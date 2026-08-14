// microham_roundtrip.cpp — encode, then decode through the REAL suite model
//
// Build: c++ -std=c++23 -O3 -I. -I.. -o microham_roundtrip \
//            microham_roundtrip.cpp obj/render.o
// Usage: microham_roundtrip [image.png ...]
//
// The suite model (super-engine/render.cpp) is the semantics truth.  This
// harness encodes an image with microham.h, feeds the resulting plane and
// per-line palette through PixelUnit/CompositorUnit exactly the way a display
// list would — four VIDCMD SETs in each line's horizontal blanking, then
// render_active_line over 80 PIXELS words — and requires the suite's frame to
// equal microham.h's own decode PIXEL FOR PIXEL.  A disagreement is a bug in
// microham.h, never in the suite.
//
// The error against the quantized source is reported separately and is purely
// informational: micro-HAM cannot reproduce an arbitrary image and is not
// expected to.
//
// A synthetic gradient is always tested in addition to any PNGs given, so the
// check has something to run against even before the assets are built.

#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

#include "microham.h"

#include "super-engine/render.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace MicroHam;

namespace
{

// Drive the suite's own PixelUnit / CompositorUnit the way the display list
// will: SETs land in blanking, 80 PIXELS words per line, no RUN records at all
// (the compositor holds passthrough for the whole frame, which is legal JIT
// framing and costs one word for a whole frame in the real list).
std::vector<Rgb444> suite_decode(const EncodedFrame &e)
{
    using namespace SuperEngine;

    PixelUnit      pix;
    CompositorUnit cmp;
    pix.reset();
    cmp.reset();

    std::vector<Rgb444> frame(static_cast<size_t>(WIDTH) * HEIGHT, 0);

    for (int y = 0; y < HEIGHT; y++)
    {
        cmp.push_word(vidcmd_set(SET_PIX_MODE, PIXEL_MODE_MICRO_HAM));
        cmp.push_word(vidcmd_set(SET_PIX_PIXEL_SKIP, 0));
        cmp.push_word(vidcmd_set(SET_PIX_PAL_FG, e.pal_fg[y]));
        cmp.push_word(vidcmd_set(SET_PIX_PAL_BG, e.pal_bg[y]));
        run_blanking(pix, cmp);

        for (int w = 0; w < WORDS_PER_LINE; w++)
        {
            if (!pix.push_word(e.plane[static_cast<size_t>(y) * WORDS_PER_LINE + w]))
            {
                std::fprintf(stderr, "PIXELS FIFO overflow at line %d\n", y);
            }
        }

        cmp.set_pixel_position(static_cast<uint64_t>(y) * H_TOTAL);
        render_active_line(pix, cmp, &frame[static_cast<size_t>(y) * WIDTH]);
    }
    return frame;
}

bool load_png_444(const char *path, std::vector<Rgb444> &out)
{
    int width  = 0;
    int height = 0;
    int comps  = 0;
    unsigned char *image = stbi_load(path, &width, &height, &comps, 3);
    if (image == nullptr)
    {
        std::fprintf(stderr, "cannot read %s\n", path);
        return false;
    }
    if (width != WIDTH || height != HEIGHT)
    {
        std::fprintf(stderr, "%s is %dx%d, expected %dx%d\n", path, width, height,
                     WIDTH, HEIGHT);
        stbi_image_free(image);
        return false;
    }
    out = quantize_frame(image);
    stbi_image_free(image);
    return true;
}

// A gradient nothing in micro-HAM can cheat on: a full-range hue sweep across
// the line and an independent luminance ramp down the frame.
std::vector<Rgb444> synthetic_gradient()
{
    std::vector<Rgb444> t(static_cast<size_t>(WIDTH) * HEIGHT);
    for (int y = 0; y < HEIGHT; y++)
    {
        for (int x = 0; x < WIDTH; x++)
        {
            const int r = (x * 15) / (WIDTH - 1);
            const int g = (y * 15) / (HEIGHT - 1);
            const int b = ((x + y) * 15) / (WIDTH + HEIGHT - 2);
            t[static_cast<size_t>(y) * WIDTH + x] = rgb444(r, g, b);
        }
    }
    return t;
}

bool check(const std::string &name, const std::vector<Rgb444> &target)
{
    const EncodedFrame        e     = encode_frame(target);
    const std::vector<Rgb444> suite = suite_decode(e);

    size_t mismatches = 0;
    size_t first      = 0;
    for (size_t i = 0; i < suite.size(); i++)
    {
        if (suite[i] != e.decoded[i])
        {
            if (mismatches == 0)
            {
                first = i;
            }
            mismatches++;
        }
    }

    int64_t total = 0;
    int     worst = 0;
    for (size_t i = 0; i < suite.size(); i++)
    {
        const int err = colour_error(suite[i], target[i]);
        total += err;
        worst = std::max(worst, err);
    }
    const double mean = static_cast<double>(total) / static_cast<double>(suite.size());

    if (mismatches == 0)
    {
        std::printf("%-28s suite==own EXACT (%zu pixels)   "
                    "vs source: mean sq err %.2f, max %d\n",
                    name.c_str(), suite.size(), mean, worst);
        return true;
    }

    std::printf("%-28s MISMATCH: %zu of %zu pixels; first at (%zu,%zu) "
                "suite 0x%03X own 0x%03X\n",
                name.c_str(), mismatches, suite.size(), first % WIDTH, first / WIDTH,
                suite[first], e.decoded[first]);
    return false;
}

}  // namespace

int main(int argc, char **argv)
{
    bool ok = check("synthetic-gradient", synthetic_gradient());

    for (int i = 1; i < argc; i++)
    {
        std::vector<Rgb444> target;
        if (!load_png_444(argv[i], target))
        {
            return 1;
        }
        ok = check(argv[i], target) && ok;
    }

    std::printf("%s\n", ok ? "ROUND TRIP OK" : "ROUND TRIP FAILED");
    return ok ? 0 : 1;
}
