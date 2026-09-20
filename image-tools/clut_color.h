// clut_color.h — R4G4B4 colour arithmetic and the blue-noise ordered-dither mask
//
// The ONE home for both, shared by every image tool here and by the CLUT
// experiment in clut-experiment/.  It was split across two files until
// 2026-09-20: the colour helpers lived in clut-experiment/clut_format.h and the
// blue-noise mask in the retired microham.h.  Micro-HAM went with the indexed
// PIXEL (vidcmd-spec.md's 2026-09-20 addendum); the mask is a general-purpose
// dither and outlived it, so it moved here rather than to obsolete/.
//
// Colours are 12-bit R4G4B4 in the low bits of a uint16_t, the packing
// super-engine/descriptor.h uses on the wire.

#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace ClutColor
{

using Rgb444 = uint16_t;

constexpr Rgb444 rgb444(int r, int g, int b)
{
    return static_cast<Rgb444>(((r & 0xF) << 8) | ((g & 0xF) << 4) | (b & 0xF));
}

constexpr int rgb444_r(Rgb444 c) { return (c >> 8) & 0xF; }
constexpr int rgb444_g(Rgb444 c) { return (c >> 4) & 0xF; }
constexpr int rgb444_b(Rgb444 c) { return c & 0xF; }

// A 4-bit field widened to 8 bits by replication, which is what the resistor
// ladder does at the other end of the chain.
constexpr uint8_t nibble_to_8(int nibble)
{
    return static_cast<uint8_t>((nibble & 0xF) * 17);
}

constexpr int quantize_8_to_4(int v)
{
    return (v * 15 + 127) / 255;
}

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

// One palette write a stream must carry: entry `index` takes `colour`.  Here
// rather than in a format header because every format that reloads a palette per
// line carries the same pair, and the planner in clut_quant.h emits it.
struct PaletteLoad
{
    uint8_t index  = 0;
    Rgb444  colour = 0;
};

// ---------------------------------------------------------------------------
// Dither modes and the blue-noise threshold mask
// ---------------------------------------------------------------------------

enum class Dither
{
    None,
    Ordered,
    FloydSteinberg,
};

// The ordered dither's perturbation, in 8-bit units, peak to peak: the mask
// perturbs all three channels by amplitude * threshold, so the perturbation
// spans +-amplitude/2.
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
// same offset on all three channels, so the noise is achromatic and a palette
// built from the dithered line is not asked to oscillate per channel.
inline int ordered_offset(int x, int y, int amplitude)
{
    return (amplitude * ordered_threshold_q8(x, y) + 128) >> 8;
}


}  // namespace ClutColor
