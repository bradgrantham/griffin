// clut_encode.cpp — reference PNG -> .clut stream for the CLUT photo experiment
//
// Build: see the Makefile in this directory.
// Usage: clut_encode --mode c4h|c2f|c4f|attr [--k 16|12|4] [--n 0|4|8|16]
//                    [--dither none|ordered|fs] [--dither-amp N]
//                    [--predict pred.png] <ref.png> <out.clut>
//
// Everything here is heuristic; clut_decode is the truth.  What the encoder
// owes the experiment is (a) a stream the validator accepts and (b) a
// PREDICTION of what the decoder will show, written by --predict, which
// `make check` compares byte for byte against the decoder's own render.  The
// prediction is built by this file's own simulation and shares nothing with
// clut_decode.cpp but clut_format.h, so the comparison is a real cross-check.
//
// Pipeline per line (DESIGN.md §4):
//
//   1. Candidate palette: farthest-point-seeded k-means in 8-bit RGB over this
//      line and its two neighbours (weights 1,2,1 — the neighbours are what
//      stops the CLUT jittering line to line and streaking the picture),
//      centroids quantised to R4G4B4, plus up to 8 extra candidates taken from
//      the line's worst-fit pixels (worth about 2% on both metrics).  A line
//      whose distinct R4G4B4 colours already fit the mode's reachable entries
//      takes them verbatim, which is both optimal and what makes the §6
//      exact-reproduction gate pass.
//   2. CLUT update: the previous line's CLUT is the starting point and at most K
//      (slot, colour) replacements are chosen greedily, each round taking the
//      replacement that most reduces this line's nearest-entry error.  Per-pixel
//      best and second-best distances make a candidate replacement O(width) to
//      score.  When K would have paid for a wholesale reload, the k-means
//      palette outright is costed too and taken if it is better.
//   3. Index assignment with the chosen dither, against the CLUT the decoder
//      will actually hold.
//   4. Spans: a DP over a 4-pixel grid (which is what enforces the >= 4 pixel
//      length and >= 4 pixel gap of DESIGN.md §2) choosing up to N spans that
//      maximise squared-error reduction against the undithered 640-wide
//      reference.  The 4-pixel grid is the conservative stand-in for the real
//      planner recurrence that DESIGN.md asks be declared as one.
//
// Half-rate modes (c4h, attr) decide their indices in their own 320-wide
// domain, box-averaged from the reference; spans are a COMPOSITOR overlay and
// therefore always work in the 640-wide output domain.

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image.h"
#include "stb/stb_image_write.h"

#include "clut_format.h"
#include "clut_quant.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace Clut;

namespace
{

// ---------------------------------------------------------------------------
// The palette chooser, the reload planner and the dither all live in
// ../clut_quant.h / ../clut_color.h (promoted out of this file 2026-09-20, same
// arithmetic).  Only the experiment's own knobs and its span planner are here.
// ---------------------------------------------------------------------------

using ClutColor::Dither;
using ClutColor::DEFAULT_DITHER_AMPLITUDE;
using ClutColor::ordered_offset;
using ClutQuant::FS_RESIDUAL_LIMIT;
using ClutQuant::FS_SHIFT;
using ClutQuant::FsRows;
using ClutQuant::Sample;
using ClutQuant::dist8;
using ClutQuant::line_palette;
using ClutQuant::nearest_entry;
using ClutQuant::quantize_colour;
using ClutQuant::update_clut;

struct Options
{
    const ModeInfo *mode      = nullptr;
    int             k         = CLUT_SIZE;
    int             n         = 0;
    Dither          dither    = Dither::None;
    int             amplitude = DEFAULT_DITHER_AMPLITUDE;
    const char     *predict   = nullptr;
};

}  // namespace

int main(int argc, char **argv)
{
    Options options;
    int     arg = 1;

    while (arg < argc && std::strncmp(argv[arg], "--", 2) == 0)
    {
        const char *const name  = argv[arg];
        const char *const value = (arg + 1 < argc) ? argv[arg + 1] : nullptr;
        if (std::strcmp(name, "--mode") == 0 && value != nullptr)
        {
            options.mode = mode_by_name(value);
            if (options.mode == nullptr)
            {
                std::fprintf(stderr, "%s: unknown mode %s\n", argv[0], value);
                return 1;
            }
            arg += 2;
        }
        else if (std::strcmp(name, "--k") == 0 && value != nullptr)
        {
            options.k = std::atoi(value);
            arg += 2;
        }
        else if (std::strcmp(name, "--n") == 0 && value != nullptr)
        {
            options.n = std::atoi(value);
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
                std::fprintf(stderr, "%s: unknown dither %s\n", argv[0], value);
                return 1;
            }
            arg += 2;
        }
        else if (std::strcmp(name, "--dither-amp") == 0 && value != nullptr)
        {
            options.amplitude = std::atoi(value);
            arg += 2;
        }
        else if (std::strcmp(name, "--predict") == 0 && value != nullptr)
        {
            options.predict = value;
            arg += 2;
        }
        else
        {
            std::fprintf(stderr, "%s: unknown option %s\n", argv[0], name);
            return 1;
        }
    }

    if (options.mode == nullptr || argc - arg != 2)
    {
        std::fprintf(stderr,
                     "Usage: %s --mode c4h|c2f|c4f|attr [--k N] [--n N] "
                     "[--dither none|ordered|fs] [--dither-amp N] [--predict pred.png] "
                     "<ref.png> <out.clut>\n",
                     argv[0]);
        return 1;
    }
    if (options.k < 1 || options.k > CLUT_SIZE)
    {
        std::fprintf(stderr, "%s: K must be 1..%d\n", argv[0], CLUT_SIZE);
        return 1;
    }
    if (options.n < 0 || options.n > 64)
    {
        std::fprintf(stderr, "%s: N must be 0..64\n", argv[0]);
        return 1;
    }

    const ModeInfo &mi      = *options.mode;
    const char *const in_path  = argv[arg];
    const char *const out_path = argv[arg + 1];

    const int budget_words = mi.words_per_line + SPAN_WORDS * options.n;
    if (budget_words > VIDCMD_WORDS_PER_LINE_CAP && mi.mode != Mode::C4F)
    {
        std::fprintf(stderr, "%s: mode %s with N=%d needs %d words/line, over the %d-word cap\n",
                     argv[0], mi.name, options.n, budget_words, VIDCMD_WORDS_PER_LINE_CAP);
        return 1;
    }

    int            width  = 0;
    int            height = 0;
    int            comps  = 0;
    unsigned char *image  = stbi_load(in_path, &width, &height, &comps, 3);
    if (image == nullptr)
    {
        std::fprintf(stderr, "%s: cannot read %s\n", argv[0], in_path);
        return 1;
    }
    if (width != OUT_WIDTH || height != HEIGHT)
    {
        std::fprintf(stderr, "%s: %s is %dx%d, expected %dx%d\n", argv[0], in_path, width, height,
                     OUT_WIDTH, HEIGHT);
        stbi_image_free(image);
        return 1;
    }
    const std::vector<uint8_t> reference(image,
                                         image + static_cast<size_t>(OUT_WIDTH) * HEIGHT * 3);
    stbi_image_free(image);

    // The mode's own pixel domain: 640 as-is, or box-averaged pairs for 320.
    const int             pw = mi.pixel_width;
    std::vector<int>      source(static_cast<size_t>(pw) * HEIGHT * 3, 0);
    for (int y = 0; y < HEIGHT; y++)
    {
        for (int x = 0; x < pw; x++)
        {
            for (int i = 0; i < 3; i++)
            {
                int value = 0;
                if (mi.half_rate)
                {
                    const size_t a = (static_cast<size_t>(y) * OUT_WIDTH + x * 2) * 3 + i;
                    const size_t b = a + 3;
                    value          = (reference[a] + reference[b] + 1) / 2;
                }
                else
                {
                    value = reference[(static_cast<size_t>(y) * OUT_WIDTH + x) * 3 + i];
                }
                source[(static_cast<size_t>(y) * pw + x) * 3 + i] = value;
            }
        }
    }

    Stream stream;
    stream.mode = mi.mode;
    stream.k    = options.k;
    stream.n    = options.n;
    stream.lines.assign(HEIGHT, Line());

    std::vector<Rgb444> prediction(static_cast<size_t>(OUT_WIDTH) * HEIGHT, 0);

    Rgb444 clut[CLUT_SIZE] = { 0 };
    const int usable       = mi.usable_entries;

    FsRows fs;
    fs.current.assign(static_cast<size_t>(pw) * 3, 0);
    fs.next.assign(static_cast<size_t>(pw) * 3, 0);

    std::vector<Sample> samples;
    std::vector<int>    codes(static_cast<size_t>(pw), 0);
    std::vector<int>    dithered(static_cast<size_t>(pw) * 3, 0);

    // Span-planner scratch, allocated once: the gain of every (start, length)
    // on the 4-pixel grid, the DP tables, and the reference prefix sums.
    constexpr int                       SPAN_CELLS = OUT_WIDTH / 4;
    std::vector<int64_t>                gain_table;
    std::vector<int64_t>                best;
    std::vector<int16_t>                choice;
    std::array<std::vector<int64_t>, 3> prefix_sum;
    std::array<std::vector<int64_t>, 3> prefix_square;
    std::vector<int64_t>                prefix_base;
    if (options.n > 0)
    {
        gain_table.assign(static_cast<size_t>(SPAN_CELLS) * (SPAN_CELLS + 1), 0);
        best.assign(static_cast<size_t>(options.n + 1) * (SPAN_CELLS + 2), 0);
        choice.assign(static_cast<size_t>(options.n + 1) * (SPAN_CELLS + 2), 0);
        for (int c = 0; c < 3; c++)
        {
            prefix_sum[static_cast<size_t>(c)].assign(OUT_WIDTH + 1, 0);
            prefix_square[static_cast<size_t>(c)].assign(OUT_WIDTH + 1, 0);
        }
        prefix_base.assign(OUT_WIDTH + 1, 0);
    }

    for (int y = 0; y < HEIGHT; y++)
    {
        Line      &line         = stream.lines[static_cast<size_t>(y)];
        const int *line_values  = &source[static_cast<size_t>(y) * pw * 3];

        // 1. candidates from this line weighted 2 and its neighbours weighted 1
        samples.clear();
        samples.reserve(static_cast<size_t>(pw) * 3);
        for (int dy = -1; dy <= 1; dy++)
        {
            const int ny = y + dy;
            if (ny < 0 || ny >= HEIGHT)
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
        const std::vector<Rgb444> candidates = line_palette(samples, line_values, pw, usable);

        // 2. CLUT update under the reload budget
        const int budget = (y == 0) ? CLUT_SIZE : options.k;
        update_clut(clut, usable, budget, candidates, line_values, pw, line.loads);

        // 3. index assignment
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

        if (mi.mode == Mode::ATTR)
        {
            // Per 8-pixel cell: the best (fg, bg) pair out of the CLUT plus an
            // 8-bit pattern.  The pair is chosen on the undithered targets and
            // the pattern under the requested dither, so an edge inside a cell
            // still picks the pair that spans it.
            const int cells = pw / 8;
            for (int cell = 0; cell < cells; cell++)
            {
                const int base = cell * 8;

                int pick_fg = 0;
                int pick_bg = 0;
                int64_t best = -1;
                for (int a = 0; a < usable; a++)
                {
                    for (int b = a; b < usable; b++)
                    {
                        int64_t cost = 0;
                        for (int i = 0; i < 8; i++)
                        {
                            const int *v  = &line_values[(base + i) * 3];
                            const int  da = dist8(v, clut[a]);
                            const int  db = dist8(v, clut[b]);
                            cost += std::min(da, db);
                        }
                        if (best < 0 || cost < best)
                        {
                            best    = cost;
                            pick_fg = a;
                            pick_bg = b;
                        }
                    }
                }

                for (int i = 0; i < 8; i++)
                {
                    const int x = base + i;
                    int       value[3];
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
                            const int32_t v = (static_cast<int32_t>(line_values[x * 3 + c])
                                               << FS_SHIFT)
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

                    const int chosen =
                        (dist8(value, clut[pick_fg]) <= dist8(value, clut[pick_bg])) ? pick_fg
                                                                                    : pick_bg;
                    codes[static_cast<size_t>(x)] = chosen;

                    if (options.dither == Dither::FloydSteinberg)
                    {
                        for (int c = 0; c < 3; c++)
                        {
                            int32_t residual =
                                ((static_cast<int32_t>(line_values[x * 3 + c]) << FS_SHIFT)
                                 + fs.current[static_cast<size_t>(x) * 3 + c])
                                - (static_cast<int32_t>(rgb444_channel_8(clut[chosen], c))
                                   << FS_SHIFT);
                            residual = std::max(-FS_RESIDUAL_LIMIT,
                                                std::min(FS_RESIDUAL_LIMIT, residual));
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
            }
        }
        else
        {
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
                        const int32_t v =
                            (static_cast<int32_t>(line_values[x * 3 + c]) << FS_SHIFT)
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

                const int chosen = nearest_entry(value, clut, usable);
                codes[static_cast<size_t>(x)] = chosen;

                if (options.dither == Dither::FloydSteinberg)
                {
                    for (int c = 0; c < 3; c++)
                    {
                        int32_t residual =
                            ((static_cast<int32_t>(line_values[x * 3 + c]) << FS_SHIFT)
                             + fs.current[static_cast<size_t>(x) * 3 + c])
                            - (static_cast<int32_t>(rgb444_channel_8(clut[chosen], c)) << FS_SHIFT);
                        residual =
                            std::max(-FS_RESIDUAL_LIMIT, std::min(FS_RESIDUAL_LIMIT, residual));
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
        }

        std::swap(fs.current, fs.next);
        std::fill(fs.next.begin(), fs.next.end(), 0);

        // pack the line's words, MSB first out of big-endian 16-bit words
        line.words.assign(static_cast<size_t>(mi.words_per_line), 0);
        {
            int      index = 0;
            uint16_t accumulator = 0;
            int      held = 0;
            auto     put  = [&](int value, int bits)
            {
                for (int i = 0; i < bits; i++)
                {
                    accumulator = static_cast<uint16_t>((accumulator << 1)
                                                        | ((value >> (bits - 1 - i)) & 1));
                    held++;
                    if (held == 16)
                    {
                        line.words[static_cast<size_t>(index)] = accumulator;
                        index++;
                        accumulator = 0;
                        held        = 0;
                    }
                }
            };

            if (mi.mode == Mode::ATTR)
            {
                for (int cell = 0; cell < pw / 8; cell++)
                {
                    // The cell's two codes, in the order the pattern's set and
                    // clear bits will select them.
                    int fg = codes[static_cast<size_t>(cell * 8)];
                    int bg = fg;
                    for (int i = 1; i < 8; i++)
                    {
                        const int c = codes[static_cast<size_t>(cell * 8 + i)];
                        if (c != fg)
                        {
                            bg = c;
                        }
                    }
                    int pattern = 0;
                    for (int i = 0; i < 8; i++)
                    {
                        if (codes[static_cast<size_t>(cell * 8 + i)] == fg)
                        {
                            pattern |= 1 << (7 - i);
                        }
                    }
                    put(fg, 4);
                    put(bg, 4);
                    put(pattern, 8);
                }
            }
            else
            {
                for (int x = 0; x < pw; x++)
                {
                    put(codes[static_cast<size_t>(x)], mi.bits_per_pixel);
                }
            }
        }

        // predict the pixel stream, with doubling for half-rate modes
        Rgb444 *row = &prediction[static_cast<size_t>(y) * OUT_WIDTH];
        for (int x = 0; x < pw; x++)
        {
            const Rgb444 c = clut[codes[static_cast<size_t>(x)]];
            if (mi.half_rate)
            {
                row[x * 2 + 0] = c;
                row[x * 2 + 1] = c;
            }
            else
            {
                row[x] = c;
            }
        }

        // 4. spans: DP on a 4-pixel grid over the 640-wide output
        if (options.n > 0)
        {
            constexpr int GRID  = 4;
            constexpr int CELLS = OUT_WIDTH / GRID;

            const uint8_t *ref = &reference[static_cast<size_t>(y) * OUT_WIDTH * 3];
            for (int x = 0; x < OUT_WIDTH; x++)
            {
                int64_t base = 0;
                for (int c = 0; c < 3; c++)
                {
                    const int64_t v = ref[x * 3 + c];
                    prefix_sum[static_cast<size_t>(c)][x + 1] =
                        prefix_sum[static_cast<size_t>(c)][x] + v;
                    prefix_square[static_cast<size_t>(c)][x + 1] =
                        prefix_square[static_cast<size_t>(c)][x] + v * v;
                    const int64_t d = v - rgb444_channel_8(row[x], c);
                    base += d * d;
                }
                prefix_base[x + 1] = prefix_base[x] + base;
            }

            auto span_colour = [&](int x0, int length)
            {
                int mean[3];
                for (int c = 0; c < 3; c++)
                {
                    const int64_t sum = prefix_sum[static_cast<size_t>(c)][x0 + length]
                                        - prefix_sum[static_cast<size_t>(c)][x0];
                    mean[c] = static_cast<int>((sum + length / 2) / length);
                }
                return quantize_colour(mean[0], mean[1], mean[2]);
            };

            auto span_gain = [&](int x0, int length)
            {
                const Rgb444 colour = span_colour(x0, length);
                int64_t      cost   = 0;
                for (int c = 0; c < 3; c++)
                {
                    const int64_t v = rgb444_channel_8(colour, c);
                    const int64_t s = prefix_sum[static_cast<size_t>(c)][x0 + length]
                                      - prefix_sum[static_cast<size_t>(c)][x0];
                    const int64_t q = prefix_square[static_cast<size_t>(c)][x0 + length]
                                      - prefix_square[static_cast<size_t>(c)][x0];
                    cost += q - 2 * v * s + static_cast<int64_t>(length) * v * v;
                }
                return (prefix_base[x0 + length] - prefix_base[x0]) - cost;
            };

            // Every (start cell, length in cells) gain, once per line: the DP
            // below reads each of them N times and they do not depend on j.
            for (int c = 0; c < CELLS; c++)
            {
                for (int len = 1; c + len <= CELLS; len++)
                {
                    gain_table[static_cast<size_t>(c) * (CELLS + 1) + static_cast<size_t>(len)] =
                        span_gain(c * GRID, len * GRID);
                }
            }

            // best[j][c] = best gain from cell c onwards with j spans left.
            // A span of `len` cells at c forces the next span to start at
            // c + len + 1, which is the >= 4 pixel gap.
            const int spans_max = options.n;
            auto      at        = [&](int j, int c) -> size_t
            {
                return static_cast<size_t>(j) * (CELLS + 2) + static_cast<size_t>(c);
            };
            std::fill(best.begin(), best.end(), 0);
            std::fill(choice.begin(), choice.end(), 0);

            for (int j = 1; j <= spans_max; j++)
            {
                for (int c = CELLS - 1; c >= 0; c--)
                {
                    int64_t        take_best = best[at(j, c + 1)];
                    int16_t        take_len  = 0;
                    const int64_t *gains =
                        &gain_table[static_cast<size_t>(c) * (CELLS + 1)];
                    for (int len = 1; c + len <= CELLS; len++)
                    {
                        const int64_t gain =
                            gains[len] + best[at(j - 1, std::min(c + len + 1, CELLS + 1))];
                        if (gain > take_best)
                        {
                            take_best = gain;
                            take_len  = static_cast<int16_t>(len);
                        }
                    }
                    best[at(j, c)]   = take_best;
                    choice[at(j, c)] = take_len;
                }
            }

            int cell  = 0;
            int left  = spans_max;
            while (left > 0 && cell < CELLS)
            {
                const int16_t len = choice[at(left, cell)];
                if (len == 0)
                {
                    cell++;
                    continue;
                }
                Span span;
                span.x      = static_cast<uint16_t>(cell * GRID);
                span.length = static_cast<uint16_t>(len * GRID);
                span.colour = span_colour(cell * GRID, len * GRID);
                line.spans.push_back(span);
                for (int x = span.x; x < span.x + span.length; x++)
                {
                    row[x] = span.colour;
                }
                cell = cell + len + 1;
                left--;
            }
        }
    }

    if (!write_stream(out_path, stream))
    {
        std::fprintf(stderr, "%s: cannot write %s\n", argv[0], out_path);
        return 1;
    }

    if (options.predict != nullptr)
    {
        std::vector<uint8_t> rgb(static_cast<size_t>(OUT_WIDTH) * HEIGHT * 3);
        for (size_t i = 0; i < prediction.size(); i++)
        {
            rgb[i * 3 + 0] = nibble_to_8(rgb444_r(prediction[i]));
            rgb[i * 3 + 1] = nibble_to_8(rgb444_g(prediction[i]));
            rgb[i * 3 + 2] = nibble_to_8(rgb444_b(prediction[i]));
        }
        if (stbi_write_png(options.predict, OUT_WIDTH, HEIGHT, 3, rgb.data(), OUT_WIDTH * 3) == 0)
        {
            std::fprintf(stderr, "%s: cannot write %s\n", argv[0], options.predict);
            return 1;
        }
    }

    std::printf("%s: mode %s K=%d N=%d, %d words/line\n", out_path, mi.name, options.k, options.n,
                mi.words_per_line);
    return 0;
}
