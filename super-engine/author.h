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

enum class PixelMode : uint8_t
{
    DIRECT_1BPP = 0,   // 40 words/line, 1 bit per pixel clock
    MICRO_HAM   = 1,   // 80 words/line, 2 bits per pixel clock
};

enum class SpriteStyle : uint8_t
{
    NONE         = 0,   // the line's VIDCMD stream is just its coverage RUN
    CURSOR       = 1,   // one 16x16 arrow on 16 lines, as spans
    FOUR_SPRITES = 2,   // worst case: four sprites on every line, as spans
};

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

// Bytes reserved per line in the authored VIDCMD region.  160 words is well
// above the ~150-word worst case the tempest density sweep reaches, and keeps
// the per-line address a small multiply on the target.
inline constexpr uint32_t VIDCMD_LINE_STRIDE_BYTES = 320;

struct FrameParams
{
    // --- where things live in RAM ---
    uint32_t table_base       = DESC_TABLE_BASE;
    uint32_t table_bytes      = DESC_TABLE_BYTES;
    uint32_t fb_base          = 0;
    uint32_t fb_stride_bytes  = 0;
    uint32_t vidcmd_base      = 0;
    uint32_t audio_base       = 0;

    // --- raster content ---
    PixelMode mode            = PixelMode::DIRECT_1BPP;
    uint32_t  v_scroll_lines  = 0;   // first framebuffer line shown
    uint32_t  h_scroll_pixels = 0;   // word offset + pixel_skip

    // --- VIDCMD content ---
    // A SET emitted *before* the line's first RUN is consumed during
    // blanking, applies immediately and costs no active slot.  A SET emitted
    // between records lands in active video and costs exactly one slot, which
    // is precisely the mechanism the mid-line split uses.
    bool per_line_palette     = false;   // SET pix_pal_fg / pix_pal_bg per line
    bool per_line_mode        = false;   // SET pix_mode / pix_pixel_skip per line

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

// A micro-HAM line, hand-checkable by construction:
//   pixels   0..159  code 0_1  -> held <- pix_pal_fg
//   pixels 160..319  code 0_0  -> held <- pix_pal_bg
//   pixels 320..639  4-bit chroma codes stepping red/green/blue between 0x0
//                    and 0xF, giving eight flat colour blocks
// 160*2 + 160*2 + 160*4 = 1280 bits = exactly 80 words.
void write_test_pattern_microham(Memory ram, uint32_t base, uint32_t stride_bytes,
                                 uint32_t lines);

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

}  // namespace SuperEngine
