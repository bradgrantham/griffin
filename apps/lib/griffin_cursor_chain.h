// THE ADVANCED CURSOR RECIPE: a mouse cursor as ONE whole-frame COMPOSITOR
// record chain, for an app whose screen is otherwise one static passthrough
// playfield with no other VIDCMD content.
//
// THIS IS NOT THE DEFAULT.  The default is griffin_cursor.h, the per-line
// recipe: every line carries one fixed four-word record, a blank one or a
// cursor row, and each line's correctness is local to that line.  This chain
// instead spends about 850 SYSCLK a field (against 10,560) by letting one record
// stream span the whole frame, at the price of a two-part delivery whose
// deadline and FIFO bound are hand-derived below.  Use it only when the bus
// matters more than the simplicity, and keep suite case m12-cursor-chain
// passing if you touch it.  (Moved here unchanged in behaviour from
// griffin_cursor.h, 2026-10-03; the art is shared with that header.)
//
// The cursor is NOT drawn into the framebuffer.  It is a short VIDCMD record
// chain -- passthrough RUNs and MASK records (super-engine/descriptor.h, "MASK")
// -- that COMPOSITOR plays over whatever PIXEL is producing.  That buys three
// things a framebuffer cursor cannot have:
//
//   * 640x480 placement over a 320-wide playfield;
//   * colours that bypass the palette RAM (cmp_color0 / cmp_color1), so the
//     cursor costs no palette index and does not move when the palette cycles;
//   * no save-under: moving the cursor rewrites CHAIN_WORDS words and no pixels.
//
// THE CHAIN IS ONE WHOLE-FRAME RECORD STREAM.  COMPOSITOR counts
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
// and the per-line display list never mentions the cursor's POSITION, so it
// stays static: moving the cursor touches only the chain's words.
//
// IT IS DELIVERED IN TWO FIXED PARTS (2026-10-03).  RUN's count is ten bits
// (1..1023), so the lead-in alone is up to 301 words and the chain no longer
// fits the 256-word VIDCMD FIFO in one vblank deposit.  The chain is therefore
// LEFT-ALIGNED — lead RUNs first, the cursor rows, RUN(passthrough, 1), then
// target-7 FILLER SETs up to CHAIN_WORDS — and delivered as:
//
//   part A   words [0, PART_A_WORDS), in vertical blanking with the caller's
//            preamble (which must total <= VIDCMD_FIFO_WORDS with it);
//   part B   words [PART_A_WORDS, CHAIN_WORDS), PART_B_CHUNK words per line
//            on lines PART_B_FIRST_LINE .. PART_B_FIRST_LINE + PART_B_LINES - 1,
//            each chunk a VIDCMD descriptor appended to that line's group.
//
// Both properties are static_asserts below, derived by hand:
//
//   DEADLINE  every playback record is at most RUN_MAX_COUNT slots (a RUN; a
//             MASK is 2 words per 16 slots, a filler 1 per <= 2), so word i of
//             the chain cannot start before slot (i - REAL_TAIL - 1) *
//             RUN_MAX_COUNT unless it is a filler: if it is a cursor-row word,
//             the lead-in in front of it has at least i - REAL_TAIL words and
//             all but the first are full.  Fillers have no deadline (they
//             execute while passthrough holds, invisibly).  Chunk j's first
//             word must be deposited at least a line before that slot.
//   CAPACITY  words executed by slot S >= floor(S / RUN_MAX_COUNT), for the
//             same reason, and two more sit in COMPOSITOR's bank, so at
//             PART_B_FIRST_LINE at most PART_A_WORDS - floor(S / 1023) - 2
//             part-A words are still in the FIFO; all of part B on top of
//             that must fit.
//
// A filler is decoded nowhere and does not touch the source, so the padding
// may sit at the end, after the chain's last RUN, rather than in blanking.
//
// WHY EVERY SEAM IS EXACT (descriptor.h, "MASK seam constants"): RUN -> MASK is
// zero slots, MASK -> next record is zero slots, and every RUN here is either
// hundreds of slots long or sits behind one that is, so the two-slots-per-word
// fetch cadence never stretches anything.  super-engine's cursor-chain case
// asserts the result pixel-exact, including the two clipping cases below and
// cursors whose rows straddle the part A / part B split.
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
// must come BEFORE this chain -- a SET behind a RUN waits for that RUN -- and
// the caller's list must deliver part B where this header says.
//
// Bare-metal safe: header-only, no allocation, bounded loops.

#ifndef GRIFFIN_CURSOR_CHAIN_H
#define GRIFFIN_CURSOR_CHAIN_H

#include <cstdint>

#include "../../super-engine/descriptor.h"
#include "griffin_cursor.h"

namespace GriffinCursorChain
{

namespace se = SuperEngine;

// The geometry and the art are the default header's (one home per fact).
using GriffinCursor::ROWS;
using GriffinCursor::COLUMNS;
using GriffinCursor::ARROW_ART;
using GriffinCursor::art_pixel;
using GriffinCursor::shape_ok;

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

// The classic arrow, in this recipe's record form.
inline constexpr Shape ARROW = make_shape(ARROW_ART);

// --- chain size ----------------------------------------------------------------
//
// The leading passthrough is at most T(639, 479) + 15 slots in RUNs of at most
// RUN_MAX_COUNT, and each emitted row adds at most one more RUN (a gap between
// rows is under 2 * 640 + 16 slots and every RUN word but one per gap is full)
// plus its MASK; one RUN(passthrough, 1) closes the chain.
inline constexpr uint32_t RUN_MAX_COUNT = se::VIDCMD_RUN_COUNT_MAX;   // 1023, ten bits
inline constexpr uint32_t FRAME_SLOTS   = se::H_ACTIVE * se::V_ACTIVE;
inline constexpr uint32_t LEAD_RUN_WORDS_MAX =
    (FRAME_SLOTS + COLUMNS + RUN_MAX_COUNT - 1) / RUN_MAX_COUNT;                // 301
inline constexpr uint32_t CHAIN_WORDS =
    LEAD_RUN_WORDS_MAX + ROWS * (1 + se::MASK_RECORD_WORDS) + 1;                // 350
static_assert(RUN_MAX_COUNT == 1023);
static_assert(LEAD_RUN_WORDS_MAX == 301);
static_assert(CHAIN_WORDS == 350);

// Words that can follow the lead-in and still be real (not filler): every row's
// RUN and MASK, and the closing RUN.
inline constexpr uint32_t REAL_TAIL = ROWS * (1 + se::MASK_RECORD_WORDS) + 1;   // 49

// --- the two-part delivery (see the header comment) -------------------------
inline constexpr uint32_t PART_A_WORDS      = 6 * se::DESC_MAX_COUNT;           // 192
inline constexpr uint32_t PART_B_WORDS      = CHAIN_WORDS - PART_A_WORDS;       // 158
inline constexpr uint32_t PART_B_CHUNK      = se::DESC_MAX_COUNT;               // 32
inline constexpr uint32_t PART_B_LINES      = (PART_B_WORDS + PART_B_CHUNK - 1) / PART_B_CHUNK;   // 5
inline constexpr uint32_t PART_B_FIRST_LINE = 184;

// Words of part B delivered on part-B line j (0 .. PART_B_LINES - 1).
constexpr uint32_t part_b_chunk_words(uint32_t j)
{
    const uint32_t done = j * PART_B_CHUNK;
    return (PART_B_WORDS - done > PART_B_CHUNK) ? PART_B_CHUNK : (PART_B_WORDS - done);
}

// The first line on which chain word i (if it is not a filler) can be needed.
constexpr uint32_t earliest_line_of_word(uint32_t i)
{
    return (i <= REAL_TAIL) ? 0u : ((i - REAL_TAIL - 1u) * RUN_MAX_COUNT) / se::H_ACTIVE;
}

// DEADLINE: chunk j is deposited in line (PART_B_FIRST_LINE + j)'s group, at
// least one whole line before the earliest line its first word can play on.
constexpr bool part_b_deadlines_met()
{
    for (uint32_t j = 0; j < PART_B_LINES; j++)
    {
        if (PART_B_FIRST_LINE + j + 1u >= earliest_line_of_word(PART_A_WORDS + j * PART_B_CHUNK))
        {
            return false;
        }
    }
    return true;
}
static_assert(part_b_deadlines_met());
static_assert(earliest_line_of_word(PART_A_WORDS) == 226);

// CAPACITY: part A still queued at PART_B_FIRST_LINE (bank of two excluded)
// plus ALL of part B, as if nothing more were consumed while it arrives.
inline constexpr uint32_t PART_B_FIFO_PEAK =
    PART_A_WORDS - (PART_B_FIRST_LINE * se::H_ACTIVE) / RUN_MAX_COUNT - se::VIDCMD_BANK_DEPTH +
    PART_B_WORDS;                                                               // 233
static_assert(PART_B_FIFO_PEAK == 233);
static_assert(PART_B_FIFO_PEAK < se::VIDCMD_FIFO_WORDS);
static_assert(PART_B_FIRST_LINE + PART_B_LINES <= se::V_ACTIVE);

// The pad: target 7 is decoded nowhere (descriptor.h SET_FILLER), so a filler
// executing behind the chain's last RUN costs a slot of held passthrough and
// nothing else.
inline constexpr uint16_t FILLER_WORD = se::vidcmd_set(se::SET_FILLER, 0);

// Write the chain for a cursor whose hotspot is at raster (x, y), 0 <= x < 640,
// 0 <= y < 480, into out[0 .. CHAIN_WORDS).
//
// ALWAYS EXACTLY CHAIN_WORDS WORDS, so the descriptors that deliver it never
// change: a chain shorter than the maximum is padded AT THE END with filler
// SETs.
inline void build_chain(uint16_t *out, const Shape &shape, uint32_t x, uint32_t y)
{
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
            out[n++] = se::vidcmd_run(se::RUN_SRC_PASSTHROUGH, gap);
        }
        for (; full != 0; full--)
        {
            out[n++] = se::vidcmd_run(se::RUN_SRC_PASSTHROUGH, RUN_MAX_COUNT);
        }

        // The record, with every dibit that would fall past the right edge
        // (and so onto the start of the next line) forced to passthrough.
        const uint32_t on_line = se::H_ACTIVE - column;          // >= 1
        uint32_t       dibits  = row.dibits;
        if (on_line < se::MASK_SLOTS)
        {
            dibits &= ~((1u << (2u * (se::MASK_SLOTS - on_line))) - 1u);
        }
        out[n++] = static_cast<uint16_t>(0x4000u | (dibits >> 16));
        out[n++] = static_cast<uint16_t>(dibits & 0xFFFFu);

        pos = line * se::H_ACTIVE + column + se::MASK_SLOTS;
    }

    // Whatever is left of the frame.  One slot is enough: the count goes
    // terminal, nothing else is staged, and COMPOSITOR holds the source.
    out[n++] = se::vidcmd_run(se::RUN_SRC_PASSTHROUGH, 1);

    for (; n < CHAIN_WORDS; n++)
    {
        out[n] = FILLER_WORD;
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

}   // namespace GriffinCursorChain

#endif /* GRIFFIN_CURSOR_CHAIN_H */
