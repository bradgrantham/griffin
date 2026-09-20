// clut_compare.cpp — the one metric implementation for the whole experiment
//
// Build: see the Makefile in this directory.
// Usage: clut_compare [--header] <ref.png> <image.png> [image.png ...]
//
// Prints one CSV row per image: path, the two metrics microham_encode prints,
// and 8-bit PSNR for convenience.
//
//   per_pixel_mse  mean squared R4G4B4 error against the reference quantised to
//                  R4G4B4, summed over the three channels.  This is
//                  microham.h's measure_frame() metric 1, recomputed here so
//                  that micro-HAM previews and CLUT decodes are scored by the
//                  same code (check.sh verifies the two agree on a micro-HAM
//                  preview).
//   box4_mse       mean squared 8-bit error of 4x4 box means, the window
//                  sliding over every position, against the TRUE 8-bit
//                  reference.  metric 2, the one dithering is meant to improve.
//   psnr           10*log10(255^2 / mean squared 8-bit error).
//
// The images being scored come from a decoder, so every channel is a nibble
// replicated (v = nibble * 17); anything else is a bug worth shouting about
// rather than silently rounding.

#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

#include "clut_format.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace Clut;

namespace
{

struct Metrics
{
    double per_pixel_mse = 0.0;
    double box4_mse      = 0.0;
    double psnr          = 0.0;
    int    max_pixel     = 0;
};

// Undoes nibble replication; complains once per image if the decoded PNG is not
// made of replicated nibbles, since then it did not come out of a CLUT decoder.
int to_nibble(int v, bool &complained, const char *path)
{
    const int nibble = (v + 8) / 17;
    if (nibble * 17 != v && !complained)
    {
        std::fprintf(stderr, "clut_compare: %s: channel value %d is not a replicated nibble; "
                             "per-pixel error is approximate for this image\n",
                     path, v);
        complained = true;
    }
    return std::min(15, std::max(0, nibble));
}

Metrics measure(const std::vector<uint8_t> &reference, const std::vector<uint8_t> &image,
                const char *path)
{
    Metrics metrics;
    bool    complained = false;

    int64_t total_444 = 0;
    int64_t total_8   = 0;
    for (size_t i = 0; i < static_cast<size_t>(OUT_WIDTH) * HEIGHT; i++)
    {
        const Rgb444 decoded = rgb444(to_nibble(image[i * 3 + 0], complained, path),
                                      to_nibble(image[i * 3 + 1], complained, path),
                                      to_nibble(image[i * 3 + 2], complained, path));
        const Rgb444 target  = rgb444(quantize_8_to_4(reference[i * 3 + 0]),
                                      quantize_8_to_4(reference[i * 3 + 1]),
                                      quantize_8_to_4(reference[i * 3 + 2]));
        const int    e       = colour_error(decoded, target);
        total_444 += e;
        metrics.max_pixel = std::max(metrics.max_pixel, e);
        for (int c = 0; c < 3; c++)
        {
            const int d = static_cast<int>(image[i * 3 + c]) - static_cast<int>(reference[i * 3 + c]);
            total_8 += static_cast<int64_t>(d) * d;
        }
    }
    const double pixels = static_cast<double>(OUT_WIDTH) * HEIGHT;
    metrics.per_pixel_mse = static_cast<double>(total_444) / pixels;

    const double mse8 = static_cast<double>(total_8) / (pixels * 3.0);
    metrics.psnr      = (mse8 > 0.0) ? 10.0 * std::log10(255.0 * 255.0 / mse8) : 99.0;

    // Sliding 4x4 box means, column sums over 4 rows then a running 4-wide sum:
    // the same construction microham.h uses, so the numbers are comparable.
    constexpr int        BOX = 4;
    double               accumulator = 0.0;
    int64_t              windows     = 0;
    std::vector<int32_t> column_image(static_cast<size_t>(OUT_WIDTH) * 3, 0);
    std::vector<int32_t> column_reference(static_cast<size_t>(OUT_WIDTH) * 3, 0);

    for (int y = 0; y + BOX <= HEIGHT; y++)
    {
        std::fill(column_image.begin(), column_image.end(), 0);
        std::fill(column_reference.begin(), column_reference.end(), 0);
        for (int dy = 0; dy < BOX; dy++)
        {
            const size_t base = static_cast<size_t>(y + dy) * OUT_WIDTH;
            for (int x = 0; x < OUT_WIDTH; x++)
            {
                for (int c = 0; c < 3; c++)
                {
                    column_image[static_cast<size_t>(x) * 3 + c] += image[(base + x) * 3 + c];
                    column_reference[static_cast<size_t>(x) * 3 + c] +=
                        reference[(base + x) * 3 + c];
                }
            }
        }

        int32_t sum_image[3]     = { 0, 0, 0 };
        int32_t sum_reference[3] = { 0, 0, 0 };
        for (int x = 0; x < OUT_WIDTH; x++)
        {
            for (int c = 0; c < 3; c++)
            {
                sum_image[c] += column_image[static_cast<size_t>(x) * 3 + c];
                sum_reference[c] += column_reference[static_cast<size_t>(x) * 3 + c];
                if (x >= BOX)
                {
                    sum_image[c] -= column_image[static_cast<size_t>(x - BOX) * 3 + c];
                    sum_reference[c] -= column_reference[static_cast<size_t>(x - BOX) * 3 + c];
                }
            }
            if (x + 1 >= BOX)
            {
                for (int c = 0; c < 3; c++)
                {
                    const double d =
                        static_cast<double>(sum_image[c] - sum_reference[c]) / (BOX * BOX);
                    accumulator += d * d;
                }
                windows++;
            }
        }
    }
    metrics.box4_mse = (windows > 0) ? accumulator / static_cast<double>(windows) : 0.0;
    return metrics;
}

bool load_rgb(const char *path, std::vector<uint8_t> &out)
{
    int            width  = 0;
    int            height = 0;
    int            comps  = 0;
    unsigned char *data   = stbi_load(path, &width, &height, &comps, 3);
    if (data == nullptr)
    {
        std::fprintf(stderr, "clut_compare: cannot read %s\n", path);
        return false;
    }
    if (width != OUT_WIDTH || height != HEIGHT)
    {
        std::fprintf(stderr, "clut_compare: %s is %dx%d, expected %dx%d\n", path, width, height,
                     OUT_WIDTH, HEIGHT);
        stbi_image_free(data);
        return false;
    }
    out.assign(data, data + static_cast<size_t>(OUT_WIDTH) * HEIGHT * 3);
    stbi_image_free(data);
    return true;
}

}  // namespace

int main(int argc, char **argv)
{
    bool header = false;
    int  arg    = 1;
    while (arg < argc && std::strncmp(argv[arg], "--", 2) == 0)
    {
        if (std::strcmp(argv[arg], "--header") == 0)
        {
            header = true;
            arg++;
        }
        else
        {
            std::fprintf(stderr, "%s: unknown option %s\n", argv[0], argv[arg]);
            return 1;
        }
    }
    if (argc - arg < 2)
    {
        std::fprintf(stderr, "Usage: %s [--header] <ref.png> <image.png> [image.png ...]\n",
                     argv[0]);
        return 1;
    }

    std::vector<uint8_t> reference;
    if (!load_rgb(argv[arg], reference))
    {
        return 1;
    }

    if (header)
    {
        std::printf("image,per_pixel_mse,box4_mse,psnr,max_pixel_err\n");
    }
    for (int i = arg + 1; i < argc; i++)
    {
        std::vector<uint8_t> image;
        if (!load_rgb(argv[i], image))
        {
            return 1;
        }
        const Metrics m = measure(reference, image, argv[i]);
        std::printf("%s,%.4f,%.4f,%.3f,%d\n", argv[i], m.per_pixel_mse, m.box4_mse, m.psnr,
                    m.max_pixel);
    }
    return 0;
}
