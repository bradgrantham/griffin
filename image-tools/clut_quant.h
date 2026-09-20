// clut_quant.h — the palette chooser and the CLUT reload planner, shared
//
// The ONE implementation of the heuristic that turns a line of 8-bit RGB into a
// 16-entry R4G4B4 palette and a bounded set of per-line reloads.  Promoted out
// of clut-experiment/clut_encode.cpp on 2026-09-20, unchanged arithmetic, when
// that experiment's c4h rung became the shipping mode and the tool became
// image-tools/clut_encode.  Both the shipping encoder and the experiment include
// this file, so the sweep's numbers and the pictures on the CRT come from the
// same code.
//
// The heuristic is heuristic: the decoder and the suite model are the truth.
// What this owes its callers is determinism — same input, same bytes, every run.
//
//   line_palette()  farthest-point-seeded weighted k-means in 8-bit RGB over a
//                   line and its neighbours, centroids quantised to R4G4B4,
//                   plus up to EXTRA_CANDIDATES taken from the worst-fit
//                   pixels.  A line whose distinct R4G4B4 colours already fit
//                   the reachable entries takes them verbatim, which is what
//                   makes exact reproduction of a <=16-colour line exact.
//   update_clut()   at most `budget` (slot, colour) replacements chosen
//                   greedily against this line's nearest-entry error, starting
//                   from the palette the previous line left behind; when the
//                   budget could have paid for a wholesale reload, the k-means
//                   palette outright is costed too and taken if it is better.
//   nearest_entry() index assignment, one pixel.

#pragma once

#include "clut_color.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace ClutQuant
{

using ClutColor::Rgb444;
using ClutColor::clamp_8;
using ClutColor::quantize_8_to_4;
using ClutColor::rgb444;
using ClutColor::PaletteLoad;
using ClutColor::rgb444_channel_8;

inline constexpr int CLUT_SIZE = 16;

// Floyd-Steinberg residual arithmetic, in Q8 with a clamp that stops a single
// hard edge from ringing across the whole line.
inline constexpr int FS_SHIFT          = 8;
inline constexpr int FS_RESIDUAL_LIMIT = 128 << FS_SHIFT;

struct FsRows
{
    std::vector<int32_t> current;
    std::vector<int32_t> next;
};

// Squared error between an 8-bit RGB triple and a palette entry.
inline int dist8(const int *value, Rgb444 c)
{
    const int dr = value[0] - rgb444_channel_8(c, 0);
    const int dg = value[1] - rgb444_channel_8(c, 1);
    const int db = value[2] - rgb444_channel_8(c, 2);
    return dr * dr + dg * dg + db * db;
}

inline Rgb444 quantize_colour(int r, int g, int b)
{
    return rgb444(quantize_8_to_4(clamp_8(r)), quantize_8_to_4(clamp_8(g)),
                  quantize_8_to_4(clamp_8(b)));
}

struct Sample
{
    int value[3] = { 0, 0, 0 };
    int weight   = 1;
};

inline std::vector<Rgb444> line_palette(const std::vector<Sample> &samples, const int *line_values,
                                 int width, int wanted)
{
    // Exact path: if the line itself holds no more distinct R4G4B4 colours than
    // the mode can reach, those colours ARE the palette and the line is
    // reproduced exactly.
    {
        std::vector<Rgb444> distinct;
        bool                fits = true;
        for (int x = 0; x < width && fits; x++)
        {
            const Rgb444 c = quantize_colour(line_values[x * 3 + 0], line_values[x * 3 + 1],
                                             line_values[x * 3 + 2]);
            if (std::find(distinct.begin(), distinct.end(), c) == distinct.end())
            {
                distinct.push_back(c);
                fits = static_cast<int>(distinct.size()) <= wanted;
            }
        }
        if (fits)
        {
            std::sort(distinct.begin(), distinct.end());
            while (static_cast<int>(distinct.size()) < wanted)
            {
                distinct.push_back(distinct.back());
            }
            return distinct;
        }
    }

    const int count = static_cast<int>(samples.size());
    std::vector<std::array<double, 3>> centre(static_cast<size_t>(wanted));
    std::vector<int>                   nearest(static_cast<size_t>(count), 0);
    std::vector<int64_t>               far(static_cast<size_t>(count), 0);

    // Seed 0 = weighted mean; each later seed = the sample farthest from the
    // seeds so far.  Deterministic, and it never collapses on flat lines.
    {
        double sum[3]      = { 0.0, 0.0, 0.0 };
        double weight_sum  = 0.0;
        for (const Sample &s : samples)
        {
            for (int i = 0; i < 3; i++)
            {
                sum[i] += static_cast<double>(s.value[i]) * s.weight;
            }
            weight_sum += s.weight;
        }
        for (int i = 0; i < 3; i++)
        {
            centre[0][i] = (weight_sum > 0.0) ? sum[i] / weight_sum : 0.0;
        }
    }
    for (int j = 0; j < count; j++)
    {
        int c[3] = { static_cast<int>(std::lround(centre[0][0])),
                     static_cast<int>(std::lround(centre[0][1])),
                     static_cast<int>(std::lround(centre[0][2])) };
        const int dr = samples[static_cast<size_t>(j)].value[0] - c[0];
        const int dg = samples[static_cast<size_t>(j)].value[1] - c[1];
        const int db = samples[static_cast<size_t>(j)].value[2] - c[2];
        far[static_cast<size_t>(j)] = static_cast<int64_t>(dr * dr + dg * dg + db * db);
    }
    for (int s = 1; s < wanted; s++)
    {
        int     pick   = 0;
        int64_t best   = -1;
        for (int j = 0; j < count; j++)
        {
            const int64_t score =
                far[static_cast<size_t>(j)] * samples[static_cast<size_t>(j)].weight;
            if (score > best)
            {
                best = score;
                pick = j;
            }
        }
        for (int i = 0; i < 3; i++)
        {
            centre[static_cast<size_t>(s)][i] = samples[static_cast<size_t>(pick)].value[i];
        }
        for (int j = 0; j < count; j++)
        {
            const int dr = samples[static_cast<size_t>(j)].value[0]
                           - samples[static_cast<size_t>(pick)].value[0];
            const int dg = samples[static_cast<size_t>(j)].value[1]
                           - samples[static_cast<size_t>(pick)].value[1];
            const int db = samples[static_cast<size_t>(j)].value[2]
                           - samples[static_cast<size_t>(pick)].value[2];
            const int64_t d = static_cast<int64_t>(dr * dr + dg * dg + db * db);
            far[static_cast<size_t>(j)] = std::min(far[static_cast<size_t>(j)], d);
        }
    }

    constexpr int ITERATIONS = 8;
    for (int iteration = 0; iteration < ITERATIONS; iteration++)
    {
        bool moved = false;
        for (int j = 0; j < count; j++)
        {
            int    pick = 0;
            double best = 0.0;
            for (int s = 0; s < wanted; s++)
            {
                double d = 0.0;
                for (int i = 0; i < 3; i++)
                {
                    const double e = samples[static_cast<size_t>(j)].value[i]
                                     - centre[static_cast<size_t>(s)][i];
                    d += e * e;
                }
                if (s == 0 || d < best)
                {
                    best = d;
                    pick = s;
                }
            }
            if (nearest[static_cast<size_t>(j)] != pick)
            {
                nearest[static_cast<size_t>(j)] = pick;
                moved                           = true;
            }
        }
        std::vector<std::array<double, 3>> sum(static_cast<size_t>(wanted), { 0.0, 0.0, 0.0 });
        std::vector<double>                weight(static_cast<size_t>(wanted), 0.0);
        for (int j = 0; j < count; j++)
        {
            const int s = nearest[static_cast<size_t>(j)];
            for (int i = 0; i < 3; i++)
            {
                sum[static_cast<size_t>(s)][i] +=
                    static_cast<double>(samples[static_cast<size_t>(j)].value[i])
                    * samples[static_cast<size_t>(j)].weight;
            }
            weight[static_cast<size_t>(s)] += samples[static_cast<size_t>(j)].weight;
        }
        for (int s = 0; s < wanted; s++)
        {
            if (weight[static_cast<size_t>(s)] > 0.0)
            {
                for (int i = 0; i < 3; i++)
                {
                    centre[static_cast<size_t>(s)][i] =
                        sum[static_cast<size_t>(s)][i] / weight[static_cast<size_t>(s)];
                }
            }
        }
        if (!moved && iteration > 0)
        {
            break;
        }
    }

    std::vector<Rgb444> palette(static_cast<size_t>(wanted));
    for (int s = 0; s < wanted; s++)
    {
        palette[static_cast<size_t>(s)] =
            quantize_colour(static_cast<int>(std::lround(centre[static_cast<size_t>(s)][0])),
                            static_cast<int>(std::lround(centre[static_cast<size_t>(s)][1])),
                            static_cast<int>(std::lround(centre[static_cast<size_t>(s)][2])));
    }

    // Duplicate centroids waste an entry; hand each duplicate to the worst-fit
    // pixel on the line instead.
    for (int s = 0; s < wanted; s++)
    {
        bool duplicate = false;
        for (int t = 0; t < s; t++)
        {
            duplicate = duplicate || palette[static_cast<size_t>(t)] == palette[static_cast<size_t>(s)];
        }
        if (!duplicate)
        {
            continue;
        }
        int worst   = -1;
        int worst_e = -1;
        for (int x = 0; x < width; x++)
        {
            const int *v = &line_values[x * 3];
            int        best = -1;
            for (int t = 0; t < wanted; t++)
            {
                const int d = dist8(v, palette[static_cast<size_t>(t)]);
                if (best < 0 || d < best)
                {
                    best = d;
                }
            }
            const Rgb444 candidate = quantize_colour(v[0], v[1], v[2]);
            const bool   present =
                std::find(palette.begin(), palette.end(), candidate) != palette.end();
            if (!present && best > worst_e)
            {
                worst_e = best;
                worst   = x;
            }
        }
        if (worst >= 0)
        {
            palette[static_cast<size_t>(s)] = quantize_colour(line_values[worst * 3 + 0],
                                                             line_values[worst * 3 + 1],
                                                             line_values[worst * 3 + 2]);
        }
    }
    // Extra candidates from the worst-fit pixels: worth about 2% on both metrics.
    {
        std::vector<std::pair<int,int>> scored;
        for (int x = 0; x < width; x++)
        {
            const int *v = &line_values[x * 3];
            int best = -1;
            for (int t = 0; t < wanted; t++)
            {
                const int d = dist8(v, palette[static_cast<size_t>(t)]);
                if (best < 0 || d < best) { best = d; }
            }
            scored.push_back({best, x});
        }
        std::sort(scored.begin(), scored.end(), [](auto a, auto b){ return a.first > b.first; });
        int added = 0;
        constexpr int EXTRA_CANDIDATES = 8;
        for (size_t i = 0; i < scored.size() && added < EXTRA_CANDIDATES; i++)
        {
            const int x = scored[i].second;
            const Rgb444 c = quantize_colour(line_values[x*3+0], line_values[x*3+1], line_values[x*3+2]);
            if (std::find(palette.begin(), palette.end(), c) == palette.end())
            {
                palette.push_back(c);
                added++;
            }
        }
    }
    return palette;
}

// Replaces at most `budget` of the `usable` reachable slots, greedily, scoring
// against the line's undithered nearest-entry error.  `loads` gets the writes
// the stream must carry.  clut[] is updated in place.
//
// EVERY K goes through this, K = 16 included, and that matters: an earlier
// version reloaded all 16 entries blind from the k-means centroids when it was
// allowed to, and measurably LOST to K = 12 and K = 4 on maya2 (per-pixel 0.72
// against 0.54), because the greedy path is optimising this line's own error
// while the blind reload was only quantising a three-line k-means.  Sweeping K
// is only meaningful if the reload budget is the single thing that changes.
inline void update_clut(Rgb444 *clut, int usable, int budget, const std::vector<Rgb444> &candidates,
                 const int *line_values, int width, std::vector<PaletteLoad> &loads)
{
    loads.clear();
    const int rounds          = std::min(budget, usable);
    const int candidate_count = static_cast<int>(candidates.size());
    std::vector<int> best(static_cast<size_t>(width), 0);
    std::vector<int> second(static_cast<size_t>(width), 0);
    std::vector<int> owner(static_cast<size_t>(width), 0);
    std::vector<int> candidate_distance(static_cast<size_t>(candidate_count) * width, 0);

    for (int c = 0; c < candidate_count; c++)
    {
        for (int x = 0; x < width; x++)
        {
            candidate_distance[static_cast<size_t>(c) * width + x] =
                dist8(&line_values[x * 3], candidates[static_cast<size_t>(c)]);
        }
    }

    auto rescore = [&]()
    {
        for (int x = 0; x < width; x++)
        {
            int b = -1;
            int s = -1;
            int o = 0;
            for (int slot = 0; slot < usable; slot++)
            {
                const int d = dist8(&line_values[x * 3], clut[slot]);
                if (b < 0 || d < b)
                {
                    s = b;
                    b = d;
                    o = slot;
                }
                else if (s < 0 || d < s)
                {
                    s = d;
                }
            }
            best[static_cast<size_t>(x)]   = b;
            second[static_cast<size_t>(x)] = (s < 0) ? b : s;
            owner[static_cast<size_t>(x)]  = o;
        }
    };
    rescore();

    std::vector<bool> written(static_cast<size_t>(usable), false);
    for (int round = 0; round < rounds; round++)
    {
        int64_t best_gain = 0;
        int     best_slot = -1;
        int     best_cand = -1;
        for (int slot = 0; slot < usable; slot++)
        {
            for (int c = 0; c < candidate_count; c++)
            {
                if (candidates[static_cast<size_t>(c)] == clut[slot])
                {
                    continue;
                }
                const int *cd   = &candidate_distance[static_cast<size_t>(c) * width];
                int64_t    gain = 0;
                for (int x = 0; x < width; x++)
                {
                    const int old = best[static_cast<size_t>(x)];
                    const int now = (owner[static_cast<size_t>(x)] == slot)
                                        ? std::min(cd[x], second[static_cast<size_t>(x)])
                                        : std::min(cd[x], old);
                    gain += old - now;
                }
                if (gain > best_gain)
                {
                    best_gain = gain;
                    best_slot = slot;
                    best_cand = c;
                }
            }
        }
        if (best_slot < 0)
        {
            break;
        }
        clut[best_slot] = candidates[static_cast<size_t>(best_cand)];
        written[static_cast<size_t>(best_slot)] = true;
        rescore();
    }

    // When the budget could have paid for a wholesale reload, check whether one
    // would have been better than refining what persisted: the greedy is a
    // local search and a line that has nothing in common with the one above it
    // wants the k-means palette outright.  Without this, K = 16 can lose to
    // K = 4 and the sweep's K column stops meaning anything.
    if (rounds >= usable)
    {
        int64_t greedy_error = 0;
        for (int x = 0; x < width; x++)
        {
            greedy_error += best[static_cast<size_t>(x)];
        }

        int64_t reload_error = 0;
        for (int x = 0; x < width; x++)
        {
            int nearest = -1;
            for (int s = 0; s < usable; s++)
            {
                const int d = candidate_distance[static_cast<size_t>(s) * width + x];
                if (nearest < 0 || d < nearest)
                {
                    nearest = d;
                }
            }
            reload_error += nearest;
        }

        if (reload_error < greedy_error)
        {
            loads.clear();
            for (int s = 0; s < usable; s++)
            {
                const Rgb444 colour = candidates[static_cast<size_t>(s)];
                clut[s]             = colour;
                loads.push_back(PaletteLoad{ static_cast<uint8_t>(s), colour });
            }
            return;
        }
    }

    for (int slot = 0; slot < usable; slot++)
    {
        if (written[static_cast<size_t>(slot)])
        {
            loads.push_back(PaletteLoad{ static_cast<uint8_t>(slot), clut[slot] });
        }
    }
}
// ---------------------------------------------------------------------------
// Index assignment
// ---------------------------------------------------------------------------

inline int nearest_entry(const int *value, const Rgb444 *clut, int usable)
{
    int pick = 0;
    int best = -1;
    for (int s = 0; s < usable; s++)
    {
        const int d = dist8(value, clut[s]);
        if (best < 0 || d < best)
        {
            best = d;
            pick = s;
        }
    }
    return pick;
}

}  // namespace ClutQuant
