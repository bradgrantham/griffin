// fixup_encode.cpp — a photograph -> a .gcx file: 4bpp half rate with per-line
// palette loads AND COMPOSITOR fix-ups, as a memory image ENGINE can walk
//
// Build: see the Makefile in this directory.
// Usage: fixup_encode [--k 0..16] [--vidcmd-words N] [--fixups on|off]
//                     [--masks on|off] [--rounds N] [--word-cost N] [--flat-max N]
//                     [--dither none|ordered|fs] [--dither-amp N] [--dither-noise PCT]
//                     [--fs-limit N] [--fs-literal N]
//                     [--threads N] [--arbitration4-only] [--preview out.png]
//                     [--fault NAME] <source> <out.gcx>
//
// WHAT IT ADDS TO clut_encode.  The same sixteen-colour half-rate picture and the
// same per-line palette loads (clut_quant.h, unchanged), plus the thing
// clut_encode left to "the display-list planner": the rest of each line's VIDCMD
// budget is spent on COMPOSITOR records that REPLACE the pixels the palette
// serves worst with direct RGB444.
//
//   LITERAL          one word, one 320-density pixel of any colour
//   LITERAL + HOLD   two words, a flat span of three or more pixels
//   MASK             two words, sixteen slots at 640 RESOLUTION, each one
//                    passthrough or one of two held colours the line loads in
//                    its HBLANK -- the only record finer than the 320 grid, and
//                    used only undithered (see mask_setup())
//   RUN(passthrough) one word, a stretch of the indexed picture left alone
//
// Every record covers at least two slots, which is COMPOSITOR's sustained fetch
// cadence, so a line built from these has no seam states: where a record lands is
// the sum of the widths before it, and the planner's costs are the rendered
// costs.  gcx_format.h validates each line against descriptor.h's slot planner
// anyway, and the finished file is rendered through the suite model, with
// delivery timing, before it is written (gcx_verify.h).
//
// THE PLANNER is a dynamic programme per line over (pixel, words spent): the
// cheapest way to tile pixels [0, x) with w words, in two layers -- one whose last
// record is an open passthrough run (extending it is free) and one whose last
// record is a fix-up.  Lines are independent once the palette is fixed, so they
// are planned in parallel.
//
// THE JOINT LOOP.  A pixel a fix-up will cover should not vote for palette
// colours, and a palette load costs the three words three literals would have
// used.  So: plan, mark what the fix-ups cover, rebuild the palette chain from
// the uncovered pixels only, plan again, and keep a round only if the whole
// frame's error fell.
//
// ERROR is squared 8-bit RGB against the source resampled to the 640-wide output
// (ClutSource::resample), summed over the two output pixels of each 320-density
// pixel.
//
// DITHER runs last, serially, over fix-up and passthrough pixels alike: a
// passthrough pixel takes the nearest palette entry to (source + diffused error),
// a literal takes the nearest RGB444, a flat span keeps the planner's colour, and
// every one of them diffuses what is left.
//
// --fault NAME writes a deliberately broken file for `make check`'s negative
// controls and skips this tool's own verification:
//   lookahead0   each line's packet in its own group (late delivery)
//   short-line   a 639-slot line          long-line   a 641-slot line
//   bad-filler   a palette entry whose third word differs
//   stream-end   a line ending in a STREAM RUN ahead of a palette load
//   over-budget  a line group that outruns its scanline

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image.h"
#include "stb/stb_image_write.h"

#include "clut_quant.h"
#include "clut_source.h"
#include "gcx_format.h"
#include "gcx_verify.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace se = SuperEngine;

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
using ClutQuant::FS_SHIFT;
using ClutQuant::FsRows;
using ClutQuant::Sample;
using ClutQuant::dist8;
using ClutQuant::line_palette;
using ClutQuant::nearest_entry;
using ClutQuant::quantize_colour;
using ClutQuant::update_clut;

namespace
{

constexpr int PW = Gcx::PIXEL_WIDTH;
constexpr int PH = Gcx::HEIGHT;
constexpr int OW = Gcx::OUT_WIDTH;

constexpr int MAX_K = CLUT_SIZE;

struct Options
{
    int         k           = 4;
    int         line_words  = Gcx::DEFAULT_LINE_WORDS;
    bool        fixups      = true;
    int         rounds      = 4;
    int         word_cost   = 24;    // squared 8-bit error one word must buy
    int         flat_max    = 64;    // longest LITERAL + HOLD span, in pixels
    int         noise_pct   = -1;    // see dither_noise_percent(); -1 = by dither mode
    int         fs_limit    = 32;    // error diffusion: most a pixel passes on, 8-bit units
    int         fs_literal  = 8;     // and the most diffused error a LITERAL takes in
    int         masks       = -1;    // 1 on, 0 off, -1 = on only when undithered
    Dither      dither      = Dither::FloydSteinberg;
    int         amplitude   = DEFAULT_DITHER_AMPLITUDE;
    int         threads     = 0;
    bool        arb4_only   = false;
    const char *preview     = nullptr;
    const char *fault       = nullptr;
};

using Clut = std::array<Rgb444, CLUT_SIZE>;

enum class SegmentKind : uint8_t
{
    PASS,      // RUN(passthrough): the indexed picture
    LITERAL,   // one exact pixel
    FLAT,      // LITERAL + HOLD: one colour over n >= 3 pixels
    MASK,      // sixteen slots at 640 resolution: passthrough, colour 0 or colour 1
};

constexpr int MASK_PIXELS = static_cast<int>(se::MASK_SLOTS) / 2;   // 8 on the 320 grid
constexpr int MASK_CODE   = 1000;   // the planner's back pointer for a MASK

struct Segment
{
    SegmentKind kind   = SegmentKind::PASS;
    int         x      = 0;
    int         n      = 0;
    Rgb444      colour = 0;   // FLAT only; a LITERAL's colour is the ditherer's
    uint32_t    dibits = 0;   // MASK only: slot i's dibit at bits 2i+1:2i
};

struct LinePlan
{
    std::vector<Segment> segments;
    int64_t              error = 0;   // undithered, summed over the 640 output pixels
    int                  words = 0;   // records only, loads not included

    // A line that uses MASK records loads both held colours in its HBLANK, two
    // words that `words` already counts.
    bool   masks   = false;
    Rgb444 colour0 = 0;
    Rgb444 colour1 = 0;
};

// What a MASK starting at each pixel would cost and show, for one line's pair
// of held colours.
struct MaskSetup
{
    Rgb444                colour0 = 0;
    Rgb444                colour1 = 0;
    std::vector<int64_t>  cost;     // PW entries; valid for x <= PW - MASK_PIXELS
    std::vector<uint32_t> dibits;
};

// Deterministic whatever the thread count: every index is computed
// independently and written to its own slot.
template <typename Fn>
void parallel_for(int count, int threads, Fn fn)
{
    if (threads <= 1 || count <= 1)
    {
        for (int i = 0; i < count; i++)
        {
            fn(i);
        }
        return;
    }
    std::atomic<int>         next{ 0 };
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; t++)
    {
        pool.emplace_back([&]()
        {
            for (;;)
            {
                const int i = next.fetch_add(1);
                if (i >= count)
                {
                    return;
                }
                fn(i);
            }
        });
    }
    for (std::thread &t : pool)
    {
        t.join();
    }
}

// ---------------------------------------------------------------------------
// The palette chain: sixteen entries carried down the frame, at most K replaced
// per line.  With nothing covered this is clut_encode's loop exactly.
//
// `covered` marks pixels a fix-up paints; they are left out of the samples and
// out of the line the replacement is scored against, so the palette is spent on
// the pixels that will actually show it.
// ---------------------------------------------------------------------------

struct Chain
{
    Clut                                  initial{};
    std::vector<Clut>                     clut;    // the palette line y is shown with
    std::vector<std::vector<PaletteLoad>> loads;
};

Chain palette_chain(const std::vector<int> &source, const std::vector<uint8_t> &covered,
                    const Options &options)
{
    Chain chain;
    chain.clut.assign(PH, Clut{});
    chain.loads.assign(PH, {});

    Rgb444 clut[CLUT_SIZE] = { 0 };
    if (options.k == 0)
    {
        const std::vector<Rgb444> global = ClutSource::global_palette(source, PW, PH);
        for (int i = 0; i < CLUT_SIZE; i++)
        {
            clut[i] = global[static_cast<size_t>(i)];
        }
    }

    // Each line's uncovered pixels, compacted, and the candidate colours for it.
    // The candidates depend only on the source and the cover, so they are built
    // in parallel; the chain itself is serial because each line starts from the
    // palette the one above left.
    std::vector<std::vector<int>>    line_values(PH);
    std::vector<std::vector<Rgb444>> candidates(PH);
    if (options.k > 0)
    {
        parallel_for(PH, options.threads, [&](int y)
        {
            std::vector<int> &values = line_values[static_cast<size_t>(y)];
            const int        *row    = &source[static_cast<size_t>(y) * PW * 3];
            for (int x = 0; x < PW; x++)
            {
                if (covered[static_cast<size_t>(y) * PW + x] == 0)
                {
                    values.insert(values.end(), row + x * 3, row + x * 3 + 3);
                }
            }
            if (values.empty())
            {
                return;
            }
            std::vector<Sample> samples;
            for (int dy = -1; dy <= 1; dy++)
            {
                const int ny = y + dy;
                if (ny < 0 || ny >= PH)
                {
                    continue;
                }
                const int *near = &source[static_cast<size_t>(ny) * PW * 3];
                for (int x = 0; x < PW; x++)
                {
                    if (covered[static_cast<size_t>(ny) * PW + x] != 0)
                    {
                        continue;
                    }
                    Sample s;
                    s.value[0] = near[x * 3 + 0];
                    s.value[1] = near[x * 3 + 1];
                    s.value[2] = near[x * 3 + 2];
                    s.weight   = (dy == 0) ? 2 : 1;
                    samples.push_back(s);
                }
            }
            candidates[static_cast<size_t>(y)] =
                line_palette(samples, values.data(), static_cast<int>(values.size() / 3), CLUT_SIZE);
        });
    }

    for (int y = 0; y < PH; y++)
    {
        const std::vector<int> &values = line_values[static_cast<size_t>(y)];
        if (options.k > 0 && !values.empty())
        {
            // Line 0's loads are the preamble's sixteen entries, deposited once
            // in vertical blanking: it gets the whole palette and pays nothing.
            const int budget = (y == 0) ? CLUT_SIZE : options.k;
            update_clut(clut, CLUT_SIZE, budget, candidates[static_cast<size_t>(y)], values.data(),
                        static_cast<int>(values.size() / 3), chain.loads[static_cast<size_t>(y)]);
            if (y == 0)
            {
                chain.loads[0].clear();
            }
        }
        if (y == 0)
        {
            std::copy(clut, clut + CLUT_SIZE, chain.initial.begin());
        }
        std::copy(clut, clut + CLUT_SIZE, chain.clut[static_cast<size_t>(y)].begin());
    }
    return chain;
}

// ---------------------------------------------------------------------------
// The per-line planner
// ---------------------------------------------------------------------------

constexpr int64_t INF = INT64_MAX / 4;

// How much of the dither's own noise the planner charges, in percent.
int dither_noise_percent(const Options &options)
{
    if (options.noise_pct >= 0)
    {
        return options.noise_pct;
    }
    return (options.dither == Dither::None) ? 100 : 50;
}

// Squared distance from a colour to the convex hull of a sixteen-entry palette,
// in 8-bit units, approximated from below by its faces up to triangles: every
// entry, every segment between two, every triangle between three.  A point
// inside a tetrahedron of entries is charged its distance to the nearest face,
// which over-states a little and never under-states.
class Hull
{
public:
    Hull(const Clut &clut, bool build)
    {
        if (!build)
        {
            return;
        }
        // Distinct entries only: a duplicated colour adds no geometry.
        for (Rgb444 c : clut)
        {
            const std::array<double, 3> p = { static_cast<double>(rgb444_channel_8(c, 0)),
                                              static_cast<double>(rgb444_channel_8(c, 1)),
                                              static_cast<double>(rgb444_channel_8(c, 2)) };
            if (std::find(points_.begin(), points_.end(), p) == points_.end())
            {
                points_.push_back(p);
            }
        }
        const size_t n = points_.size();
        for (size_t i = 0; i < n; i++)
        {
            for (size_t j = i + 1; j < n; j++)
            {
                for (size_t k = j + 1; k < n; k++)
                {
                    Triangle t;
                    t.a = i;
                    for (int c = 0; c < 3; c++)
                    {
                        t.u[c] = points_[j][c] - points_[i][c];
                        t.v[c] = points_[k][c] - points_[i][c];
                    }
                    t.uu = dot(t.u, t.u);
                    t.uv = dot(t.u, t.v);
                    t.vv = dot(t.v, t.v);
                    t.det = t.uu * t.vv - t.uv * t.uv;
                    if (t.det > 1e-6)
                    {
                        triangles_.push_back(t);
                    }
                }
            }
        }
    }

    double distance2(const double *p) const
    {
        const size_t n    = points_.size();
        double       best = 1e30;
        for (size_t i = 0; i < n; i++)
        {
            double d = 0.0;
            for (int c = 0; c < 3; c++)
            {
                d += (p[c] - points_[i][c]) * (p[c] - points_[i][c]);
            }
            best = std::min(best, d);
        }
        for (size_t i = 0; i < n; i++)
        {
            for (size_t j = i + 1; j < n; j++)
            {
                double w[3];
                double e[3];
                for (int c = 0; c < 3; c++)
                {
                    w[c] = p[c] - points_[i][c];
                    e[c] = points_[j][c] - points_[i][c];
                }
                const double t = dot(w, e) / dot(e, e);
                if (t <= 0.0 || t >= 1.0)
                {
                    continue;
                }
                double d = 0.0;
                for (int c = 0; c < 3; c++)
                {
                    d += (w[c] - t * e[c]) * (w[c] - t * e[c]);
                }
                best = std::min(best, d);
            }
        }
        for (const Triangle &t : triangles_)
        {
            double w[3];
            for (int c = 0; c < 3; c++)
            {
                w[c] = p[c] - points_[t.a][c];
            }
            const double wu = dot(w, t.u);
            const double wv = dot(w, t.v);
            const double s  = (wu * t.vv - wv * t.uv) / t.det;
            const double r  = (wv * t.uu - wu * t.uv) / t.det;
            if (s <= 0.0 || r <= 0.0 || s + r >= 1.0)
            {
                continue;
            }
            double d = 0.0;
            for (int c = 0; c < 3; c++)
            {
                const double off = w[c] - s * t.u[c] - r * t.v[c];
                d += off * off;
            }
            best = std::min(best, d);
        }
        return best;
    }

private:
    struct Triangle
    {
        size_t a = 0;
        double u[3] = {};
        double v[3] = {};
        double uu = 0.0, uv = 0.0, vv = 0.0, det = 0.0;
    };

    static double dot(const double *a, const double *b)
    {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    }

    std::vector<std::array<double, 3>> points_;
    std::vector<Triangle>              triangles_;
};

// `wide` is the line's 640 target pixels, 8-bit RGB.  `budget` is the words the
// records may use, at least 1.
struct PixelCosts
{
    std::vector<int64_t> pass;
    std::vector<int64_t> literal;
};

PixelCosts pixel_costs(const int *wide, const Clut &clut, const Options &options)
{
    // Per 320-density pixel: the indexed picture's error and a literal's.
    //
    // UNDITHERED, a passthrough pixel's error is its distance to the nearest
    // palette entry.  DITHERED, error diffusion can show any colour INSIDE THE
    // PALETTE'S HULL as a mixture, so what it cannot repair is the distance to
    // the hull, and the rest -- nearest minus hull -- comes back as dither noise
    // rather than as a wrong colour.  noise_pct is how much of that noise is
    // charged: 100 is the undithered cost, 0 trusts the dither completely.  The
    // same factor scales a literal's quantisation error, which diffuses too.
    const int            noise = dither_noise_percent(options);
    const Hull           hull(clut, noise < 100);
    PixelCosts           costs;
    costs.pass.assign(PW, 0);
    costs.literal.assign(PW, 0);
    std::vector<int64_t> &pass_cost    = costs.pass;
    std::vector<int64_t> &literal_cost = costs.literal;
    for (int x = 0; x < PW; x++)
    {
        const int *a = &wide[(x * 2) * 3];
        const int *b = &wide[(x * 2 + 1) * 3];
        int64_t    best = INF;
        for (int e = 0; e < CLUT_SIZE; e++)
        {
            const int64_t d = dist8(a, clut[static_cast<size_t>(e)])
                              + dist8(b, clut[static_cast<size_t>(e)]);
            best = std::min(best, d);
        }
        const Rgb444 q = quantize_colour((a[0] + b[0] + 1) / 2, (a[1] + b[1] + 1) / 2,
                                         (a[2] + b[2] + 1) / 2);
        const int64_t literal = dist8(a, q) + dist8(b, q);
        if (noise >= 100)
        {
            pass_cost[static_cast<size_t>(x)]    = best;
            literal_cost[static_cast<size_t>(x)] = literal;
            continue;
        }
        // On the pair mean: both output pixels show the same colour, so their
        // difference from the mean is a cost no choice here changes.
        const double mean[3] = { (a[0] + b[0]) * 0.5, (a[1] + b[1]) * 0.5, (a[2] + b[2]) * 0.5 };
        int64_t      spread  = 0;
        for (int c = 0; c < 3; c++)
        {
            spread += static_cast<int64_t>(a[c] - b[c]) * (a[c] - b[c]);
        }
        spread /= 2;
        const int64_t out_of_hull = static_cast<int64_t>(2.0 * hull.distance2(mean) + 0.5);
        const int64_t near_mean   = std::max<int64_t>(best - spread, out_of_hull);
        const int64_t lit_mean    = std::max<int64_t>(literal - spread, 0);
        pass_cost[static_cast<size_t>(x)] =
            spread + out_of_hull + (near_mean - out_of_hull) * noise / 100;
        literal_cost[static_cast<size_t>(x)] = spread + lit_mean * noise / 100;
    }
    return costs;
}

LinePlan plan_records(const int *wide, const PixelCosts &costs, int budget, const Options &options,
                      const MaskSetup *mask)
{
    const std::vector<int64_t> &pass_cost    = costs.pass;
    const std::vector<int64_t> &literal_cost = costs.literal;

    LinePlan plan;
    if (!options.fixups || budget < 2)
    {
        plan.segments.push_back(Segment{ SegmentKind::PASS, 0, PW, 0 });
        plan.words = 1;
        for (int64_t c : pass_cost)
        {
            plan.error += c;
        }
        return plan;
    }

    // Prefix sums over the 640 output pixels, for a flat span's error in O(1):
    // sum |p - c|^2 = sum |p|^2 - 2 c . sum p + n |c|^2.
    std::vector<int64_t> sum(static_cast<size_t>(OW + 1) * 3, 0);
    std::vector<int64_t> square(static_cast<size_t>(OW + 1), 0);
    for (int i = 0; i < OW; i++)
    {
        int64_t sq = 0;
        for (int c = 0; c < 3; c++)
        {
            const int64_t v = wide[i * 3 + c];
            sum[static_cast<size_t>(i + 1) * 3 + c] = sum[static_cast<size_t>(i) * 3 + c] + v;
            sq += v * v;
        }
        square[static_cast<size_t>(i + 1)] = square[static_cast<size_t>(i)] + sq;
    }
    auto flat = [&](int x, int n, Rgb444 &colour) -> int64_t
    {
        const int     lo    = x * 2;
        const int     hi    = (x + n) * 2;
        const int64_t count = hi - lo;
        int64_t       s[3];
        int           mean[3];
        for (int c = 0; c < 3; c++)
        {
            s[c]    = sum[static_cast<size_t>(hi) * 3 + c] - sum[static_cast<size_t>(lo) * 3 + c];
            mean[c] = static_cast<int>((s[c] + count / 2) / count);
        }
        colour       = quantize_colour(mean[0], mean[1], mean[2]);
        int64_t cost = square[static_cast<size_t>(hi)] - square[static_cast<size_t>(lo)];
        for (int c = 0; c < 3; c++)
        {
            const int64_t v = rgb444_channel_8(colour, c);
            cost += -2 * v * s[c] + count * v * v;
        }
        return cost;
    };

    // pass[x][w]: pixels [0,x) tiled with w words, the last record an open
    // passthrough run.  fix[x][w]: the last record a fix-up (or nothing yet).
    const int    words  = budget;
    const size_t stride = static_cast<size_t>(words) + 1;
    std::vector<int64_t> pass(static_cast<size_t>(PW + 1) * stride, INF);
    std::vector<int64_t> fix(static_cast<size_t>(PW + 1) * stride, INF);
    // Back pointers.  pass: 0 = extended the run, 1 = opened it.  fix: the
    // record's width in pixels (1 = LITERAL), negative if it follows a pass run.
    std::vector<int8_t>  pass_from(static_cast<size_t>(PW + 1) * stride, 0);
    std::vector<int16_t> fix_from(static_cast<size_t>(PW + 1) * stride, 0);

    fix[0] = 0;
    const int flat_max = std::max(3, options.flat_max);
    for (int x = 1; x <= PW; x++)
    {
        const int64_t pc = pass_cost[static_cast<size_t>(x - 1)];
        const int64_t lc = literal_cost[static_cast<size_t>(x - 1)];
        for (int w = 0; w <= words; w++)
        {
            const size_t at   = static_cast<size_t>(x) * stride + static_cast<size_t>(w);
            const size_t prev = static_cast<size_t>(x - 1) * stride + static_cast<size_t>(w);

            int64_t best_pass = (pass[prev] < INF) ? pass[prev] + pc : INF;
            int8_t  from_pass = 0;
            if (w >= 1 && fix[prev - 1] < INF && fix[prev - 1] + pc < best_pass)
            {
                best_pass = fix[prev - 1] + pc;
                from_pass = 1;
            }
            pass[at]      = best_pass;
            pass_from[at] = from_pass;

            int64_t best_fix = INF;
            int16_t from_fix = 0;
            if (w >= 1)
            {
                if (fix[prev - 1] < INF && fix[prev - 1] + lc < best_fix)
                {
                    best_fix = fix[prev - 1] + lc;
                    from_fix = 1;
                }
                if (pass[prev - 1] < INF && pass[prev - 1] + lc < best_fix)
                {
                    best_fix = pass[prev - 1] + lc;
                    from_fix = -1;
                }
            }
            if (w >= 2)
            {
                const int longest = std::min(flat_max, x);
                for (int n = 3; n <= longest; n++)
                {
                    const size_t  back = static_cast<size_t>(x - n) * stride
                                         + static_cast<size_t>(w - 2);
                    const int64_t under = std::min(fix[back], pass[back]);
                    if (under >= INF || under >= best_fix)
                    {
                        continue;
                    }
                    Rgb444        colour = 0;
                    const int64_t cost   = under + flat(x - n, n, colour);
                    if (cost < best_fix)
                    {
                        best_fix = cost;
                        from_fix = static_cast<int16_t>((fix[back] <= pass[back]) ? n : -n);
                    }
                }
            }
            if (mask != nullptr && w >= 2 && x >= MASK_PIXELS)
            {
                const size_t  back  = static_cast<size_t>(x - MASK_PIXELS) * stride
                                      + static_cast<size_t>(w - 2);
                const int64_t under = std::min(fix[back], pass[back]);
                if (under < INF
                    && under + mask->cost[static_cast<size_t>(x - MASK_PIXELS)] < best_fix)
                {
                    best_fix = under + mask->cost[static_cast<size_t>(x - MASK_PIXELS)];
                    from_fix = static_cast<int16_t>((fix[back] <= pass[back]) ? MASK_CODE
                                                                              : -MASK_CODE);
                }
            }
            fix[at]      = best_fix;
            fix_from[at] = from_fix;
        }
    }

    // The cheapest finish, a word priced at word_cost so a fix-up that buys
    // nothing is not emitted.  Ties go to fewer words.
    int     best_w    = -1;
    bool    best_pass = false;
    int64_t best      = INF;
    for (int w = 1; w <= words; w++)
    {
        const size_t  at = static_cast<size_t>(PW) * stride + static_cast<size_t>(w);
        const int64_t price = static_cast<int64_t>(options.word_cost) * w;
        if (pass[at] < INF && pass[at] + price < best)
        {
            best      = pass[at] + price;
            best_w    = w;
            best_pass = true;
        }
        if (fix[at] < INF && fix[at] + price < best)
        {
            best      = fix[at] + price;
            best_w    = w;
            best_pass = false;
        }
    }

    plan.words = best_w;
    plan.error = best - static_cast<int64_t>(options.word_cost) * best_w;

    int  x       = PW;
    int  w       = best_w;
    bool in_pass = best_pass;
    while (x > 0)
    {
        const size_t at = static_cast<size_t>(x) * stride + static_cast<size_t>(w);
        if (in_pass)
        {
            int end = x;
            while (pass_from[static_cast<size_t>(x) * stride + static_cast<size_t>(w)] == 0)
            {
                x--;
            }
            x--;
            w--;
            plan.segments.push_back(Segment{ SegmentKind::PASS, x, end - x, 0 });
            in_pass = false;
        }
        else
        {
            const int from = fix_from[at];
            int       n    = (from < 0) ? -from : from;
            if (n == MASK_CODE)
            {
                n = MASK_PIXELS;
                Segment m{ SegmentKind::MASK, x - n, n, 0 };
                m.dibits = mask->dibits[static_cast<size_t>(x - n)];
                plan.segments.push_back(m);
                plan.masks = true;
                w -= 2;
            }
            else if (n == 1)
            {
                plan.segments.push_back(Segment{ SegmentKind::LITERAL, x - 1, 1, 0 });
                w -= 1;
            }
            else
            {
                Rgb444 colour = 0;
                flat(x - n, n, colour);
                plan.segments.push_back(Segment{ SegmentKind::FLAT, x - n, n, colour });
                w -= 2;
            }
            x -= n;
            in_pass = from < 0;
        }
    }
    std::reverse(plan.segments.begin(), plan.segments.end());
    return plan;
}

// The pair of held colours a line's MASK records choose between, and each
// possible record's cost.
//
// A MASK is the one record finer than the 320 grid: each of its sixteen slots is
// passthrough, colour 0 or colour 1, and slot 0 is always colour 0.  So the two
// colours should be the ones the line's WORST-SERVED output pixels want, and
// they are found by a two-centre k-means over the 640 output pixels weighted by
// what passthrough gets wrong.
MaskSetup mask_setup(const int *wide, const Clut &clut, const PixelCosts &costs, bool discounted)
{
    // Each output pixel's passthrough error: its group shows the entry nearest
    // the pair, which is what the pixel plane will hold undithered.
    std::vector<int64_t> slot_error(OW, 0);
    for (int x = 0; x < PW; x++)
    {
        const int *a    = &wide[(x * 2) * 3];
        const int *b    = &wide[(x * 2 + 1) * 3];
        int64_t    best = INF;
        size_t     pick = 0;
        for (size_t e = 0; e < CLUT_SIZE; e++)
        {
            const int64_t d = dist8(a, clut[e]) + dist8(b, clut[e]);
            if (d < best)
            {
                best = d;
                pick = e;
            }
        }
        slot_error[static_cast<size_t>(x) * 2]     = dist8(a, clut[pick]);
        slot_error[static_cast<size_t>(x) * 2 + 1] = dist8(b, clut[pick]);
    }

    // Seeds: the worst-served pixel, then the pixel that is badly served AND
    // far from the first.
    double centre[2][3] = {};
    {
        int first = 0;
        for (int i = 1; i < OW; i++)
        {
            if (slot_error[static_cast<size_t>(i)] > slot_error[static_cast<size_t>(first)])
            {
                first = i;
            }
        }
        int     second = first;
        int64_t far    = -1;
        for (int i = 0; i < OW; i++)
        {
            int64_t d = 0;
            for (int c = 0; c < 3; c++)
            {
                const int64_t e = wide[i * 3 + c] - wide[first * 3 + c];
                d += e * e;
            }
            const int64_t score = std::min(d, slot_error[static_cast<size_t>(i)] * 4);
            if (score > far)
            {
                far    = score;
                second = i;
            }
        }
        for (int c = 0; c < 3; c++)
        {
            centre[0][c] = wide[first * 3 + c];
            centre[1][c] = wide[second * 3 + c];
        }
    }
    for (int iteration = 0; iteration < 6; iteration++)
    {
        double sum[2][3]  = {};
        double weight[2]  = {};
        for (int i = 0; i < OW; i++)
        {
            double d[2] = { 0.0, 0.0 };
            for (int k = 0; k < 2; k++)
            {
                for (int c = 0; c < 3; c++)
                {
                    const double e = wide[i * 3 + c] - centre[k][c];
                    d[k] += e * e;
                }
            }
            const int k = (d[1] < d[0]) ? 1 : 0;
            // Only a pixel a held colour would serve BETTER than passthrough
            // pulls on a centre, by as much as it would gain.
            const double gain = static_cast<double>(slot_error[static_cast<size_t>(i)]) - d[k];
            if (gain <= 0.0)
            {
                continue;
            }
            for (int c = 0; c < 3; c++)
            {
                sum[k][c] += gain * wide[i * 3 + c];
            }
            weight[k] += gain;
        }
        for (int k = 0; k < 2; k++)
        {
            if (weight[k] > 0.0)
            {
                for (int c = 0; c < 3; c++)
                {
                    centre[k][c] = sum[k][c] / weight[k];
                }
            }
        }
    }

    MaskSetup setup;
    setup.colour0 = quantize_colour(static_cast<int>(centre[0][0] + 0.5),
                                    static_cast<int>(centre[0][1] + 0.5),
                                    static_cast<int>(centre[0][2] + 0.5));
    setup.colour1 = quantize_colour(static_cast<int>(centre[1][0] + 0.5),
                                    static_cast<int>(centre[1][1] + 0.5),
                                    static_cast<int>(centre[1][2] + 0.5));
    setup.cost.assign(PW, INF);
    setup.dibits.assign(PW, 0);

    std::vector<int64_t> d0(OW, 0);
    std::vector<int64_t> d1(OW, 0);
    for (int i = 0; i < OW; i++)
    {
        d0[static_cast<size_t>(i)] = dist8(&wide[i * 3], setup.colour0);
        d1[static_cast<size_t>(i)] = dist8(&wide[i * 3], setup.colour1);
    }
    for (int x = 0; x + MASK_PIXELS <= PW; x++)
    {
        int64_t  cost   = 0;
        uint32_t dibits = 0;
        for (int j = 0; j < static_cast<int>(se::MASK_SLOTS); j++)
        {
            const size_t i     = static_cast<size_t>(x) * 2 + static_cast<size_t>(j);
            int64_t      best  = d0[i];
            uint32_t     dibit = se::MASK_DIBIT_COLOR0;
            if (j > 0)
            {
                if (d1[i] < best)
                {
                    best  = d1[i];
                    dibit = se::MASK_DIBIT_COLOR1;
                }
                // A passthrough slot is charged what the planner charges the
                // pixel elsewhere (half of it: a pixel is two slots), so that
                // under dither a mask competes with the dithered picture and
                // not with the undithered one.
                const int64_t through = discounted
                    ? std::min(slot_error[i],
                               costs.pass[static_cast<size_t>(x) + static_cast<size_t>(j) / 2] / 2)
                    : slot_error[i];
                if (through <= best)
                {
                    best  = through;
                    dibit = se::MASK_DIBIT_PASSTHROUGH;
                }
            }
            cost += best;
            dibits |= dibit << (2 * j);
        }
        setup.cost[static_cast<size_t>(x)]   = cost;
        setup.dibits[static_cast<size_t>(x)] = dibits;
    }
    return setup;
}

// One line: planned without MASK records, and with them when the two words
// their colours cost leave room, keeping whichever is cheaper.
LinePlan plan_line(const int *wide, const Clut &clut, int budget, const Options &options)
{
    const PixelCosts costs = pixel_costs(wide, clut, options);
    LinePlan         plain = plan_records(wide, costs, budget, options, nullptr);
    if (!options.fixups || options.masks == 0 || budget < 5)
    {
        return plain;
    }
    const MaskSetup setup  = mask_setup(wide, clut, costs, dither_noise_percent(options) < 100);
    LinePlan        masked = plan_records(wide, costs, budget - 2, options, &setup);
    if (!masked.masks)
    {
        return plain;
    }
    masked.words  += 2;
    masked.colour0 = setup.colour0;
    masked.colour1 = setup.colour1;
    const int64_t price = options.word_cost;
    return (masked.error + price * masked.words < plain.error + price * plain.words) ? masked
                                                                                     : plain;
}

struct FramePlan
{
    Chain                 chain;
    std::vector<LinePlan> lines;
    int64_t               error = 0;
};

FramePlan plan_frame(const std::vector<int> &wide, const std::vector<int> &source,
                     const std::vector<uint8_t> &covered, const Options &options)
{
    FramePlan frame;
    frame.chain = palette_chain(source, covered, options);
    frame.lines.assign(PH, LinePlan());
    parallel_for(PH, options.threads, [&](int y)
    {
        const int loads  = static_cast<int>(frame.chain.loads[static_cast<size_t>(y)].size());
        const int budget = std::max(1, options.line_words - 3 * loads);
        frame.lines[static_cast<size_t>(y)] =
            plan_line(&wide[static_cast<size_t>(y) * OW * 3],
                      frame.chain.clut[static_cast<size_t>(y)], budget, options);
    });
    for (const LinePlan &line : frame.lines)
    {
        frame.error += line.error;
    }
    return frame;
}

std::vector<uint8_t> cover_of(const FramePlan &frame)
{
    std::vector<uint8_t> covered(static_cast<size_t>(PW) * PH, 0);
    for (int y = 0; y < PH; y++)
    {
        for (const Segment &s : frame.lines[static_cast<size_t>(y)].segments)
        {
            if (s.kind == SegmentKind::LITERAL || s.kind == SegmentKind::FLAT)
            {
                std::fill_n(covered.begin() + static_cast<long>(y) * PW + s.x, s.n, 1);
            }
        }
    }
    return covered;
}

bool parse_dither(const char *value, Dither &out)
{
    if (std::strcmp(value, "none") == 0)
    {
        out = Dither::None;
    }
    else if (std::strcmp(value, "ordered") == 0)
    {
        out = Dither::Ordered;
    }
    else if (std::strcmp(value, "fs") == 0)
    {
        out = Dither::FloydSteinberg;
    }
    else
    {
        return false;
    }
    return true;
}

void usage(const char *self)
{
    std::fprintf(stderr,
                 "usage: %s [--k 0..%d] [--vidcmd-words N] [--fixups on|off] [--masks on|off]\n"
                 "          [--rounds N] [--word-cost N] [--flat-max N]\n"
                 "          [--dither none|ordered|fs] [--dither-amp N] [--dither-noise PCT]\n"
                 "          [--fs-limit N] [--fs-literal N]\n"
                 "          [--threads N] [--arbitration4-only] [--preview out.png]\n"
                 "          [--fault NAME] <source> <out.gcx>\n",
                 self, MAX_K);
}

}   // namespace

int main(int argc, char **argv)
{
    Options options;
    int     arg = 1;

    while (arg < argc && std::strncmp(argv[arg], "--", 2) == 0)
    {
        const char *const name  = argv[arg];
        const char *const value = (arg + 1 < argc) ? argv[arg + 1] : nullptr;
        if (std::strcmp(name, "--arbitration4-only") == 0)
        {
            options.arb4_only = true;
            arg += 1;
            continue;
        }
        if (value == nullptr)
        {
            usage(argv[0]);
            return 1;
        }
        if (std::strcmp(name, "--k") == 0)
        {
            options.k = std::atoi(value);
        }
        else if (std::strcmp(name, "--vidcmd-words") == 0)
        {
            options.line_words = std::atoi(value);
        }
        else if (std::strcmp(name, "--fixups") == 0)
        {
            options.fixups = std::strcmp(value, "off") != 0;
        }
        else if (std::strcmp(name, "--rounds") == 0)
        {
            options.rounds = std::atoi(value);
        }
        else if (std::strcmp(name, "--word-cost") == 0)
        {
            options.word_cost = std::atoi(value);
        }
        else if (std::strcmp(name, "--flat-max") == 0)
        {
            options.flat_max = std::atoi(value);
        }
        else if (std::strcmp(name, "--masks") == 0)
        {
            options.masks = (std::strcmp(value, "off") != 0) ? 1 : 0;
        }
        else if (std::strcmp(name, "--fs-limit") == 0)
        {
            options.fs_limit = std::atoi(value);
        }
        else if (std::strcmp(name, "--fs-literal") == 0)
        {
            options.fs_literal = std::atoi(value);
        }
        else if (std::strcmp(name, "--dither-noise") == 0)
        {
            options.noise_pct = std::atoi(value);
        }
        else if (std::strcmp(name, "--dither") == 0)
        {
            if (!parse_dither(value, options.dither))
            {
                std::fprintf(stderr, "%s: unknown dither mode %s\n", argv[0], value);
                return 1;
            }
        }
        else if (std::strcmp(name, "--dither-amp") == 0)
        {
            options.amplitude = std::atoi(value);
        }
        else if (std::strcmp(name, "--threads") == 0)
        {
            options.threads = std::atoi(value);
        }
        else if (std::strcmp(name, "--preview") == 0)
        {
            options.preview = value;
        }
        else if (std::strcmp(name, "--fault") == 0)
        {
            options.fault = value;
        }
        else
        {
            std::fprintf(stderr, "%s: unknown option %s\n", argv[0], name);
            return 1;
        }
        arg += 2;
    }

    if (argc - arg != 2)
    {
        usage(argv[0]);
        return 1;
    }
    if (options.k < 0 || options.k > MAX_K)
    {
        std::fprintf(stderr, "%s: K must be 0..%d\n", argv[0], MAX_K);
        return 1;
    }
    if (options.line_words < 3 * options.k + 1
        || options.line_words + Gcx::PREAMBLE_WORDS > static_cast<int>(se::VIDCMD_FIFO_WORDS))
    {
        std::fprintf(stderr, "%s: --vidcmd-words must hold K loads and one record, and fit the "
                             "FIFO beside the preamble (%d..%d)\n",
                     argv[0], 3 * options.k + 1,
                     static_cast<int>(se::VIDCMD_FIFO_WORDS) - Gcx::PREAMBLE_WORDS);
        return 1;
    }
    if (options.masks < 0)
    {
        // MASK slots show one of two fixed colours and cannot take part in error
        // diffusion; measured on the reference photographs they cut undithered
        // error and RAISE dithered error, so dither turns them off by default.
        options.masks = (options.dither == Dither::None) ? 1 : 0;
    }
    if (options.threads <= 0)
    {
        options.threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    }

    const char *const in_path  = argv[arg];
    const char *const out_path = argv[arg + 1];

    std::vector<uint8_t> src;
    int                  sw = 0;
    int                  sh = 0;
    if (!ClutSource::load_source(in_path, src, sw, sh))
    {
        std::fprintf(stderr, "%s: cannot read %s (expected PNG or binary PPM)\n", argv[0], in_path);
        return 1;
    }

    // The 640-wide target, and its pair means: the 320 pixels the mode samples.
    const std::vector<int> wide = ClutSource::resample(src, sw, sh, OW, PH);
    std::vector<int>       source(static_cast<size_t>(PW) * PH * 3, 0);
    for (int y = 0; y < PH; y++)
    {
        for (int x = 0; x < PW; x++)
        {
            for (int i = 0; i < 3; i++)
            {
                const size_t a = (static_cast<size_t>(y) * OW + x * 2) * 3 + i;
                source[(static_cast<size_t>(y) * PW + x) * 3 + i] = (wide[a] + wide[a + 3] + 1) / 2;
            }
        }
    }

    // Plan, then let the fix-ups' cover reshape the palette for as long as the
    // frame's error keeps falling.
    std::vector<uint8_t> covered(static_cast<size_t>(PW) * PH, 0);
    FramePlan            frame = plan_frame(wide, source, covered, options);
    const int64_t        first_error = frame.error;
    int                  rounds_kept = 0;
    if (options.fixups)
    {
        for (int round = 0; round < options.rounds; round++)
        {
            covered            = cover_of(frame);
            FramePlan refined  = plan_frame(wide, source, covered, options);
            if (refined.error >= frame.error)
            {
                break;
            }
            frame = std::move(refined);
            rounds_kept++;
        }
    }

    // Dither and pack, serially: Floyd-Steinberg carries error line to line.
    Gcx::Picture picture;
    picture.lines.assign(PH, Gcx::Line());
    std::copy(frame.chain.initial.begin(), frame.chain.initial.end(), picture.initial);

    std::vector<Rgb444> prediction(static_cast<size_t>(OW) * PH, 0);
    FsRows              fs;
    fs.current.assign(static_cast<size_t>(PW) * 3, 0);
    fs.next.assign(static_cast<size_t>(PW) * 3, 0);

    unsigned loads_total    = 0;
    unsigned literals_total = 0;
    unsigned flats_total    = 0;
    unsigned masks_total    = 0;
    unsigned words_total    = 0;
    unsigned words_max      = 0;

    for (int y = 0; y < PH; y++)
    {
        Gcx::Line       &line        = picture.lines[static_cast<size_t>(y)];
        const LinePlan  &plan        = frame.lines[static_cast<size_t>(y)];
        const Clut      &clut        = frame.chain.clut[static_cast<size_t>(y)];
        const int       *line_values = &source[static_cast<size_t>(y) * PW * 3];
        Rgb444          *row         = &prediction[static_cast<size_t>(y) * OW];

        line.loads = frame.chain.loads[static_cast<size_t>(y)];
        line.pixels.assign(Gcx::WORDS_PER_LINE, 0);

        if (plan.masks)
        {
            // Eager, in this line's HBLANK, behind the palette loads.
            line.records.push_back(se::vidcmd_set(se::SET_CMP_COLOR0, plan.colour0));
            line.records.push_back(se::vidcmd_set(se::SET_CMP_COLOR1, plan.colour1));
        }
        for (const Segment &s : plan.segments)
        {
            for (int x = s.x; x < s.x + s.n; x++)
            {
                int value[3];
                for (int c = 0; c < 3; c++)
                {
                    value[c] = line_values[x * 3 + c];
                    if (options.dither == Dither::Ordered)
                    {
                        value[c] = clamp_8(value[c] + ordered_offset(x, y, options.amplitude));
                    }
                    else if (options.dither == Dither::FloydSteinberg)
                    {
                        // A LITERAL can show any colour, so whatever error has
                        // piled up behind a stretch the palette could not reach
                        // would all land on it as one wrong, usually bright,
                        // pixel.  It takes in at most its own rounding's worth.
                        int32_t carried = fs.current[static_cast<size_t>(x) * 3 + c];
                        if (s.kind == SegmentKind::LITERAL)
                        {
                            const int32_t limit = options.fs_literal << FS_SHIFT;
                            carried = std::max(-limit, std::min(limit, carried));
                        }
                        const int32_t v = (static_cast<int32_t>(value[c]) << FS_SHIFT) + carried;
                        value[c] = (v + (1 << (FS_SHIFT - 1))) >> FS_SHIFT;
                    }
                }

                // The index goes into the pixel plane even under a fix-up, so
                // the plane alone is still a sane picture.
                const int code  = nearest_entry(value, clut.data(), CLUT_SIZE);
                const int word  = x / 4;
                const int shift = 12 - 4 * (x % 4);
                line.pixels[static_cast<size_t>(word)] = static_cast<uint16_t>(
                    line.pixels[static_cast<size_t>(word)] | ((code & 0xF) << shift));

                Rgb444 shown = clut[static_cast<size_t>(code)];
                if (s.kind == SegmentKind::LITERAL)
                {
                    shown = quantize_colour(value[0], value[1], value[2]);
                    line.records.push_back(se::vidcmd_literal(shown));
                }
                else if (s.kind == SegmentKind::FLAT)
                {
                    shown = s.colour;
                }
                row[x * 2 + 0] = shown;
                row[x * 2 + 1] = shown;
                if (s.kind == SegmentKind::MASK)
                {
                    for (int j = 0; j < 2; j++)
                    {
                        const uint32_t dibit = (s.dibits >> (2 * ((x - s.x) * 2 + j))) & 3u;
                        if (dibit == se::MASK_DIBIT_COLOR0)
                        {
                            row[x * 2 + j] = plan.colour0;
                        }
                        else if (dibit == se::MASK_DIBIT_COLOR1)
                        {
                            row[x * 2 + j] = plan.colour1;
                        }
                    }
                }

                if (options.dither == Dither::FloydSteinberg)
                {
                    for (int c = 0; c < 3; c++)
                    {
                        // What this pixel passes on.  Under a fix-up it is the
                        // fix-up's own error against the source and nothing
                        // more: error from pixels the palette could not reach is
                        // dropped there rather than carried across the span.
                        const int32_t carried =
                            (s.kind == SegmentKind::PASS)
                                ? fs.current[static_cast<size_t>(x) * 3 + c] : 0;
                        int32_t residual =
                            ((static_cast<int32_t>(line_values[x * 3 + c]) << FS_SHIFT) + carried)
                            - ((static_cast<int32_t>(rgb444_channel_8(row[x * 2], c))
                                + static_cast<int32_t>(rgb444_channel_8(row[x * 2 + 1], c)))
                               << (FS_SHIFT - 1));
                        const int32_t limit = options.fs_limit << FS_SHIFT;
                        residual = std::max(-limit, std::min(limit, residual));
                        if (x + 1 < PW)
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

            if (s.kind == SegmentKind::PASS)
            {
                line.records.push_back(se::vidcmd_run(se::RUN_SRC_PASSTHROUGH,
                                                      static_cast<uint32_t>(s.n) * 2));
            }
            else if (s.kind == SegmentKind::FLAT)
            {
                line.records.push_back(se::vidcmd_literal(s.colour));
                line.records.push_back(se::vidcmd_hold(static_cast<uint32_t>(s.n) * 2 - 2));
                flats_total++;
            }
            else if (s.kind == SegmentKind::MASK)
            {
                std::array<uint32_t, se::MASK_SLOTS> d{};
                for (uint32_t j = 0; j < se::MASK_SLOTS; j++)
                {
                    d[j] = (s.dibits >> (2 * j)) & 3u;
                }
                const se::VidcmdMask m = se::vidcmd_mask(d);
                line.records.push_back(m.header);
                line.records.push_back(m.data);
                masks_total++;
            }
            else
            {
                literals_total++;
            }
        }

        std::swap(fs.current, fs.next);
        std::fill(fs.next.begin(), fs.next.end(), 0);

        const unsigned packet = static_cast<unsigned>(line.loads.size() * 3 + line.records.size());
        loads_total += static_cast<unsigned>(line.loads.size());
        words_total += packet;
        words_max    = std::max(words_max, packet);
    }

    // The negative controls break the picture or the list on purpose.
    Gcx::BuildOptions build;
    bool              bad_filler = false;
    if (options.fault != nullptr)
    {
        constexpr size_t LINE = 100;
        Gcx::Line &victim = picture.lines[LINE];
        if (std::strcmp(options.fault, "lookahead0") == 0)
        {
            build.lookahead = 0;
        }
        else if (std::strcmp(options.fault, "short-line") == 0)
        {
            victim.records = { se::vidcmd_run(se::RUN_SRC_PASSTHROUGH, OW - 1) };
        }
        else if (std::strcmp(options.fault, "long-line") == 0)
        {
            victim.records = { se::vidcmd_run(se::RUN_SRC_PASSTHROUGH, OW + 1) };
        }
        else if (std::strcmp(options.fault, "bad-filler") == 0)
        {
            bad_filler = true;
        }
        else if (std::strcmp(options.fault, "stream-end") == 0)
        {
            victim.records = { se::vidcmd_run(se::RUN_SRC_PASSTHROUGH, OW - 16),
                               se::vidcmd_stream_run(se::RUN_SRC_PASSTHROUGH, 16) };
            Gcx::Line &next = picture.lines[LINE + 1];
            if (next.loads.empty())
            {
                next.loads.push_back(PaletteLoad{ 0, frame.chain.clut[LINE + 1][0] });
            }
        }
        else if (std::strcmp(options.fault, "over-budget") == 0)
        {
            std::vector<uint16_t> padded(120, se::vidcmd_set(se::SET_FILLER, 0));
            padded.insert(padded.end(), victim.records.begin(), victim.records.end());
            victim.records = padded;
        }
        else
        {
            std::fprintf(stderr, "%s: unknown fault %s\n", argv[0], options.fault);
            return 1;
        }
    }
    else
    {
        std::string error;
        if (!Gcx::validate_picture(picture, error))
        {
            std::fprintf(stderr, "%s: the encoder produced an invalid picture: %s\n", argv[0],
                         error.c_str());
            return 1;
        }
    }

    std::vector<uint8_t> data = Gcx::picture_bytes(picture, build);
    if (bad_filler)
    {
        // The filler of the first per-line load.  (Not a preamble entry: the
        // preamble is consumed faster than ENGINE delivers it, so its write
        // pulse has passed before the filler arrives and the fault would not
        // show.  A line's packet is queued a line ahead and is consumed at the
        // fetch cadence, where the filler is what the RAM latches.)
        size_t vidcmd = 0;
        for (size_t i = 0; i < 4; i++)
        {
            vidcmd = (vidcmd << 8) | data[2 * GcxTarget::HDR_VIDCMD_OFFSET + i];
        }
        size_t word = Gcx::PREAMBLE_WORDS;
        size_t y    = 0;
        while (y < picture.lines.size() && picture.lines[y].loads.empty())
        {
            word += Gcx::line_packet(picture.lines[y]).size();
            y++;
        }
        if (y == picture.lines.size())
        {
            std::fprintf(stderr, "%s: --fault bad-filler needs a picture with a palette load\n",
                         argv[0]);
            return 1;
        }
        data[vidcmd + (word + 2) * 2 + 1] ^= 0x01;
    }

    Gcx::VerifyOptions verify;
    if (options.arb4_only)
    {
        verify.arbitrations = { se::ENGINE_ARBITRATION_CYCLES };
    }
    if (options.fault == nullptr)
    {
        Gcx::VerifyReport report = Gcx::verify_file(data, verify);
        if (report.ok() && report.frame != prediction)
        {
            size_t first = 0;
            size_t count = 0;
            for (size_t i = 0; i < prediction.size(); i++)
            {
                if (report.frame[i] != prediction[i])
                {
                    first = (count == 0) ? i : first;
                    count++;
                }
            }
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "the suite model renders %zu pixels differently from the prediction; "
                          "first at (%zu,%zu): model 0x%03X, predicted 0x%03X",
                          count, first % OW, first / OW, report.frame[first], prediction[first]);
            report.problems.emplace_back(buf);
        }
        if (!report.ok())
        {
            std::fprintf(stderr, "%s: the file failed its own verification, nothing written:\n",
                         argv[0]);
            for (size_t i = 0; i < report.problems.size() && i < 12; i++)
            {
                std::fprintf(stderr, "  %s\n", report.problems[i].c_str());
            }
            return 1;
        }
        Gcx::poke_be16(data, 2 * GcxTarget::HDR_GROUP_SYSCLK, report.worst_group);
        Gcx::poke_be16(data, 2 * GcxTarget::HDR_FIFO_HIGH, report.fifo_high);
        std::printf("%s: verified on the suite model: %u descriptors, worst group %u of %u SYSCLK, "
                    "VIDCMD FIFO high water %u\n",
                    out_path, report.descriptors, report.worst_group, Gcx::GROUP_SYSCLK_LIMIT,
                    report.fifo_high);
    }

    if (!Gcx::write_bytes(out_path, data))
    {
        std::fprintf(stderr, "%s: cannot write %s\n", argv[0], out_path);
        return 1;
    }

    if (options.preview != nullptr)
    {
        std::vector<uint8_t> rgb(static_cast<size_t>(OW) * PH * 3);
        for (size_t i = 0; i < prediction.size(); i++)
        {
            rgb[i * 3 + 0] = nibble_to_8(rgb444_r(prediction[i]));
            rgb[i * 3 + 1] = nibble_to_8(rgb444_g(prediction[i]));
            rgb[i * 3 + 2] = nibble_to_8(rgb444_b(prediction[i]));
        }
        if (stbi_write_png(options.preview, OW, PH, 3, rgb.data(), OW * 3) == 0)
        {
            std::fprintf(stderr, "%s: cannot write %s\n", argv[0], options.preview);
            return 1;
        }
    }

    std::printf("%s: %zu bytes, K=%d, %u palette loads (%.2f/line), %u literals, %u flat spans, "
                "%u masks, %.1f VIDCMD words/line (max %u of %d)\n",
                out_path, data.size(), options.k, loads_total,
                static_cast<double>(loads_total) / PH, literals_total, flats_total, masks_total,
                static_cast<double>(words_total) / PH, words_max, options.line_words);
    // What no 320-density record can repair: each output pixel's distance from
    // its pair's mean.  It is the measure of what MASK (640 resolution) could buy.
    int64_t detail = 0;
    for (size_t i = 0; i < static_cast<size_t>(PW) * PH; i++)
    {
        for (int c = 0; c < 3; c++)
        {
            const int64_t d = wide[i * 6 + c] - wide[i * 6 + 3 + c];
            detail += d * d;
        }
    }
    detail /= 2;
    std::printf("%s: of the planned error, %lld is 640-wide detail the 320 grid cannot carry\n",
                out_path, static_cast<long long>(detail));
    std::printf("%s: planned error %lld -> %lld (%d refinement rounds kept)%s\n", out_path,
                static_cast<long long>(first_error), static_cast<long long>(frame.error),
                rounds_kept, (options.fault != nullptr) ? "  [FAULT INJECTED, NOT VERIFIED]" : "");
    return 0;
}
