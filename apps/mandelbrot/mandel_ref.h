// The Mandelbrot explorer's arithmetic, view geometry and colouring, in
// portable C++.
//
// THIS FILE IS THE DEFINITION.  Three things are built from it and have to
// agree bit for bit:
//
//   * mandel_iter.s, the 68000 inner loop, is the same equations by hand and
//     is checked against step() on the target by `mandel selftest`;
//   * main.cpp uses everything else here directly;
//   * host_ref.cpp renders the expected screen on the host, which is the
//     oracle the emulator's screenshot is compared against pixel for pixel.
//
// So nothing in here is "about right".  Where a product is truncated, the
// truncation is spelled out, because the assembly has to truncate identically.
//
// NUMBER FORMAT: Q4.28, signed 32-bit.  ONE = 2^28.
//
//   Range.  An iteration tests for escape BEFORE it forms the next z, so the
//   next z is only ever computed from |x| < 2, |y| < 2 and x^2 + y^2 <= 4:
//       x' = x^2 - y^2 + cx   is within  4 + |cx|
//       y' = 2xy + cy         is within  4 + |cy|     (2|xy| <= x^2 + y^2)
//   and the views keep |cx|, |cy| < 4, so |x'|, |y'| < 8 = the format's range.
//
//   Squares.  sq(a) takes a magnitude a < 2^30 (|value| < 4), forms A = a << 2
//   (< 2^32) and returns  Ah*Ah + ((Ah*Al) >> 15)  with Ah:Al the halves of A.
//   That is the top 32 bits of A*A -- which is a*a >> 28 -- with the Al*Al
//   partial product dropped: two 16x16 multiplies instead of three, for a
//   result that is the exact floor or one less (Al*Al < 2^32, under one unit of
//   the result).  One unit is 2^-28.
//
//   The cross term.  2xy is formed as (x+y)^2 - x^2 - y^2, a third square
//   instead of a four-multiply signed product, and it needs no sign logic.
//   |x + y| <= sqrt(2 (x^2 + y^2)) < 2.83, inside sq()'s domain.  Its error is
//   within three units.
//
// A 68000 iteration is therefore six MULUs; mandel_iter.s has the cycle count.
//
// THE FRACTIONAL COUNT.  An integer escape count makes every band edge a hard
// edge, and where counts change by dozens per pixel that reads as noise.  The
// usual remedy is the normalised count
//
//     mu = n + 1 - log2(log2 |z_n|)            (z_n the first escaped z)
//
// which is continuous across band boundaries because squaring z adds one to
// n and doubles log2 |z|.  (I have not re-derived its constant against a
// reference; what matters here is the continuity, and the formula is
// self-consistent under z -> z^2.)  With |z_n|^2 = R that is
//
//     mu = n + 2 - log2(log2 R)
//
// and the loop records R/4 at escape (norm4 below, so that |z| < 8 fits
// sq()'s domain).  R/4 is in [1, 12), so log2 R = 2 + e + m with e the
// position of norm4's leading one and m its mantissa, taken as a straight
// line (error under 0.09 in log2 R, well under the quarter steps kept).  A
// 64-entry table then gives log2(log2 R) in quarters, and mu in quarters is
// the colour's whole band plus a two-bit fraction that a 2x2 ordered dither
// turns into one of four mixes of the band's colour and the next.  The dither
// is keyed to the playfield pixel's coordinates, so it is as deterministic as
// everything else in this file.

#ifndef MANDEL_REF_H
#define MANDEL_REF_H

#include <cstdint>

namespace Mandel
{

inline constexpr int32_t  ONE        = 1 << 28;
inline constexpr uint32_t LIMIT_AXIS = 2u << 28;      // |x| or |y| >= 2.0: escaped
inline constexpr uint32_t LIMIT_NORM = 4u << 28;      // x^2 + y^2 > 4.0: escaped

constexpr uint32_t magnitude(int32_t v)
{
    return (v < 0) ? (0u - static_cast<uint32_t>(v)) : static_cast<uint32_t>(v);
}

// a < 2^30.  See "Squares" above.
constexpr uint32_t sq(uint32_t a)
{
    const uint32_t big  = a << 2;
    const uint32_t high = big >> 16;
    const uint32_t low  = big & 0xFFFFu;
    return high * high + ((high * low) >> 15);
}

// Exact floor(a * b / 2^28) for a, b < 2^29, from four 16x16 products.  Only
// the cardioid test uses it; it is not in the inner loop.
constexpr uint32_t mul(uint32_t a, uint32_t b)
{
    const uint32_t ah = a >> 16;
    const uint32_t al = a & 0xFFFFu;
    const uint32_t bh = b >> 16;
    const uint32_t bl = b & 0xFFFFu;
    const uint32_t mid = ah * bl + al * bh + ((al * bl) >> 16);
    return ((ah * bh) << 4) + (mid >> 12);
}

// The layout mandel_iter.s indexes by hand: x 0, y 4, cx 8, cy 12, norm4 16,
// n 20.
struct State
{
    int32_t  x;
    int32_t  y;
    int32_t  cx;
    int32_t  cy;
    uint32_t norm4;     // |z|^2 / 4 of the escaped z, valid once escaped
    uint16_t n;         // iterations completed
};

constexpr State start(int32_t cx, int32_t cy)
{
    return State{0, 0, cx, cy, 0, 0};
}

// |z|^2 / 4, formed from the halved magnitudes so that any z the loop can
// hold (|x|, |y| < 8) is inside sq()'s domain.
constexpr uint32_t norm_quarter(const State &s)
{
    return sq(magnitude(s.x) >> 1) + sq(magnitude(s.y) >> 1);
}

// One iteration.  Returns true, leaving z untouched and recording norm4, if
// z has escaped; otherwise advances z and counts the iteration.
constexpr bool step(State &s)
{
    const uint32_t ax = magnitude(s.x);
    if (ax >= LIMIT_AXIS)
    {
        s.norm4 = norm_quarter(s);
        return true;
    }
    const uint32_t ay = magnitude(s.y);
    if (ay >= LIMIT_AXIS)
    {
        s.norm4 = norm_quarter(s);
        return true;
    }
    const uint32_t xx = sq(ax);
    const uint32_t yy = sq(ay);
    if (xx + yy > LIMIT_NORM)
    {
        s.norm4 = norm_quarter(s);
        return true;
    }
    const uint32_t sum = magnitude(s.x + s.y);
    const uint32_t two_xy = sq(sum) - (xx + yy);        // wraps exactly as the 68000 does
    s.x = static_cast<int32_t>(xx - yy + static_cast<uint32_t>(s.cx));
    s.y = static_cast<int32_t>(two_xy + static_cast<uint32_t>(s.cy));
    s.n++;
    return false;
}

// Run to escape or to max_iter.  The result's n is the iterations completed
// before z was seen to have escaped, and norm4 is meaningful if n < max_iter.
constexpr State iterate(int32_t cx, int32_t cy, unsigned max_iter)
{
    State s = start(cx, cy);
    while (s.n < max_iter)
    {
        if (step(s))
        {
            break;
        }
    }
    return s;
}

// True for a point inside the main cardioid or the period-2 bulb, which never
// escapes: the closed forms are far cheaper than running it to max_iter, and
// at the top view those two shapes are most of the cost.
//
//     bulb       (x + 1)^2 + y^2 <= 1/16
//     cardioid   q (q + p) <= y^2 / 4,   p = x - 1/4,  q = p^2 + y^2
//
// Both shapes lie inside x in [-1.25, 0.375], |y| < 0.65, which is the cheap
// reject.  The products are floors, which leans the boundary inward by a few
// units of 2^-28 -- points that close to the boundary are far past any
// max_iter this program uses.
constexpr bool in_main_bulbs(int32_t cx, int32_t cy)
{
    const uint32_t ay = magnitude(cy);
    if (ay >= static_cast<uint32_t>(ONE) || cx < -(ONE + ONE / 4) || cx > ONE / 2)
    {
        return false;
    }
    const uint32_t yy = sq(ay);

    const uint32_t r = magnitude(cx + ONE);
    if (r < static_cast<uint32_t>(ONE) && sq(r) + yy <= static_cast<uint32_t>(ONE / 16))
    {
        return true;
    }

    const int32_t  p  = cx - ONE / 4;                   // -1.5 .. 0.25
    const uint32_t q  = sq(magnitude(p)) + yy;          // < 2.25 + 1
    if (q > static_cast<uint32_t>(ONE))
    {
        return false;                                   // farther than 1 from (1/4, 0)
    }
    const int32_t t = static_cast<int32_t>(q) + p;      // -1.5 .. 1.25
    if (t <= 0)
    {
        return true;                                    // q (q + p) <= 0 <= y^2 / 4
    }
    return mul(q, static_cast<uint32_t>(t)) <= (yy >> 2);
}

// ---------------------------------------------------------------------------
// The screen and the view
// ---------------------------------------------------------------------------

inline constexpr unsigned WIDTH  = 320;
inline constexpr unsigned HEIGHT = 240;

// Pixel pitch at zoom level 0: 3 * 2^-8, which shows 3.75 x 2.8125 of the
// plane.  It is 3 * 2^20 units, so it halves exactly at every level down to
// MAX_LEVEL, where a pixel is still 48 units -- comfortably more than the
// three-unit arithmetic error above.
inline constexpr int32_t  BASE_STEP     = 3 << 20;
inline constexpr unsigned MAX_LEVEL     = 16;
inline constexpr int32_t  HOME_CENTER_X = -(3 * (ONE / 4));     // -0.75
inline constexpr int32_t  HOME_CENTER_Y = 0;

struct View
{
    int32_t  center_x = HOME_CENTER_X;
    int32_t  center_y = HOME_CENTER_Y;
    unsigned level    = 0;
};

constexpr int32_t view_step(const View &v)
{
    return BASE_STEP >> v.level;
}

// c for the pixel at column px, row py.  Row 0 is the top, imaginary up.
constexpr int32_t pixel_cx(const View &v, unsigned px)
{
    return v.center_x + (static_cast<int32_t>(px) - static_cast<int32_t>(WIDTH / 2)) * view_step(v);
}

constexpr int32_t pixel_cy(const View &v, unsigned py)
{
    return v.center_y - (static_cast<int32_t>(py) - static_cast<int32_t>(HEIGHT / 2)) * view_step(v);
}

// Zoom by two ABOUT the pixel (px, py): the point under it stays under it.
// Zooming in stops at MAX_LEVEL; zooming out to level 0 snaps back to the home
// view, so the set can never be lost off-screen.  Both return false if the
// view did not change.
constexpr bool zoom_in(View &v, unsigned px, unsigned py)
{
    if (v.level >= MAX_LEVEL)
    {
        return false;
    }
    const int32_t dx = static_cast<int32_t>(px) - static_cast<int32_t>(WIDTH / 2);
    const int32_t dy = static_cast<int32_t>(py) - static_cast<int32_t>(HEIGHT / 2);
    v.level++;
    v.center_x += dx * view_step(v);
    v.center_y -= dy * view_step(v);
    return true;
}

constexpr bool zoom_out(View &v, unsigned px, unsigned py)
{
    if (v.level == 0)
    {
        return false;
    }
    const int32_t dx = static_cast<int32_t>(px) - static_cast<int32_t>(WIDTH / 2);
    const int32_t dy = static_cast<int32_t>(py) - static_cast<int32_t>(HEIGHT / 2);
    v.center_x -= dx * view_step(v);
    v.center_y += dy * view_step(v);
    v.level--;
    if (v.level == 0)
    {
        v.center_x = HOME_CENTER_X;
        v.center_y = HOME_CENTER_Y;
    }
    return true;
}

// Deeper views need more iterations to resolve their detail.
constexpr unsigned max_iterations(const View &v)
{
    const unsigned n = 64 + 16 * v.level;
    return (n > 256) ? 256u : n;
}

// ---------------------------------------------------------------------------
// Colour
// ---------------------------------------------------------------------------
//
// Palette index 0 is the set itself.  Indices 1..15 are a closed loop of
// fifteen colours; a point that escaped after n iterations is 1 + n mod 15.
// Palette cycling rotates the loop under the indices, so the framebuffer is
// never touched, and switching palettes rewrites the same fifteen entries.

inline constexpr unsigned CYCLE_COLOURS = 15;

// round(4 * log2(L)) for L = 2 + k/16, k = 0..63: log2(log2 R) in quarters,
// indexed by log2 R in sixteenths above 2.  Generated once with
// python3 -c "import math; print([round(4*math.log2(L/16)) for L in range(32,96)])"
inline constexpr uint8_t QUARTER_LOG2[64] = {
    4, 4, 4, 5, 5, 5, 5, 5, 5, 5, 6, 6, 6, 6, 6, 6, 6, 6, 7, 7, 7, 7, 7, 7, 7, 7, 7, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10,
};

// log2 R in sixteenths above 2, from norm4 = R/4 in Q4.28, which is in
// [1, 12): e is the leading one's position above the units bit, m the four
// bits below it.
constexpr unsigned log2_norm_sixteenths(uint32_t norm4)
{
    unsigned e = 0;
    while (e < 3 && norm4 >= (static_cast<uint32_t>(ONE) << (e + 1)))
    {
        e++;
    }
    const unsigned m = (norm4 >> (28 + e - 4)) & 0xFu;
    return e * 16 + m;
}

// mu in quarters: 4n + 8 - 4 log2(log2 R), never below zero.
constexpr unsigned quarter_count(unsigned n, uint32_t norm4)
{
    const unsigned g = QUARTER_LOG2[log2_norm_sixteenths(norm4)];
    const unsigned q = 4 * n + 8;
    return (q > g) ? q - g : 0u;
}

// 2x2 ordered dither: the fraction f (0..3) of the way from band k to band
// k+1 shows band k+1 on f of the four pixels of each 2x2 cell.
inline constexpr uint8_t BAYER_2X2[2][2] = {{0, 2}, {3, 1}};

constexpr unsigned colour_index(unsigned n, unsigned max_iter, uint32_t norm4, unsigned px,
                                unsigned py)
{
    if (n >= max_iter)
    {
        return 0;
    }
    const unsigned q    = quarter_count(n, norm4);
    const unsigned band = q / 4 + ((q % 4 > BAYER_2X2[py & 1u][px & 1u]) ? 1u : 0u);
    return 1u + band % CYCLE_COLOURS;
}

// R4G4B4, 0x0RGB.
inline constexpr uint16_t SET_COLOUR = 0x000;

inline constexpr unsigned PALETTE_COUNT = 4;
inline constexpr const char *const PALETTE_NAMES[PALETTE_COUNT] = {
    "rainbow", "ochre", "grey", "black and white",
};
inline constexpr uint16_t PALETTES[PALETTE_COUNT][CYCLE_COLOURS] = {
    // rainbow: a hue loop, so cycling has no seam
    {0x00F, 0x04F, 0x08F, 0x0CF, 0x0FC, 0x0F6, 0x4F0, 0xAF0,
     0xFE0, 0xFA0, 0xF60, 0xF20, 0xF06, 0xC0C, 0x60F},
    // ochre: dark umber up to pale straw
    {0x210, 0x320, 0x431, 0x541, 0x652, 0x762, 0x873, 0x983,
     0xA94, 0xBA4, 0xCB5, 0xDC6, 0xED8, 0xFEA, 0xFFC},
    // grey: the fifteen non-black greys
    {0x111, 0x222, 0x333, 0x444, 0x555, 0x666, 0x777, 0x888,
     0x999, 0xAAA, 0xBBB, 0xCCC, 0xDDD, 0xEEE, 0xFFF},
    // black and white: every band an edge (fifteen is odd, so one pair of
    // adjacent white bands marks the loop's seam)
    {0xFFF, 0x000, 0xFFF, 0x000, 0xFFF, 0x000, 0xFFF, 0x000,
     0xFFF, 0x000, 0xFFF, 0x000, 0xFFF, 0x000, 0xFFF},
};

// The colour palette entry `index` shows, from palette `palette` at cycle
// phase `phase` (0..14).
constexpr uint16_t palette_colour(unsigned palette, unsigned index, unsigned phase)
{
    return (index == 0) ? SET_COLOUR
                        : PALETTES[palette % PALETTE_COUNT][(index - 1 + phase) % CYCLE_COLOURS];
}

// The whole pixel, for hosts and self-tests: what main.cpp's render loop
// arrives at by way of mandel_iter.s.
constexpr unsigned pixel_index(const View &v, unsigned px, unsigned py)
{
    const int32_t cx = pixel_cx(v, px);
    const int32_t cy = pixel_cy(v, py);
    if (in_main_bulbs(cx, cy))
    {
        return 0;
    }
    const unsigned max_iter = max_iterations(v);
    const State    s        = iterate(cx, cy, max_iter);
    return colour_index(s.n, max_iter, s.norm4, px, py);
}

// Sanity pins: the origin never escapes, a far point escapes at once, and the
// two closed forms agree with what they are shortcuts for.
static_assert(iterate(0, 0, 64).n == 64);
static_assert(iterate(3 * (ONE / 2), 3 * (ONE / 2), 64).n <= 2);
// The fractional count is continuous across a band edge: R = 4 at n gives
// the same mu as R = 16 at n + 1, both n + 1 whole.
static_assert(quarter_count(5, static_cast<uint32_t>(ONE)) == 4 * 6);
static_assert(quarter_count(6, static_cast<uint32_t>(ONE) * 4) == 4 * 6);
static_assert(in_main_bulbs(0, 0) && in_main_bulbs(-ONE, 0) && in_main_bulbs(-ONE / 2, ONE / 2));
static_assert(!in_main_bulbs(ONE / 2, 0) && !in_main_bulbs(-(ONE / 4) * 7, 0));
static_assert(!in_main_bulbs(-(ONE / 100) * 76, ONE / 5));

}   // namespace Mandel

#endif /* MANDEL_REF_H */
