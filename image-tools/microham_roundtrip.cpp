// microham_roundtrip.cpp — encode, then decode through the REAL suite model
//
// Build: c++ -std=c++23 -O3 -I. -I.. -o microham_roundtrip \
//            microham_roundtrip.cpp obj/render.o
// Usage: microham_roundtrip [--dither none|ordered|fs] [--dither-amp N] [image.png ...]
//
// The suite model (super-engine/render.cpp) is the semantics truth.  This
// harness encodes an image with microham.h, feeds the resulting plane and
// per-line palette through PixelUnit/CompositorUnit exactly the way a display
// list would — four VIDCMD SETs in each line's horizontal blanking, then
// render_active_line over 80 PIXELS words — and requires the suite's frame to
// equal microham.h's own decode PIXEL FOR PIXEL.  A disagreement is a bug in
// microham.h, never in the suite.
//
// EVERY dither mode is checked, not just the default: dithering changes which
// codes the encoder picks, so each mode is a different walk through the same
// code space and each one has to land on a legal stream.  Give --dither to
// check one mode only.
//
// The error against the source is reported separately and is purely
// informational: micro-HAM cannot reproduce an arbitrary image and is not
// expected to.  Two numbers are printed, per-pixel and 4x4-box (see the metrics
// comment in microham.h); dithering is meant to make the first WORSE and the
// second better.
//
// A synthetic gradient is always tested in addition to any PNGs given, so the
// check has something to run against even before the assets are built.

#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

#include "microham.h"

#include "super-engine/render.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
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

bool load_png_8(const char *path, std::vector<uint8_t> &out)
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
        std::fprintf(stderr, "%s is %dx%d, expected %dx%d\n", path, width, height, WIDTH, HEIGHT);
        stbi_image_free(image);
        return false;
    }
    out.assign(image, image + static_cast<size_t>(WIDTH) * HEIGHT * 3);
    stbi_image_free(image);
    return true;
}

// A gradient nothing in micro-HAM can cheat on: a full-range hue sweep across
// the line and an independent luminance ramp down the frame.  Built in RGB444
// and expanded, so it is exactly representable and the only error the encoder
// can show is its own.
std::vector<uint8_t> synthetic_gradient()
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
    return expand_frame(t);
}

const char *dither_name(Dither mode)
{
    switch (mode)
    {
        case Dither::None:           return "none";
        case Dither::Ordered:        return "ordered";
        case Dither::FloydSteinberg: return "fs";
    }
    return "?";
}

bool check(const std::string &name, const std::vector<uint8_t> &source, const DitherParams &dither)
{
    const EncodedFrame        e     = encode_frame(source, dither);
    const std::vector<Rgb444> suite = suite_decode(e);
    const FrameMetrics        m     = measure_frame(e.decoded, source);

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

    const std::string label = name + " [" + dither_name(dither.mode) + "]";

    if (mismatches != 0)
    {
        std::printf("%-38s MISMATCH: %zu of %zu pixels; first at (%zu,%zu) "
                    "suite 0x%03X own 0x%03X\n",
                    label.c_str(), mismatches, suite.size(), first % WIDTH, first / WIDTH,
                    suite[first], e.decoded[first]);
        return false;
    }
    if (e.model_mismatches != 0)
    {
        std::printf("%-38s DITHER MODEL DISAGREES with the decoder at %zu pixels\n", label.c_str(),
                    e.model_mismatches);
        return false;
    }

    std::printf("%-38s suite==own EXACT (%zu pixels)   per-pixel %7.2f  4x4-box %7.2f  max %d\n",
                label.c_str(), suite.size(), m.per_pixel_mse, m.box4_mse, m.max_pixel_error);
    return true;
}

}  // namespace

int main(int argc, char **argv)
{
    std::vector<DitherParams> modes;
    DitherParams              one;
    bool                      pinned = false;
    int                       arg    = 1;

    while (arg < argc && argv[arg][0] == '-' && argv[arg][1] == '-')
    {
        if (std::strcmp(argv[arg], "--dither") == 0 && arg + 1 < argc)
        {
            const char *const text = argv[arg + 1];
            if (std::strcmp(text, "none") == 0)
            {
                one.mode = Dither::None;
            }
            else if (std::strcmp(text, "ordered") == 0)
            {
                one.mode = Dither::Ordered;
            }
            else if (std::strcmp(text, "fs") == 0)
            {
                one.mode = Dither::FloydSteinberg;
            }
            else
            {
                std::fprintf(stderr, "%s: unknown dither mode %s\n", argv[0], text);
                return 1;
            }
            pinned = true;
            arg += 2;
        }
        else if (std::strcmp(argv[arg], "--dither-amp") == 0 && arg + 1 < argc)
        {
            one.amplitude = std::atoi(argv[arg + 1]);
            arg += 2;
        }
        else
        {
            std::fprintf(stderr, "%s: unknown option %s\n", argv[0], argv[arg]);
            return 1;
        }
    }

    if (pinned)
    {
        modes.push_back(one);
    }
    else
    {
        modes.push_back({Dither::None, one.amplitude});
        modes.push_back({Dither::Ordered, one.amplitude});
        modes.push_back({Dither::FloydSteinberg, one.amplitude});
    }

    struct Image
    {
        std::string          name;
        std::vector<uint8_t> pixels;
    };
    std::vector<Image> images;
    images.push_back({"synthetic-gradient", synthetic_gradient()});

    for (int i = arg; i < argc; i++)
    {
        std::vector<uint8_t> pixels;
        if (!load_png_8(argv[i], pixels))
        {
            return 1;
        }
        images.push_back({argv[i], std::move(pixels)});
    }

    bool ok = true;
    for (const DitherParams &d : modes)
    {
        for (const Image &img : images)
        {
            ok = check(img.name, img.pixels, d) && ok;
        }
    }

    std::printf("%s\n", ok ? "ROUND TRIP OK" : "ROUND TRIP FAILED");
    return ok ? 0 : 1;
}
