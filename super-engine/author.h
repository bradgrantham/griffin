// author.h — display-list and VIDCMD authoring for the validation cases.
//
// Same bare-metal constraints as descriptor.h: these functions are meant to be
// the thing the 68000 firmware eventually runs to build a frame's list in RAM,
// so they take a caller-provided Memory (a std::span of bus words), never
// allocate, never print, and never throw.  Every function reports how much it
// used through its return value or through caller-provided spans.
//
// One knobbed FrameParams drives all the cases rather than six near-copies of
// the same loop: the cases differ only in which deposits join the per-line
// group and which records join the line's VIDCMD stream, and keeping them one
// function keeps the *ordering* of those — the thing under test — identical
// across cases.

#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "descriptor.h"

namespace SuperEngine
{

// ---------------------------------------------------------------------------
// Tempest-style well geometry
// ---------------------------------------------------------------------------
//
// Shared between the test driver (which Bresenhams the static wireframe into a
// 1bpp bitmap once, modelling the CPU drawing a level) and the per-frame VIDCMD
// authoring (which places moving objects as coloured spans and never touches
// the bitmap at all).  Integer only, cos/sin scaled by 1024, so the whole thing
// is byte-reproducible.
inline constexpr uint32_t TEMPEST_LANES = 16;

inline constexpr int32_t TEMPEST_OUTER_CX = 320;
inline constexpr int32_t TEMPEST_OUTER_CY = 262;
inline constexpr int32_t TEMPEST_OUTER_RX = 300;
inline constexpr int32_t TEMPEST_OUTER_RY = 205;

inline constexpr int32_t TEMPEST_INNER_CX = 320;
inline constexpr int32_t TEMPEST_INNER_CY = 170;
inline constexpr int32_t TEMPEST_INNER_RX = 70;
inline constexpr int32_t TEMPEST_INNER_RY = 45;

// 16 points at 22.5 degree steps, x1024.
inline constexpr std::array<int32_t, TEMPEST_LANES> TEMPEST_COS1024 = {
    1024, 946, 724, 392, 0, -392, -724, -946, -1024, -946, -724, -392, 0, 392, 724, 946};
inline constexpr std::array<int32_t, TEMPEST_LANES> TEMPEST_SIN1024 = {
    0, 392, 724, 946, 1024, 946, 724, 392, 0, -392, -724, -946, -1024, -946, -724, -392};

constexpr int32_t tempest_outer_x(uint32_t lane)
{
    return TEMPEST_OUTER_CX + TEMPEST_COS1024[lane % TEMPEST_LANES] * TEMPEST_OUTER_RX / 1024;
}
constexpr int32_t tempest_outer_y(uint32_t lane)
{
    return TEMPEST_OUTER_CY + TEMPEST_SIN1024[lane % TEMPEST_LANES] * TEMPEST_OUTER_RY / 1024;
}
constexpr int32_t tempest_inner_x(uint32_t lane)
{
    return TEMPEST_INNER_CX + TEMPEST_COS1024[lane % TEMPEST_LANES] * TEMPEST_INNER_RX / 1024;
}
constexpr int32_t tempest_inner_y(uint32_t lane)
{
    return TEMPEST_INNER_CY + TEMPEST_SIN1024[lane % TEMPEST_LANES] * TEMPEST_INNER_RY / 1024;
}

// Position along a lane's spoke: depth 0 = far (inner) rim, 100 = near (outer).
constexpr int32_t tempest_depth_x(uint32_t lane, int32_t depth)
{
    return tempest_inner_x(lane) + (tempest_outer_x(lane) - tempest_inner_x(lane)) * depth / 100;
}
constexpr int32_t tempest_depth_y(uint32_t lane, int32_t depth)
{
    return tempest_inner_y(lane) + (tempest_outer_y(lane) - tempest_inner_y(lane)) * depth / 100;
}

// Depth fade for the wireframe: bright blue on the near rim at the bottom of
// the screen, dimming toward the far rim's rows near the top.
constexpr Rgb444 tempest_palette_fg(uint32_t line)
{
    const uint32_t depth = 3u + (line * 12u) / (V_ACTIVE - 1);   // 3..15
    return rgb444(0, depth / 4u, depth);
}

constexpr Rgb444 tempest_palette_bg(uint32_t)
{
    return RGB444_BLACK;
}

// Object colours, as RUN_COLOR 3-bit codes.
inline constexpr uint32_t TEMPEST_CLAW_COLOR    = RUN_COLOR_YELLOW;
inline constexpr uint32_t TEMPEST_FLIPPER_COLOR = RUN_COLOR_RED;
inline constexpr uint32_t TEMPEST_SHOT_COLOR    = RUN_COLOR_WHITE;

// Object counts and shapes, fixed so the checks can hand-count pixels.
inline constexpr uint32_t TEMPEST_CLAW_ROWS     = 4;
inline constexpr uint32_t TEMPEST_CLAW_PRONGS   = 2;
inline constexpr uint32_t TEMPEST_CLAW_WIDTH    = 3;
inline constexpr uint32_t TEMPEST_FLIPPERS      = 3;
inline constexpr uint32_t TEMPEST_FLIPPER_ROWS  = 6;
inline constexpr uint32_t TEMPEST_FLIPPER_WIDTH = 3;
inline constexpr uint32_t TEMPEST_SHOTS         = 2;
inline constexpr uint32_t TEMPEST_SHOT_ROWS     = 2;
inline constexpr uint32_t TEMPEST_SHOT_WIDTH    = 2;

// Density stress: N one-pixel spans on one band of rows, SPREAD evenly across
// the line.  Sweeping this is how the suite turns "the demo works" into an
// object budget, and what it measures is DELIVERY: the engine puts one VIDCMD
// word on the bus every 2 SYSCLK, i.e. one word per ~3.6 pixel clocks, so a
// dense line has to be pre-buffered during HBLANK and the limit is how much bus
// time that steals from the PIXELS stream.  Spreading rather than packing is
// also what a well full of objects actually looks like.
//
// There is a SECOND ceiling since 2026-08-19, independent of the bus: the
// compositor fetches one word per two pixel clocks, so a record list has to
// AVERAGE two slots per record however early its words arrived.  Below is the
// tightest pitch a one-pixel-span list can be authored at and still land where
// it was authored — DERIVED from L4/L5, not measured.  A 1-px span alternating
// with a (pitch-1)-px gap averages pitch/2 slots per record against a
// requirement of VIDCMD_SLOTS_PER_WORD, so pitch >= 4.  At pitch 3 the list
// loses half a slot per record; at pitch 2 it loses one per record and the
// band is drawn at half density with every span two pixels wide
// (compositor_tb's SUSTAINED_2SLOT).  The on-chip bank — staged_word plus the
// parked Q — pays for the first two records and nothing after them.
//
// The two ceilings do not meet here: 64 spans spread over 640 pixels is a
// 9-pixel pitch, so this sweep is still measuring the bus.  main.cpp asserts
// that rather than assuming it, and the cadence floor itself is measured
// clean-room in main.cpp's cadence traces where no deposit schedule can
// confound it.
inline constexpr uint32_t TEMPEST_STRESS_MIN_PITCH = 2 * VIDCMD_SLOTS_PER_WORD;

inline constexpr uint32_t TEMPEST_MAX_STRESS_SPANS = 64;
inline constexpr uint32_t TEMPEST_STRESS_ROW       = 236;
inline constexpr uint32_t TEMPEST_STRESS_ROWS      = 6;
inline constexpr uint32_t TEMPEST_STRESS_COLOR     = RUN_COLOR_MAGENTA;

// The most coloured spans any one line can carry.
inline constexpr uint32_t TEMPEST_MAX_SPANS =
    TEMPEST_CLAW_PRONGS + TEMPEST_FLIPPERS + TEMPEST_SHOTS + TEMPEST_MAX_STRESS_SPANS;

// ---------------------------------------------------------------------------
// A 5x7 font, and the two PURE-VIDCMD screens built on it
// ---------------------------------------------------------------------------
//
// A "pure-VIDCMD screen" authors NO PIXELS descriptors at all: the display list
// IS the framebuffer.  Text is MASK records, background is RUNs, colour is
// SETs, and the pixel bitmap the rest of the suite scrolls around does not
// exist.  What makes that affordable is the MASK record's density — sixteen
// pixels for two words — and what makes it laid out the way it is is the
// record's implicit pixel 0: whatever falls on a record boundary is cmp_color0,
// so BOTH screens put a blank column there.
//
//   console  8-px cell, glyph in columns 1..5, so ONE RECORD IS TWO GLYPHS and
//            the implicit pixel 0 is the left glyph's inter-character gap.
//   kiosk    16-px cell, glyph scaled x3 horizontally into columns 1..15, so
//            ONE RECORD IS ONE BIG GLYPH.  The vertical scale is x4 (28 rows of
//            a 32-row cell) — the aspect is not square, and that is the record
//            talking: 16 pixels is the width a record has to spend, so a big
//            font either takes two records per glyph (and then its middle
//            column is forced to cmp_color0) or it is 15 pixels wide.
//
// Every glyph's column 0 of the FONT is free to be ink; it is the CELL's column
// 0 that must be blank, and both cell layouts leave it so by construction.
inline constexpr uint32_t FONT_COLS = 5;
inline constexpr uint32_t FONT_ROWS = 7;

// The character set, in glyph order.  A character outside it renders blank,
// which is the honest behaviour for a font this small.
inline constexpr char FONT_CHARSET[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,:-/>()!*+=";
inline constexpr uint32_t FONT_GLYPHS = static_cast<uint32_t>(sizeof(FONT_CHARSET) - 1);

// Seven rows of five bits, MSB (bit 4) leftmost.
inline constexpr std::array<uint8_t, FONT_GLYPHS * FONT_ROWS> FONT5X7 = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   // ' '
    0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11,   // A
    0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E,   // B
    0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E,   // C
    0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E,   // D
    0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F,   // E
    0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10,   // F
    0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F,   // G
    0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11,   // H
    0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E,   // I
    0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C,   // J
    0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11,   // K
    0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F,   // L
    0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11,   // M
    0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11,   // N
    0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E,   // O
    0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10,   // P
    0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D,   // Q
    0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11,   // R
    0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E,   // S
    0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,   // T
    0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E,   // U
    0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04,   // V
    0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11,   // W
    0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11,   // X
    0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04,   // Y
    0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F,   // Z
    0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E,   // 0
    0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E,   // 1
    0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F,   // 2
    0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E,   // 3
    0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02,   // 4
    0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E,   // 5
    0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E,   // 6
    0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08,   // 7
    0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E,   // 8
    0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C,   // 9
    0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C,   // .
    0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08,   // ,
    0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00,   // :
    0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00,   // -
    0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10,   // /
    0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08,   // >
    0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02,   // (
    0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08,   // )
    0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04,   // !
    0x00, 0x0A, 0x04, 0x1F, 0x04, 0x0A, 0x00,   // *
    0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00,   // +
    0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00,   // =
};

constexpr uint32_t font_glyph_index(char c)
{
    for (uint32_t i = 0; i < FONT_GLYPHS; i++)
    {
        if (FONT_CHARSET[i] == c)
        {
            return i;
        }
    }
    return 0;   // blank
}

constexpr bool font_pixel(char c, uint32_t col, uint32_t row)
{
    if (col >= FONT_COLS || row >= FONT_ROWS)
    {
        return false;
    }
    const uint8_t bits = FONT5X7[font_glyph_index(c) * FONT_ROWS + row];
    return ((bits >> (FONT_COLS - 1 - col)) & 1u) != 0;
}

// What a pixel of a screen line is made of.  The three classes are exactly the
// three things a MASK dibit can select, which is why the screens are authored
// in this alphabet and not in colours.
enum class InkClass : uint8_t
{
    BG  = 0,   // dibit 10, cmp_color0 — and the record's implicit pixel 0
    INK = 1,   // dibit 11, cmp_color1
    ALT = 2,   // dibit 00, PASSTHROUGH (see the note in author.cpp)
};

constexpr uint32_t ink_class_dibit(InkClass k)
{
    switch (k)
    {
        case InkClass::INK: return MASK_DIBIT_COLOR1;
        case InkClass::ALT: return MASK_DIBIT_PASSTHROUGH;
        default:            return MASK_DIBIT_COLOR0;
    }
}

// --- the console screen -----------------------------------------------------

inline constexpr uint32_t CONSOLE_CELL_W = 8;
inline constexpr uint32_t CONSOLE_CELL_H = 8;
inline constexpr uint32_t CONSOLE_COLS   = H_ACTIVE / CONSOLE_CELL_W;   // 80
inline constexpr uint32_t CONSOLE_ROWS   = V_ACTIVE / CONSOLE_CELL_H;   // 60
inline constexpr uint32_t CONSOLE_GLYPHS_PER_RECORD = MASK_SLOTS / CONSOLE_CELL_W;   // 2

// A full-width console line: every 16-pixel group is a record, plus the two
// per-line SETs.  This is the screen's ceiling, quoted against
// VIDCMD_WORDS_PER_LINE_CAP.
inline constexpr uint32_t CONSOLE_FULL_LINE_WORDS =
    2 + (H_ACTIVE / MASK_SLOTS) * MASK_RECORD_WORDS;                     // 82

// The screen's text.  Shared by the authoring (which turns it into dibits) and
// by the checker's reference rasterizer (which turns it into pixels), so the
// two cannot drift on WHAT is drawn — what is under test is whether the VIDCMD
// records reproduce it, pixel for pixel, at the right x.
char console_char(uint32_t row, uint32_t col);

// Which cmp register a console row's ink comes from.  Rows 0 and 59 and every
// row's "NN>" prefix are ALT, i.e. dibit 00 — passthrough, the third colour.
InkClass console_ink_class(uint32_t row, uint32_t col);

// The class of every pixel on screen line `line`.  `out` must be H_ACTIVE long.
void console_line_classes(uint32_t line, std::span<InkClass> out);

// Per-row colours.  fg walks down the frame so a row whose SET went missing
// breaks a smooth gradient, exactly like line_palette_fg does for the bitmap
// cases.
constexpr Rgb444 console_row_color1(uint32_t row)
{
    const uint32_t g = 6u + (row * 9u) / (CONSOLE_ROWS - 1);   // 6..15
    return rgb444(g / 2u, g, 4u);
}

constexpr Rgb444 console_row_color0(uint32_t row)
{
    return ((row & 1u) != 0) ? rgb444(0, 0, 2) : rgb444(1, 1, 3);
}

// The third colour, set once per frame and never again: PIXEL's pal_bg, which
// is what a passthrough dibit resolves to when no PIXELS word is ever fetched.
inline constexpr Rgb444 CONSOLE_ALT_COLOR = rgb444(15, 10, 0);

// --- the kiosk screen -------------------------------------------------------

inline constexpr uint32_t KIOSK_CELL_W  = MASK_SLOTS;   // 16: one record per glyph
inline constexpr uint32_t KIOSK_CELL_H  = 32;
inline constexpr uint32_t KIOSK_SCALE_X = 3;            // 5 cols -> 15 px, +1 blank
inline constexpr uint32_t KIOSK_SCALE_Y = 4;            // 7 rows -> 28 px, +4 blank

inline constexpr uint32_t KIOSK_MAX_SEGMENTS = 4;
inline constexpr uint32_t KIOSK_MAX_TEXT     = 16;

// One recoloured stretch of a kiosk line: a SET pair followed by `records`
// chained MASK records, one big glyph each.
struct KioskSegment
{
    uint32_t x0      = 0;   // pixel of the first record's pixel 0
    uint32_t records = 0;
    Rgb444   color1  = 0;
    Rgb444   color0  = 0;
    char     text[KIOSK_MAX_TEXT] = {};
    uint8_t  alt[KIOSK_MAX_TEXT]  = {};   // 1 = draw this glyph in passthrough
};

struct KioskLinePlan
{
    uint32_t     background_color = RUN_COLOR_BLUE;   // a RUN_COLOR code
    uint32_t     cell_top         = 0;                // first screen line of this cell
    uint32_t     seg_n            = 0;
    KioskSegment seg[KIOSK_MAX_SEGMENTS] = {};
};

KioskLinePlan kiosk_plan_line(uint32_t line);

// The class of every pixel of a kiosk segment's glyph row, written into `out`
// at the segment's own x.  Pixels outside every segment stay BG, which the
// caller paints from the line's RUN_COLOR background rather than from a cmp
// register — the two must agree in value, and kiosk_plan_line() makes them.
void kiosk_line_classes(const KioskLinePlan &plan, uint32_t line, std::span<InkClass> out);

// The kiosk's third colour: PIXEL's pal_bg again.
inline constexpr Rgb444 KIOSK_ALT_COLOR = rgb444(15, 15, 15);

// Full-density MASK recolouring, for the budget arithmetic the case prints: a
// SET pair plus a record for EVERY 16-pixel group.
inline constexpr uint32_t KIOSK_FULL_DENSITY_WORDS =
    (H_ACTIVE / MASK_SLOTS) * (2 + MASK_RECORD_WORDS);                   // 160
inline constexpr uint32_t KIOSK_FULL_DENSITY_SLOTS =
    (H_ACTIVE / MASK_SLOTS) * (MASK_SLOTS + MASK_GAP_AFTER_MASK_SET_SET);  // 800

enum class ScreenStyle : uint8_t
{
    NONE    = 0,
    CONSOLE = 1,
    KIOSK   = 2,
};

// The PIXEL mode register as pixel.v decodes it (descriptor.h): a DEPTH
// ENUMERATION in mode[1:0] plus the half-rate flag in mode[2].  Every playfield
// pixel is a palette index, so a mode says how many bits a pixel costs and
// nothing about colour.
enum class PixelMode : uint8_t
{
    DIRECT_1BPP   = PIXEL_DEPTH_1BPP,                             // 40 words/line
    TWO_BPP       = PIXEL_DEPTH_2BPP,                             // 80 words/line
    ATTR          = PIXEL_DEPTH_ATTR,                             // 80 words/line
    HALF_1BPP     = PIXEL_DEPTH_1BPP | PIXEL_MODE_HALF_RATE,      // 20 words/line
    HALF_TWO_BPP  = PIXEL_DEPTH_2BPP | PIXEL_MODE_HALF_RATE,      // 40 words/line
    HALF_FOUR_BPP = PIXEL_DEPTH_4BPP | PIXEL_MODE_HALF_RATE,      // 80 words/line
    HALF_ATTR     = PIXEL_DEPTH_ATTR | PIXEL_MODE_HALF_RATE,      // 40 words/line
    // 4bpp at FULL rate is undefined and deliberately has no name here.
};

constexpr uint32_t pixel_mode_bits(PixelMode m)
{
    return static_cast<uint32_t>(m);
}

enum class SpriteStyle : uint8_t
{
    NONE         = 0,   // the line's VIDCMD stream is just its coverage RUN
    CURSOR       = 1,   // one 16x16 arrow on 16 lines, as spans
    FOUR_SPRITES = 2,   // worst case: four sprites on every line, as spans
    MASK_SPRITES = 3,   // four 16x16 MASK records on 16-pixel cell boundaries
};

// MASK_SPRITES geometry.  The cells are 16-pixel aligned so the record
// boundaries fall on group boundaries and the line's slot sum is exactly
// H_ACTIVE by construction, exactly as the console screen's are.
inline constexpr uint32_t MASK_SPRITE_COUNT = 4;
inline constexpr std::array<uint32_t, MASK_SPRITE_COUNT> MASK_SPRITE_X = {64, 208, 352, 496};
inline constexpr uint32_t MASK_SPRITE_H = 16;

// The art: a bordered box with a diamond inside it.  Column 0 is ALWAYS the
// border, which is what makes the record legal — a MASK's pixel 0 is an
// implicit opaque cmp_color0 and cannot be transparent.  The holes are
// PASSTHROUGH, so the playfield shows through them; over a half-rate
// playfield that is the interop proof, because the mask's dibits step once per
// PIXEL CLOCK while the playfield underneath steps once per two.
uint32_t mask_sprite_dibit(uint32_t row, uint32_t col);

// How a list frames its lines.  Both come off compositor.v's single hold rule;
// the difference is entirely in what the list builder promises and therefore in
// what the checker may assert.
enum class FramingMode : uint8_t
{
    // Records are buffered ahead in VBLANK, the FIFO never empties mid-line,
    // hold never engages, and every line's slots must total EXACTLY H_ACTIVE.
    // Overrunning is the hazard: a leftover record staged at the H_ACTIVE fall
    // plays at the start of the next line and blocks that line's eager SETs.
    CUSHION = 0,

    // A line's records may total FEWER than H_ACTIVE slots; the last source
    // replicates to the end of the line, and a line with no records at all
    // keeps holding.  A whole passthrough frame costs one VIDCMD word.  The
    // obligation moves from slot arithmetic to a delivery deadline: a line's
    // packet must land before that line's pixel 0.
    JIT = 1,
};

// The palette PAIRS the slot-arithmetic cases switch between with a mid-line
// SET(pix_base).  1bpp forms its index as {pix_base[3:1], bit}, so a base value
// names a pair and its low bit is ignored; each pair below is loaded with a
// single colour so a pixel's colour names the pair and not the bitmap.
// They are chosen so the SAME values name a distinct PAIR in 1bpp and a distinct
// QUAD in 2bpp ({base[3:2], dibit}), which is what lets one authoring path serve
// both the 1bpp and the 2bpp slot-arithmetic cases.
inline constexpr uint32_t REGRESSION_BASE_HI = 8;   // 1bpp pair 8/9
inline constexpr uint32_t SPLIT_BASE_MID     = 4;   // 1bpp pair 4/5, 2bpp quad 4..7
inline constexpr uint32_t SPLIT_BASE_HI      = 8;   // 1bpp pair 8/9, 2bpp quad 8..B

// Bytes reserved per line in the authored VIDCMD region.  160 words is well
// above the ~150-word worst case the tempest density sweep reaches, and keeps
// the per-line address a small multiply on the target.
inline constexpr uint32_t VIDCMD_LINE_STRIDE_BYTES = 320;

// Where the frame's VBLANK palette preamble is staged: one spare line slot above
// the 480 per-line regions.  The preamble is deposited during vertical blanking,
// with no HBLANK wait, which is where a sixteen-entry load belongs — 48 words is
// more than an HBLANK can deliver, and in vblank the writes' disturbed pixel
// slots do not exist.
inline constexpr uint32_t VIDCMD_PREAMBLE_SLOT = 480;   // V_ACTIVE

struct FrameParams
{
    // --- where things live in RAM ---
    uint32_t table_base       = DESC_TABLE_BASE;
    uint32_t table_bytes      = DESC_TABLE_BYTES;
    uint32_t fb_base          = 0;
    uint32_t fb_stride_bytes  = 0;
    uint32_t vidcmd_base      = 0;
    uint32_t audio_base       = 0;   // write_audio_source's layout, see below
    uint32_t audio_source_pairs = 0;

    // --- raster content ---
    PixelMode mode            = PixelMode::DIRECT_1BPP;
    uint32_t  v_scroll_lines  = 0;   // first framebuffer line shown
    uint32_t  h_scroll_pixels = 0;   // word offset + pixel_skip

    // --- VIDCMD content ---
    // A SET emitted *before* the line's first RUN is consumed during
    // blanking, applies immediately and costs no active slot.  A SET emitted
    // between records lands in active video and costs exactly one slot, which
    // is precisely the mechanism the mid-line split uses.
    bool per_line_palette     = false;   // reload entries 0 and 1 in every HBLANK
    bool per_line_mode        = false;   // SET pix_mode / pix_pixel_skip per line

    // THE PALETTE THE FRAME LOADS.  Nothing in hardware clears the RAM — not
    // /RS, not power-up — so a list must load every entry it displays, and the
    // model powers the RAM up with random contents so a list that forgets one
    // fails instead of reading a convenient black.  These entries are loaded
    // ONCE PER FRAME, in the frame's own preamble (line 0's blanking), each as
    // the three-word entry of vidcmd-spec.md §3.2.
    //
    // Loading once per frame rather than once per line is itself an assertion:
    // the entries have to survive 479 following lines and the /RS in between
    // frames, which is what "the RAM is not reset" means.
    std::array<Rgb444, PALETTE_ENTRIES> palette = {};
    uint32_t palette_entries  = 0;       // how many of them to load

    // pix_base: which PAIR (1bpp) or QUAD (2bpp) of palette entries the
    // playfield selects.  Emitted with the mode.
    uint32_t pix_base         = 0;

    // THE MID-LINE SPLIT.  Two SET(pix_base) records in a banked pair, landing
    // on consecutive slots: slot `split_pixel` selects the pair loaded with
    // split_bg and slot split_pixel+1 the pair loaded with split_fg, so each
    // SET's own pixel names it.  Both pairs carry ONE colour, which is what
    // makes "the pixel this SET landed on" unambiguous whatever the bitmap says.
    bool     mid_line_split   = false;
    uint32_t split_pixel      = 320;
    Rgb444   split_fg         = rgb444(15, 0, 0);
    Rgb444   split_bg         = rgb444(0, 15, 0);

    // The normative slot-arithmetic regression: RUN(passthrough,1),
    // SET(pix_pal_fg,C2), then a tail RUN whose length the builder derives —
    // 637 slots at the 2-clock fetch cadence, because the tail is a fresh fetch
    // and lands on slot 3 with slot 2 holding the same passthrough behind it.
    // Pixel 0 must render C1 and pixels 1..639 must render C2 either way: the
    // SET is the leading RUN's banked pair partner and still owns pixel 1.
    bool     slot_regression  = false;
    Rgb444   regression_c1    = rgb444(15, 0, 0);
    Rgb444   regression_c2    = rgb444(0, 0, 15);

    // Tempest-web: the wireframe lives in the pixel bitmap and never changes;
    // every moving object is authored per frame as RUN_COLOR spans in VIDCMD.
    bool     tempest_objects  = false;
    bool     depth_fade_palette = false;   // use tempest_palette_fg instead of the ramp
    uint32_t frame_index      = 0;         // animation phase, set per frame by the driver
    uint32_t tempest_stress_spans = 0;     // extra 1-px spans, for the density sweep

    SpriteStyle sprites       = SpriteStyle::NONE;
    FramingMode framing       = FramingMode::CUSHION;

    // A pure-VIDCMD screen: MASK/RUN/SET records only, and NO PIXELS
    // descriptors anywhere in the list.  pure_vidcmd is what author_frame()
    // reads; screen is what write_vidcmd_records() reads.  They are separate
    // because "which screen" and "does this list feed PIXEL" are separate
    // questions — a screen could in principle be composited over a bitmap.
    ScreenStyle screen        = ScreenStyle::NONE;
    bool        pure_vidcmd   = false;

    // JIT only: emit the {SET fg, SET bg, RUN(pt,1)} frame preamble on line 0
    // and nothing at all on the other 479 lines.  This is exactly what the
    // firmware console will do — three words per frame for the whole screen.
    bool     jit_frame_preamble = false;
    uint32_t  cursor_x        = 300;
    uint32_t  cursor_y        = 200;
    Rgb444    held_fg         = rgb444(15, 0, 0);
    Rgb444    held_bg         = rgb444(0, 0, 15);

    // Where the line's VIDCMD deposit sits in the per-line descriptor group.
    // See the group-order finding in main.cpp: the two consumers are not
    // symmetric, because COMPOSITOR gets 160 pixel clocks of HBLANK to fetch
    // and stage its first record while PIXEL's shift register reloads at pixel
    // 0 with nothing banked.
    bool vidcmd_after_pixels  = false;

    // Split the line's VIDCMD deposit: this many words ahead of the pixel
    // stream, the rest behind it.  0 means "all of it ahead".
    uint32_t vidcmd_head_words = 0;

    // Split the line's PIXELS deposit instead: this many pixel words first,
    // then the whole VIDCMD stream, then the rest of the pixels.  This is the
    // shape that scales, and the reason is that the two consumers drain at
    // wildly different rates.  One pixel word feeds 16 pixel clocks, so a dozen
    // of them buy PIXEL a third of a line of immunity for 24 SYSCLK.  One VIDCMD
    // word can feed as little as ONE pixel clock, so VIDCMD has no cheap head at
    // all and simply has to arrive first.  0 means "no head", i.e. the ordering
    // the short-stream cases use.
    uint32_t pixels_head_words = 0;

    // --- cross-chip skew compensation ---
    // The author places a SET this many slots early so it *takes effect* at
    // the pixel the caller asked for.  Provisional values live in
    // descriptor.h; passing them through here rather than reading the
    // constants directly is what lets main.cpp prove the compensation works by
    // re-running a case with nonzero skew and getting the same image.
    uint32_t skew_pix         = SKEW_PIX_TARGET;
    uint32_t skew_cmp         = SKEW_CMP_TARGET;

    // --- audio ---
    bool     audio            = false;
    uint32_t audio_preamble_pairs   = 23;  // covers the 45 blanked lines' pops
    uint32_t audio_burst_pairs      = 16;  // 16 pairs per 32 lines == break-even
    uint32_t audio_burst_interval   = 32;
    uint32_t audio_frame_pair_base  = 0;   // pair index into the source waveform

    // --- frame re-alignment ---
    // edma3.v only implements wait_hblank; there is no wait-VBLANK and no
    // wait-last-line-of-VBLANK.  A list armed by the ISR therefore has no way
    // to say "start at the top of the frame" — its first wait_hblank fires on
    // whatever line the CPU happened to arm it in.  The fix that costs nothing
    // in hardware is a run of wait_hblank descriptors with signal mask 0 and
    // count 1: each consumes exactly one HBLANK edge, so N of them walk the
    // list forward N lines.
    uint32_t vblank_pacing_lines = 0;

    // --- model parameter, so the author schedules with the same cost model
    //     the interpreter charges ---
    uint32_t arbitration_cycles = ENGINE_ARBITRATION_CYCLES;
};

struct AuthorResult
{
    uint32_t first_descriptor = 0;   // byte address to write into ENGINE's DESC
    uint32_t descriptor_count = 0;
    uint32_t table_bytes      = 0;
    uint32_t payload_words    = 0;   // total words the list reads off the bus
    uint32_t pacing_words     = 0;   // of which are mask=0 pacing reads
    uint32_t audio_pairs      = 0;
    bool     table_overflow   = false;
};

// --- payload / source-data writers -----------------------------------------

// A 1bpp test pattern: 16-pixel diagonal bands plus a 2-line marker bar every
// 32 lines, so both vertical scroll (bars move) and horizontal scroll (bands
// shift) are visible by eye and checkable by program.
void write_test_pattern_1bpp(Memory ram, uint32_t base, uint32_t stride_bytes,
                             uint32_t words_per_line, uint32_t lines);

// Every bit set, i.e. every pixel takes pix_pal_fg.  The slot-arithmetic
// regression needs a line whose colour depends only on the palette register,
// so that the pixel at which a SET takes effect is unambiguous.
void write_solid_pattern_1bpp(Memory ram, uint32_t base, uint32_t stride_bytes,
                              uint32_t words_per_line, uint32_t lines);

// A 2bpp line: sixteen 40-pixel blocks cycling through all four codes,
// with the phase stepping every 8 lines so the blocks staircase down the frame.
//
//   code(x, y) = ((x / IDX2_BLOCK_W) + (y / IDX2_BLOCK_PHASE)) % 4
//
// 640 dibits = 1280 bits = exactly 80 words, and the FIRST 40 words are the
// first 320 dibits — which is precisely what a HALF-RATE indexed line consumes,
// so one buffer serves both rates and the half-rate expectation is the
// full-rate one indexed by k/2.
inline constexpr uint32_t IDX2_BLOCK_W     = 40;
inline constexpr uint32_t IDX2_BLOCK_PHASE = 8;

constexpr uint32_t idx2_pattern_code(uint32_t x, uint32_t y)
{
    return ((x / IDX2_BLOCK_W) + (y / IDX2_BLOCK_PHASE)) % 4u;
}

void write_test_pattern_indexed2(Memory ram, uint32_t base, uint32_t stride_bytes,
                                 uint32_t lines);

// A 4bpp HALF-RATE line: 320 nibbles, each a palette index straight out, in
// sixteen 20-group bands that rotate one entry per line so every entry of the
// sixteen is exercised on every line and the bands stripe down the frame.
// 320 nibbles = 1280 bits = exactly 80 words.
constexpr uint32_t idx4_pattern_code(uint32_t group, uint32_t y)
{
    return ((group / 20u) + y) % PALETTE_ENTRIES;
}

void write_test_pattern_indexed4_half(Memory ram, uint32_t base, uint32_t stride_bytes,
                                      uint32_t lines);

// An ATTRIBUTE line: one {fg[3:0], bg[3:0], pattern[7:0]} word per eight-group
// cell, so 80 words at full rate and 40 at half.  The cell's colours step down
// the frame and across the line so a mis-paired colour byte is obvious, and the
// pattern is a glyph-like column pair that makes a wrong pattern bit visible.
constexpr uint32_t attr_cell_fg(uint32_t cell, uint32_t y)
{
    return (cell + y) % PALETTE_ENTRIES;
}

constexpr uint32_t attr_cell_bg(uint32_t cell, uint32_t y)
{
    return (cell + y + 8u) % PALETTE_ENTRIES;
}

constexpr uint32_t attr_cell_pattern(uint32_t cell, uint32_t y)
{
    return ((cell * 37u + y * 11u) & 0xFFu);
}

void write_test_pattern_attr(Memory ram, uint32_t base, uint32_t stride_bytes,
                             uint32_t cells_per_line, uint32_t lines);

// Writes each line's VIDCMD records.  Reports the per-line word count, record
// count, ACTIVE SLOT OCCUPANCY and cadence STRETCH through the caller-provided
// spans.  The occupancy is what vidcmd_plan_line() says the records really take
// — authored slots plus the HOLD slots the 2-clock fetch cadence spends between
// them — and it must be exactly H_ACTIVE under CUSHION or the stream desyncs
// for the rest of the frame.  The stretch is occupancy minus authored sum at
// the last record, i.e. how far the cadence pushed the line back; it is zero
// for any list whose records average two slots, and it is the WORST record's
// delay, not the last one's — a long RUN lets playback fall back behind the
// fetch, so a line can stretch in the middle and recover by its end.  Returns
// the total word count.
uint32_t write_vidcmd_records(const FrameParams &p, Memory ram,
                              std::span<uint8_t> line_words,
                              std::span<uint8_t> line_records,
                              std::span<uint16_t> line_slots,
                              std::span<uint16_t> line_stretch);

// An integer sawtooth on left and triangle on right — no <cmath>, so this
// compiles for the target and produces identical bytes on every host.
// The suite's stereo test signal, as the AUDIO chip takes it: `pairs`
// interleaved words at base, left in the high byte (channel 0's lane) and
// right in the low byte (channel 2's), then AUDIO_CONTROL_WORDS control bytes
// setting channels 0 and 2 to PERIOD 99 (the tick rate) and VOLUME 63.
inline constexpr uint32_t AUDIO_CONTROL_WORDS = 8;
constexpr uint32_t audio_control_base(uint32_t base, uint32_t pairs) { return base + pairs * 2; }
void write_audio_source(Memory ram, uint32_t base, uint32_t pairs);

// --- the list itself --------------------------------------------------------

// `vidcmd_line_words` must be the span write_vidcmd_records filled.
AuthorResult author_frame(const FrameParams &p, Memory ram,
                          std::span<const uint8_t> vidcmd_line_words);

// --- helpers the checks share with the authoring ----------------------------

// Per-line palette: fg walks a red-up/green-down ramp down the frame so any
// line whose SET went missing or landed late breaks a smooth gradient.
constexpr Rgb444 line_palette_fg(uint32_t line)
{
    const uint32_t r = (line * 16u) / V_ACTIVE;
    return rgb444(r, 15u - r, 15u);
}

constexpr Rgb444 line_palette_bg(uint32_t)
{
    return rgb444(0, 0, 5);
}

uint32_t pixel_words_for(PixelMode mode, uint32_t pixel_skip);

// ===========================================================================
// STATIC PIECES — a sprite line assembled by ENGINE out of read-only fragments
// ===========================================================================
//
// The question this answers is in STATIC-PIECES.md.  Two ways to put the same
// thirteen VIDCMD words in front of the compositor:
//
//   MONOLITHIC   what apps/smurf does today.  The CPU copies the fragments into
//                one contiguous program in scratch RAM, pads it to a fixed
//                SP_PROGRAM_WORDS shape, and one wait_hblank descriptor delivers
//                the whole thing in that line's own horizontal blanking.
//   PIECES       five descriptors, each pointing at data that is never written
//                after start-up: a two-word left cap, a gap word out of a
//                640-entry RUN(passthrough,n) table, the pose row, a tail word
//                out of that same table, and the right border word.  Nothing is
//                copied, so nothing can tear and no arena is needed — but five
//                descriptors cost four extra ENGINE turn-arounds, which is why
//                they have to be deposited AHEAD of the line they feed.
//
// The program, identical in both forms and framed at exactly H_ACTIVE slots:
//
//   SET(pix_base, b)          eager, executes in blanking, zero slots
//   RUN_COLOR(black, 64)      left border
//   RUN(passthrough, g)       gap                     } g + t == SP_MIDDLE_PIXELS
//   8 x (RUN_COLOR | RUN) of SP_POSE_SPAN slots       } so the sum is H_ACTIVE
//   RUN(passthrough, t)       tail
//   RUN_COLOR(black, 64)      right border
//
// Every record plays at least SP_POSE_SPAN slots, well above the two-slot fetch
// cadence, so the authored sum IS the occupancy and no line can stretch.

inline constexpr uint32_t SP_FIRST_LINE     = 200;   // first of the consecutive sprite lines
inline constexpr uint32_t SP_SPRITE_LINES   = 32;
inline constexpr uint32_t SP_BORDER_PIXELS  = 64;    // left and right, RUN_COLOR black
inline constexpr uint32_t SP_POSE_WORDS     = 8;     // words in one pose row
inline constexpr uint32_t SP_POSE_SPAN      = 8;     // slots each pose word paints
inline constexpr uint32_t SP_SPRITE_PIXELS  = SP_POSE_WORDS * SP_POSE_SPAN;
inline constexpr uint32_t SP_MIDDLE_PIXELS  =
    H_ACTIVE - 2u * SP_BORDER_PIXELS - SP_SPRITE_PIXELS;                    // 448
inline constexpr uint32_t SP_GAP_MIN        = 100;   // sprite x varies by moving the gap
inline constexpr uint32_t SP_GAP_STEP       = 8;
inline constexpr uint32_t SP_SEMANTIC_WORDS = 2 + 1 + SP_POSE_WORDS + 1 + 1;   // 13
inline constexpr uint32_t SP_PROGRAM_WORDS  = 21;   // the monolithic form's frozen shape
inline constexpr uint32_t SP_PAD_WORDS      = SP_PROGRAM_WORDS - SP_SEMANTIC_WORDS;
inline constexpr uint32_t SP_PROGRAM_STRIDE_BYTES = 64;
inline constexpr uint32_t SP_GAP_TABLE_ENTRIES    = H_ACTIVE;   // RUN(passthrough, 1..640)
inline constexpr uint32_t SP_PALETTE_ENTRIES      = 4;          // two 1bpp pairs
inline constexpr uint32_t SP_PALETTE_WORDS        =
    SP_PALETTE_ENTRIES * PALETTE_WORDS_PER_ENTRY;               // 12
inline constexpr uint32_t SP_PLAIN_LINE_WORDS     = 1;          // RUN(passthrough, 640)
inline constexpr uint32_t SP_PIXELS_CHUNK         = 20;         // two descriptors a line

// Offsets inside the read-only data region.  Spaced so a descriptor that walks
// off one table lands in zeroes rather than in the next one.
inline constexpr uint32_t SP_OFF_GAP_TABLE    = 0x000;
inline constexpr uint32_t SP_OFF_LEFT_CAPS    = 0x600;
inline constexpr uint32_t SP_OFF_RIGHT_BORDER = 0x610;
inline constexpr uint32_t SP_OFF_POSE_TABLE   = 0x700;
inline constexpr uint32_t SP_OFF_PALETTE      = 0xA00;

constexpr uint32_t sp_gap_word_addr(uint32_t data_base, uint32_t n)
{
    return data_base + SP_OFF_GAP_TABLE + (n - 1u) * 2u;
}

constexpr uint32_t sp_left_cap_addr(uint32_t data_base, uint32_t sel)
{
    return data_base + SP_OFF_LEFT_CAPS + sel * 4u;
}

constexpr uint32_t sp_right_border_addr(uint32_t data_base)
{
    return data_base + SP_OFF_RIGHT_BORDER;
}

constexpr uint32_t sp_pose_row_addr(uint32_t data_base, uint32_t i)
{
    return data_base + SP_OFF_POSE_TABLE + i * SP_POSE_WORDS * 2u;
}

constexpr uint32_t sp_palette_addr(uint32_t data_base)
{
    return data_base + SP_OFF_PALETTE;
}

constexpr uint32_t sp_program_addr(uint32_t program_base, uint32_t i)
{
    return program_base + i * SP_PROGRAM_STRIDE_BYTES;
}

constexpr bool sp_is_sprite_line(uint32_t line)
{
    return line >= SP_FIRST_LINE && line < SP_FIRST_LINE + SP_SPRITE_LINES;
}

constexpr uint32_t sp_sprite_index(uint32_t line)
{
    return line - SP_FIRST_LINE;
}

constexpr uint32_t sp_gap_for(uint32_t i)
{
    return SP_GAP_MIN + SP_GAP_STEP * i;
}

constexpr uint32_t sp_tail_for(uint32_t i)
{
    return SP_MIDDLE_PIXELS - sp_gap_for(i);
}

// Where the sprite's leftmost pixel lands: the whole point of "varying x".
constexpr uint32_t sp_sprite_x(uint32_t i)
{
    return SP_BORDER_PIXELS + sp_gap_for(i);
}

// Which 1bpp PAIR the playfield selects under sprite line i.  Alternating it is
// the probe for question 1 — a SET whose word is deposited four lines early
// still has to execute in ITS line's blanking, and if it did not the banding
// would shift by four lines and the pixel-exact comparison would break.
constexpr uint32_t sp_pix_base_sel(uint32_t i)
{
    return i & 1u;
}

constexpr uint32_t sp_pix_base_for(uint32_t i)
{
    return sp_pix_base_sel(i) * 2u;   // 0 -> entries {0,1}, 2 -> entries {2,3}
}

// A hole in the pose shows the playfield through, which is what makes the
// pix_base probe visible inside the sprite as well as beside it.
constexpr bool sp_pose_hole(uint32_t i, uint32_t j)
{
    return ((i + j) % 4u) == 3u;
}

constexpr uint32_t sp_pose_color(uint32_t i, uint32_t j)
{
    return 1u + ((i * 3u + j) % 7u);   // never RUN_COLOR_BLACK, so never the border
}

constexpr uint16_t sp_pose_word(uint32_t i, uint32_t j)
{
    return sp_pose_hole(i, j) ? vidcmd_run(RUN_SRC_PASSTHROUGH, SP_POSE_SPAN)
                              : vidcmd_run_color(sp_pose_color(i, j), SP_POSE_SPAN);
}

// The words of sprite line i's program, in stream order, semantic part only.
// Both forms deliver exactly these; MONOLITHIC appends SP_PAD_WORDS filler SETs.
uint32_t sp_program_words(uint32_t i, std::span<uint16_t> out);

enum class StaticPiecesForm : uint8_t
{
    MONOLITHIC = 0,
    PIECES     = 1,
};

struct StaticPiecesParams
{
    uint32_t table_base       = DESC_TABLE_BASE;
    uint32_t table_bytes      = DESC_TABLE_BYTES;
    uint32_t fb_base          = 0;
    uint32_t fb_stride_bytes  = 0;
    uint32_t data_base        = 0;   // the read-only fragments
    uint32_t program_base     = 0;   // MONOLITHIC's assembled programs

    StaticPiecesForm form      = StaticPiecesForm::MONOLITHIC;
    uint32_t         lookahead = 0;   // k: line y's program is deposited in group y-k

    // Where the line's VIDCMD descriptors sit relative to its PIXELS
    // descriptors.  At k == 0 the VIDCMD words belong to the imminent line and
    // must go first; at k > 0 they belong to a line k away and PIXELS — whose
    // PIX_PRELOAD deadline is 16 pixel clocks before the line — goes first.
    bool pixels_first = false;

    // The vblank walk, split so the preamble lands AFTER TIMING's /RS: the
    // FIFOs are emptied by that pulse, so a program deposited before it is
    // gone.  pacers_before_preamble pacers are consumed first, then the palette
    // and the first `lookahead` lines' programs, then the rest of the walk.
    uint32_t vblank_pacing_lines    = 0;
    uint32_t pacers_before_preamble = 0;

    uint32_t arbitration_cycles = ENGINE_ARBITRATION_CYCLES;
};

// Fill the read-only region (gap table, left caps, right border, pose table,
// palette entries) and MONOLITHIC's assembled programs.  Runs once, not per
// frame: that is the claim the PIECES form makes and this models it.
void write_static_pieces_data(const StaticPiecesParams &p, Memory ram,
                              std::span<const Rgb444> palette);

AuthorResult author_static_pieces(const StaticPiecesParams &p, Memory ram);

// VIDCMD words line `line` contributes to the stream, so a checker can turn a
// stream index into a line and test that line's delivery deadline.
constexpr uint32_t sp_line_stream_words(uint32_t line, StaticPiecesForm form)
{
    if (!sp_is_sprite_line(line))
    {
        return SP_PLAIN_LINE_WORDS;
    }
    return (form == StaticPiecesForm::MONOLITHIC) ? SP_PROGRAM_WORDS : SP_SEMANTIC_WORDS;
}

}  // namespace SuperEngine
