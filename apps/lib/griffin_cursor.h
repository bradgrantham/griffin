// A mouse cursor as a COMPOSITOR overlay: the PER-LINE recipe (the default).
//
// The cursor is NOT drawn into the framebuffer.  Each of the 480 visible lines
// gets ONE VIDCMD descriptor of RECORD_WORDS words, and every line's words sum
// to exactly 640 slots on their own:
//
//   a line without the cursor    the ONE shared blank record
//                                    FILLER FILLER FILLER RUN(pt, 640)
//   cursor row r (16 of them)    that row's own record
//                                    gap  MASK-header  MASK-data  tail
//                                gap  = RUN(pt, c)            (FILLER if c == 0)
//                                tail = RUN(pt, 624 - c)      (FILLER if c == 624)
//                                c    = x + the row's first opaque column
//
// so no line relies on anything another line left behind (no hold framing, no
// record running across a line end).  Moving in x patches the gap and tail
// words of the row records; moving in y re-points line descriptors between the
// blank record and the row records.  Nothing else changes, and nothing is
// multiplied or divided.
//
// WHY EVERY LINE IS EXACT (super-engine/descriptor.h, "MASK seam constants"):
// RUN -> MASK and MASK -> next record are zero-slot seams, a RUN(pt, 1..3) in
// front of the MASK is a banked pair at the line start, and a FILLER (SET
// target 7, decoded nowhere) costs no slot where it sits -- in front of the
// line's first playback record it executes in blanking, and behind a MASK that
// ends on slot 639 it is consumed in the blanking that follows.  So the
// occupancy is the authored sum, 640, at every x.  Suite case
// m15-cursor-per-line asserts it pixel-exact at x = 0, 1, 2, 3, 619, 620,
// 623, 624, 625, 638 and 639 among others, and through a sequence of moves.
//
// THE RIGHT EDGE CLIPS WHOLE ROWS.  A MASK is always sixteen slots and its
// pixel 0 is always opaque cmp_color0, so a row record must start at the row's
// first opaque pixel and must end by slot 640.  A row whose first opaque
// pixel lies right of X_FIT_MAX (624) therefore cannot be drawn on its own line
// at all: shortening the record is impossible, starting it earlier paints
// cmp_color0 on a transparent pixel, and letting it run past 640 makes the next
// line depend on this one.  Such a row is simply not shown (its line keeps the
// blank record).  For the arrow that means the whole cursor disappears once the
// hot spot passes x = 624 (row 15, offset 5, already at x = 620).  An app that
// wants the cursor always whole clamps the hot spot to whole_x_max(shape).
// griffin_cursor_chain.h, the advanced recipe, does clip exactly at the right
// edge, because there a record IS allowed to run on into the next line.
//
// THE BOTTOM EDGE clips by not pointing: a row below line 479 has no line.
//
// DELIVERY is the caller's display list, with these rules (vidcmd-spec.md,
// griffin-video-datasheet.html):
//   * line L's descriptor sits in line L-1's group BEHIND that group's PIXELS
//     descriptors (one line early, so it is in the FIFO long before line L);
//   * line 0's descriptor goes in vertical blanking, AFTER /RS (which empties
//     the FIFOs) -- e.g. as the last item before line 0's group;
//   * the 480 descriptors are evenly spaced, 1 << stride_shift bytes apart,
//     so the descriptor of line L is at line0_desc + (L << stride_shift);
//   * every one is (count RECORD_WORDS, DEST_VIDCMD_FIFO_W, no wait) with its
//     source in the same 64K as the Records block; place() writes them all.
//   * cmp_color0 (outline) and cmp_color1 (fill) are set once in the vblank
//     preamble, before any of this.
//
// TEARING.  A line's gap and tail are two separate stores, and a line that
// ENGINE delivers between them sums to the wrong count and shifts every line
// after it.  So these words must never be edited while ENGINE may read them:
// keep two lists with their own Records and edit the one not playing.
//
// COST.  ENGINE spends engine_descriptor_cycles(4, RECORD_WORDS) = 22 SYSCLK
// per line, 10,560 SYSCLK = 4.5% of a 233,625-SYSCLK field.  An x move is at
// most 3 word stores per cursor row (gap, tail, one re-point), a y move at most
// 32 re-points (one store each: only the source's low word changes).
//
// Bare-metal safe: header-only, no allocation, bounded loops.

#ifndef GRIFFIN_CURSOR_H
#define GRIFFIN_CURSOR_H

#include <array>
#include <cstdint>

#include "../../super-engine/descriptor.h"

namespace GriffinCursor
{

namespace se = SuperEngine;

inline constexpr unsigned ROWS    = 16;
inline constexpr unsigned COLUMNS = 16;

// --- art ---------------------------------------------------------------------
//
// Art is ROWS strings of COLUMNS characters, hot spot at row 0 column 0:
//     '#'  outline, cmp_color0
//     '.'  fill,    cmp_color1
//     ' '  transparent
// A row's record starts at its first non-space column, so that column must be
// '#' (pixel 0 of a MASK is cmp_color0 whether the art likes it or not);
// shape_ok() is the assertion for that.

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

// One row of art in record form: where its record starts and its MASK words.
struct Row
{
    uint8_t  offset  = 0;       // column of the row's first opaque pixel
    uint8_t  visible = 0;       // 0 = the row has no opaque pixel and is never shown
    uint16_t header  = 0;       // MASK word 1
    uint16_t data    = 0;       // MASK word 2
};

struct Shape
{
    Row rows[ROWS];
};

// Encoded by descriptor.h's reference encoder, so this header never splits
// dibits by hand.
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
        std::array<uint32_t, se::MASK_SLOTS> d{};
        d[0] = se::MASK_PIXEL0_DIBIT;
        for (unsigned i = 1; i < se::MASK_SLOTS; i++)
        {
            const unsigned p = art_pixel(art, first + i, r);
            d[i] = (p == 1) ? se::MASK_DIBIT_COLOR0
                            : ((p == 2) ? se::MASK_DIBIT_COLOR1 : se::MASK_DIBIT_PASSTHROUGH);
        }
        const se::VidcmdMask m = se::vidcmd_mask(d);
        s.rows[r].offset  = static_cast<uint8_t>(first);
        s.rows[r].visible = 1;
        s.rows[r].header  = m.header;
        s.rows[r].data    = m.data;
    }
    return s;
}

inline constexpr Shape ARROW = make_shape(ARROW_ART);

// --- records -------------------------------------------------------------------

inline constexpr uint32_t RECORD_WORDS = 4;
inline constexpr uint32_t RECORD_BYTES = RECORD_WORDS * 2;

// The last column a sixteen-slot record can start on and still end by slot 640.
inline constexpr uint32_t X_FIT_MAX = se::H_ACTIVE - se::MASK_SLOTS;          // 624
static_assert(X_FIT_MAX == 624);

// Target 7 is decoded nowhere (descriptor.h SET_FILLER).
inline constexpr uint16_t FILLER = se::vidcmd_set(se::SET_FILLER, 0);

inline constexpr uint16_t BLANK_RECORD[RECORD_WORDS] = {
    FILLER, FILLER, FILLER, se::vidcmd_run(se::RUN_SRC_PASSTHROUGH, se::H_ACTIVE)};
static_assert(se::H_ACTIVE <= se::VIDCMD_RUN_COUNT_MAX);

// The gap and tail words of a row record starting at column c <= X_FIT_MAX.
// There is no zero-count RUN (RUN(0) is 1024 slots), so an empty gap or tail
// is a FILLER in the same position: the record keeps its shape.
constexpr uint16_t gap_word(uint32_t c)
{
    return (c == 0) ? FILLER : se::vidcmd_run(se::RUN_SRC_PASSTHROUGH, c);
}

constexpr uint16_t tail_word(uint32_t c)
{
    return (c == X_FIT_MAX) ? FILLER : se::vidcmd_run(se::RUN_SRC_PASSTHROUGH, X_FIT_MAX - c);
}

// The blank record and the sixteen row records, together and aligned so they
// share one 64K bank: then a re-point changes only a descriptor's low source
// word.
struct alignas(256) Records
{
    uint16_t blank[RECORD_WORDS];
    uint16_t rows[ROWS][RECORD_WORDS];
};
static_assert(sizeof(Records) <= 256);

// Byte offsets inside a Records block.
inline constexpr uint32_t BLANK_OFFSET = 0;
constexpr uint32_t row_offset(unsigned r)
{
    return RECORD_BYTES + r * RECORD_BYTES;
}
inline constexpr uint32_t GAP_WORD  = 0;   // word index inside a row record
inline constexpr uint32_t TAIL_WORD = 3;

// Is row r of a cursor whose hot spot is at (x, y) on screen?
constexpr bool row_fits(const Shape &s, unsigned r, uint32_t x)
{
    return s.rows[r].visible != 0 && x + s.rows[r].offset <= X_FIT_MAX;
}

constexpr bool row_shown(const Shape &s, unsigned r, uint32_t x, uint32_t y)
{
    return row_fits(s, r, x) && y + r < se::V_ACTIVE;
}

// The largest hot-spot x at which every row of the shape is still shown.
constexpr uint32_t whole_x_max(const Shape &s)
{
    uint32_t widest = 0;
    for (unsigned r = 0; r < ROWS; r++)
    {
        if (s.rows[r].visible != 0 && s.rows[r].offset > widest)
        {
            widest = s.rows[r].offset;
        }
    }
    return X_FIT_MAX - widest;
}
static_assert(whole_x_max(ARROW) == 619);

// --- where things are --------------------------------------------------------
//
// All addresses are ENGINE byte addresses (on the target, plain pointers).
struct Layout
{
    uint32_t records      = 0;   // a Records block
    uint32_t line0_desc   = 0;   // line 0's cursor descriptor
    uint32_t stride_shift = 0;   // log2 of the bytes from one line's descriptor to the next
};

// Word stores go through a callable st(byte_address, word), so the suite can
// run exactly this code against its model RAM and count the stores.
struct DirectStore
{
    void operator()(uint32_t address, uint16_t word) const
    {
        // Volatile so the compiler cannot sink these stores past the store
        // that hands the list to the vsync handler.
        *reinterpret_cast<volatile uint16_t *>(static_cast<uintptr_t>(address)) = word;
    }
};

// The descriptor's source low word (descriptor word 2) for a record.
constexpr uint16_t source_low(uint32_t record_address)
{
    return static_cast<uint16_t>(record_address & 0xFFFEu);
}

constexpr uint32_t descriptor_address(const Layout &l, uint32_t line)
{
    return l.line0_desc + (line << l.stride_shift);
}

// Everything, from nothing: the records for a hot spot at (x, y), and all 480
// line descriptors written whole.  Call once per list, before it is armed.
template <class Store>
void place(Store &st, const Layout &l, const Shape &s, uint32_t x, uint32_t y)
{
    for (uint32_t i = 0; i < RECORD_WORDS; i++)
    {
        st(l.records + BLANK_OFFSET + i * 2, BLANK_RECORD[i]);
    }
    for (unsigned r = 0; r < ROWS; r++)
    {
        const uint32_t row = l.records + row_offset(r);
        const uint32_t c   = x + s.rows[r].offset;
        st(row + 0, row_fits(s, r, x) ? gap_word(c) : FILLER);
        st(row + 2, s.rows[r].header);
        st(row + 4, s.rows[r].data);
        st(row + 6, row_fits(s, r, x) ? tail_word(c) : FILLER);
    }

    se::Descriptor d;
    d.count       = static_cast<uint16_t>(RECORD_WORDS);
    d.destination = se::DEST_VIDCMD_FIFO_W;
    d.src         = l.records + BLANK_OFFSET;
    const se::DescriptorWords w = se::encode_descriptor(d);
    uint32_t at = l.line0_desc;
    for (uint32_t line = 0; line < se::V_ACTIVE; line++)
    {
        for (uint32_t i = 0; i < se::DESC_WORDS; i++)
        {
            st(at + i * 2, w.w[i]);
        }
        at += 1u << l.stride_shift;
    }
    for (unsigned r = 0; r < ROWS; r++)
    {
        if (row_shown(s, r, x, y))
        {
            st(descriptor_address(l, y + r) + 4, source_low(l.records + row_offset(r)));
        }
    }
}

// Move the cursor of ONE list from (ox, oy) -- where that list last had it --
// to (nx, ny).  Stores only what changes:
//   * the gap and tail of every row whose start column changes (or that comes
//     back into fit at the right edge): <= 2 stores a row;
//   * the old lines that no longer show a row go back to the blank record,
//     then the new lines are pointed at their rows: one store a line.
// The blanking pass comes first so that a line that is both old and new ends
// pointing at its new row.
template <class Store>
void move(Store &st, const Layout &l, const Shape &s, uint32_t ox, uint32_t oy, uint32_t nx,
          uint32_t ny)
{
    for (unsigned r = 0; r < ROWS; r++)
    {
        if (!row_fits(s, r, nx))
        {
            continue;
        }
        const uint32_t oc = ox + s.rows[r].offset;
        const uint32_t nc = nx + s.rows[r].offset;
        if (!row_fits(s, r, ox) || oc != nc)
        {
            const uint32_t row = l.records + row_offset(r);
            st(row + GAP_WORD * 2, gap_word(nc));
            st(row + TAIL_WORD * 2, tail_word(nc));
        }
    }

    const uint16_t blank = source_low(l.records + BLANK_OFFSET);
    for (unsigned r = 0; r < ROWS; r++)
    {
        if (!row_shown(s, r, ox, oy))
        {
            continue;
        }
        const uint32_t line = oy + r;
        if (line >= ny && line - ny < ROWS && row_shown(s, line - ny, nx, ny))
        {
            continue;   // the second pass points it at its new row
        }
        st(descriptor_address(l, line) + 4, blank);
    }

    for (unsigned r = 0; r < ROWS; r++)
    {
        if (!row_shown(s, r, nx, ny))
        {
            continue;
        }
        if (ny == oy && row_shown(s, r, ox, oy))
        {
            continue;   // already points at row r
        }
        st(descriptor_address(l, ny + r) + 4, source_low(l.records + row_offset(r)));
    }
}

}   // namespace GriffinCursor

#endif /* GRIFFIN_CURSOR_H */
