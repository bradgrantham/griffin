// A mouse cursor as a COMPOSITOR overlay, for an app whose screen is otherwise
// one static passthrough playfield.
//
// The cursor is NOT drawn into the framebuffer.  It is a short VIDCMD record
// chain -- passthrough RUNs and MASK records (super-engine/descriptor.h, "MASK")
// -- that COMPOSITOR plays over whatever PIXEL is producing.  That buys three
// things a framebuffer cursor cannot have:
//
//   * 640x480 placement over a 320-wide playfield;
//   * colours that bypass the palette RAM (cmp_color0 / cmp_color1), so the
//     cursor costs no palette index and does not move when the palette cycles;
//   * no save-under: moving the cursor rewrites ~125 words and no pixels.
//
// THE WHOLE FRAME'S CHAIN IS DELIVERED IN VERTICAL BLANKING.  COMPOSITOR counts
// a record's duration in ACTIVE pixel slots only (playback is frozen while
// H_ACTIVE is low), and there are exactly 640 slots per line, so the slot at
// which raster position (x, y) is painted is simply
//
//         T(x, y) = y * 640 + x
//
// counted from the first pixel of the frame.  The chain is therefore plain
// duration arithmetic:
//
//         passthrough RUNs totalling T(first record)
//         MASK                                    16 slots, a cursor row
//         passthrough RUN  T(next) - T(prev) - 16
//         MASK
//         ...
//         RUN(passthrough, 1)                     then COMPOSITOR holds
//
// and every word of it sits in the VIDCMD FIFO before line 0.  The per-line
// display list never mentions the cursor, so it stays static: moving the
// cursor touches only the preamble's words.
//
// WHY EVERY SEAM IS EXACT (descriptor.h, "MASK seam constants"): RUN -> MASK is
// zero slots, MASK -> next record is zero slots, and every RUN here is either
// hundreds of slots long or sits behind one that is, so the two-slots-per-word
// fetch cadence never stretches anything.  super-engine's cursor-chain case
// asserts the result pixel-exact, including the two clipping cases below.
//
// CLIPPING.  A MASK is sixteen slots and its pixel 0 is implicitly opaque
// cmp_color0, so a row's record begins at that row's FIRST OPAQUE pixel (hence
// the per-row offset) and a record is never shifted to fit.  At the right edge
// the record simply runs off the line: its remaining slots are the first slots
// of the next line, which is consistent with the arithmetic above, and the
// dibits that would land there are forced to passthrough so nothing shows.  A
// row that starts beyond the right edge, or below line 479, is not emitted.
//
// FAILURE IS CONFINED TO THE CURSOR.  Every source in the chain other than a
// mask's opaque dibits is passthrough, so a framing slip can shear the cursor
// but cannot disturb the playfield, and /RS at the next vsync discards
// whatever is left.
//
// The caller owns everything else in the preamble: the palette, pix_base,
// pix_mode, and SETs of cmp_color0 (outline) and cmp_color1 (fill).  All SETs
// must come BEFORE this chain -- a SET behind a RUN waits for that RUN.
//
// Bare-metal safe: header-only, no allocation, bounded loops.

#ifndef GRIFFIN_CURSOR_H
#define GRIFFIN_CURSOR_H

#include <cstdint>

#include "../../super-engine/descriptor.h"

namespace GriffinCursor
{

namespace se = SuperEngine;

inline constexpr unsigned ROWS    = 16;
inline constexpr unsigned COLUMNS = 16;

// One row of art, already in record form.
struct Row
{
    uint8_t  offset  = 0;       // column of the row's first opaque pixel
    uint8_t  visible = 0;       // 0 = the row has no opaque pixel, emit nothing
    uint32_t dibits  = 0;       // pixels 1..15 of the record, pixel 1 in bits 29:28
};

struct Shape
{
    Row rows[ROWS];
};

// Art is ROWS strings of COLUMNS characters, hotspot at row 0 column 0:
//     '#'  outline, cmp_color0
//     '.'  fill,    cmp_color1
//     ' '  transparent
// A row's record starts at its first non-space column, so that column must be
// '#' (pixel 0 of a MASK is cmp_color0 whether the art likes it or not);
// shape_ok() is the assertion for that.
constexpr Shape make_shape(const char *const art[ROWS])
{
    Shape s;
    for (unsigned r = 0; r < ROWS; r++)
    {
        unsigned first = COLUMNS;
        for (unsigned c = 0; c < COLUMNS; c++)
        {
            if (art[r][c] != ' ')
            {
                first = c;
                break;
            }
        }
        if (first == COLUMNS)
        {
            continue;
        }
        uint32_t dibits = 0;
        for (unsigned i = 1; i < se::MASK_SLOTS; i++)
        {
            const unsigned c = first + i;
            uint32_t       d = se::MASK_DIBIT_PASSTHROUGH;
            if (c < COLUMNS)
            {
                if (art[r][c] == '#')
                {
                    d = se::MASK_DIBIT_COLOR0;
                }
                else if (art[r][c] == '.')
                {
                    d = se::MASK_DIBIT_COLOR1;
                }
            }
            dibits |= d << (2u * (se::MASK_SLOTS - 1u - i));
        }
        s.rows[r].offset  = static_cast<uint8_t>(first);
        s.rows[r].visible = 1;
        s.rows[r].dibits  = dibits;
    }
    return s;
}

constexpr bool shape_ok(const char *const art[ROWS])
{
    for (unsigned r = 0; r < ROWS; r++)
    {
        for (unsigned c = 0; c < COLUMNS; c++)
        {
            if (art[r][c] != ' ')
            {
                if (art[r][c] != '#')
                {
                    return false;
                }
                break;
            }
        }
    }
    return true;
}

// The classic arrow.  Row 15 starts at column 5, which is what exercises the
// per-row offset.
inline constexpr const char *const ARROW_ART[ROWS] = {
    "#               ",
    "##              ",
    "#.#             ",
    "#..#            ",
    "#...#           ",
    "#....#          ",
    "#.....#         ",
    "#......#        ",
    "#.......#       ",
    "#........#      ",
    "#.....#####     ",
    "#..#..#         ",
    "#.# #..#        ",
    "##  #..#        ",
    "#    #..#       ",
    "     ###        ",
};
static_assert(shape_ok(ARROW_ART));
inline constexpr Shape ARROW = make_shape(ARROW_ART);

// What the art shows at (column, row): 0 transparent, 1 cmp_color0, 2 cmp_color1.
// For checkers and host tools; the target never calls it.
constexpr unsigned art_pixel(const char *const art[ROWS], unsigned column, unsigned row)
{
    if (row >= ROWS || column >= COLUMNS)
    {
        return 0;
    }
    return art[row][column] == '#' ? 1u : (art[row][column] == '.' ? 2u : 0u);
}

// --- chain size -------------------------------------------------------------
//
// The leading passthrough is at most T(639, 479) + 15 slots in RUNs of at most
// 4095, the first row is a bare MASK and each later row is a RUN plus a MASK.
inline constexpr uint32_t RUN_MAX_COUNT = 4095;      // RUN's 12-bit count
inline constexpr uint32_t FRAME_SLOTS   = se::H_ACTIVE * se::V_ACTIVE;
inline constexpr uint32_t LEAD_RUN_WORDS_MAX =
    (FRAME_SLOTS + COLUMNS + RUN_MAX_COUNT - 1) / RUN_MAX_COUNT;                // 76
inline constexpr uint32_t CHAIN_WORDS =
    LEAD_RUN_WORDS_MAX + ROWS * (1 + se::MASK_RECORD_WORDS) + 1;                // 125
static_assert(LEAD_RUN_WORDS_MAX == 76);
static_assert(CHAIN_WORDS == 125);

// Write the chain for a cursor whose hotspot is at raster (x, y), 0 <= x < 640,
// 0 <= y < 480, into out[0 .. CHAIN_WORDS).
//
// ALWAYS EXACTLY CHAIN_WORDS WORDS, so the descriptors that deliver the
// preamble never change: a chain shorter than the maximum is padded AT THE
// FRONT with `pad`, which must be a SET that is harmless to repeat (the
// caller's own SET of pix_mode is the obvious one).  Front, because the pad
// has to be consumed eagerly in blanking, and nothing behind a RUN is.
inline void build_chain(uint16_t *out, const Shape &shape, uint32_t x, uint32_t y, uint16_t pad)
{
    uint16_t chain[CHAIN_WORDS];
    uint32_t n   = 0;
    uint32_t pos = 0;       // slot at which the next record would start

    for (unsigned r = 0; r < ROWS; r++)
    {
        const Row &row = shape.rows[r];
        const uint32_t line   = y + r;
        const uint32_t column = x + row.offset;
        if (row.visible == 0 || line >= se::V_ACTIVE || column >= se::H_ACTIVE)
        {
            continue;
        }

        // Passthrough up to this row's first opaque pixel.  The remainder goes
        // FIRST so the RUN in front of a MASK is always a long one.
        // Counted by subtraction: a 68000 has no 32-bit divide, and the
        // quotient is at most LEAD_RUN_WORDS_MAX.
        uint32_t gap  = line * se::H_ACTIVE + column - pos;
        uint32_t full = 0;
        while (gap >= RUN_MAX_COUNT)
        {
            gap -= RUN_MAX_COUNT;
            full++;
        }
        if (gap != 0)
        {
            chain[n++] = se::vidcmd_run(se::RUN_SRC_PASSTHROUGH, gap);
        }
        for (; full != 0; full--)
        {
            chain[n++] = se::vidcmd_run(se::RUN_SRC_PASSTHROUGH, RUN_MAX_COUNT);
        }

        // The record, with every dibit that would fall past the right edge
        // (and so onto the start of the next line) forced to passthrough.
        const uint32_t on_line = se::H_ACTIVE - column;          // >= 1
        uint32_t       dibits  = row.dibits;
        if (on_line < se::MASK_SLOTS)
        {
            dibits &= ~((1u << (2u * (se::MASK_SLOTS - on_line))) - 1u);
        }
        chain[n++] = static_cast<uint16_t>(0x4000u | (dibits >> 16));
        chain[n++] = static_cast<uint16_t>(dibits & 0xFFFFu);

        pos = line * se::H_ACTIVE + column + se::MASK_SLOTS;
    }

    // Whatever is left of the frame.  One slot is enough: the count goes
    // terminal, nothing else is staged, and COMPOSITOR holds the source.
    chain[n++] = se::vidcmd_run(se::RUN_SRC_PASSTHROUGH, 1);

    uint32_t o = 0;
    for (; o < CHAIN_WORDS - n; o++)
    {
        out[o] = pad;
    }
    for (uint32_t i = 0; i < n; i++)
    {
        out[o + i] = chain[i];
    }
}

// The MASK header/data split above is descriptor.h's encoding: check it against
// the reference encoder for the arrow's first few rows rather than trust it.
constexpr bool encoding_matches_reference(const Shape &shape, const char *const art[ROWS])
{
    for (unsigned r = 0; r < ROWS; r++)
    {
        const Row &row = shape.rows[r];
        if (row.visible == 0)
        {
            continue;
        }
        std::array<uint32_t, se::MASK_SLOTS> d{};
        d[0] = se::MASK_PIXEL0_DIBIT;
        for (unsigned i = 1; i < se::MASK_SLOTS; i++)
        {
            const unsigned p = art_pixel(art, row.offset + i, r);
            d[i] = (p == 1) ? se::MASK_DIBIT_COLOR0
                            : (p == 2) ? se::MASK_DIBIT_COLOR1 : se::MASK_DIBIT_PASSTHROUGH;
        }
        const se::VidcmdMask m = se::vidcmd_mask(d);
        if (m.header != static_cast<uint16_t>(0x4000u | (row.dibits >> 16)) ||
            m.data != static_cast<uint16_t>(row.dibits & 0xFFFFu))
        {
            return false;
        }
    }
    return true;
}
static_assert(encoding_matches_reference(ARROW, ARROW_ART));

}   // namespace GriffinCursor

#endif /* GRIFFIN_CURSOR_H */
