// clut_source.h — reading a source picture and fitting it to the mode's box
//
// Shared by clut_encode and fixup_encode, which must start from the SAME 640-wide
// target or their outputs are not comparable.  Lifted out of clut_encode.cpp
// unchanged.
//
// The including .cpp includes stb/stb_image.h (with its implementation) BEFORE
// this header: stb's implementation section has no include guard of its own.

#pragma once

#include "clut_quant.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace ClutSource
{

using ClutColor::Rgb444;
using ClutColor::rgb444_channel_8;
using ClutQuant::CLUT_SIZE;
using ClutQuant::Sample;
using ClutQuant::line_palette;
using ClutQuant::quantize_colour;

// ---------------------------------------------------------------------------
// Source loading: PNG through stb, or a binary PPM, which is what the reference
// photographs are.  Either way the result is 8-bit RGB, tightly packed.
// ---------------------------------------------------------------------------

inline bool load_ppm(const char *path, std::vector<uint8_t> &rgb, int &width, int &height)
{
    std::FILE *f = std::fopen(path, "rb");
    if (f == nullptr)
    {
        return false;
    }

    // Header fields are whitespace separated with '#' comments; three numbers
    // after the magic, then exactly one whitespace byte before the data.
    char magic[3] = { 0, 0, 0 };
    if (std::fread(magic, 1, 2, f) != 2 || magic[0] != 'P' || magic[1] != '6')
    {
        std::fclose(f);
        return false;
    }

    int values[3] = { 0, 0, 0 };
    int got       = 0;
    while (got < 3)
    {
        const int c = std::fgetc(f);
        if (c == EOF)
        {
            std::fclose(f);
            return false;
        }
        if (c == '#')
        {
            int skip = 0;
            while ((skip = std::fgetc(f)) != EOF && skip != '\n')
            {
            }
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
        {
            continue;
        }
        if (c < '0' || c > '9')
        {
            std::fclose(f);
            return false;
        }
        int value = c - '0';
        int digit = 0;
        while ((digit = std::fgetc(f)) != EOF && digit >= '0' && digit <= '9')
        {
            value = value * 10 + (digit - '0');
        }
        values[got] = value;
        got++;
    }

    width  = values[0];
    height = values[1];
    if (width <= 0 || height <= 0 || values[2] != 255)
    {
        std::fclose(f);
        return false;
    }

    rgb.assign(static_cast<size_t>(width) * height * 3, 0);
    const bool ok = std::fread(rgb.data(), 1, rgb.size(), f) == rgb.size();
    std::fclose(f);
    return ok;
}

inline bool load_source(const char *path, std::vector<uint8_t> &rgb, int &width, int &height)
{
    if (load_ppm(path, rgb, width, height))
    {
        return true;
    }
    int            comps = 0;
    unsigned char *image = stbi_load(path, &width, &height, &comps, 3);
    if (image == nullptr)
    {
        return false;
    }
    rgb.assign(image, image + static_cast<size_t>(width) * height * 3);
    stbi_image_free(image);
    return true;
}

// Scale to COVER the destination box preserving aspect, centre crop, area
// average.  The same rule clut-experiment/clut_prepare uses, so a source that
// went through clut_prepare comes out unchanged here.
//
// THE DESTINATION BOX IS THE 640-WIDE OUTPUT, not the mode's 320 groups.  Half
// rate halves the horizontal SAMPLING, not the picture's shape: a group is two
// pixels wide on the CRT, so the frame is still 4:3 and a source fitted to a
// 320x480 box would be centre-cropped to half its width and then stretched back.
// Fit to 640 x height, then box-average pairs into the 320 groups -- which is
// exactly what clut-experiment's c4h rung did.
inline std::vector<int> resample(const std::vector<uint8_t> &src, int sw, int sh, int dw, int dh)
{
    std::vector<int> out(static_cast<size_t>(dw) * dh * 3, 0);

    // The source rectangle that maps onto the whole destination.
    const double scale = std::max(static_cast<double>(dw) / sw, static_cast<double>(dh) / sh);
    const double box_w = dw / scale;
    const double box_h = dh / scale;
    const double x0    = (sw - box_w) / 2.0;
    const double y0    = (sh - box_h) / 2.0;

    for (int y = 0; y < dh; y++)
    {
        const double sy0 = y0 + (box_h * y) / dh;
        const double sy1 = y0 + (box_h * (y + 1)) / dh;
        for (int x = 0; x < dw; x++)
        {
            const double sx0 = x0 + (box_w * x) / dw;
            const double sx1 = x0 + (box_w * (x + 1)) / dw;

            double sum[3] = { 0.0, 0.0, 0.0 };
            double weight = 0.0;
            const int iy0 = static_cast<int>(sy0);
            const int iy1 = static_cast<int>(std::max(sy0, sy1 - 1e-9));
            const int ix0 = static_cast<int>(sx0);
            const int ix1 = static_cast<int>(std::max(sx0, sx1 - 1e-9));
            for (int sy = iy0; sy <= iy1; sy++)
            {
                const double fy =
                    std::min(static_cast<double>(sy + 1), sy1) - std::max(static_cast<double>(sy), sy0);
                if (fy <= 0.0 || sy < 0 || sy >= sh)
                {
                    continue;
                }
                for (int sx = ix0; sx <= ix1; sx++)
                {
                    const double fx = std::min(static_cast<double>(sx + 1), sx1)
                                      - std::max(static_cast<double>(sx), sx0);
                    if (fx <= 0.0 || sx < 0 || sx >= sw)
                    {
                        continue;
                    }
                    const double w = fx * fy;
                    for (int c = 0; c < 3; c++)
                    {
                        sum[c] += w * src[(static_cast<size_t>(sy) * sw + sx) * 3 + c];
                    }
                    weight += w;
                }
            }
            for (int c = 0; c < 3; c++)
            {
                out[(static_cast<size_t>(y) * dw + x) * 3 + c] =
                    (weight > 0.0) ? static_cast<int>(sum[c] / weight + 0.5) : 0;
            }
        }
    }
    return out;
}

// The whole picture's sixteen colours, for K = 0.
//
// A HISTOGRAM over quantised R4G4B4, one sample per occupied bin, weighted by
// the SQUARE ROOT of the bin's population.  The square root is the whole trick
// and it is worth stating why: straight area weighting is what a k-means wants,
// and it throws away every small bright thing in the picture.  On the smurf
// meadow backdrop -- a sky gradient, a grass gradient, and a sun, two clouds and
// a brown fence covering a few per cent of the frame between them -- area
// weighting spends all sixteen entries on the two gradients and paints the sun
// and the fence in sky and grass.  Weighting by sqrt(count) pulls the ratio
// between "most of the picture" and "a small distinct object" from ~1000:1 to
// ~30:1, which is enough for the object to hold an entry, and the gradients lose
// one step each.  (Perceptually it is the standard fix; nothing here measures it
// beyond looking at the three backdrops, which is what decided it.)
//
// The bin colours are also handed in as the "line", so line_palette()'s exact
// path still fires: a picture with sixteen or fewer distinct R4G4B4 colours is
// reproduced exactly rather than k-meansed.
inline std::vector<Rgb444> global_palette(const std::vector<int> &source, int width, int height)
{
    constexpr int BINS = 4096;
    std::vector<int> count(BINS, 0);
    for (int i = 0; i < width * height; i++)
    {
        const Rgb444 c = quantize_colour(source[static_cast<size_t>(i) * 3 + 0],
                                         source[static_cast<size_t>(i) * 3 + 1],
                                         source[static_cast<size_t>(i) * 3 + 2]);
        count[c & 0xFFF]++;
    }

    std::vector<Sample> samples;
    std::vector<int>    distinct;
    for (int bin = 0; bin < BINS; bin++)
    {
        if (count[static_cast<size_t>(bin)] == 0)
        {
            continue;
        }
        const Rgb444 c = static_cast<Rgb444>(bin);
        Sample       s;
        for (int i = 0; i < 3; i++)
        {
            s.value[i] = rgb444_channel_8(c, i);
            distinct.push_back(s.value[i]);
        }
        int weight = 1;
        while ((weight + 1) * (weight + 1) <= count[static_cast<size_t>(bin)])
        {
            weight++;
        }
        s.weight = weight;
        samples.push_back(s);
    }

    const int distinct_count = static_cast<int>(distinct.size() / 3);
    std::vector<Rgb444> palette =
        line_palette(samples, distinct.data(), distinct_count, CLUT_SIZE);
    palette.resize(CLUT_SIZE);
    return palette;
}

}   // namespace ClutSource
