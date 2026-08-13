// descriptor.h — encodings for the Griffin "super-engine" display-list DMA and
// the VIDCMD instruction stream.
//
// ===========================================================================
// WHAT THIS SUITE PROVES, AND WHAT IT DOES NOT
// ===========================================================================
//
// It proves SEMANTICS: that display lists can express the use cases, that the
// per-line bus budget closes, that the FIFOs stay fed, and that the VIDCMD
// slot arithmetic lands deposits on the pixels the author intended.
//
// It does NOT prove FIT.  The COMPOSITOR macrocell budget is the open
// feasibility question — roughly 140+ flip-flops against 128 macrocells before
// any levers are pulled, and going from R3G3B2 to R4G4B4 made it worse, not
// better.  Everything below is written as if the generous version fits.  The
// assumptions that the fit experiment may force to shrink are listed here in
// one place so that shrinking one later is a visible, deliberate change to
// this model rather than an archaeological dig through render.cpp:
//
//   FIT-RISKY ASSUMPTION 1: 12-bit playback counters.  RUN carries a 12-bit
//     complemented count and the playback machine upcounts it.  A 10-bit
//     counter covers every count a 640-pixel line can use and saves 2 FFs in
//     the staging copy and 2 in playback; the encoding would keep 12 bits.
//   FIT-RISKY ASSUMPTION 2: TILE keeps TWO 16-bit mask words (select and
//     opacity) — 32 FFs staged plus 32 shifting.  Dropping to a single opacity
//     mask with a colour chosen by a flag bit saves 32 FFs and loses two-colour
//     tiles.  This is the single biggest lever and the first thing to give up.
//   FIT-RISKY ASSUMPTION 3: a 12-bit SET staging/commit path.  SET's value
//     field is 12 bits and this model commits it atomically into whichever
//     register the 3-bit target names.  If the register file has to be written
//     in halves the slot semantics below change shape.
//   FIT-RISKY ASSUMPTION 4: a 3-stage PIXEL shadow/pending/commit pipeline is
//     assumed to be short enough that SKEW_PIX_TARGET can be zero.  It is a
//     provisional guess; see the skew constants near the bottom of this file.
//   FIT-RISKY ASSUMPTION 5 — RUN_COLOR, AND THIS ONE IS EXPLICITLY AT RISK.
//     RUN_COLOR needs its own 3-bit colour latch in the PLAYBACK registers, not
//     in staging: staging is clobbered by the next record's prefetch while the
//     run is still playing, so the ~3 FFs cannot be shared.  It also adds a
//     fourth input to the 12-bit output mux — roughly one extra product term on
//     each of the twelve output equations — plus a sliver of decode on the RUN
//     source field.  The compositor is ALREADY estimated over 128 FFs before
//     this, so RUN_COLOR is a line item that may not survive the fit.  The
//     natural thing to trade for it is run-invert, which is what RUN_COLOR
//     displaced from the src=11 encoding in the first place.
//
// ===========================================================================
// PIPELINE
// ===========================================================================
//
//   ENGINE (descriptor DMA) --> PIXELS FIFO --> PIXEL --12-bit RGB--> COMPOSITOR --> DAC
//                          \--> VIDCMD FIFO --------------------------^
//
// Neither PIXEL nor COMPOSITOR has a CPU bus; both live in the 25.175 MHz
// pixel-clock domain and are configured entirely in band.  COMPOSITOR is the
// only instruction decoder: it plays RUN/TILE records against the pixel stream
// and it also issues register writes out of that same stream into PIXEL.
//
// Colour is 12-bit R4G4B4 everywhere — palette entries, the micro-HAM held
// colour, the compositor's held colours and the DAC.
//
// ===========================================================================
// PORTABILITY
// ===========================================================================
//
// This header and author.cpp must also compile with the 68000 app toolchain,
// because the same authoring code is meant to run on the target to build
// display lists in RAM.  Only <cstdint>, <cstddef>, <span>, <array> and
// ../griffin.generated.h; no allocation, no I/O, no exceptions.  NEVER include
// griffin.generated.refs.h — it defines objects, not just constants.
//
// Naming follows griffin.generated.h style so these constants can migrate into
// griffin.yml and be emitted by codegen.py verbatim once the format settles.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../griffin.generated.h"

namespace SuperEngine
{

// ============================================================================
// Raster geometry and the SYSCLK <-> pixel-clock mapping
// ============================================================================

// SYSCLK is the one number that already lives in griffin.yml; the VGA numbers
// below are hard-wired in cpld/video/video.v and are repeated here because the
// suite must reason about them at cycle granularity.
inline constexpr uint64_t SYSCLK_HZ    = Griffin::SYSCLK_HZ;   // 14,000,000
inline constexpr uint64_t PIXEL_CLK_HZ = 25175000ULL;          // VGA 640x480@60

inline constexpr uint32_t H_TOTAL      = 800;
inline constexpr uint32_t H_ACTIVE     = 640;
inline constexpr uint32_t H_BLANK      = H_TOTAL - H_ACTIVE;   // 160 pixel clocks
inline constexpr uint32_t V_TOTAL      = 525;
inline constexpr uint32_t V_ACTIVE     = 480;
inline constexpr uint32_t V_SYNC_START = 490;                  // /RS to PIXELS+VIDCMD

// GCD(14000000, 25175000) = 25000, so one pixel clock is exactly 560/1007
// SYSCLK cycles.  Doing the conversion with this integer pair (the same pair
// emulator.cpp's VideoState uses as LINE_NUM/LINE_DEN) keeps the entire suite
// free of floating point, which is what makes two runs byte-identical.
inline constexpr uint64_t PIXEL_SYSCLK_NUM = 560;
inline constexpr uint64_t PIXEL_SYSCLK_DEN = 1007;

// SYSCLK cycle at which pixel clock number `pixel` (counted from the raster
// origin) occurs.  Truncating is the right rounding here: the pixel is
// "visible from" this cycle onward, so a deposit at exactly this cycle is
// still in time for it.
constexpr uint64_t sysclk_of_pixel(uint64_t pixel)
{
    return (pixel * PIXEL_SYSCLK_NUM) / PIXEL_SYSCLK_DEN;
}

// Reporting-only rounded durations.  All scheduling uses the exact rational
// above; these two exist so the budget table can print "x of 445" without
// re-deriving it.  445 = round(448000/1007), 89 = round(89600/1007).
inline constexpr uint64_t LINE_SYSCLK =
    (H_TOTAL * PIXEL_SYSCLK_NUM + PIXEL_SYSCLK_DEN / 2) / PIXEL_SYSCLK_DEN;
inline constexpr uint64_t HBLANK_SYSCLK =
    (H_BLANK * PIXEL_SYSCLK_NUM + PIXEL_SYSCLK_DEN / 2) / PIXEL_SYSCLK_DEN;

// ============================================================================
// Colour — 12-bit R4G4B4
// ============================================================================

using Rgb444 = uint16_t;   // 0x0RGB, four bits per channel

inline constexpr Rgb444 RGB444_WHITE = 0x0FFF;
inline constexpr Rgb444 RGB444_BLACK = 0x0000;

constexpr Rgb444 rgb444(uint32_t r, uint32_t g, uint32_t b)
{
    return static_cast<Rgb444>(((r & 0xF) << 8) | ((g & 0xF) << 4) | (b & 0xF));
}

constexpr uint32_t rgb444_r(Rgb444 c) { return (c >> 8) & 0xF; }
constexpr uint32_t rgb444_g(Rgb444 c) { return (c >> 4) & 0xF; }
constexpr uint32_t rgb444_b(Rgb444 c) { return c & 0xF; }

// 4 bits to 8 bits is exactly x17 (0xF*17 = 255), i.e. nibble replication —
// the same "replicate the field across the byte" rule emulator.cpp:1065 uses
// for R3G3B2, just with a field that divides the byte evenly.
constexpr uint8_t rgb444_channel_to_8(uint32_t nibble)
{
    return static_cast<uint8_t>((nibble & 0xF) * 17);
}

// ============================================================================
// Memory
// ============================================================================

// Rev-1 memory is 2x AS6C4008 = 4 MB of 0-wait-state SRAM.  The engine's
// source address is 22 bits of word address, so it can reach all of it.
inline constexpr uint32_t RAM_BYTES = 0x400000;

// A flat RAM image viewed as 16-bit words, exactly how the DMA engine sees it:
// index = byte_address / 2.  Host and target agree because nothing here cares
// about host endianness — the "words" *are* the bus words.
struct Memory
{
    std::span<uint16_t> words;

    constexpr uint16_t &operator[](uint32_t byte_address) const
    {
        return words[byte_address >> 1];
    }

    constexpr uint32_t size_bytes() const
    {
        return static_cast<uint32_t>(words.size() * sizeof(uint16_t));
    }
};

// ============================================================================
// Deposit signals — bit index in the descriptor's nSIGNAL mask
// ============================================================================
//
// Three one-hot strobes and one spare, down from seven.  VIDEO_PALETTE and
// VIDEO_MODE are gone: every palette / mode / pixel_skip change now travels as
// a VIDCMD SET instruction, so the only things the engine has to strobe are
// the three FIFOs.
//
// A mask of 0 strobes nothing: the descriptor still reads its payload words
// off the bus, so it is a *pure pacing* descriptor that costs bus time and
// nothing else.  It is free in hardware and it is how a list waits (see
// vblank_pacing_lines in author.h).

inline constexpr uint8_t SIGNAL_NONE          = 0x00;
inline constexpr uint8_t SIGNAL_PIXELS_FIFO_W = 0x01;  // bit 0
inline constexpr uint8_t SIGNAL_VIDCMD_FIFO_W = 0x02;  // bit 1
inline constexpr uint8_t SIGNAL_AUDIO_FIFO_W  = 0x04;  // bit 2
inline constexpr uint8_t SIGNAL_SPARE_3       = 0x08;  // bit 3 (unassigned)
inline constexpr uint8_t SIGNAL_MASK_ALL      = 0x0F;

// ============================================================================
// Descriptor format (edma3.v:15-24, with the narrowed signal field)
// ============================================================================

inline constexpr uint32_t DESC_WORDS       = 4;    // three used + one pad
inline constexpr uint32_t DESC_BYTES       = DESC_WORDS * 2;
inline constexpr uint32_t DESC_TABLE_BASE  = 0x3F0000;  // RAM's top 64K
inline constexpr uint32_t DESC_TABLE_BYTES = 0x10000;   // 15-bit desc_ptr
inline constexpr uint32_t DESC_MAX_COUNT   = 32;   // count[13:9] is 0-biased

inline constexpr uint16_t DESC_WAIT_HBLANK_MASK  = 0x8000;
inline constexpr uint16_t DESC_STOP_AFTER_MASK   = 0x4000;
inline constexpr uint16_t DESC_COUNT_MASK        = 0x3E00;
inline constexpr uint16_t DESC_COUNT_SHIFT       = 9;
inline constexpr uint16_t DESC_SIGNAL_MASK       = 0x000F;

// word0 [8:7] were always reserved; [6:4] became reserved when the strobe
// count dropped from seven to four.  The fit experiment may well reclaim them
// (a wider count field is the obvious customer — a 6-bit count would let one
// descriptor carry a whole 40-word 1bpp line), so they are checked for zero
// here rather than quietly ignored.
inline constexpr uint16_t DESC_RESERVED_MASK     = 0x01F0;

// Host/target-side view of one descriptor.  `src` is a *byte* address (even);
// the engine keeps A[22:1] so bit 0 is not representable.
struct Descriptor
{
    uint32_t src         = 0;
    uint16_t count       = 1;      // payload words, 1..32
    uint8_t  signal_mask = SIGNAL_NONE;
    bool     wait_hblank = false;
    bool     stop_after  = false;
};

struct DescriptorWords
{
    uint16_t w[DESC_WORDS];
};

constexpr DescriptorWords encode_descriptor(const Descriptor &d)
{
    DescriptorWords out{};
    const uint16_t biased = static_cast<uint16_t>((d.count - 1) & 0x1F);
    out.w[0] = static_cast<uint16_t>((d.wait_hblank ? DESC_WAIT_HBLANK_MASK : 0) |
                                     (d.stop_after ? DESC_STOP_AFTER_MASK : 0) |
                                     (biased << DESC_COUNT_SHIFT) |
                                     (d.signal_mask & DESC_SIGNAL_MASK));
    out.w[1] = static_cast<uint16_t>((d.src >> 16) & 0x7F);
    out.w[2] = static_cast<uint16_t>(d.src & 0xFFFE);
    out.w[3] = 0;
    return out;
}

constexpr Descriptor decode_descriptor(const uint16_t *w)
{
    Descriptor d{};
    d.wait_hblank = (w[0] & DESC_WAIT_HBLANK_MASK) != 0;
    d.stop_after  = (w[0] & DESC_STOP_AFTER_MASK) != 0;
    d.count       = static_cast<uint16_t>(((w[0] & DESC_COUNT_MASK) >> DESC_COUNT_SHIFT) + 1);
    d.signal_mask = static_cast<uint8_t>(w[0] & DESC_SIGNAL_MASK);
    d.src         = (static_cast<uint32_t>(w[1] & 0x7F) << 16) |
                    static_cast<uint32_t>(w[2] & 0xFFFE);
    return d;
}

// ============================================================================
// Engine cost model — shared by the interpreter and by the authoring code
// ============================================================================
//
// Straight from the edma3.v state machine, one state per SYSCLK:
//   RELEASE(1) is charged to the descriptor that ends, ASSERT(1) settles the
//   SRAM, each of the four descriptor words takes SETTLE+STROBE = 2, and each
//   payload word takes SETTLE+STROBE = 2.
//
// ENGINE_ARBITRATION_CYCLES is the one *modelled* number rather than a counted
// one: STATE_REQUEST waits for nBG and STATE_WAIT_FREE waits for the CPU's nAS
// to go idle, and how long that takes depends on where in its bus cycle the
// 68000 happens to be (plus two SYSCLK of input synchronizer each).  A real
// 68000 grants BG in 1-3 clocks and can hold AS for the rest of a 4-clock bus
// cycle, so 4 is the honest mid-range figure; it is a parameter everywhere so
// a case can be re-run pessimistically, and main.cpp does exactly that.
inline constexpr uint32_t ENGINE_ARBITRATION_CYCLES = 4;
inline constexpr uint32_t ENGINE_ASSERT_CYCLES      = 1;
inline constexpr uint32_t ENGINE_FETCH_CYCLES       = DESC_WORDS * 2;  // 8
inline constexpr uint32_t ENGINE_RELEASE_CYCLES     = 1;
inline constexpr uint32_t ENGINE_CYCLES_PER_WORD    = 2;

// edma3.v samples HBLANK through a 2-FF synchronizer and then looks for the
// rising edge of the *synchronized* signal, so the engine reacts two SYSCLK
// after the pixel-domain edge at h=640.
inline constexpr uint32_t ENGINE_HBLANK_SYNC_CYCLES = 2;

// Cost of one non-waiting descriptor, arbitration through release.
constexpr uint32_t engine_descriptor_cycles(uint32_t arbitration, uint32_t count)
{
    return arbitration + ENGINE_ASSERT_CYCLES + ENGINE_FETCH_CYCLES +
           ENGINE_CYCLES_PER_WORD * count + ENGINE_RELEASE_CYCLES;
}

// A wait_hblank descriptor pays for the bus twice: once to fetch the four
// descriptor words, then STATE_HBLANK_RELEASE gives the bus back and it
// re-arbitrates after the edge to run the payload.
constexpr uint32_t engine_wait_descriptor_cycles(uint32_t arbitration, uint32_t count)
{
    return arbitration + ENGINE_ASSERT_CYCLES + ENGINE_FETCH_CYCLES + 1 /* HBLANK_RELEASE */ +
           arbitration + ENGINE_ASSERT_CYCLES +
           ENGINE_CYCLES_PER_WORD * count + ENGINE_RELEASE_CYCLES;
}

// ============================================================================
// PIXELS stream
// ============================================================================
//
// Pure pixel bits — no header words at all any more, because palette and mode
// left the stream for VIDCMD.  The line length is a function of the mode:
//
//   direct 1bpp   1 bit per pixel clock,  640 bits  = 40 words
//   micro-HAM     2 bits per pixel clock, 1280 bits = 80 words
//
// Big-endian, MSB of each word leftmost.  pixel_skip discards that many bits
// of the line's first word at line start, which is why a nonzero skip needs
// one more word to reach the last pixel.

inline constexpr uint32_t PIXELS_WORDS_1BPP     = 40;
inline constexpr uint32_t PIXELS_WORDS_MICROHAM = 80;

// IDT7200L-15, 256x9, two in parallel: each FIFO holds 256 bus words.
inline constexpr uint32_t PIXELS_FIFO_WORDS = 256;
inline constexpr uint32_t VIDCMD_FIFO_WORDS = 256;

inline constexpr uint32_t PIXEL_MODE_DIRECT_1BPP = 0;
inline constexpr uint32_t PIXEL_MODE_MICRO_HAM   = 1;

// pixel_skip now rides a 12-bit SET value instead of a 3-bit slice of a MODE
// word, so it finally spans a whole 16-bit FIFO word.  That closes the
// horizontal-scroll gap the previous round of this suite reported: the source
// address moves in whole words and skip covers 0..15 within one, so every
// pixel offset is now reachable.
inline constexpr uint32_t PIXELS_SKIP_MAX = 15;

// micro-HAM codes, consumed at exactly 2 bits per pixel clock so a 2-bit code
// covers 1 pixel and a 4-bit code covers 2:
//
//   0 p        held <- p ? pix_pal_fg : pix_pal_bg          1 pixel
//   1 0 g r    held.green <- g*0xF, held.red   <- r*0xF     2 pixels
//   1 1 g b    held.green <- g*0xF, held.blue  <- b*0xF     2 pixels
//
// At the start of every visible line held <- pix_pal_fg.  The codes can only
// reach the 0x0/0xF extremes of a channel; a full-precision colour arrives via
// SET(pix_ham_held), which is the whole reason that target exists.
inline constexpr uint32_t HAM_CODE_PALETTE_BITS = 2;
inline constexpr uint32_t HAM_CODE_CHROMA_BITS  = 4;

// Bits of the pixel stream consumed per pixel clock, by mode.
constexpr uint32_t pixels_bits_per_clock(uint32_t mode)
{
    return (mode == PIXEL_MODE_MICRO_HAM) ? 2u : 1u;
}

constexpr uint32_t pixels_words_per_line(uint32_t mode, uint32_t pixel_skip)
{
    const uint32_t bits = H_ACTIVE * pixels_bits_per_clock(mode) + pixel_skip;
    return (bits + 15) / 16;
}

// ============================================================================
// VIDCMD instruction stream
// ============================================================================
//
// 16-bit words, prefix coded on the top bits:
//
//   00 ss ~count[11:0]              RUN    1 word
//   01 ~skip[9:0] flags[3:0]        TILE   3 words (+ select mask, + opacity mask)
//   1  ttt value[11:0]              SET    1 word
//
// Counts are stored complemented exactly as the compositor wants them: on
// ATF15xx an up-counter with an all-1s terminal is far cheaper than a
// down-counter's zero detect, so the encoder stores ~count and the decoder
// complements it back.
//
// FRAMING IS DURATION ARITHMETIC.  There is no fixed word count per line and
// no drain-to-N.  A line's records must account for exactly H_ACTIVE active
// slots, where RUN and TILE contribute their playback durations and every SET
// consumed *during active video* contributes exactly one slot.  Get the sum
// wrong and the stream desyncs and stays desynced until vsync's /RS — the
// error is not contained to its own line the way drain-to-N used to contain
// it.  render.cpp models that containment; author.cpp reports each line's slot
// sum so a list can be checked before it is run.

enum class VidcmdType : uint8_t
{
    RUN  = 0,
    TILE = 1,
    SET  = 2,
};

// RUN's 2-bit source select.
inline constexpr uint32_t RUN_SRC_PASSTHROUGH = 0;   // RGB_IN from PIXEL
inline constexpr uint32_t RUN_SRC_HELD_FG     = 1;   // cmp_held_fg
inline constexpr uint32_t RUN_SRC_HELD_BG     = 2;   // cmp_held_bg
inline constexpr uint32_t RUN_SRC_COLOR       = 3;   // RUN_COLOR, see below

// --- RUN_COLOR ------------------------------------------------------------
//
//   { 2'b00, 2'b11, colour[2:0], ~count[8:0] }              1 word
//
// A playback record like any other RUN: it emits its colour for `count`
// pixels, one slot per pixel, and those slots count toward the line's 640.
// What makes it worth an encoding is what it does NOT do — it never touches
// held_fg or held_bg.  Painting a span with the held colours costs a SET, and
// because each SET owns a pixel slot a two-register change lands one pixel
// apart (see the mid-line-split case).  RUN_COLOR is a single atomic word.
//
// colour[2:0] is {R, G, B}: bit 11 red, bit 10 green, bit 9 blue.  Each bit is
// replicated across its whole nibble — {4{r}, 4{g}, 4{b}} — the same trick the
// micro-HAM chroma codes use, giving the eight saturated corners of the cube.
//
// The count is 9 bits and stored complemented, matching the convention the
// 12-bit RUN count already uses: the field is ~count masked to the field
// width, so decoding is another complement and the maximum span is 511 pixels.
// A one-word record also satisfies the prefetch invariant unconditionally,
// since playback(k) >= 1 >= words(k+1) for any k whose successor is one word.
inline constexpr uint32_t RUN_COLOR_MAX_COUNT = 511;

inline constexpr uint32_t RUN_COLOR_BLACK   = 0;   // 000
inline constexpr uint32_t RUN_COLOR_BLUE    = 1;   // 001
inline constexpr uint32_t RUN_COLOR_GREEN   = 2;   // 010
inline constexpr uint32_t RUN_COLOR_CYAN    = 3;   // 011
inline constexpr uint32_t RUN_COLOR_RED     = 4;   // 100
inline constexpr uint32_t RUN_COLOR_MAGENTA = 5;   // 101
inline constexpr uint32_t RUN_COLOR_YELLOW  = 6;   // 110
inline constexpr uint32_t RUN_COLOR_WHITE   = 7;   // 111

constexpr Rgb444 run_colour_to_rgb444(uint32_t code)
{
    return rgb444(((code >> 2) & 1u) != 0 ? 0xFu : 0x0u,
                  ((code >> 1) & 1u) != 0 ? 0xFu : 0x0u,
                  ((code >> 0) & 1u) != 0 ? 0xFu : 0x0u);
}

// SET targets, PROVISIONAL numbering (index order as specified).  0..1 are
// COMPOSITOR's own registers, 2..6 are forwarded to PIXEL, 7 is spare.
inline constexpr uint32_t SET_CMP_HELD_FG    = 0;
inline constexpr uint32_t SET_CMP_HELD_BG    = 1;
inline constexpr uint32_t SET_PIX_PAL_FG     = 2;
inline constexpr uint32_t SET_PIX_PAL_BG     = 3;
inline constexpr uint32_t SET_PIX_HAM_HELD   = 4;
inline constexpr uint32_t SET_PIX_MODE       = 5;
inline constexpr uint32_t SET_PIX_PIXEL_SKIP = 6;
inline constexpr uint32_t SET_SPARE_7        = 7;

constexpr bool vidcmd_set_targets_pixel(uint32_t target)
{
    return target >= SET_PIX_PAL_FG && target <= SET_PIX_PIXEL_SKIP;
}

inline constexpr uint32_t VIDCMD_TILE_WORDS  = 3;
inline constexpr uint32_t VIDCMD_TILE_PIXELS = 16;

// Held colours at /RS (vsync).  Every frame's VBLANK preamble must re-assert
// whatever it actually wants.
inline constexpr Rgb444 VIDCMD_RESET_HELD_FG = RGB444_WHITE;
inline constexpr Rgb444 VIDCMD_RESET_HELD_BG = RGB444_BLACK;

constexpr VidcmdType vidcmd_type_of(uint16_t w)
{
    if ((w & 0x8000) != 0)
    {
        return VidcmdType::SET;
    }
    return ((w >> 14) & 1) != 0 ? VidcmdType::TILE : VidcmdType::RUN;
}

constexpr uint16_t vidcmd_run(uint32_t src, uint32_t count)
{
    return static_cast<uint16_t>((0u << 14) | ((src & 0x3) << 12) | ((~count) & 0xFFF));
}

constexpr uint32_t vidcmd_run_src(uint16_t w)   { return (w >> 12) & 0x3; }
constexpr uint32_t vidcmd_run_count(uint16_t w) { return static_cast<uint32_t>((~w) & 0xFFF); }

constexpr uint16_t vidcmd_run_color(uint32_t colour, uint32_t count)
{
    return static_cast<uint16_t>((0u << 14) | (RUN_SRC_COLOR << 12) |
                                 ((colour & 0x7) << 9) | ((~count) & 0x1FF));
}

constexpr uint32_t vidcmd_run_color_code(uint16_t w)  { return (w >> 9) & 0x7; }
constexpr uint32_t vidcmd_run_color_count(uint16_t w) { return static_cast<uint32_t>((~w) & 0x1FF); }

constexpr uint16_t vidcmd_tile(uint32_t skip, uint32_t flags)
{
    return static_cast<uint16_t>((1u << 14) | (((~skip) & 0x3FF) << 4) | (flags & 0xF));
}

constexpr uint32_t vidcmd_tile_skip(uint16_t w)  { return static_cast<uint32_t>((~(w >> 4)) & 0x3FF); }
constexpr uint32_t vidcmd_tile_flags(uint16_t w) { return static_cast<uint32_t>(w & 0xF); }

constexpr uint16_t vidcmd_set(uint32_t target, uint32_t value)
{
    return static_cast<uint16_t>(0x8000u | ((target & 0x7) << 12) | (value & 0xFFF));
}

constexpr uint32_t vidcmd_set_target(uint16_t w) { return (w >> 12) & 0x7; }
constexpr uint32_t vidcmd_set_value(uint16_t w)  { return w & 0xFFF; }

// Words this record occupies in the stream, from its lead word.
constexpr uint32_t vidcmd_record_words(uint16_t lead)
{
    return vidcmd_type_of(lead) == VidcmdType::TILE ? VIDCMD_TILE_WORDS : 1u;
}

// Active slots this record consumes, from its lead word.  A SET is one slot;
// TILE is skip plus its 16 masked pixels; RUN is its count.
constexpr uint32_t vidcmd_record_slots(uint16_t lead)
{
    switch (vidcmd_type_of(lead))
    {
        // RUN_COLOR steals three of RUN's count bits for its colour, so its
        // count has to be read out of the narrower field.
        case VidcmdType::RUN:  return (vidcmd_run_src(lead) == RUN_SRC_COLOR)
                                          ? vidcmd_run_color_count(lead)
                                          : vidcmd_run_count(lead);
        case VidcmdType::TILE: return vidcmd_tile_skip(lead) + VIDCMD_TILE_PIXELS;
        default:               return 1;
    }
}

// ---------------------------------------------------------------------------
// Cross-chip skew — PROVISIONAL, and loudly so
// ---------------------------------------------------------------------------
//
// A SET aimed at COMPOSITOR's own held colours commits inside COMPOSITOR; a
// SET aimed at PIXEL has to cross a chip boundary and land in a register one
// or two pipeline stages away.  The two therefore take effect at slightly
// different pixels, and the difference is a property of the RTL, not of the
// list.
//
// BOTH VALUES BELOW ARE GUESSES.  They are zero because the model assumes a
// short enough forwarding path (FIT-RISKY ASSUMPTION 4), and they exist as
// named constants so that when RTL simulation pins the real numbers, exactly
// two lines change here and the list builder's compensation follows
// automatically.  main.cpp deliberately re-runs a case with nonzero skew to
// prove the compensation is real rather than vacuous.
inline constexpr uint32_t SKEW_PIX_TARGET = 0;   // PROVISIONAL
inline constexpr uint32_t SKEW_CMP_TARGET = 0;   // PROVISIONAL

constexpr uint32_t vidcmd_set_skew(uint32_t target, uint32_t skew_pix, uint32_t skew_cmp)
{
    return vidcmd_set_targets_pixel(target) ? skew_pix : skew_cmp;
}

// ============================================================================
// AUDIO — unchanged by the 12-bit colour move
// ============================================================================
//
// One deposited word is a stereo pair {L[15:8], R[7:0]}, unsigned 8-bit per
// channel.  PORTS pops one pair every second LINE_STROBE, so the sample rate
// is exactly half the 31.46875 kHz line rate — 15734.375 Hz, which is what
// griffin.yml rounds to AUDIO_SAMPLES_PER_SECOND.  The /2 phase is a free-
// running counter in PORTS, so it must be carried across frames here too:
// V_TOTAL is odd, so a frame alternately consumes 263 and 262 pairs.
inline constexpr uint32_t AUDIO_FIFO_PAIRS   = Griffin::AUDIO_FIFO_DEPTH;          // 1024
inline constexpr uint32_t AUDIO_SAMPLE_RATE  = Griffin::AUDIO_SAMPLES_PER_SECOND;  // 15734
inline constexpr uint32_t AUDIO_LINES_PER_SAMPLE = 2;

constexpr uint16_t audio_pair_word(uint8_t left, uint8_t right)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(left) << 8) | right);
}

constexpr uint8_t audio_pair_left(uint16_t w)  { return static_cast<uint8_t>(w >> 8); }
constexpr uint8_t audio_pair_right(uint16_t w) { return static_cast<uint8_t>(w & 0xFF); }

}  // namespace SuperEngine
