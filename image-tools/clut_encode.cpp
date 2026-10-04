// clut_encode.cpp — a photograph -> a .gc4 file a Griffin program can DMA
//
// Build: see the Makefile in this directory.
// Usage: clut_encode [--k 0..8] [--dither none|ordered|fs] [--dither-amp N]
//                    [--height 480|240] [--preview out.png] <source> <out.gc4>
//
//   source   PNG or binary PPM (P6), any size: it is scaled to cover the mode's
//            320 x height box, centre cropped, area averaged.  640x480, 320x480
//            and 320x240 sources land on it without resampling loss.
//   out.gc4  gc4_format.h: 4bpp half rate, 80 words/line, plus one fixed-stride
//            palette-load record per line.
//
// WHAT THIS TOOL IS.  The promotion of clut-experiment's c4h rung, which the
// 2026-09-19 sweep picked as the mode a photograph survives in once PIXEL became
// index-only.  The heuristic itself is ../clut_quant.h and is shared with the
// experiment, unchanged; what is new here is the output format and the K range.
//
// WHAT IT DELIBERATELY IS NOT.  There are NO REFINEMENT SPANS.  The experiment's
// span planner buys real error reduction, but a span is a COMPOSITOR record that
// has to be placed against everything else the line's VIDCMD packet is doing —
// sprites, mode changes, the palette loads themselves — and that placement is the
// display-list planner's job, not an image tool's.  The span experiment stays in
// clut-experiment/ as the record of what it was worth.
//
// K, the per-line reload budget, is the one knob that matters: 0 means "one
// sixteen-colour palette for the whole picture, no per-line cost at all" (a
// game's backdrop), and 1..8 spend three VIDCMD words per load in each line's
// HBLANK packet (a photograph).  The ceiling is 8 because 8 * 3 + 1 = 25 words is
// what a 26-word HBLANK packet holds.
//
// The truth about what a .gc4 file shows is, in order: the hardware, then
// super-engine/render.cpp driven by clut_roundtrip, then clut_decode.  This
// tool's --preview PNG is a PREDICTION, and `make check` requires all three to
// agree pixel for pixel.

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image.h"
#include "stb/stb_image_write.h"

#include "clut_quant.h"
#include "clut_source.h"
#include "gc4_format.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using ClutColor::DEFAULT_DITHER_AMPLITUDE;
using ClutColor::Dither;
using ClutColor::PaletteLoad;
using ClutColor::Rgb444;
using ClutColor::clamp_8;
using ClutColor::nibble_to_8;
using ClutColor::ordered_offset;
using ClutColor::rgb444_b;
using ClutColor::rgb444_channel_8;
using ClutColor::rgb444_g;
using ClutColor::rgb444_r;

using ClutQuant::CLUT_SIZE;
using ClutQuant::FS_RESIDUAL_LIMIT;
using ClutQuant::FS_SHIFT;
using ClutQuant::FsRows;
using ClutQuant::Sample;
using ClutQuant::dist8;
using ClutQuant::line_palette;
using ClutQuant::nearest_entry;
using ClutQuant::quantize_colour;
using ClutQuant::update_clut;

using ClutSource::global_palette;
using ClutSource::load_source;
using ClutSource::resample;

namespace
{

struct Options
{
    int         k         = Gc4::DEFAULT_K;
    int         height    = 480;
    Dither      dither    = Dither::FloydSteinberg;
    int         amplitude = DEFAULT_DITHER_AMPLITUDE;
    const char *preview   = nullptr;
};

}   // namespace

int main(int argc, char **argv)
{
    Options options;
    int     arg = 1;

    while (arg < argc && std::strncmp(argv[arg], "--", 2) == 0)
    {
        const char *const name  = argv[arg];
        const char *const value = (arg + 1 < argc) ? argv[arg + 1] : nullptr;
        if (std::strcmp(name, "--k") == 0 && value != nullptr)
        {
            options.k = std::atoi(value);
            arg += 2;
        }
        else if (std::strcmp(name, "--height") == 0 && value != nullptr)
        {
            options.height = std::atoi(value);
            arg += 2;
        }
        else if (std::strcmp(name, "--dither") == 0 && value != nullptr)
        {
            if (std::strcmp(value, "none") == 0)
            {
                options.dither = Dither::None;
            }
            else if (std::strcmp(value, "ordered") == 0)
            {
                options.dither = Dither::Ordered;
            }
            else if (std::strcmp(value, "fs") == 0)
            {
                options.dither = Dither::FloydSteinberg;
            }
            else
            {
                std::fprintf(stderr, "%s: unknown dither mode %s\n", argv[0], value);
                return 1;
            }
            arg += 2;
        }
        else if (std::strcmp(name, "--dither-amp") == 0 && value != nullptr)
        {
            options.amplitude = std::atoi(value);
            arg += 2;
        }
        else if (std::strcmp(name, "--preview") == 0 && value != nullptr)
        {
            options.preview = value;
            arg += 2;
        }
        else
        {
            std::fprintf(stderr, "%s: unknown option %s\n", argv[0], name);
            return 1;
        }
    }

    if (argc - arg != 2)
    {
        std::fprintf(stderr,
                     "usage: %s [--k 0..%d] [--dither none|ordered|fs] [--dither-amp N]\n"
                     "          [--height 480|240] [--preview out.png] <source> <out.gc4>\n",
                     argv[0], Gc4::MAX_K);
        return 1;
    }
    if (options.k < 0 || options.k > Gc4::MAX_K)
    {
        std::fprintf(stderr, "%s: K must be 0..%d (a load is three VIDCMD words and a "
                             "26-word HBLANK packet holds eight)\n", argv[0], Gc4::MAX_K);
        return 1;
    }
    if (options.height <= 0 || options.height > 480)
    {
        std::fprintf(stderr, "%s: height must be 1..480\n", argv[0]);
        return 1;
    }

    const char *const in_path  = argv[arg];
    const char *const out_path = argv[arg + 1];

    std::vector<uint8_t> src;
    int                  sw = 0;
    int                  sh = 0;
    if (!load_source(in_path, src, sw, sh))
    {
        std::fprintf(stderr, "%s: cannot read %s (expected PNG or binary PPM)\n", argv[0], in_path);
        return 1;
    }

    const int pw = Gc4::PIXEL_WIDTH;
    const int ph = options.height;
    const std::vector<int> wide = resample(src, sw, sh, Gc4::OUT_WIDTH, ph);

    std::vector<int> source(static_cast<size_t>(pw) * ph * 3, 0);
    for (int y = 0; y < ph; y++)
    {
        for (int x = 0; x < pw; x++)
        {
            for (int i = 0; i < 3; i++)
            {
                const size_t a = (static_cast<size_t>(y) * Gc4::OUT_WIDTH + x * 2) * 3 + i;
                source[(static_cast<size_t>(y) * pw + x) * 3 + i] =
                    (wide[a] + wide[a + 3] + 1) / 2;
            }
        }
    }

    Gc4::Picture picture;
    picture.k      = options.k;
    picture.height = ph;
    picture.lines.assign(static_cast<size_t>(ph), Gc4::Line());

    Rgb444 clut[CLUT_SIZE] = { 0 };
    if (options.k == 0)
    {
        const std::vector<Rgb444> global = global_palette(source, pw, ph);
        for (int i = 0; i < CLUT_SIZE; i++)
        {
            clut[i] = global[static_cast<size_t>(i)];
        }
    }

    // The prediction, in the 640-wide output domain: every group is held two
    // pixel clocks at half rate, so a source pixel becomes two.
    std::vector<Rgb444> prediction(static_cast<size_t>(Gc4::OUT_WIDTH) * ph, 0);

    FsRows fs;
    fs.current.assign(static_cast<size_t>(pw) * 3, 0);
    fs.next.assign(static_cast<size_t>(pw) * 3, 0);

    std::vector<Sample> samples;
    std::vector<int>    codes(static_cast<size_t>(pw), 0);
    std::vector<int>    dithered(static_cast<size_t>(pw) * 3, 0);
    unsigned            loads_total = 0;

    for (int y = 0; y < ph; y++)
    {
        Gc4::Line &line        = picture.lines[static_cast<size_t>(y)];
        const int *line_values = &source[static_cast<size_t>(y) * pw * 3];

        // 1 & 2. the palette, and what it costs to get there.  K = 0 fixes the
        // palette for the whole picture, so neither step runs; the entries the
        // file's header carries are the ones every line reads.
        if (options.k > 0)
        {
            samples.clear();
            samples.reserve(static_cast<size_t>(pw) * 3);
            for (int dy = -1; dy <= 1; dy++)
            {
                const int ny = y + dy;
                if (ny < 0 || ny >= ph)
                {
                    continue;
                }
                const int *row = &source[static_cast<size_t>(ny) * pw * 3];
                for (int x = 0; x < pw; x++)
                {
                    Sample s;
                    s.value[0] = row[x * 3 + 0];
                    s.value[1] = row[x * 3 + 1];
                    s.value[2] = row[x * 3 + 2];
                    s.weight   = (dy == 0) ? 2 : 1;
                    samples.push_back(s);
                }
            }
            const std::vector<Rgb444> candidates = line_palette(samples, line_values, pw, CLUT_SIZE);

            // Line 0's loads are the FILE HEADER's initial palette, not a
            // per-line record: they are deposited once in vertical blanking,
            // where all sixteen entries fit, and every later line spends at most
            // K.  So line 0 gets the whole budget and pays nothing for it.
            const int budget = (y == 0) ? CLUT_SIZE : options.k;
            update_clut(clut, CLUT_SIZE, budget, candidates, line_values, pw, line.loads);
            if (y == 0)
            {
                line.loads.clear();
            }
        }
        if (y == 0)
        {
            for (int i = 0; i < CLUT_SIZE; i++)
            {
                picture.initial[i] = clut[i];
            }
        }
        loads_total += static_cast<unsigned>(line.loads.size());

        // 3. index assignment, against the palette the hardware will hold.
        if (options.dither == Dither::Ordered)
        {
            for (int x = 0; x < pw; x++)
            {
                const int offset = ordered_offset(x, y, options.amplitude);
                for (int i = 0; i < 3; i++)
                {
                    dithered[static_cast<size_t>(x) * 3 + i] =
                        clamp_8(line_values[x * 3 + i] + offset);
                }
            }
        }

        for (int x = 0; x < pw; x++)
        {
            int value[3];
            if (options.dither == Dither::Ordered)
            {
                for (int c = 0; c < 3; c++)
                {
                    value[c] = dithered[static_cast<size_t>(x) * 3 + c];
                }
            }
            else if (options.dither == Dither::FloydSteinberg)
            {
                for (int c = 0; c < 3; c++)
                {
                    const int32_t v = (static_cast<int32_t>(line_values[x * 3 + c]) << FS_SHIFT)
                                      + fs.current[static_cast<size_t>(x) * 3 + c];
                    value[c] = (v + (1 << (FS_SHIFT - 1))) >> FS_SHIFT;
                }
            }
            else
            {
                for (int c = 0; c < 3; c++)
                {
                    value[c] = line_values[x * 3 + c];
                }
            }

            const int chosen              = nearest_entry(value, clut, CLUT_SIZE);
            codes[static_cast<size_t>(x)] = chosen;

            if (options.dither == Dither::FloydSteinberg)
            {
                for (int c = 0; c < 3; c++)
                {
                    int32_t residual =
                        ((static_cast<int32_t>(line_values[x * 3 + c]) << FS_SHIFT)
                         + fs.current[static_cast<size_t>(x) * 3 + c])
                        - (static_cast<int32_t>(rgb444_channel_8(clut[chosen], c)) << FS_SHIFT);
                    residual = std::max(-FS_RESIDUAL_LIMIT, std::min(FS_RESIDUAL_LIMIT, residual));
                    if (x + 1 < pw)
                    {
                        fs.current[static_cast<size_t>(x + 1) * 3 + c] += residual * 7 / 16;
                        fs.next[static_cast<size_t>(x + 1) * 3 + c] += residual * 1 / 16;
                    }
                    fs.next[static_cast<size_t>(x) * 3 + c] += residual * 5 / 16;
                    if (x > 0)
                    {
                        fs.next[static_cast<size_t>(x - 1) * 3 + c] += residual * 3 / 16;
                    }
                }
            }
        }

        std::swap(fs.current, fs.next);
        std::fill(fs.next.begin(), fs.next.end(), 0);

        // 4. pack the line: four bits per group, MSB first into big-endian words.
        line.words.assign(Gc4::WORDS_PER_LINE, 0);
        for (int x = 0; x < pw; x++)
        {
            const int word  = x / 4;
            const int shift = 12 - 4 * (x % 4);
            line.words[static_cast<size_t>(word)] = static_cast<uint16_t>(
                line.words[static_cast<size_t>(word)]
                | (static_cast<uint16_t>(codes[static_cast<size_t>(x)] & 0xF) << shift));
        }

        Rgb444 *row = &prediction[static_cast<size_t>(y) * Gc4::OUT_WIDTH];
        for (int x = 0; x < pw; x++)
        {
            const Rgb444 c = clut[codes[static_cast<size_t>(x)]];
            row[x * 2 + 0] = c;
            row[x * 2 + 1] = c;
        }
    }

    std::string error;
    if (!Gc4::validate_picture(picture, error))
    {
        std::fprintf(stderr, "%s: the encoder produced an invalid picture: %s\n", argv[0],
                     error.c_str());
        return 1;
    }
    if (!Gc4::write_picture(out_path, picture))
    {
        std::fprintf(stderr, "%s: cannot write %s\n", argv[0], out_path);
        return 1;
    }

    if (options.preview != nullptr)
    {
        std::vector<uint8_t> rgb(static_cast<size_t>(Gc4::OUT_WIDTH) * ph * 3);
        for (size_t i = 0; i < prediction.size(); i++)
        {
            rgb[i * 3 + 0] = nibble_to_8(rgb444_r(prediction[i]));
            rgb[i * 3 + 1] = nibble_to_8(rgb444_g(prediction[i]));
            rgb[i * 3 + 2] = nibble_to_8(rgb444_b(prediction[i]));
        }
        if (stbi_write_png(options.preview, Gc4::OUT_WIDTH, ph, 3, rgb.data(),
                           Gc4::OUT_WIDTH * 3) == 0)
        {
            std::fprintf(stderr, "%s: cannot write %s\n", argv[0], options.preview);
            return 1;
        }
    }

    std::printf("%s: 4bpp half rate, %dx%d, K=%d, %d words/line, %u palette loads "
                "(%.2f/line)\n",
                out_path, Gc4::PIXEL_WIDTH, ph, options.k, Gc4::WORDS_PER_LINE, loads_total,
                static_cast<double>(loads_total) / ph);
    return 0;
}
