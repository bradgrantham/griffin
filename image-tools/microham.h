// microham.h — micro-HAM (2 bits/pixel) encoder, decoder, and file formats.
//
// SEMANTICS.  This file mirrors, and must never contradict, the suite model in
// super-engine/render.cpp (PixelUnit::next_pixel, lines 137-186) and the RTL it
// models, cpld/pixel/pixel.v.  The suite is the truth; if the two ever disagree
// the fix belongs here.
//
//   stream is consumed MSB-first out of consecutive 16-bit words, exactly two
//   bits per pixel clock (author.cpp's BitPacker packs it the same way):
//
//     0 p        1 clock,  1 pixel   held <- p ? pal_fg : pal_bg
//     1 0  g r   2 clocks, 2 pixels  held.green <- {4{g}}, held.red  <- {4{r}}
//     1 1  g b   2 clocks, 2 pixels  held.green <- {4{g}}, held.blue <- {4{b}}
//
//   A 4-bit chroma code straddles two pixel clocks and the decoder cannot see
//   the chroma pair until the second, so the FIRST pixel of the code shows the
//   OLD held colour and the SECOND shows both channels updated together.
//   Chroma writes are whole-nibble replications of one bit: a channel touched
//   by a chroma code becomes 0x0 or 0xF, never anything between.  Only pal_fg /
//   pal_bg carry arbitrary 4-bit levels into held.
//
//   held reloads from pal_fg at the start of every visible line
//   (PixelUnit::begin_line, render.cpp:110).
//
// The encoder is therefore an exact dynamic program over a small reachable
// state set rather than a greedy walk: with two bits per pixel there is no rate
// constraint at all (every pixel costs exactly two bits whichever code is
// used), so the optimal line encoding for a given (pal_fg, pal_bg) is just a
// shortest path.
//
// DITHERING.  Minimizing per-pixel squared error is exactly the wrong thing for
// a two-colour-per-line format: the cheapest way to cover a gradient is a flat
// fill, so gradients band.  A palette code is one pixel wide, so the format can
// alternate fg/bg at full pixel rate for free — dithering costs no bits at all.
// Two dither modes therefore feed the encoder:
//
//   Dither::Ordered  perturbs the 8-bit source by a void-and-cluster blue-noise
//                    mask before quantizing to the RGB444 target, then runs the
//                    unmodified exact DP.  The DP stays exact with respect to
//                    the perturbed target.  This is the default; the reasoning
//                    and the measurements are at the top of microham_encode.cpp.
//   Dither::FloydSteinberg
//                    replaces the DP with a greedy chooser that diffuses the
//                    residual of the colour the hardware will actually SHOW
//                    (chroma latency included) into the neighbours.  Much better
//                    tonally on photographs, much noisier on hard edges.
//
// Neither mode can emit an illegal stream: dithering only changes which codes
// are picked, never what a code means.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

namespace MicroHam
{

inline constexpr int WIDTH          = 640;
inline constexpr int HEIGHT         = 480;
inline constexpr int WORDS_PER_LINE = 80;   // 640 pixels * 2 bits / 16

// 12-bit R4G4B4, the same packing descriptor.h uses: r << 8 | g << 4 | b.
using Rgb444 = uint16_t;

constexpr Rgb444 rgb444(int r, int g, int b)
{
    return static_cast<Rgb444>(((r & 0xF) << 8) | ((g & 0xF) << 4) | (b & 0xF));
}

constexpr int rgb444_r(Rgb444 c) { return (c >> 8) & 0xF; }
constexpr int rgb444_g(Rgb444 c) { return (c >> 4) & 0xF; }
constexpr int rgb444_b(Rgb444 c) { return c & 0xF; }

// Nibble replication, matching descriptor.h's rgb444_channel_to_8.
constexpr uint8_t nibble_to_8(int nibble)
{
    return static_cast<uint8_t>((nibble & 0xF) * 17);
}

constexpr int quantize_8_to_4(int v)
{
    return (v * 15 + 127) / 255;
}

// Channel i (0 = red, 1 = green, 2 = blue) of a colour, expanded to 8 bits.
constexpr int rgb444_channel_8(Rgb444 c, int i)
{
    const int nib = (i == 0) ? rgb444_r(c) : ((i == 1) ? rgb444_g(c) : rgb444_b(c));
    return nib * 17;
}

constexpr int colour_error(Rgb444 a, Rgb444 b)
{
    const int dr = rgb444_r(a) - rgb444_r(b);
    const int dg = rgb444_g(a) - rgb444_g(b);
    const int db = rgb444_b(a) - rgb444_b(b);
    return dr * dr + dg * dg + db * db;
}

constexpr int clamp_8(int v)
{
    return (v < 0) ? 0 : ((v > 255) ? 255 : v);
}

// ---------------------------------------------------------------------------
// Dither modes and the blue-noise threshold mask
// ---------------------------------------------------------------------------

enum class Dither
{
    None,
    Ordered,
    FloydSteinberg,
};

// The ISOTROPIC part of the ordered dither, in 8-bit units, peak to peak: the
// mask perturbs all three channels by amplitude * threshold, so the perturbation
// spans +-amplitude/2.  (The other, larger part of the perturbation runs along
// the palette axis and needs no amplitude — see ordered_dither_line.)
//
// One RGB444 step is 255/15 = 17 8-bit units, and 17 is exactly the amplitude at
// which this term becomes COMPLETE: every 8-bit level between two RGB444 levels
// gets a mask threshold that can round it either way, in proportion.  Below 17 a
// dead band survives in the middle of each step; above 17 the extra spread buys
// nothing but grain, since the palette axis already covers everything wider than
// one step.  Sweeping 8, 17, 20, 24 and 34 over the test images agrees: the
// 4x4-box error is flat to within its own noise across the whole range, so the
// principled value wins on the tie.
inline constexpr int DEFAULT_DITHER_AMPLITUDE = 17;

struct DitherParams
{
    Dither mode      = Dither::Ordered;
    int    amplitude = DEFAULT_DITHER_AMPLITUDE;
};

inline constexpr int MASK_N = 16;   // mask tile is MASK_N x MASK_N

// Rank per cell, a permutation of 0 .. MASK_N*MASK_N-1.
using DitherMask = std::array<uint8_t, MASK_N * MASK_N>;

// Void-and-cluster (Ulichney 1993).  The generator needs a starting pattern and
// uses a fixed-seed LCG for it, so the mask is a compile-time-fixed constant in
// everything but spelling: same binary, same bytes, every run, on every input.
// It is built once (function-local static) and shared.
inline DitherMask build_blue_noise_mask()
{
    constexpr int    N     = MASK_N;
    constexpr int    CELLS = N * N;
    constexpr int    ONES  = CELLS / 8;
    constexpr double SIGMA = 1.5;

    // Toroidal Gaussian, in integer weights so the energy accumulation is exact.
    std::array<int32_t, CELLS> kernel{};
    for (int dy = 0; dy < N; dy++)
    {
        for (int dx = 0; dx < N; dx++)
        {
            const int    wy = std::min(dy, N - dy);
            const int    wx = std::min(dx, N - dx);
            const double d2 = static_cast<double>(wx * wx + wy * wy);
            kernel[dy * N + dx] =
                static_cast<int32_t>(std::lround(4096.0 * std::exp(-d2 / (2.0 * SIGMA * SIGMA))));
        }
    }

    std::array<uint8_t, CELLS> pattern{};
    std::array<int64_t, CELLS> energy{};

    auto stamp = [&](int pos, int sign)
    {
        const int py = pos / N;
        const int px = pos % N;
        for (int y = 0; y < N; y++)
        {
            for (int x = 0; x < N; x++)
            {
                const int dy = (y - py + N) % N;
                const int dx = (x - px + N) % N;
                energy[y * N + x] += static_cast<int64_t>(sign) * kernel[dy * N + dx];
            }
        }
    };

    // Tightest cluster of ones / largest void among zeros.  Raster-order first
    // match on ties, so the whole search is deterministic.
    auto tightest_cluster = [&]()
    {
        int     best = -1;
        int64_t best_e = 0;
        for (int i = 0; i < CELLS; i++)
        {
            if (pattern[i] != 0 && (best < 0 || energy[i] > best_e))
            {
                best   = i;
                best_e = energy[i];
            }
        }
        return best;
    };
    auto largest_void = [&]()
    {
        int     best = -1;
        int64_t best_e = 0;
        for (int i = 0; i < CELLS; i++)
        {
            if (pattern[i] == 0 && (best < 0 || energy[i] < best_e))
            {
                best   = i;
                best_e = energy[i];
            }
        }
        return best;
    };

    uint32_t lcg = 0x13579BDFu;
    auto     next_random = [&]()
    {
        lcg = lcg * 1664525u + 1013904223u;
        return static_cast<int>((lcg >> 16) % static_cast<uint32_t>(CELLS));
    };

    for (int placed = 0; placed < ONES;)
    {
        const int p = next_random();
        if (pattern[p] == 0)
        {
            pattern[p] = 1;
            stamp(p, +1);
            placed++;
        }
    }

    // Equalize the initial pattern: move the tightest cluster into the largest
    // void until the move becomes a no-op.
    for (int iter = 0; iter < 4 * CELLS; iter++)
    {
        const int c = tightest_cluster();
        pattern[c] = 0;
        stamp(c, -1);
        const int v = largest_void();
        if (v == c)
        {
            pattern[c] = 1;
            stamp(c, +1);
            break;
        }
        pattern[v] = 1;
        stamp(v, +1);
    }

    const std::array<uint8_t, CELLS> initial = pattern;
    DitherMask                       mask{};

    // Phase 1: pull ones out of the initial pattern, ranking downward.
    for (int rank = ONES - 1; rank >= 0; rank--)
    {
        const int c = tightest_cluster();
        pattern[c] = 0;
        stamp(c, -1);
        mask[c] = static_cast<uint8_t>(rank);
    }

    // Phases 2 and 3: refill from the initial pattern, ranking upward.  Once the
    // tile is more than half full, "the zero furthest from any one" is the same
    // pixel the classic phase-3 cluster measure on the complement would pick, so
    // one loop covers both phases.
    pattern = initial;
    energy.fill(0);
    for (int i = 0; i < CELLS; i++)
    {
        if (pattern[i] != 0)
        {
            stamp(i, +1);
        }
    }
    for (int rank = ONES; rank < CELLS; rank++)
    {
        const int v = largest_void();
        pattern[v] = 1;
        stamp(v, +1);
        mask[v] = static_cast<uint8_t>(rank);
    }
    return mask;
}

inline const DitherMask &blue_noise_mask()
{
    static const DitherMask mask = build_blue_noise_mask();
    return mask;
}

// The mask's threshold for one cell as a signed Q8 fraction of a whole step:
// 256 * ((rank + 1/2) / CELLS - 1/2), so the range is (-128, +128) in Q8, i.e.
// (-1/2, +1/2).  Q8 rather than a float keeps every later decision integral.
inline int ordered_threshold_q8(int x, int y)
{
    const int rank = blue_noise_mask()[(y % MASK_N) * MASK_N + (x % MASK_N)];
    return ((2 * rank + 1 - MASK_N * MASK_N) * 256) / (2 * MASK_N * MASK_N);
}

// The perturbation for one cell, in 8-bit units: amplitude * threshold.  The
// same offset on all three channels — micro-HAM's palette codes move R, G and B
// together, so a per-channel mask would only ask for oscillations the format
// cannot deliver.
inline int ordered_offset(int x, int y, int amplitude)
{
    return (amplitude * ordered_threshold_q8(x, y) + 128) >> 8;
}

// ---------------------------------------------------------------------------
// Bit packing / unpacking — MSB first into consecutive 16-bit words
// ---------------------------------------------------------------------------

class BitPacker
{
public:
    explicit BitPacker(std::span<uint16_t> words) : words_(words) {}

    void put(uint32_t value, int count)
    {
        for (int i = 0; i < count; i++)
        {
            acc_ = static_cast<uint16_t>((acc_ << 1) | ((value >> (count - 1 - i)) & 1u));
            held_++;
            if (held_ == 16)
            {
                words_[index_] = acc_;
                index_++;
                acc_  = 0;
                held_ = 0;
            }
        }
    }

    int words_written() const { return index_; }
    int bits_held() const { return held_; }

private:
    std::span<uint16_t> words_;
    int      index_ = 0;
    uint16_t acc_   = 0;
    int      held_  = 0;
};

// ---------------------------------------------------------------------------
// Decoder — a transcription of PixelUnit::next_pixel in micro-HAM mode
// ---------------------------------------------------------------------------

// Decodes one line of WORDS_PER_LINE words into WIDTH colours.  `out` must hold
// WIDTH entries.  Mirrors render.cpp:137-186 exactly, including the one-pixel
// chroma latency and the line-start reload of held from pal_fg.
inline void decode_line(std::span<const uint16_t> words, Rgb444 pal_fg, Rgb444 pal_bg,
                        std::span<Rgb444> out)
{
    uint64_t bitbuf     = 0;
    int      bits_avail = 0;
    int      next_word  = 0;

    auto refill = [&](int need)
    {
        while (bits_avail < need)
        {
            const uint16_t w = (next_word < static_cast<int>(words.size()))
                                   ? words[next_word]
                                   : 0;
            next_word++;
            bitbuf = (bitbuf << 16) | w;
            bits_avail += 16;
        }
    };
    auto peek = [&](int n) -> uint32_t
    {
        return static_cast<uint32_t>((bitbuf >> (bits_avail - n)) & ((1u << n) - 1u));
    };
    auto take = [&](int n) -> uint32_t
    {
        const uint32_t v = peek(n);
        bits_avail -= n;
        bitbuf &= (bits_avail == 0) ? 0ull : ((1ull << bits_avail) - 1ull);
        return v;
    };

    Rgb444 held    = pal_fg;   // held reloads from pal_fg at every line start
    bool   pending = false;
    int    type    = 0;
    int    chroma_g = 0;
    int    chroma_v = 0;

    for (int x = 0; x < WIDTH; x++)
    {
        if (pending)
        {
            pending = false;
            (void)take(2);
            if (type == 0)
            {
                held = rgb444(chroma_v, chroma_g, rgb444_b(held));   // 10_g_r
            }
            else
            {
                held = rgb444(rgb444_r(held), chroma_g, chroma_v);   // 11_g_b
            }
            out[x] = held;
            continue;
        }

        refill(1);
        if (peek(1) == 0)
        {
            refill(2);
            const uint32_t code = take(2);
            held   = ((code & 1u) != 0) ? pal_fg : pal_bg;
            out[x] = held;
            continue;
        }

        refill(4);
        const uint32_t code = peek(4);
        (void)take(2);
        type     = static_cast<int>((code >> 2) & 1u);
        chroma_g = (((code >> 1) & 1u) != 0) ? 0xF : 0x0;
        chroma_v = (((code >> 0) & 1u) != 0) ? 0xF : 0x0;
        pending  = true;
        out[x]   = held;   // the OLD held colour, per pixel.v
    }
}

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------

// The reachable held-colour set for a given palette pair.  A chroma code always
// saturates the two channels it writes, so once the line has left the palette
// colours behind, at most one channel can still carry a palette level:
//
//   red   in {0, F, pal_fg.r, pal_bg.r}
//   green in {0, F}                       (or exactly pal_fg / pal_bg)
//   blue  in {0, F, pal_fg.b, pal_bg.b}
//
// That is at most 2 + 4*2*4 = 34 states, so an exact shortest path per line is
// cheap.  The set is closed under both chroma forms by construction.
struct StateSet
{
    std::vector<Rgb444>       colours;
    std::array<int16_t, 4096> index{};   // colour -> state index, -1 if absent

    int size() const { return static_cast<int>(colours.size()); }
    int find(Rgb444 c) const { return index[c]; }
};

inline StateSet build_states(Rgb444 pal_fg, Rgb444 pal_bg)
{
    StateSet s;
    s.index.fill(-1);

    auto add = [&](Rgb444 c)
    {
        if (s.index[c] < 0)
        {
            s.index[c] = static_cast<int16_t>(s.colours.size());
            s.colours.push_back(c);
        }
    };

    add(pal_fg);
    add(pal_bg);

    const std::array<int, 4> r_levels = {0x0, 0xF, rgb444_r(pal_fg), rgb444_r(pal_bg)};
    const std::array<int, 4> b_levels = {0x0, 0xF, rgb444_b(pal_fg), rgb444_b(pal_bg)};

    for (int r : r_levels)
    {
        for (int g : {0x0, 0xF})
        {
            for (int b : b_levels)
            {
                add(rgb444(r, g, b));
            }
        }
    }
    return s;
}

// One encoded line: the packed words plus the colours the hardware will show.
struct EncodedLine
{
    std::array<uint16_t, WORDS_PER_LINE> words{};
    std::array<Rgb444, WIDTH>            decoded{};
    int64_t                              cost = 0;   // sum of squared RGB444 error
};

// A per-line code list, in the one encoding both choosers speak: 0 = palette bg,
// 1 = palette fg, 8|code = the four-bit chroma code 1<code>.  A palette entry
// covers one pixel, a chroma entry two, so any legal list is exactly WIDTH
// pixels and exactly WORDS_PER_LINE words: two bits per pixel either way.
inline void pack_codes(std::span<const uint8_t> codes, Rgb444 pal_fg, Rgb444 pal_bg,
                       EncodedLine &out)
{
    BitPacker packer{std::span<uint16_t>(out.words)};
    out.words.fill(0);
    for (uint8_t c : codes)
    {
        if ((c & 8) == 0)
        {
            packer.put(c & 1u, 2);
        }
        else
        {
            packer.put(static_cast<uint32_t>(0b1000 | (c & 7)), 4);
        }
    }
    decode_line(std::span<const uint16_t>(out.words), pal_fg, pal_bg,
                std::span<Rgb444>(out.decoded));
}

// The colour a chroma code leaves in held, given the incoming held colour.
constexpr Rgb444 chroma_result(Rgb444 held, int code)
{
    const int type = (code >> 2) & 1;
    const int g    = (((code >> 1) & 1) != 0) ? 0xF : 0x0;
    const int v    = (((code >> 0) & 1) != 0) ? 0xF : 0x0;
    return (type == 0) ? rgb444(v, g, rgb444_b(held)) : rgb444(rgb444_r(held), g, v);
}

// Scratch buffers, hoisted so a whole frame's worth of candidate palettes can
// be evaluated without touching the allocator.
struct EncoderScratch
{
    std::vector<int64_t> dp;       // (WIDTH + 1) * states
    std::vector<uint8_t> choice;   // same shape: 0 = bg, 1 = fg, 8|code = chroma
    std::vector<int>     err;      // states, per pixel scratch
    std::vector<uint8_t> codes;    // traceback output, in the pack_codes encoding
};

inline constexpr int64_t COST_INF = static_cast<int64_t>(1) << 40;

// Exact optimal encoding of one line for a fixed palette pair.
inline void encode_line(std::span<const Rgb444> target, Rgb444 pal_fg, Rgb444 pal_bg,
                        EncoderScratch &scratch, EncodedLine &out)
{
    const StateSet states = build_states(pal_fg, pal_bg);
    const int      n      = states.size();

    scratch.dp.assign(static_cast<size_t>(WIDTH + 1) * n, COST_INF);
    scratch.choice.assign(static_cast<size_t>(WIDTH + 1) * n, 0);
    scratch.err.assign(n, 0);

    const int fg_index = states.find(pal_fg);
    const int bg_index = states.find(pal_bg);

    // Successor table for the eight chroma codes, per state.
    std::vector<int> chroma_next(static_cast<size_t>(n) * 8, 0);
    for (int s = 0; s < n; s++)
    {
        const Rgb444 c = states.colours[s];
        for (int code = 0; code < 8; code++)
        {
            chroma_next[static_cast<size_t>(s) * 8 + code] = states.find(chroma_result(c, code));
        }
    }

    for (int s = 0; s < n; s++)
    {
        scratch.dp[static_cast<size_t>(WIDTH) * n + s] = 0;
    }

    for (int x = WIDTH - 1; x >= 0; x--)
    {
        const Rgb444   t  = target[x];
        int64_t *const dp = &scratch.dp[static_cast<size_t>(x) * n];
        uint8_t *const ch = &scratch.choice[static_cast<size_t>(x) * n];
        const int64_t *const dp_next = &scratch.dp[static_cast<size_t>(x + 1) * n];

        // The palette options do not depend on the incoming held colour.
        const int64_t cost_fg = colour_error(pal_fg, t) + dp_next[fg_index];
        const int64_t cost_bg = colour_error(pal_bg, t) + dp_next[bg_index];
        const int64_t pal_best  = std::min(cost_fg, cost_bg);
        const uint8_t pal_code  = (cost_fg <= cost_bg) ? 1 : 0;

        const bool           chroma_ok = (x + 2 <= WIDTH);
        const Rgb444         t2        = chroma_ok ? target[x + 1] : 0;
        const int64_t *const dp_next2  = chroma_ok
                                             ? &scratch.dp[static_cast<size_t>(x + 2) * n]
                                             : nullptr;

        if (chroma_ok)
        {
            for (int s = 0; s < n; s++)
            {
                scratch.err[s] = colour_error(states.colours[s], t2);
            }
        }

        for (int s = 0; s < n; s++)
        {
            int64_t best   = pal_best;
            uint8_t best_c = pal_code;

            if (chroma_ok)
            {
                const int64_t here = colour_error(states.colours[s], t);
                const int *const nxt = &chroma_next[static_cast<size_t>(s) * 8];
                for (int code = 0; code < 8; code++)
                {
                    const int     ns = nxt[code];
                    const int64_t c  = here + scratch.err[ns] + dp_next2[ns];
                    if (c < best)
                    {
                        best   = c;
                        best_c = static_cast<uint8_t>(8 | code);
                    }
                }
            }

            dp[s] = best;
            ch[s] = best_c;
        }
    }

    // Traceback.  held starts the line at pal_fg.
    scratch.codes.clear();
    int s = fg_index;
    int x = 0;
    out.cost = scratch.dp[static_cast<size_t>(0) * n + fg_index];

    while (x < WIDTH)
    {
        const uint8_t c = scratch.choice[static_cast<size_t>(x) * n + s];
        scratch.codes.push_back(c);
        if ((c & 8) == 0)
        {
            s = (c & 1u) != 0 ? fg_index : bg_index;
            x += 1;
        }
        else
        {
            s = chroma_next[static_cast<size_t>(s) * 8 + (c & 7)];
            x += 2;
        }
    }

    pack_codes(scratch.codes, pal_fg, pal_bg, out);
}

// ---------------------------------------------------------------------------
// Palette selection
// ---------------------------------------------------------------------------

// k-means in RGB444 space over one line, seeded by luma-sorted percentiles so
// the result is deterministic and does not need an RNG.
inline std::vector<Rgb444> line_candidates(std::span<const Rgb444> line, int k)
{
    std::vector<Rgb444> sorted(line.begin(), line.end());
    auto luma = [](Rgb444 c)
    {
        return 299 * rgb444_r(c) + 587 * rgb444_g(c) + 114 * rgb444_b(c);
    };
    std::sort(sorted.begin(), sorted.end(),
              [&](Rgb444 a, Rgb444 b) { return luma(a) < luma(b); });

    std::vector<std::array<int, 3>> centre(k);
    for (int i = 0; i < k; i++)
    {
        const Rgb444 c = sorted[(static_cast<size_t>(i) * 2 + 1) * sorted.size() /
                                (static_cast<size_t>(k) * 2)];
        centre[i] = {rgb444_r(c), rgb444_g(c), rgb444_b(c)};
    }

    for (int iter = 0; iter < 8; iter++)
    {
        std::vector<std::array<int64_t, 4>> acc(k, {0, 0, 0, 0});
        for (Rgb444 c : line)
        {
            int best = 0;
            int best_d = 1 << 30;
            for (int i = 0; i < k; i++)
            {
                const int dr = rgb444_r(c) - centre[i][0];
                const int dg = rgb444_g(c) - centre[i][1];
                const int db = rgb444_b(c) - centre[i][2];
                const int d  = dr * dr + dg * dg + db * db;
                if (d < best_d)
                {
                    best_d = d;
                    best   = i;
                }
            }
            acc[best][0] += rgb444_r(c);
            acc[best][1] += rgb444_g(c);
            acc[best][2] += rgb444_b(c);
            acc[best][3] += 1;
        }
        for (int i = 0; i < k; i++)
        {
            if (acc[i][3] > 0)
            {
                centre[i][0] = static_cast<int>((acc[i][0] + acc[i][3] / 2) / acc[i][3]);
                centre[i][1] = static_cast<int>((acc[i][1] + acc[i][3] / 2) / acc[i][3]);
                centre[i][2] = static_cast<int>((acc[i][2] + acc[i][3] / 2) / acc[i][3]);
            }
        }
    }

    std::vector<Rgb444> out;
    for (int i = 0; i < k; i++)
    {
        const Rgb444 c = rgb444(centre[i][0], centre[i][1], centre[i][2]);
        if (std::find(out.begin(), out.end(), c) == out.end())
        {
            out.push_back(c);
        }
    }

    // The line mean is a useful extra: k-means can leave a gap in the middle of
    // a smooth gradient, and one of the two palette slots usually wants to sit
    // there.
    int64_t sr = 0;
    int64_t sg = 0;
    int64_t sb = 0;
    for (Rgb444 c : line)
    {
        sr += rgb444_r(c);
        sg += rgb444_g(c);
        sb += rgb444_b(c);
    }
    const auto   count = static_cast<int64_t>(line.size());
    const Rgb444 mean  = rgb444(static_cast<int>(sr / count), static_cast<int>(sg / count),
                                static_cast<int>(sb / count));
    auto push_unique = [&out](Rgb444 c)
    {
        if (std::find(out.begin(), out.end(), c) == out.end())
        {
            out.push_back(c);
        }
    };
    push_unique(mean);

    // Endpoints.  A dithered gradient wants the two palette slots at the ENDS of
    // the line's colour range, not at cluster centroids: an oscillation between
    // two extremes reproduces every level between them, while two centroids can
    // only reproduce the two centroids.  k-means will never propose that, so the
    // luma percentiles and the channel-wise extremes go in as candidates and the
    // pair search decides.  Percentiles rather than min/max of luma so one stray
    // specular pixel cannot drag a slot to white.
    auto at_percentile = [&sorted](int permille)
    {
        const size_t i = (static_cast<size_t>(permille) * (sorted.size() - 1)) / 1000;
        return sorted[i];
    };
    push_unique(at_percentile(50));
    push_unique(at_percentile(950));
    push_unique(at_percentile(100));
    push_unique(at_percentile(900));

    int lo_r = 15, lo_g = 15, lo_b = 15;
    int hi_r = 0, hi_g = 0, hi_b = 0;
    for (Rgb444 c : line)
    {
        lo_r = std::min(lo_r, rgb444_r(c));
        lo_g = std::min(lo_g, rgb444_g(c));
        lo_b = std::min(lo_b, rgb444_b(c));
        hi_r = std::max(hi_r, rgb444_r(c));
        hi_g = std::max(hi_g, rgb444_g(c));
        hi_b = std::max(hi_b, rgb444_b(c));
    }
    push_unique(rgb444(lo_r, lo_g, lo_b));
    push_unique(rgb444(hi_r, hi_g, hi_b));
    return out;
}

struct LineResult
{
    Rgb444      pal_fg = 0;
    Rgb444      pal_bg = 0;
    EncodedLine encoded;
};

// A cheap greedy walk used only to RANK candidate palette pairs — the winner is
// then re-encoded exactly by encode_line().  Ranking with the exact DP would be
// correct too, just ~30x slower per line for no visible gain.
inline int64_t approx_cost(std::span<const Rgb444> target, Rgb444 pal_fg, Rgb444 pal_bg)
{
    Rgb444  held = pal_fg;
    int64_t cost = 0;
    int     x    = 0;

    while (x < WIDTH)
    {
        const Rgb444 t = target[x];
        const int    e_fg = colour_error(pal_fg, t);
        const int    e_bg = colour_error(pal_bg, t);
        const int    pal_here = std::min(e_fg, e_bg);
        const Rgb444 pal_next = (e_fg <= e_bg) ? pal_fg : pal_bg;

        if (x + 2 > WIDTH)
        {
            cost += pal_here;
            break;
        }

        const Rgb444 t2 = target[x + 1];
        const int64_t pal_two = pal_here + std::min(colour_error(pal_fg, t2),
                                                    colour_error(pal_bg, t2));

        const int here_held = colour_error(held, t);
        int64_t   best      = COST_INF;
        Rgb444    best_next = held;
        for (int code = 0; code < 8; code++)
        {
            const Rgb444  nc = chroma_result(held, code);
            const int64_t c  = here_held + colour_error(nc, t2);
            if (c < best)
            {
                best      = c;
                best_next = nc;
            }
        }

        if (best < pal_two)
        {
            cost += best;
            held = best_next;
            x += 2;
        }
        else
        {
            cost += pal_here;
            held = pal_next;
            x += 1;
        }
    }
    return cost;
}

struct Pair
{
    Rgb444  fg   = 0;
    Rgb444  bg   = 0;
    int64_t rank = 0;
};

// The ranking cost for a DITHERED pair: the distance from each target colour to
// the SEGMENT between the two palette colours, not to the nearer endpoint.  That
// is what dithering actually buys — an oscillation reaches every colour on the
// line between the two slots — and it is why the undithered ranking must not be
// reused here.  approx_cost would rank two centroids above two endpoints on a
// gradient, and the endpoints would never even reach the finals.
//
// Distances are in RGB444 units scaled by 256 so the projection keeps its
// fractional part without floating point.
inline int64_t segment_cost(std::span<const Rgb444> target, Rgb444 pal_fg, Rgb444 pal_bg)
{
    const int ar = rgb444_r(pal_bg);
    const int ag = rgb444_g(pal_bg);
    const int ab = rgb444_b(pal_bg);
    const int dr = rgb444_r(pal_fg) - ar;
    const int dg = rgb444_g(pal_fg) - ag;
    const int db = rgb444_b(pal_fg) - ab;
    const int len2 = dr * dr + dg * dg + db * db;

    int64_t cost = 0;
    for (Rgb444 c : target)
    {
        const int vr = rgb444_r(c) - ar;
        const int vg = rgb444_g(c) - ag;
        const int vb = rgb444_b(c) - ab;

        int t = 0;   // position along the segment, in 1/256ths
        if (len2 > 0)
        {
            t = std::clamp(((vr * dr + vg * dg + vb * db) * 256) / len2, 0, 256);
        }
        const int er = vr * 256 - dr * t;
        const int eg = vg * 256 - dg * t;
        const int eb = vb * 256 - db * t;
        cost += (static_cast<int64_t>(er) * er + static_cast<int64_t>(eg) * eg +
                 static_cast<int64_t>(eb) * eb) /
                (256 * 256);
    }
    return cost;
}

// Every ordered pair of the line's candidate colours (plus the previous line's
// winner, for temporal coherence), ranked and cut to the `finalists` best.
// Ordered, not unordered: held starts the line at pal_fg, so the two slots are
// not interchangeable.  The sort is stable and the ranks are integers, so the
// result is a deterministic function of the line.
//
// `dithered` returns the union of the two rankings' top lists rather than either
// one alone, because neither is right by itself.  approx_cost only ever proposes
// pairs a flat fill can use, so on its own the dither has nothing wide to
// oscillate between; segment_cost only ever proposes the widest pair that covers
// the line, and a wide pair dithers with a wide VARIANCE — sixteen pixels of a
// black/white checkerboard average to the right grey only on average, and the
// wobble is exactly the low-frequency error dithering is supposed to remove.
// The right pair is usually between the two, so both lists go to the final and
// the caller's real objective picks.
inline std::vector<Pair> candidate_pairs(std::span<const Rgb444> target, int k, int finalists,
                                         Rgb444 prev_fg, Rgb444 prev_bg, bool dithered = false)
{
    std::vector<Rgb444> cand = line_candidates(target, k);
    if (cand.size() < 2)
    {
        // Degenerate line (a single colour): any distinct second slot will do.
        cand.push_back(static_cast<Rgb444>(cand.front() ^ 0x001));
    }

    std::vector<Pair> pairs;
    pairs.reserve(cand.size() * cand.size());
    for (Rgb444 fg : cand)
    {
        for (Rgb444 bg : cand)
        {
            if (fg != bg)
            {
                pairs.push_back({fg, bg, 0});
            }
        }
    }
    if (prev_fg != prev_bg)
    {
        pairs.push_back({prev_fg, prev_bg, 0});
    }

    auto take_best = [&](auto &&rank, int count, std::vector<Pair> &into)
    {
        for (Pair &p : pairs)
        {
            p.rank = rank(p.fg, p.bg);
        }
        std::stable_sort(pairs.begin(), pairs.end(),
                         [](const Pair &a, const Pair &b) { return a.rank < b.rank; });
        for (const Pair &p : pairs)
        {
            if (static_cast<int>(into.size()) >= count)
            {
                break;
            }
            const bool seen = std::any_of(into.begin(), into.end(), [&](const Pair &q)
                                          { return q.fg == p.fg && q.bg == p.bg; });
            if (!seen)
            {
                into.push_back(p);
            }
        }
    };

    std::vector<Pair> out;
    take_best([&](Rgb444 fg, Rgb444 bg) { return approx_cost(target, fg, bg); }, finalists, out);
    if (dithered)
    {
        take_best([&](Rgb444 fg, Rgb444 bg) { return segment_cost(target, fg, bg); },
                  2 * finalists, out);
    }
    return out;
}

// Chooses the palette pair by running the exact encoder over the finalists and
// keeping the cheapest.
inline LineResult encode_line_auto(std::span<const Rgb444> target, EncoderScratch &scratch,
                                   int k, int finalists, Rgb444 prev_fg, Rgb444 prev_bg)
{
    const std::vector<Pair> pairs = candidate_pairs(target, k, finalists, prev_fg, prev_bg);

    LineResult  best;
    EncodedLine trial;
    bool        have_best = false;

    for (const Pair &p : pairs)
    {
        encode_line(target, p.fg, p.bg, scratch, trial);
        if (!have_best || trial.cost < best.encoded.cost)
        {
            have_best    = true;
            best.pal_fg  = p.fg;
            best.pal_bg  = p.bg;
            best.encoded = trial;
        }
    }
    return best;
}

// Palette-search effort.  Both the encoder and the round-trip harness read
// these, so the harness always validates exactly what the encoder emits.
inline constexpr int DEFAULT_CANDIDATES = 6;   // k-means clusters per line
inline constexpr int DEFAULT_FINALISTS  = 6;   // pairs given the exact DP

// ---------------------------------------------------------------------------
// Ordered dither
// ---------------------------------------------------------------------------
//
// The obvious ordered dither — add mask * amplitude to every channel, quantize,
// hand the result to the DP — turns out to fix the wrong banding.  It breaks up
// the banding that comes from rounding 8-bit levels to RGB444, which in a format
// with two arbitrary colours per line is the small half of the problem.  The
// large half is the PALETTE gap: pal_fg and pal_bg are frequently four or eight
// RGB444 steps apart, and a perturbation of half a step can never make the DP
// prefer the far one, so the band survives (measured: a fixed +-10 perturbation
// moved the 4x4-box error of a photograph by 4%, while a proper dither moves it
// by a factor of three).
//
// So the perturbation runs ALONG THE PALETTE AXIS.  For a pair (F, B) and a
// threshold t in (-1/2, +1/2), perturbing by t * (F - B) makes the DP's
// nearest-palette test
//
//     pick F  iff  (v - (F+B)/2) . (F-B)  +  t |F-B|^2  >  0
//
// which is exactly "pick F with probability equal to v's position between B and
// F" — textbook ordered dithering against a two-entry palette, with the blue
// noise mask supplying t.  It costs nothing and needs no knob, because the
// amplitude that is correct is the one the palette itself dictates.  It also
// self-limits: a pixel outside the B..F segment has |projection| > 1/2 and never
// flips.  The DP is not modified in any way — it stays the exact shortest path
// for the perturbed target, and chroma codes still win wherever they are better.
//
// --dither-amp adds the isotropic component on top, for the RGB444-quantization
// banding the axis term cannot see (a line whose palette pair is one step apart
// still has to represent everything between the steps).

// Where dithering is switched off, in 8-bit units of local activity: full
// strength below EDGE_GATE_LO, nothing above EDGE_GATE_HI, linear between.
inline constexpr int EDGE_GATE_LO = 64;
inline constexpr int EDGE_GATE_HI = 128;

// How far the palette-axis oscillation may reach, in 8-bit units — six RGB444
// steps, the measured optimum.  See ordered_dither_line.
inline constexpr int ORDERED_AXIS_CAP = 102;

// Local activity: the largest channel difference between this pixel and any
// pixel in a small neighbourhood.  Wide enough (+-3 columns, +-1 row) that a
// gradient slow enough to band still registers, narrow enough that it does not
// smear an edge's veto across half a sprite.
inline int local_activity(std::span<const uint8_t> rgb8, int x, int y)
{
    const size_t here = (static_cast<size_t>(y) * WIDTH + x) * 3;
    int          worst = 0;
    for (int dy = -1; dy <= 1; dy++)
    {
        const int ny = y + dy;
        if (ny < 0 || ny >= HEIGHT)
        {
            continue;
        }
        for (int dx = -3; dx <= 3; dx++)
        {
            const int nx = x + dx;
            if (nx < 0 || nx >= WIDTH)
            {
                continue;
            }
            const size_t there = (static_cast<size_t>(ny) * WIDTH + nx) * 3;
            for (int i = 0; i < 3; i++)
            {
                worst = std::max(worst, std::abs(static_cast<int>(rgb8[here + i]) -
                                                 static_cast<int>(rgb8[there + i])));
            }
        }
    }
    return worst;
}

// The edge gate, one line's worth, as a 0..256 scale on the perturbation.
//
// Dithering only pays for itself where the source is smooth: that is where
// banding can happen and where the eye will average the noise back into the
// colour it wanted.  At an edge there is no band to break, the eye has something
// far more interesting to look at, and the noise is simply noise — an
// antialiased one-pixel column between a tree trunk and the grass turns into a
// ragged red-and-green rattle, and a flat region gains speckle in exchange for
// hiding a quantization error of at most 3%.  So the perturbation fades out as
// the neighbourhood gets busier and switches off entirely at a hard edge, where
// the undithered exact DP was already doing the right thing.
//
// It depends only on the source, so it is computed once per line and shared by
// every candidate palette pair.
inline void ordered_gate_line(std::span<const uint8_t> rgb8, int y, std::span<int> gain)
{
    for (int x = 0; x < WIDTH; x++)
    {
        const int activity = local_activity(rgb8, x, y);
        if (activity >= EDGE_GATE_HI)
        {
            gain[x] = 0;
        }
        else if (activity > EDGE_GATE_LO)
        {
            gain[x] = 256 * (EDGE_GATE_HI - activity) / (EDGE_GATE_HI - EDGE_GATE_LO);
        }
        else
        {
            gain[x] = 256;
        }
    }
}

inline void ordered_dither_line(const uint8_t *src, std::span<const int> gain, int y,
                                int amplitude, Rgb444 pal_fg, Rgb444 pal_bg,
                                std::span<Rgb444> out)
{
    std::array<int, 3> axis{};
    int                widest = 0;
    for (int i = 0; i < 3; i++)
    {
        axis[i] = rgb444_channel_8(pal_fg, i) - rgb444_channel_8(pal_bg, i);
        widest  = std::max(widest, std::abs(axis[i]));
    }

    // Cap on the axis term.  A line is often bimodal rather than smooth — tree
    // trunks on grass — and then the two palette slots are not the ends of a
    // gradient at all but two unrelated colours a long way apart.  Dithering
    // across that distance is arithmetically correct and visually expensive: the
    // oscillation's own variance grows with the gap, and sixteen pixels of a
    // wide alternation average to the right colour with a wobble that can be
    // larger than the band it replaced.  Capping the reach measurably helps
    // (4x4-box error on the ramp, 618 uncapped against 580 at this cap, with the
    // photographs and backdrops flat to within noise across 85..255), so the
    // oscillation is allowed six RGB444 steps and no more.
    if (widest > ORDERED_AXIS_CAP)
    {
        for (int i = 0; i < 3; i++)
        {
            axis[i] = axis[i] * ORDERED_AXIS_CAP / widest;
        }
    }

    for (int x = 0; x < WIDTH; x++)
    {
        const int          t = (ordered_threshold_q8(x, y) * gain[x]) >> 8;   // Q8, +-128
        std::array<int, 3> ch{};
        for (int i = 0; i < 3; i++)
        {
            const int offset = ((axis[i] + amplitude) * t + 128) >> 8;
            ch[i] = quantize_8_to_4(clamp_8(src[static_cast<size_t>(x) * 3 + i] + offset));
        }
        out[x] = rgb444(ch[0], ch[1], ch[2]);
    }
}

// Relative weights of the two halves of the finalist score below.
inline constexpr int64_t SCORE_LOW_WEIGHT  = 4;
inline constexpr int64_t SCORE_HIGH_WEIGHT = 1;

// How the palette pairs are compared once dithering is in play.  The DP's own
// cost cannot do it: each finalist is exact against its OWN perturbed target, so
// the costs are not on the same scale, and a per-pixel comparison against the
// true source would just re-elect the flat fill that dithering exists to
// destroy.  This scores a line the way the eye does — mostly on the error of a
// 4-pixel running mean, with a minority per-pixel term so the search still
// prefers the sharper of two equally-flat-looking candidates.  Everything is in
// 8-bit units, summed over the three channels.
inline int64_t line_score(std::span<const Rgb444> decoded, const uint8_t *src)
{
    std::array<int32_t, 3> sum_dec{};
    std::array<int32_t, 3> sum_src{};
    int64_t                low  = 0;
    int64_t                high = 0;

    constexpr int BOX = 4;
    for (int x = 0; x < WIDTH; x++)
    {
        for (int i = 0; i < 3; i++)
        {
            const int d = rgb444_channel_8(decoded[x], i);
            const int s = src[static_cast<size_t>(x) * 3 + i];
            high += static_cast<int64_t>(d - s) * (d - s);

            sum_dec[i] += d;
            sum_src[i] += s;
            if (x >= BOX)
            {
                sum_dec[i] -= rgb444_channel_8(decoded[x - BOX], i);
                sum_src[i] -= src[static_cast<size_t>(x - BOX) * 3 + i];
            }
            if (x + 1 >= BOX)
            {
                const int64_t e = sum_dec[i] - sum_src[i];
                low += (e * e) / (BOX * BOX);
            }
        }
    }
    return low * SCORE_LOW_WEIGHT + high * SCORE_HIGH_WEIGHT;
}

// ---------------------------------------------------------------------------
// Floyd-Steinberg mode
// ---------------------------------------------------------------------------
//
// The greedy chooser walks the line pixel by pixel and diffuses the residual of
// the colour the hardware will actually SHOW — so a chroma code diffuses the OLD
// held colour on its first pixel and the new colour on its second, latency and
// all.  Chroma codes compete on a two-pixel footing (a chroma pair against the
// best two palette codes) so they win only where they genuinely beat oscillating
// the palette, which keeps them from becoming a dither primitive: they cannot
// dither well anyway, being two pixels wide with one pixel of latency.
//
// Rejected, having been built and measured: an additional veto that allowed a
// chroma code only where it also beat the palette on the UNDIFFUSED source.  It
// does suppress some of the coloured speckle in dark areas, but it costs far
// more than it buys — the 4x4-box error of a photograph went from 199 to 362 —
// and what replaces the speckle is worse to look at, long horizontal dashes
// where the diffusion has no saturated colour left to dump its error into.  The
// two-pixel comparison above is the only restraint chroma gets.
//
// DIRECTION.  Textbook serpentine flips the scan direction on alternate lines.
// micro-HAM cannot flip the CODE walk: held propagates strictly left to right,
// a chroma code's result depends on the held colour arriving from the left, and
// its first pixel shows whatever the pixel to its left showed — so a right-to-
// left chooser cannot even evaluate a chroma candidate, let alone commit one.
// What alternates instead is the diffusion kernel: the down-row weights are
// 3/16, 5/16, 1/16 on even lines and 1/16, 5/16, 3/16 on odd ones.  That is the
// part of serpentine that matters visually — it cancels the consistent
// down-and-right bias that turns FS error into diagonal worms.  The in-line 7/16
// always goes to x+1 because x-1 is already committed.
//
// Everything is fixed point: the value being quantized is carried in Q8 8-bit
// units, so no float rounding decides a code and two runs cannot differ.

inline constexpr int FS_SHIFT = 8;   // fixed-point fraction on 8-bit levels

// Per-pixel residual clamp, in 8-bit units.  Micro-HAM lines have exactly two
// arbitrary colours, so a pixel far outside the pair's span (a red highlight on
// a green line) can carry a residual of nearly full range; diffusing all of it
// smears a halo across the neighbourhood.  Clamping bounds the smear and, with
// it, every accumulator in the row.
inline constexpr int32_t FS_RESIDUAL_LIMIT = 128 << FS_SHIFT;

struct FsScratch
{
    std::vector<int32_t> value;        // WIDTH*3, Q8: source plus the row's inherited error
    std::vector<int32_t> work;         // WIDTH*3, Q8: value as the walk mutates it
    std::vector<int32_t> next;         // (WIDTH+2)*3, Q8 into the next row, at index (x+1)*3
    std::vector<int32_t> best_next;
    std::vector<int32_t> row;          // (WIDTH+2)*3, the committed inherited error
    std::vector<uint8_t> codes;
    std::vector<uint8_t> best_codes;
    std::vector<Rgb444>  shown;        // WIDTH
    std::vector<Rgb444>  best_shown;
    std::vector<Rgb444>  target;       // WIDTH, the row-diffused target, quantized

    void reset()
    {
        value.assign(static_cast<size_t>(WIDTH) * 3, 0);
        work.assign(static_cast<size_t>(WIDTH) * 3, 0);
        next.assign(static_cast<size_t>(WIDTH + 2) * 3, 0);
        best_next.assign(static_cast<size_t>(WIDTH + 2) * 3, 0);
        row.assign(static_cast<size_t>(WIDTH + 2) * 3, 0);
        codes.clear();
        best_codes.clear();
        shown.assign(WIDTH, 0);
        best_shown.assign(WIDTH, 0);
        target.assign(WIDTH, 0);
    }
};

// One greedy pass over a line for a fixed palette pair.  Reads `value`, writes
// `work` (scratch), `next` (the error handed to the following line), `codes` and
// `shown`; returns the summed squared 8-bit error against the target as the walk
// saw it, which is the objective the palette search compares finalists on.
inline int64_t fs_encode_line(const std::vector<int32_t> &value, Rgb444 pal_fg, Rgb444 pal_bg,
                              bool mirror, FsScratch &s, std::vector<int32_t> &next,
                              std::vector<uint8_t> &codes, std::vector<Rgb444> &shown)
{
    s.work = value;
    next.assign(static_cast<size_t>(WIDTH + 2) * 3, 0);
    codes.clear();

    std::vector<int32_t> &work = s.work;

    auto err8 = [](Rgb444 c, const int32_t *v)
    {
        int64_t e = 0;
        for (int i = 0; i < 3; i++)
        {
            const int d = clamp_8((v[i] + (1 << (FS_SHIFT - 1))) >> FS_SHIFT) -
                          rgb444_channel_8(c, i);
            e += static_cast<int64_t>(d) * d;
        }
        return e;
    };

    // The target the next pixel inherits if this pixel shows `c`.
    auto forward = [&work](Rgb444 c, int x, std::array<int32_t, 3> &out)
    {
        for (int i = 0; i < 3; i++)
        {
            int32_t r = work[static_cast<size_t>(x) * 3 + i] - (rgb444_channel_8(c, i) << FS_SHIFT);
            r      = std::clamp(r, -FS_RESIDUAL_LIMIT, FS_RESIDUAL_LIMIT);
            out[i] = work[static_cast<size_t>(x + 1) * 3 + i] + ((r * 7) >> 4);
        }
    };

    auto diffuse = [&](int x, Rgb444 c)
    {
        for (int i = 0; i < 3; i++)
        {
            int32_t r = work[static_cast<size_t>(x) * 3 + i] - (rgb444_channel_8(c, i) << FS_SHIFT);
            r = std::clamp(r, -FS_RESIDUAL_LIMIT, FS_RESIDUAL_LIMIT);
            if (x + 1 < WIDTH)
            {
                work[static_cast<size_t>(x + 1) * 3 + i] += (r * 7) >> 4;
            }
            // next[] is offset by one pixel, so x-1 .. x+1 index x .. x+2.
            next[static_cast<size_t>(x + 0) * 3 + i] += (r * (mirror ? 1 : 3)) >> 4;
            next[static_cast<size_t>(x + 1) * 3 + i] += (r * 5) >> 4;
            next[static_cast<size_t>(x + 2) * 3 + i] += (r * (mirror ? 3 : 1)) >> 4;
        }
    };

    Rgb444  held  = pal_fg;   // held reloads from pal_fg at every line start
    int64_t score = 0;
    int     x     = 0;

    while (x < WIDTH)
    {
        const int32_t *const v0 = &work[static_cast<size_t>(x) * 3];

        const int64_t e_fg    = err8(pal_fg, v0);
        const int64_t e_bg    = err8(pal_bg, v0);
        const bool    fg_wins = (e_fg <= e_bg);
        const Rgb444  pal_c   = fg_wins ? pal_fg : pal_bg;
        const int64_t pal_e   = fg_wins ? e_fg : e_bg;

        bool chroma_wins = false;
        int  chroma_code = 0;

        if (x + 2 <= WIDTH)
        {
            std::array<int32_t, 3> after_pal{};
            std::array<int32_t, 3> after_held{};
            forward(pal_c, x, after_pal);
            forward(held, x, after_held);

            const int64_t pal_two = pal_e + std::min(err8(pal_fg, after_pal.data()),
                                                     err8(pal_bg, after_pal.data()));
            const int64_t e_held  = err8(held, v0);

            int64_t best = pal_two;
            for (int code = 0; code < 8; code++)
            {
                const int64_t c = e_held + err8(chroma_result(held, code), after_held.data());
                if (c < best)
                {
                    best        = c;
                    chroma_code = code;
                    chroma_wins = true;
                }
            }
        }

        if (chroma_wins)
        {
            const Rgb444 first  = held;
            const Rgb444 second = chroma_result(held, chroma_code);
            codes.push_back(static_cast<uint8_t>(8 | chroma_code));

            score += err8(first, &work[static_cast<size_t>(x) * 3]);
            shown[x] = first;
            diffuse(x, first);

            score += err8(second, &work[static_cast<size_t>(x + 1) * 3]);
            shown[x + 1] = second;
            diffuse(x + 1, second);

            held = second;
            x += 2;
        }
        else
        {
            codes.push_back(fg_wins ? 1 : 0);
            score += pal_e;
            shown[x] = pal_c;
            diffuse(x, pal_c);
            held = pal_c;
            x += 1;
        }
    }
    return score;
}

// ---------------------------------------------------------------------------
// Whole-frame encoding
// ---------------------------------------------------------------------------

struct EncodedFrame
{
    std::vector<uint16_t> plane;     // HEIGHT * WORDS_PER_LINE, in host order
    std::vector<Rgb444>   pal_fg;    // HEIGHT
    std::vector<Rgb444>   pal_bg;    // HEIGHT
    std::vector<Rgb444>   decoded;   // WIDTH * HEIGHT, this file's own decode

    // Floyd-Steinberg only: pixels where the greedy walk's idea of the shown
    // colour disagreed with decode_line.  Always zero; a non-zero count means
    // the diffusion was fed a colour the hardware will not show, which is a bug
    // in fs_encode_line, not an illegal stream.
    size_t model_mismatches = 0;
};

// 8-bit RGB triples (stbi's 3-component layout) to the quantized RGB444 frame
// the encoder's error function works in.
inline std::vector<Rgb444> quantize_frame(const unsigned char *rgb8)
{
    std::vector<Rgb444> t(static_cast<size_t>(WIDTH) * HEIGHT);
    for (size_t i = 0; i < t.size(); i++)
    {
        t[i] = rgb444(quantize_8_to_4(rgb8[i * 3 + 0]),
                      quantize_8_to_4(rgb8[i * 3 + 1]),
                      quantize_8_to_4(rgb8[i * 3 + 2]));
    }
    return t;
}

// The other direction, for synthetic RGB444 sources: nibble replication, the
// same expansion the display's DAC performs.
inline std::vector<uint8_t> expand_frame(std::span<const Rgb444> frame)
{
    std::vector<uint8_t> out(frame.size() * 3);
    for (size_t i = 0; i < frame.size(); i++)
    {
        out[i * 3 + 0] = nibble_to_8(rgb444_r(frame[i]));
        out[i * 3 + 1] = nibble_to_8(rgb444_g(frame[i]));
        out[i * 3 + 2] = nibble_to_8(rgb444_b(frame[i]));
    }
    return out;
}

// The encoder takes the 8-BIT source, not a pre-quantized RGB444 frame: both
// dither modes need the sub-step detail that quantizing to RGB444 throws away.
// An 8-bit level of 130 must be able to dither between nibbles 7 and 8; a target
// pre-rounded to 136 could only ever ask for 8.
inline EncodedFrame encode_frame(std::span<const uint8_t> rgb8, const DitherParams &dither = {})
{
    EncodedFrame f;
    f.plane.resize(static_cast<size_t>(HEIGHT) * WORDS_PER_LINE);
    f.pal_fg.resize(HEIGHT);
    f.pal_bg.resize(HEIGHT);
    f.decoded.resize(static_cast<size_t>(HEIGHT) * WIDTH);

    EncoderScratch            scratch;
    FsScratch                 fs;
    std::array<Rgb444, WIDTH> line{};
    std::array<Rgb444, WIDTH> dithered{};
    std::array<int, WIDTH>    gate{};
    EncodedLine               encoded;
    Rgb444                    prev_fg = 0;
    Rgb444                    prev_bg = 0;

    if (dither.mode == Dither::FloydSteinberg)
    {
        fs.reset();
    }

    for (int y = 0; y < HEIGHT; y++)
    {
        const uint8_t *const src = &rgb8[static_cast<size_t>(y) * WIDTH * 3];
        Rgb444               pal_fg = 0;
        Rgb444               pal_bg = 0;

        if (dither.mode == Dither::FloydSteinberg)
        {
            // The row's inherited error goes in BEFORE the palette search, so the
            // palette adapts to what this line is actually being asked for.
            for (int x = 0; x < WIDTH; x++)
            {
                for (int i = 0; i < 3; i++)
                {
                    const size_t k = static_cast<size_t>(x) * 3 + i;
                    fs.value[k] = (static_cast<int32_t>(src[k]) << FS_SHIFT) +
                                  fs.row[static_cast<size_t>(x + 1) * 3 + i];
                }
                fs.target[x] = rgb444(
                    quantize_8_to_4(clamp_8((fs.value[static_cast<size_t>(x) * 3 + 0] + 128) >> 8)),
                    quantize_8_to_4(clamp_8((fs.value[static_cast<size_t>(x) * 3 + 1] + 128) >> 8)),
                    quantize_8_to_4(clamp_8((fs.value[static_cast<size_t>(x) * 3 + 2] + 128) >> 8)));
            }

            const std::vector<Pair> pairs = candidate_pairs(fs.target, DEFAULT_CANDIDATES,
                                                            DEFAULT_FINALISTS, prev_fg, prev_bg);
            const bool              mirror = (y & 1) != 0;

            int64_t best_score = 0;
            bool    have_best  = false;
            for (const Pair &p : pairs)
            {
                const int64_t score = fs_encode_line(fs.value, p.fg, p.bg, mirror, fs, fs.next,
                                                     fs.codes, fs.shown);
                if (!have_best || score < best_score)
                {
                    have_best  = true;
                    best_score = score;
                    pal_fg     = p.fg;
                    pal_bg     = p.bg;
                    fs.best_next  = fs.next;
                    fs.best_codes = fs.codes;
                    fs.best_shown = fs.shown;
                }
            }

            pack_codes(fs.best_codes, pal_fg, pal_bg, encoded);
            fs.row = fs.best_next;

            for (int x = 0; x < WIDTH; x++)
            {
                if (encoded.decoded[x] != fs.best_shown[x])
                {
                    f.model_mismatches++;
                }
            }
        }
        else if (dither.mode == Dither::Ordered)
        {
            // The candidates come from the ISOTROPICALLY dithered line, not the
            // plain one.  A line of a vertical ramp is a single colour, and a
            // single colour proposes a single candidate — which leaves the pair
            // search with nothing to oscillate between and puts the bands
            // straight back.  Dithering first splits that line into the two
            // RGB444 levels it sits between, which is exactly the pair it wants.
            // The axis term cannot help here: it needs the pair it is choosing.
            ordered_gate_line(rgb8, y, gate);
            for (int x = 0; x < WIDTH; x++)
            {
                const int off = (ordered_offset(x, y, dither.amplitude) * gate[x]) >> 8;
                line[x] = rgb444(
                    quantize_8_to_4(clamp_8(src[static_cast<size_t>(x) * 3 + 0] + off)),
                    quantize_8_to_4(clamp_8(src[static_cast<size_t>(x) * 3 + 1] + off)),
                    quantize_8_to_4(clamp_8(src[static_cast<size_t>(x) * 3 + 2] + off)));
            }
            const std::vector<Pair> pairs = candidate_pairs(line, DEFAULT_CANDIDATES,
                                                            DEFAULT_FINALISTS, prev_fg, prev_bg,
                                                            true);

            EncodedLine trial;
            int64_t     best_score = 0;
            bool        have_best  = false;
            for (const Pair &p : pairs)
            {
                ordered_dither_line(src, gate, y, dither.amplitude, p.fg, p.bg, dithered);
                encode_line(dithered, p.fg, p.bg, scratch, trial);
                const int64_t score = line_score(trial.decoded, src);
                if (!have_best || score < best_score)
                {
                    have_best  = true;
                    best_score = score;
                    pal_fg     = p.fg;
                    pal_bg     = p.bg;
                    encoded    = trial;
                }
            }
        }
        else
        {
            for (int x = 0; x < WIDTH; x++)
            {
                line[x] = rgb444(quantize_8_to_4(src[static_cast<size_t>(x) * 3 + 0]),
                                 quantize_8_to_4(src[static_cast<size_t>(x) * 3 + 1]),
                                 quantize_8_to_4(src[static_cast<size_t>(x) * 3 + 2]));
            }

            const LineResult r = encode_line_auto(line, scratch, DEFAULT_CANDIDATES,
                                                  DEFAULT_FINALISTS, prev_fg, prev_bg);
            pal_fg  = r.pal_fg;
            pal_bg  = r.pal_bg;
            encoded = r.encoded;
        }

        prev_fg     = pal_fg;
        prev_bg     = pal_bg;
        f.pal_fg[y] = pal_fg;
        f.pal_bg[y] = pal_bg;
        std::copy(encoded.words.begin(), encoded.words.end(),
                  f.plane.begin() + static_cast<size_t>(y) * WORDS_PER_LINE);
        std::copy(encoded.decoded.begin(), encoded.decoded.end(),
                  f.decoded.begin() + static_cast<size_t>(y) * WIDTH);
    }
    return f;
}

// ---------------------------------------------------------------------------
// Metrics
// ---------------------------------------------------------------------------
//
// Two numbers, because dithering trades one for the other and only one of them
// is about how the picture looks:
//
//   per_pixel_mse  mean squared RGB444 error against the quantized source, the
//                  sum over the three channels.  Dithering makes this WORSE by
//                  construction — it deliberately shows the wrong colour at a
//                  pixel so a neighbourhood averages right.
//   box4_mse       mean squared 8-bit error between 4x4 box means of the decoded
//                  frame and of the 8-BIT source, again summed over channels.
//                  The box is a sliding window over every position, not a block
//                  grid, so a pattern that only averages correctly on a 4-pixel
//                  grid cannot flatter itself.  This is the number dithering is
//                  supposed to improve, and it is measured against the true
//                  8-bit source because RGB444 banding in the source is part of
//                  what dithering exists to hide.

struct FrameMetrics
{
    double per_pixel_mse   = 0;
    double box4_mse        = 0;
    int    max_pixel_error = 0;
};

inline FrameMetrics measure_frame(std::span<const Rgb444> decoded, std::span<const uint8_t> rgb8)
{
    FrameMetrics m;

    int64_t total = 0;
    for (size_t i = 0; i < decoded.size(); i++)
    {
        const Rgb444 t = rgb444(quantize_8_to_4(rgb8[i * 3 + 0]), quantize_8_to_4(rgb8[i * 3 + 1]),
                                quantize_8_to_4(rgb8[i * 3 + 2]));
        const int    e = colour_error(decoded[i], t);
        total += e;
        m.max_pixel_error = std::max(m.max_pixel_error, e);
    }
    m.per_pixel_mse = static_cast<double>(total) / static_cast<double>(decoded.size());

    // Column-summed prefix over 4 rows at a time, then a running 4-wide sum: one
    // pass, no integral image, no overflow worries.
    constexpr int BOX = 4;
    double        acc = 0;
    int64_t       windows = 0;
    std::vector<int32_t> col_dec(static_cast<size_t>(WIDTH) * 3, 0);
    std::vector<int32_t> col_src(static_cast<size_t>(WIDTH) * 3, 0);

    for (int y = 0; y + BOX <= HEIGHT; y++)
    {
        std::fill(col_dec.begin(), col_dec.end(), 0);
        std::fill(col_src.begin(), col_src.end(), 0);
        for (int dy = 0; dy < BOX; dy++)
        {
            const size_t base = static_cast<size_t>(y + dy) * WIDTH;
            for (int x = 0; x < WIDTH; x++)
            {
                const Rgb444 c = decoded[base + x];
                for (int i = 0; i < 3; i++)
                {
                    col_dec[static_cast<size_t>(x) * 3 + i] += rgb444_channel_8(c, i);
                    col_src[static_cast<size_t>(x) * 3 + i] += rgb8[(base + x) * 3 + i];
                }
            }
        }

        std::array<int32_t, 3> sum_dec{};
        std::array<int32_t, 3> sum_src{};
        for (int x = 0; x < WIDTH; x++)
        {
            for (int i = 0; i < 3; i++)
            {
                sum_dec[i] += col_dec[static_cast<size_t>(x) * 3 + i];
                sum_src[i] += col_src[static_cast<size_t>(x) * 3 + i];
                if (x >= BOX)
                {
                    sum_dec[i] -= col_dec[static_cast<size_t>(x - BOX) * 3 + i];
                    sum_src[i] -= col_src[static_cast<size_t>(x - BOX) * 3 + i];
                }
            }
            if (x + 1 >= BOX)
            {
                for (int i = 0; i < 3; i++)
                {
                    const double d = static_cast<double>(sum_dec[i] - sum_src[i]) / (BOX * BOX);
                    acc += d * d;
                }
                windows++;
            }
        }
    }
    m.box4_mse = (windows > 0) ? acc / static_cast<double>(windows) : 0.0;
    return m;
}

// ---------------------------------------------------------------------------
// File formats
// ---------------------------------------------------------------------------
//
// .ham — headerless, HEIGHT lines of WORDS_PER_LINE big-endian 16-bit words, in
//        scanline order.  76800 bytes for 640x480.  This is the byte order of a
//        68000 memory image, so the file can be loaded straight into RAM and
//        DMA'd verbatim by ENGINE; it matches author.cpp's BitPacker writing
//        bus words at ascending byte addresses.
//
// .pal — headerless, HEIGHT pairs of big-endian 16-bit words: pal_fg then
//        pal_bg, one pair per scanline, values are 12-bit R4G4B4 in the low
//        bits.  1920 bytes for 480 lines.  The app turns each pair into two
//        VIDCMD SETs (SET_PIX_PAL_FG = 2, SET_PIX_PAL_BG = 3) issued in that
//        line's horizontal blanking, where a SET costs no active slot.

inline void put_be16(std::vector<uint8_t> &v, uint16_t w)
{
    v.push_back(static_cast<uint8_t>(w >> 8));
    v.push_back(static_cast<uint8_t>(w & 0xFF));
}

inline std::vector<uint8_t> plane_bytes(const EncodedFrame &f)
{
    std::vector<uint8_t> out;
    out.reserve(f.plane.size() * 2);
    for (uint16_t w : f.plane)
    {
        put_be16(out, w);
    }
    return out;
}

inline std::vector<uint8_t> palette_bytes(const EncodedFrame &f)
{
    std::vector<uint8_t> out;
    out.reserve(f.pal_fg.size() * 4);
    for (size_t y = 0; y < f.pal_fg.size(); y++)
    {
        put_be16(out, f.pal_fg[y]);
        put_be16(out, f.pal_bg[y]);
    }
    return out;
}

inline bool write_file(const char *path, const std::vector<uint8_t> &data)
{
    std::FILE *f = std::fopen(path, "wb");
    if (f == nullptr)
    {
        return false;
    }
    const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    std::fclose(f);
    return ok;
}

}  // namespace MicroHam
