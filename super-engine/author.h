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

// Density stress: N one-pixel spans at a two-pixel pitch on one band of rows.
// One pixel of span alternating with one pixel of gap is the worst case the
// format allows — every record is one word and every playback is one slot, so
// the prefetch invariant playback(k) >= words(k+1) is satisfied with exactly
// zero margin.  Sweeping this is how the suite turns "the demo works" into an
// object budget.
// The spans are SPREAD evenly across the line rather than packed together,
// which is what a well full of objects actually looks like and, more
// importantly, is the arrangement the delivery rate can hope to sustain: the
// engine puts one VIDCMD word on the bus every 2 SYSCLK, i.e. one word per ~3.6
// pixel clocks, so a burst denser than that has to have been pre-buffered
// during HBLANK and cannot be streamed against the beam.
inline constexpr uint32_t TEMPEST_MAX_STRESS_SPANS = 64;
inline constexpr uint32_t TEMPEST_STRESS_ROW       = 236;
inline constexpr uint32_t TEMPEST_STRESS_ROWS      = 6;
inline constexpr uint32_t TEMPEST_STRESS_COLOR     = RUN_COLOR_MAGENTA;

// The most coloured spans any one line can carry.
inline constexpr uint32_t TEMPEST_MAX_SPANS =
    TEMPEST_CLAW_PRONGS + TEMPEST_FLIPPERS + TEMPEST_SHOTS + TEMPEST_MAX_STRESS_SPANS;

enum class PixelMode : uint8_t
{
    DIRECT_1BPP = 0,   // 40 words/line, 1 bit per pixel clock
    MICRO_HAM   = 1,   // 80 words/line, 2 bits per pixel clock
};

enum class TileStyle : uint8_t
{
    NONE         = 0,   // the line's VIDCMD stream is just its coverage RUN
    CURSOR       = 1,   // one 16x16 tile on 16 lines
    FOUR_SPRITES = 2,   // worst case: four tiles on every line
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
    // A SET emitted *before* the line's first RUN/TILE is consumed during
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
    // SET(pix_pal_fg,C2), RUN(passthrough,638) over all-foreground pixel bits.
    // Pixel 0 must render C1 and pixels 1..639 must render C2.
    bool     slot_regression  = false;
    Rgb444   regression_c1    = rgb444(15, 0, 0);
    Rgb444   regression_c2    = rgb444(0, 0, 15);

    // Tempest-web: the wireframe lives in the pixel bitmap and never changes;
    // every moving object is authored per frame as RUN_COLOR spans in VIDCMD.
    bool     tempest_objects  = false;
    bool     depth_fade_palette = false;   // use tempest_palette_fg instead of the ramp
    uint32_t frame_index      = 0;         // animation phase, set per frame by the driver
    uint32_t tempest_stress_spans = 0;     // extra 1-px spans, for the density sweep

    TileStyle tiles           = TileStyle::NONE;
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
// count and ACTIVE SLOT SUM through the caller-provided spans; the slot sum
// must be exactly H_ACTIVE or the stream desyncs for the rest of the frame, so
// exposing it is the cheapest way for a builder to catch its own arithmetic.
// Returns the total word count.
uint32_t write_vidcmd_records(const FrameParams &p, Memory ram,
                              std::span<uint8_t> line_words,
                              std::span<uint8_t> line_records,
                              std::span<uint16_t> line_slots);

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
