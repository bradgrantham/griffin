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
// It does NOT prove FIT.  It is now, however, SYNCHRONISED TO RTL: compositor.v
// and pixel.v exist, compositor_tb.v runs, and every timing rule below is that
// testbench's measurement rather than this suite's guess.  Where the two could
// not be reconciled it is called out in place rather than papered over.
//
// The remaining fit-risk register — the things the fitter may still force to
// shrink, listed here so shrinking one is a visible change to this model rather
// than an archaeological dig through render.cpp:
//
//   FIT-RISKY ASSUMPTION 1: 12-bit playback counters.  RUN carries a 12-bit
//     complemented count and the playback machine upcounts it to an all-ones
//     terminal.  A 10-bit counter covers every count a 640-pixel line can use;
//     the encoding would keep 12 bits.  compositor.v currently spends the 12.
//   FIT-RISKY ASSUMPTION 2 (RESOLVED TWICE — TILE IS GONE, MASK TOOK ITS
//     PREFIX): TILE's three words and its two mask shifters were the biggest
//     single lever and they were spent.  The `01` prefix then came back on
//     2026-08-24 as MASK, a TWO-word, SIXTEEN-pixel per-pixel overlay with NO
//     inline colour — it reuses staged_word as its shifter and the shared
//     playback counter, so it costs two macrocells plus sav_src rather than
//     two 16-bit shifters.  Sprites and cursors can now be either: RUN/RUN_COLOR
//     span lists (cheap for contiguous art) or MASK records (cheap for art with
//     holes).  See "MASK" below.
//   FIT-RISKY ASSUMPTION 3: a 12-bit SET value committed atomically into
//     whichever register the 3-bit target names.  If the register file has to
//     be written in halves the slot semantics below change shape.
//   FIT-RISKY ASSUMPTION 4 — RESOLVED 2026-08-19, TWICE OVER.  The
//     COMPOSITOR->PIXEL SET conduit used to be a shadow tap of the VIDCMD Q
//     bus, and it did not work: Q had moved on by the time PIXEL captured it.
//     Two decisions closed it.  (1) A dedicated 12-bit registered value bus,
//     set_pix_value[11:0], carries the payload point to point; PIXEL no longer
//     touches VIDCMD_Q at all (griffin.yml interfaces, "COMPOSITOR -> PIXEL SET
//     register path", 2026-08-13).  (2) The registered-/RE fetch holds Q
//     high-Z between reads, which would have made a Q tap impossible anyway.
//     value/valid/target/commit now share one pipeline stage, so a PIXEL-target
//     SET costs exactly what any other record costs and the suite's "apply at
//     the commit slot" model IS the measurement.  SKEW_PIX_TARGET stays a named
//     knob at zero so the compensation path stays exercised.
//   FIT-RISKY ASSUMPTION 5 — RUN_COLOR, still explicitly at risk.  It needs its
//     own 3-bit colour latch in the PLAYBACK registers (staging is clobbered by
//     the next record's prefetch mid-run) and a fourth input on the 12-bit
//     output mux.  compositor.v implements it and fits, but if the fitter needs
//     the room back this is the line item; the natural trade is run-invert,
//     which RUN_COLOR displaced from the src=11 encoding.
//
// ===========================================================================
// PIPELINE
// ===========================================================================
//
//   ENGINE (descriptor DMA) --> PIXELS FIFO --> PIXEL --IDX[3:0]--> PALETTE RAM
//                          \--> VIDCMD FIFO --> COMPOSITOR <--12-bit RGB--/
//                                               COMPOSITOR --> DAC
//
// Neither PIXEL nor COMPOSITOR has a CPU bus; both live in the 25.175 MHz
// pixel-clock domain and are configured entirely in band.  COMPOSITOR is the
// only instruction decoder: it plays RUN/RUN_COLOR records against the pixel
// stream and it also issues register writes out of that same stream into PIXEL.
//
// Colour is 12-bit R4G4B4 everywhere — the sixteen palette-RAM entries, the
// compositor's held colours, RUN_COLOR and the DAC.
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

#include <array>
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
// A destination of 0 strobes nothing: the descriptor still reads its payload
// words off the bus, so it is a *pure pacing* descriptor that costs bus time
// and nothing else.  It is free in hardware and it is how a list waits (see
// vblank_pacing_lines in author.h).
//
// Since 2026-09-27 the field is a 3-bit CODE, decoded on chip to one of seven
// strobes (griffin.yml ENGINE_DEST_*), not a one-hot mask: the five AUDIO
// FIFOs are byte wide and take the low byte of each word.

inline constexpr uint8_t DEST_NONE            = Griffin::ENGINE_DEST_NONE;
inline constexpr uint8_t DEST_PIXELS_FIFO_W   = Griffin::ENGINE_DEST_PIXELS_FIFO_W;
inline constexpr uint8_t DEST_VIDCMD_FIFO_W   = Griffin::ENGINE_DEST_VIDCMD_FIFO_W;
inline constexpr uint8_t DEST_AUDCTL_FIFO_W   = Griffin::ENGINE_DEST_AUDCTL_FIFO_W;
inline constexpr uint8_t DEST_SAMPLES0_FIFO_W = Griffin::ENGINE_DEST_SAMPLES0_FIFO_W;
inline constexpr uint8_t DEST_SAMPLES1_FIFO_W = Griffin::ENGINE_DEST_SAMPLES1_FIFO_W;
inline constexpr uint8_t DEST_SAMPLES2_FIFO_W = Griffin::ENGINE_DEST_SAMPLES2_FIFO_W;
inline constexpr uint8_t DEST_SAMPLES3_FIFO_W = Griffin::ENGINE_DEST_SAMPLES3_FIFO_W;
inline constexpr uint8_t DEST_CODE_MAX        = 7;
// The rev-1 stereo pair popped by PORTS sat on the pin that was nAUDIO_FIFO_W
// and is now nAUDCTL_FIFO_W.  The emulator and apps/smurf still stream it;
// audio package B retires this name.
inline constexpr uint8_t DEST_AUDIO_PAIR_REV1 = DEST_AUDCTL_FIFO_W;

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
inline constexpr uint16_t DESC_DEST_MASK         = 0x0007;

// word0 [8:3] are reserved (the destination code took [2:0]; the fourth
// mask bit is gone).  The fit experiment may well reclaim them
// (a wider count field is the obvious customer — a 6-bit count would let one
// descriptor carry a whole 40-word 1bpp line), so they are checked for zero
// here rather than quietly ignored.
inline constexpr uint16_t DESC_RESERVED_MASK     = 0x01F8;

// Host/target-side view of one descriptor.  `src` is a *byte* address (even);
// the engine keeps A[22:1] so bit 0 is not representable.
struct Descriptor
{
    uint32_t src         = 0;
    uint16_t count       = 1;      // payload words, 1..32
    uint8_t  destination = DEST_NONE;
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
                                     (d.destination & DESC_DEST_MASK));
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
    d.destination = static_cast<uint8_t>(w[0] & DESC_DEST_MASK);
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
// Pure pixel bits — no header words at all, because palette and mode left the
// stream for VIDCMD — and since 2026-09-20 PURE INDICES: every playfield pixel
// in every mode is a four-bit palette index into the external 16 x 12 palette
// RAM (PaletteUnit in render.h).  PIXEL holds no colour register.
//
// Words are consumed MSB FIRST, bit 15 down, out of ONE ganged 16-bit FIFO port
// with ONE read strobe.  The line length is a function of the mode; a nonzero
// pixel_skip adds exactly one word (see the record+1 rule below).
//
// Normative: cpld/pixel/pixel.v and cpld/pixel/pixel_tb.v (`make pixel-sim`),
// whose MEASURED CONSTANTS every number here is taken from; the palette write
// window is cpld/pixel/clut_pair_tb.v's (`make clut-pair-sim`).

// IDT7200L-15, 256x9, two in parallel: each FIFO holds 256 bus words.
inline constexpr uint32_t PIXELS_FIFO_WORDS = 256;
inline constexpr uint32_t VIDCMD_FIFO_WORDS = 256;

// PIX_PRELOAD to the line's first consumption clock, in pixel clocks: one for
// the read, one for the capture and up to 14 alignment shifts.  pixel_tb.v
// calls it FETCH_LEAD; timing.v emits it as PIXEL_FETCH_LEAD.
inline constexpr uint32_t PIXEL_FETCH_LEAD = Griffin::PIXEL_PRELOAD_LEAD;
static_assert(PIXEL_FETCH_LEAD == 16);

// A line whose effective skip is non-zero reads this many words more than its
// mode's record, and never more however large the skip.
inline constexpr uint32_t PIXELS_SKIP_EXTRA_WORDS = Griffin::PIXELS_SKIP_EXTRA_WORDS;

// PIX_LAST's width: the last eight consumption clocks of a line, the window the
// end-of-line fetch cutoff uses raw (timing.v: h_cnt[9:3] == 79).
inline constexpr uint32_t PIXEL_LAST_CLOCKS = 8;

// ---------------------------------------------------------------------------
// THE MODE REGISTER: A DEPTH ENUMERATION PLUS A HALF-RATE FLAG (pixel.v)
// ---------------------------------------------------------------------------
//
//   mode[1:0]  depth: 00 1bpp, 01 2bpp, 10 4bpp, 11 attribute
//   mode[2]    half rate — every consumed group held two pixel clocks, so a
//              line carries 320 groups instead of 640
//   mode[3]    RESERVED for the COMPOSITOR bypass, MUST BE WRITTEN 0
//
// SET(pix_mode) writes value[2:0] and drops the rest.  There is no order
// dependence anywhere in the chip any more: the micro-HAM / ham_held ordering
// rule died with micro-HAM.
//
// INDEX FORMATION, one line per depth:
//
//   1bpp        {pix_base[3:1], bit}          two entries, selected by pix_base
//   2bpp        {pix_base[3:2], dibit}        four entries
//   4bpp        the nibble                    all sixteen; pix_base unused
//   attribute   bit ? fg : bg                 from {fg[3:0], bg[3:0], pattern[7:0]}
//
// The attribute word is ONE WORD PER EIGHT-GROUP CELL: PIXELS_Q[15:8] the
// colour byte, PIXELS_Q[7:0] the pattern, pattern MSB first.  At full rate an
// 80-column cell-coloured text/tile mode; at half rate a 320-wide tile mode.
//
// 4BPP AT FULL RATE IS UNDEFINED and must never be emitted: four clocks per
// word puts two fetch triggers inside the eight-clock end-of-line window, the
// line ends short of its record and every following line is out of phase.
// pixels_words_per_line() refuses it rather than computing it.
inline constexpr uint32_t PIXEL_DEPTH_1BPP = 0;
inline constexpr uint32_t PIXEL_DEPTH_2BPP = 1;
inline constexpr uint32_t PIXEL_DEPTH_4BPP = 2;
inline constexpr uint32_t PIXEL_DEPTH_ATTR = 3;

inline constexpr uint32_t PIXEL_MODE_DEPTH_MASK = 3;
inline constexpr uint32_t PIXEL_MODE_HALF_RATE  = 4;   // mode[2], an OR-able flag
inline constexpr uint32_t PIXEL_MODE_RESERVED   = 8;   // mode[3], write 0
inline constexpr uint32_t PIXEL_MODE_BITS       = 7;   // what SET(pix_mode) keeps

static_assert(PIXEL_DEPTH_1BPP == Griffin::VIDCMD_PIX_MODE_1BPP);
static_assert(PIXEL_DEPTH_2BPP == Griffin::VIDCMD_PIX_MODE_2BPP);
static_assert(PIXEL_DEPTH_4BPP == Griffin::VIDCMD_PIX_MODE_4BPP);
static_assert(PIXEL_DEPTH_ATTR == Griffin::VIDCMD_PIX_MODE_ATTR);
static_assert(PIXEL_MODE_HALF_RATE == Griffin::VIDCMD_PIX_MODE_HALF_RATE);

// The mode value a SET carries, built rather than hand-ORed.
constexpr uint32_t pixel_mode(uint32_t depth, bool half_rate)
{
    return (depth & PIXEL_MODE_DEPTH_MASK) | (half_rate ? PIXEL_MODE_HALF_RATE : 0u);
}

constexpr uint32_t pixel_mode_depth(uint32_t mode)
{
    return mode & PIXEL_MODE_DEPTH_MASK;
}

constexpr bool pixel_mode_half_rate(uint32_t mode)
{
    return (mode & PIXEL_MODE_HALF_RATE) != 0;
}

// Stream bits per GROUP (W in the testbench's derivations).  Attribute mode
// consumes one pattern bit per group like 1bpp; its cell costs a whole word
// every eight groups, which is why its word count matches 2bpp's.
constexpr uint32_t pixels_bits_per_group(uint32_t mode)
{
    const uint32_t d = pixel_mode_depth(mode);
    return d == PIXEL_DEPTH_2BPP ? 2u :
           d == PIXEL_DEPTH_4BPP ? 4u : 1u;
}

// How many bit-groups a line consumes.  Half rate holds every consumed group
// for two pixel clocks, so the 640-clock window carries 320 groups: half the
// stream at half the horizontal resolution.  RGB_OUT's window, PIXEL_OUT_LEAD
// and the SET path are all untouched — it is a stream-side gate only.
constexpr uint32_t pixels_groups_per_line(uint32_t mode)
{
    return pixel_mode_half_rate(mode) ? (H_ACTIVE / 2u) : H_ACTIVE;
}

// The fetch trigger's stream position T = 16-2W at full rate, 16-W at half
// rate; attribute mode's cell is eight groups, so its trigger compares the low
// three bits only and T is 6 (full) / 7 (half).  Here for the model and for
// anything that wants to reason about the cadence; PIXEL applies it itself.
constexpr uint32_t pixels_fetch_trigger(uint32_t mode)
{
    const uint32_t w    = pixels_bits_per_group(mode);
    const bool     half = pixel_mode_half_rate(mode);
    const uint32_t t    = half ? (16u - w) : (16u - 2u * w);
    return (pixel_mode_depth(mode) == PIXEL_DEPTH_ATTR) ? (t & 7u) : t;
}

// ---------------------------------------------------------------------------
// pixel_skip — A START POSITION IN STREAM BITS INSIDE THE LINE'S FIRST WORD
// ---------------------------------------------------------------------------
//
// Four bits; skip s means pixel 0 is stream bit s of the line's first word.
// There is no whole-word discard (coarser scrolling moves the descriptor's
// source pointer), and two clamps apply, BOTH at consumption — the line's
// preload — so SET ordering cannot smuggle a stale bit in:
//
//   DEPTH   a W-bit group can only shift by multiples of W: 2bpp ignores bit 0,
//           4bpp ignores bits [1:0], attribute forces 0 (a bit-level scroll
//           would shift a pattern byte out of step with its colour byte).
//   RANGE   the trigger's lead T bounds it at FULL RATE only, and the preload's
//           own lead bounds it at 14 at BOTH rates.  Net caps: 14 (1bpp either
//           rate), 12 (2bpp full), 14 (2bpp half), 12 (4bpp half).
//
// Above the cap the value is CLAMPED, not honoured — a recorded limitation
// (pixel.v's header has the two overruns it prevents, one of which silently
// reads a word too few on every line of the frame).
inline constexpr uint32_t PIXELS_SKIP_MAX = 15;   // the register's own width

constexpr uint32_t pixels_skip_depth_clamp(uint32_t mode, uint32_t pixel_skip)
{
    switch (pixel_mode_depth(mode))
    {
        case PIXEL_DEPTH_2BPP: return pixel_skip & ~1u;
        case PIXEL_DEPTH_4BPP: return pixel_skip & ~3u;
        case PIXEL_DEPTH_ATTR: return 0u;
        default:               return pixel_skip;
    }
}

// The cap a mode's skip is clamped to: min(T at full rate, PRELOAD_LEAD - 2).
constexpr uint32_t pixels_skip_cap(uint32_t mode)
{
    const uint32_t preload_cap = PIXEL_FETCH_LEAD - 2u;
    if (pixel_mode_half_rate(mode))
    {
        return preload_cap;
    }
    const uint32_t trigger_cap = 16u - 2u * pixels_bits_per_group(mode);
    return trigger_cap < preload_cap ? trigger_cap : preload_cap;
}

// pixel_skip as CONSUMED at the line's preload: the register truncated to four
// bits, the depth clamp, then the cap.
constexpr uint32_t pixels_skip_effective(uint32_t mode, uint32_t pixel_skip)
{
    const uint32_t s   = pixels_skip_depth_clamp(mode, pixel_skip & 0xFu);
    const uint32_t cap = pixels_skip_cap(mode);
    return s > cap ? cap : s;
}

// ---------------------------------------------------------------------------
// WORDS PER LINE — the rule the display-list author must satisfy
// ---------------------------------------------------------------------------
//
//     words per line = record + (effective pixel_skip != 0 ? 1 : 0)
//
//     record = 40 (1bpp full)   20 (1bpp half)   80 (2bpp full)   40 (2bpp half)
//              80 (4bpp half)   80 (attr full)   40 (attr half)
//
// A line entered at bit s > 0 gets only 16-s bits out of its first word, so it
// needs ONE more word and its tail shows REAL stream data rather than zeros —
// which is what a horizontal scroll wanted.  Exactly one extra word is ever
// read, never two.  Attribute mode takes no skip, so it is always its record.

// A line's record, in 16-bit words, ignoring skip.  4bpp full rate has no
// answer: the caller must not ask.
constexpr uint32_t pixels_record_words(uint32_t mode)
{
    const uint32_t d    = pixel_mode_depth(mode);
    const bool     half = pixel_mode_half_rate(mode);
    if (d == PIXEL_DEPTH_ATTR)
    {
        // One word per eight-group cell.
        return pixels_groups_per_line(mode) / 8u;
    }
    if (d == PIXEL_DEPTH_4BPP && !half)
    {
        return 0u;   // UNDEFINED mode: refused, not computed
    }
    return pixels_groups_per_line(mode) * pixels_bits_per_group(mode) / 16u;
}

constexpr bool pixels_mode_defined(uint32_t mode)
{
    return !(pixel_mode_depth(mode) == PIXEL_DEPTH_4BPP && !pixel_mode_half_rate(mode));
}

constexpr uint32_t pixels_words_per_line(uint32_t mode, uint32_t pixel_skip)
{
    return pixels_record_words(mode) +
           (pixels_skip_effective(mode, pixel_skip) != 0u ? PIXELS_SKIP_EXTRA_WORDS : 0u);
}

// The seven defined line lengths, derived rather than tabulated, and checked
// against griffin.yml's constants — which are checked in turn against
// pixel_tb.v's MEASURED CONSTANTS table.
inline constexpr uint32_t PIXELS_WORDS_1BPP =
    pixels_words_per_line(pixel_mode(PIXEL_DEPTH_1BPP, false), 0);
inline constexpr uint32_t PIXELS_WORDS_1BPP_HALF =
    pixels_words_per_line(pixel_mode(PIXEL_DEPTH_1BPP, true), 0);
inline constexpr uint32_t PIXELS_WORDS_2BPP =
    pixels_words_per_line(pixel_mode(PIXEL_DEPTH_2BPP, false), 0);
inline constexpr uint32_t PIXELS_WORDS_2BPP_HALF =
    pixels_words_per_line(pixel_mode(PIXEL_DEPTH_2BPP, true), 0);
inline constexpr uint32_t PIXELS_WORDS_4BPP_HALF =
    pixels_words_per_line(pixel_mode(PIXEL_DEPTH_4BPP, true), 0);
inline constexpr uint32_t PIXELS_WORDS_ATTR =
    pixels_words_per_line(pixel_mode(PIXEL_DEPTH_ATTR, false), 0);
inline constexpr uint32_t PIXELS_WORDS_ATTR_HALF =
    pixels_words_per_line(pixel_mode(PIXEL_DEPTH_ATTR, true), 0);

static_assert(PIXELS_WORDS_1BPP      == Griffin::PIXELS_WORDS_PER_LINE_1BPP);
static_assert(PIXELS_WORDS_1BPP_HALF == Griffin::PIXELS_WORDS_PER_LINE_1BPP_HALF);
static_assert(PIXELS_WORDS_2BPP      == Griffin::PIXELS_WORDS_PER_LINE_2BPP);
static_assert(PIXELS_WORDS_2BPP_HALF == Griffin::PIXELS_WORDS_PER_LINE_2BPP_HALF);
static_assert(PIXELS_WORDS_4BPP_HALF == Griffin::PIXELS_WORDS_PER_LINE_4BPP_HALF);
static_assert(PIXELS_WORDS_ATTR      == Griffin::PIXELS_WORDS_PER_LINE_ATTR);
static_assert(PIXELS_WORDS_ATTR_HALF == Griffin::PIXELS_WORDS_PER_LINE_ATTR_HALF);

static_assert(PIXELS_WORDS_1BPP == 40 && PIXELS_WORDS_1BPP_HALF == 20);
static_assert(PIXELS_WORDS_2BPP == 80 && PIXELS_WORDS_2BPP_HALF == 40);
static_assert(PIXELS_WORDS_4BPP_HALF == 80);
static_assert(PIXELS_WORDS_ATTR == 80 && PIXELS_WORDS_ATTR_HALF == 40);
static_assert(!pixels_mode_defined(pixel_mode(PIXEL_DEPTH_4BPP, false)),
              "4bpp full rate is undefined and must be refused, not computed");
static_assert(pixels_skip_effective(pixel_mode(PIXEL_DEPTH_1BPP, false), 15) ==
              Griffin::PIXELS_SKIP_MAX_1BPP);
static_assert(pixels_skip_effective(pixel_mode(PIXEL_DEPTH_1BPP, true), 15) ==
              Griffin::PIXELS_SKIP_MAX_1BPP);
static_assert(pixels_skip_effective(pixel_mode(PIXEL_DEPTH_2BPP, false), 14) ==
              Griffin::PIXELS_SKIP_MAX_2BPP);
static_assert(pixels_skip_effective(pixel_mode(PIXEL_DEPTH_2BPP, true), 15) ==
              Griffin::PIXELS_SKIP_MAX_2BPP_HALF);
static_assert(pixels_skip_effective(pixel_mode(PIXEL_DEPTH_4BPP, true), 15) ==
              Griffin::PIXELS_SKIP_MAX_4BPP_HALF);
static_assert(pixels_skip_effective(pixel_mode(PIXEL_DEPTH_ATTR, false), 9) == 0,
              "attribute mode refuses pixel_skip");
static_assert(pixels_words_per_line(pixel_mode(PIXEL_DEPTH_1BPP, false), 3) ==
              PIXELS_WORDS_1BPP + 1,
              "a skipped line reads its record plus one word");
static_assert(pixels_words_per_line(pixel_mode(PIXEL_DEPTH_2BPP, false), 1) ==
              PIXELS_WORDS_2BPP,
              "the depth clamp turns 2bpp skip 1 into 0, so no extra word");

// ---------------------------------------------------------------------------
// THE PALETTE — 16 x 12 of external RAM, and a THREE-WORD table entry
// ---------------------------------------------------------------------------
//
// The RAM's data inputs are COMPOSITOR's set_pix_value[11:0]; PIXEL supplies
// only the address (IDX) and the write pulse.  COMPOSITOR re-registers that bus
// from EVERY staged word, and stages one every two clocks, so the clut_data
// SET's own payload owns the write window's first half and the word behind it
// owns the second: the entry is
//
//      SET pix_clut_index, i
//      SET pix_clut_data,  V
//      SET 7 (the filler), V      <- the SAME twelve bits, or the RAM stores
//                                    the next word's payload instead
//
// and the four pixel clocks after the data SET's commit (its own slot + 3) are
// DISTURBED: one shows the entry being written, two are undefined while /WE is
// low and the RAM recovers, and the fourth is a guard for a write-recovery time
// that is not pinned yet.  They must be in blanking or covered by a
// non-passthrough source; in particular the word after the filler must not be
// the RUN that resumes passthrough.  All of it is measured by clut_pair_tb.v.
//
// THE RAM IS NEVER RESET — not by /RS, not at power-up.  The model and the
// emulator fill it with reproducible random values at construction so a list
// that forgets an entry fails loudly.
inline constexpr uint32_t PALETTE_ENTRIES = 16;
inline constexpr uint32_t PALETTE_WORDS_PER_ENTRY = Griffin::VIDCMD_WORDS_PER_CLUT_ENTRY;
static_assert(PALETTE_WORDS_PER_ENTRY == 3);

// The pixel slots a clut_data SET disturbs, counted from the SET's OWN slot:
// slots +1..+3 are the measured disturbance and +4 is the recovery guard, so
// PALETTE_WRITE_DISTURBED_SLOTS slots after the SET's slot must be covered.
inline constexpr uint32_t PALETTE_WRITE_MEASURED_SLOTS  = 3;
inline constexpr uint32_t PALETTE_WRITE_DISTURBED_SLOTS = 4;

// Entries a 1bpp / 2bpp line can reach: pix_base selects a PAIR in 1bpp and a
// QUAD in 2bpp, so these are the base values that mean anything.
constexpr uint32_t palette_index_1bpp(uint32_t pix_base, uint32_t bit)
{
    return ((pix_base & 0xEu)) | (bit & 1u);
}

constexpr uint32_t palette_index_2bpp(uint32_t pix_base, uint32_t dibit)
{
    return ((pix_base & 0xCu)) | (dibit & 3u);
}


// ============================================================================
// VIDCMD instruction stream
// ============================================================================
//
// EVERY RECORD IS ONE WORD EXCEPT MASK, WHICH IS TWO.  16-bit, prefix coded on
// the top bits:
//
//   00 ss ~count[11:0]              RUN        src 00 passthrough
//                                                  01 cmp_color1 (held_fg)
//                                                  10 cmp_color0 (held_bg)
//   00 11 colour[2:0] ~count[8:0]   RUN_COLOR
//   01 d1 d2 d3 d4 d5 d6 d7         MASK header, + one data word d8..d15
//   1  ttt value[11:0]              SET
//
// Counts are stored complemented exactly as compositor.v wants them: an ATF15xx
// up-counter with an all-ones terminal is far cheaper than a down-counter's
// zero detect, so the encoded field loads straight into run_count and one
// shared incrementer walks it to 12'hFFF.  RUN_COLOR's 9-bit field loads with
// the top three bits forced to 1 so the same terminal serves both widths.
//
// THE `01` PREFIX IS MASK (2026-08-24, "experiment B", implemented and
// committed in cpld/compositor/compositor.v).  TILE is still gone; what came
// back on its prefix is a much smaller record — see the MASK section below.
//
// FRAMING IS DURATION ARITHMETIC, WITH HOLD — AND WITH THE FETCH CADENCE.
// Every active pixel clock is one slot.  A RUN contributes its count, a
// RUN_COLOR its count, a SET one slot, a MASK sixteen.  That is
// what a record costs WHEN IT EXECUTES; what it costs to DELIVER is a separate
// number, and since 2026-08-19 the two are not the same.  The fetch is one word
// per TWO pixel clocks (registered /RE, see VIDCMD_SLOTS_PER_WORD below), so a
// run of one-slot records stretches: each pays for its own slot and for the
// HOLD slot the fetch spends behind it, except across a banked pair.  The
// authored slot sum is therefore a LOWER bound on a line's occupancy, and an
// exact-640 line has to be closed against vidcmd_plan_line() rather than
// against the sum.
//
// When the count is terminal and nothing is staged the compositor HOLDS — it
// keeps the current source and keeps trying.  That covers both the cadence's
// gap slots and a genuinely dry FIFO; only the second is a list-builder bug,
// which is why render.cpp counts them separately.  Hold is first-class line
// framing, not underrun mercy, and it gives two authoring disciplines off one
// hardware rule (see author.h's FramingMode):
//
//   CUSHION   records are buffered ahead in VBLANK, the FIFO never empties
//             mid-line, hold never engages FOR WANT OF DATA, and the per-line
//             OCCUPANCY — cadence holds included — must be EXACTLY H_ACTIVE.
//             Overrunning is the hazard: a leftover record
//             staged at the H_ACTIVE fall plays at the start of the next line
//             and, while staged, blocks the fetch that would have run that
//             line's eager SETs.
//   JIT       a line's records may total FEWER than H_ACTIVE slots; the last
//             source replicates to the end of the line.  {RUN(passthrough,1)}
//             alone paints a whole line, and a line that receives no fill at
//             all keeps holding — so a full passthrough frame costs ONE VIDCMD
//             word for the whole frame.  The obligation moves from slot
//             arithmetic to a delivery deadline: a line's packet must land
//             before that line's pixel 0, or it resumes at the wrong x and
//             self-heals only at the next empty boundary.

enum class VidcmdType : uint8_t
{
    RUN  = 0,
    MASK = 1,   // the `01` prefix: header + one data word, sixteen pixels
    SET  = 2,
};

// RUN's 2-bit source select.
//
// NAMING NOTE.  compositor.v renamed cmp_held_fg/cmp_held_bg to
// cmp_color1/cmp_color0 on 2026-08-24, because a MASK dibit selects between
// them by NUMBER and "fg/bg" stopped describing what they are.  The WIRE
// ENCODINGS DID NOT MOVE, so these names are kept here (the emulator, apps/ and
// firmware/ all use them) and the color-numbered aliases below are the ones new
// code should read.
inline constexpr uint32_t RUN_SRC_PASSTHROUGH = 0;   // RGB_IN from PIXEL
inline constexpr uint32_t RUN_SRC_HELD_FG     = 1;   // cmp_color1
inline constexpr uint32_t RUN_SRC_HELD_BG     = 2;   // cmp_color0
inline constexpr uint32_t RUN_SRC_COLOR       = 3;   // RUN_COLOR, see below

inline constexpr uint32_t RUN_SRC_COLOR1      = RUN_SRC_HELD_FG;
inline constexpr uint32_t RUN_SRC_COLOR0      = RUN_SRC_HELD_BG;

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
// replicated across its whole nibble — {4{r}, 4{g}, 4{b}} — giving the eight
// saturated corners of the colour cube.
//
// The count is 9 bits and stored complemented, matching the convention the
// 12-bit RUN count already uses: the field is ~count masked to the field
// width, so decoding is another complement and the maximum span is 511 pixels.
// Being one word, a RUN_COLOR is as cheap to DELIVER as any other record — but
// "one word" is no longer free: at the 2-clock fetch cadence a record has to
// average two slots of playback to keep the stream fed (vidcmd_plan_line()).
// A one-pixel RUN_COLOR next to a one-pixel gap does not, which is the
// cadence-aware form of the old prefetch invariant.
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

// SET targets.  0..1 are COMPOSITOR's own registers, 2..6 are forwarded to
// PIXEL, and 7 is decoded NOWHERE — which is what makes it the palette-write
// filler (see "THE PALETTE" above and vidcmd-spec.md §3.2).
inline constexpr uint32_t SET_CMP_HELD_FG    = 0;
inline constexpr uint32_t SET_CMP_HELD_BG    = 1;
inline constexpr uint32_t SET_PIX_CLUT_INDEX = 2;
inline constexpr uint32_t SET_PIX_CLUT_DATA  = 3;
inline constexpr uint32_t SET_PIX_BASE       = 4;
inline constexpr uint32_t SET_PIX_MODE       = 5;
inline constexpr uint32_t SET_PIX_PIXEL_SKIP = 6;
inline constexpr uint32_t SET_FILLER         = 7;

static_assert(SET_CMP_HELD_FG    == Griffin::VIDCMD_SET_CMP_COLOR1);
static_assert(SET_CMP_HELD_BG    == Griffin::VIDCMD_SET_CMP_COLOR0);
static_assert(SET_PIX_CLUT_INDEX == Griffin::VIDCMD_SET_PIX_CLUT_INDEX);
static_assert(SET_PIX_CLUT_DATA  == Griffin::VIDCMD_SET_PIX_CLUT_DATA);
static_assert(SET_PIX_BASE       == Griffin::VIDCMD_SET_PIX_BASE);
static_assert(SET_PIX_MODE       == Griffin::VIDCMD_SET_PIX_MODE);
static_assert(SET_PIX_PIXEL_SKIP == Griffin::VIDCMD_SET_PIX_PIXEL_SKIP);
static_assert(SET_FILLER         == Griffin::VIDCMD_SET_FILLER);

// The color-numbered spelling of targets 0 and 1, matching compositor.v's
// cmp_color1 / cmp_color0 and griffin.yml's VIDCMD_SET_CMP_COLOR1 /
// VIDCMD_SET_CMP_COLOR0 (renamed 2026-08-24, numbering unchanged).
//
// THE NUMBER/COLOUR SWAP IS DELIBERATE AND STILL OPEN: target 0 writes
// cmp_color1 and target 1 writes cmp_color0.  Renaming the two constants was a
// contract change made on its own; renumbering the two TARGETS would be a wire
// change and is a separate open question (griffin.yml issues).  Nothing here
// may quietly "fix" it — the encoding is frozen by the RTL, the emulator, the
// firmware and apps/.
inline constexpr uint32_t SET_CMP_COLOR1 = SET_CMP_HELD_FG;   // target 0
inline constexpr uint32_t SET_CMP_COLOR0 = SET_CMP_HELD_BG;   // target 1

constexpr bool vidcmd_set_targets_pixel(uint32_t target)
{
    return target >= SET_PIX_CLUT_INDEX && target <= SET_PIX_PIXEL_SKIP;
}

// Held colours at /RS (vsync), and the playback state it resets to.  Straight
// out of compositor.v's reset block: cmp_color1 0xFFF, cmp_color0 0x000,
// cur_src passthrough, run_count already terminal, mask playback abandoned.
inline constexpr Rgb444 VIDCMD_RESET_HELD_FG = RGB444_WHITE;
inline constexpr Rgb444 VIDCMD_RESET_HELD_BG = RGB444_BLACK;

inline constexpr Rgb444 VIDCMD_RESET_COLOR1 = VIDCMD_RESET_HELD_FG;
inline constexpr Rgb444 VIDCMD_RESET_COLOR0 = VIDCMD_RESET_HELD_BG;

// Decodes the LEAD word of a record.  A MASK's data word is never decoded:
// compositor.v captures it with have_staged LOW, so no bit pattern of it can
// reach this decode, and the model reproduces that.
constexpr VidcmdType vidcmd_type_of(uint16_t w)
{
    if ((w & 0x8000) != 0)
    {
        return VidcmdType::SET;
    }
    return ((w >> 14) & 1) != 0 ? VidcmdType::MASK : VidcmdType::RUN;
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

// ---------------------------------------------------------------------------
// MASK — the two-word `01` record, sixteen pixels of per-pixel overlay
// ---------------------------------------------------------------------------
//
// Bit-for-bit compositor.v (2026-08-24, "experiment B"), whose header is the
// normative statement of all of this; compositor_tb.v's MASKB_* traces are the
// measurement.
//
//   word 1  { 2'b01, d1, d2, d3, d4, d5, d6, d7 }   d1 in bits [13:12]
//   word 2  { d8, d9, d10, d11, d12, d13, d14, d15 } d8 in bits [15:14]
//   pixel 0 has NO DIBIT: it is implicitly {1,0}, opaque cmp_color0.
//
// A dibit is {opacity, select}: 00 passthrough, 10 cmp_color0, 11 cmp_color1,
// 01 reserved and plays as passthrough.
//
// THERE IS NO INLINE COLOUR.  All fourteen payload bits of the header are
// dibits; recolouring a mask is an ordinary SET in front of it.  That is what
// made the record chainable — see the gap constants below.
//
// THE RECORD IS MODAL.  A dibit overrides the source for its own pixel only;
// whatever RUN was in force resumes, as if untouched, at pixel 16.  RUN_COLOR's
// colour latch is not disturbed either.  Note the corollary that catches
// authors out: dibit 00 is PASSTHROUGH, i.e. PIXEL's RGB_IN — it is NOT "the
// span underneath".  A mask over a RUN_COLOR background must paint that
// background itself, out of cmp_color0.
//
// AUTHORING RULES, all of them consequences rather than conventions:
//
//   * Leading transparency is a RUN, not dibits.  Pixel 0 is implicitly OPAQUE,
//     so a sprite with a transparent left edge starts its record at the first
//     opaque pixel, behind a passthrough RUN.  There is no gap to pay for that:
//     RUN-to-mask is zero slots.
//   * Whatever pixel lands on a record's pixel 0 is cmp_color0, so art laid out
//     in 16-pixel cells wants a blank column at each cell's left edge.  Both
//     screen cases in main.cpp are built on that: an 8-px console cell with a
//     blank column 0 puts two glyphs in one record, and a 16-px "one glyph per
//     record" big font does the same for large text.
//   * Recolouring costs slots, so recolour BETWEEN groups, not inside a run of
//     them, and price the seam (below) into the art's geometry.
inline constexpr uint32_t MASK_SLOTS         = 16;   // pixels per record
inline constexpr uint32_t MASK_RECORD_WORDS  = 2;    // header + data
inline constexpr uint32_t MASK_HEADER_DIBITS = 7;    // d1..d7
inline constexpr uint32_t MASK_DATA_DIBITS   = 8;    // d8..d15
inline constexpr uint32_t MASK_RELOAD_PIXEL  = 8;    // d8 comes off the park here

inline constexpr uint32_t MASK_DIBIT_PASSTHROUGH = 0;   // 00
inline constexpr uint32_t MASK_DIBIT_RESERVED    = 1;   // 01, plays as 00
inline constexpr uint32_t MASK_DIBIT_COLOR0      = 2;   // 10
inline constexpr uint32_t MASK_DIBIT_COLOR1      = 3;   // 11

// Pixel 0's implicit dibit, as a value the authoring code can compare against.
inline constexpr uint32_t MASK_PIXEL0_DIBIT = MASK_DIBIT_COLOR0;

// Which RUN source a dibit drives.  compositor.v computes exactly this in two
// product terms: {opacity & ~select, opacity & select}.
constexpr uint32_t vidcmd_mask_dibit_src(uint32_t dibit)
{
    if ((dibit & 2u) == 0)
    {
        return RUN_SRC_PASSTHROUGH;   // 00 and the reserved 01
    }
    return ((dibit & 1u) != 0) ? RUN_SRC_COLOR1 : RUN_SRC_COLOR0;
}

struct VidcmdMask
{
    uint16_t header = 0;
    uint16_t data   = 0;
};

// d[0] is ignored: the hardware has no bit for it.  Authoring code should keep
// d[0] == MASK_PIXEL0_DIBIT so the array reads as the picture it draws;
// vidcmd_mask_pixel0_ok() is the assertion for that.
constexpr bool vidcmd_mask_pixel0_ok(const std::array<uint32_t, MASK_SLOTS> &d)
{
    return d[0] == MASK_PIXEL0_DIBIT;
}

constexpr VidcmdMask vidcmd_mask(const std::array<uint32_t, MASK_SLOTS> &d)
{
    VidcmdMask m;
    uint32_t   h = 1u << 14;
    for (uint32_t i = 1; i <= MASK_HEADER_DIBITS; i++)
    {
        h |= (d[i] & 3u) << (14u - 2u * i);
    }
    uint32_t w = 0;
    for (uint32_t i = 0; i < MASK_DATA_DIBITS; i++)
    {
        w |= (d[MASK_HEADER_DIBITS + 1 + i] & 3u) << (14u - 2u * i);
    }
    m.header = static_cast<uint16_t>(h);
    m.data   = static_cast<uint16_t>(w);
    return m;
}

// The form art actually arrives in.  Bitmaps are PLANAR — an opacity plane and
// a select plane, one bit per pixel, MSB leftmost so bit 15 is pixel 0 — and
// the record wants them INTERLEAVED as dibits.  Doing the transpose here means
// no caller has to think about which half of a dibit is which.
constexpr VidcmdMask vidcmd_mask_from_planes(uint16_t opacity, uint16_t select)
{
    std::array<uint32_t, MASK_SLOTS> d{};
    for (uint32_t i = 0; i < MASK_SLOTS; i++)
    {
        const uint32_t bit = 15u - i;
        d[i] = (((opacity >> bit) & 1u) << 1) | ((select >> bit) & 1u);
    }
    return vidcmd_mask(d);
}

constexpr uint32_t vidcmd_mask_header_dibit(uint16_t header, uint32_t i)
{
    return (header >> (14u - 2u * i)) & 3u;      // i in 1..7
}

constexpr uint32_t vidcmd_mask_data_dibit(uint16_t data, uint32_t i)
{
    return (data >> (14u - 2u * (i - MASK_HEADER_DIBITS - 1u))) & 3u;   // i in 8..15
}

// The dibit that paints pixel i of a record, pixel 0's implicit one included.
constexpr uint32_t vidcmd_mask_dibit(const VidcmdMask &m, uint32_t i)
{
    if (i == 0)
    {
        return MASK_PIXEL0_DIBIT;
    }
    return (i <= MASK_HEADER_DIBITS) ? vidcmd_mask_header_dibit(m.header, i)
                                     : vidcmd_mask_data_dibit(m.data, i);
}

// --- MASK seam constants, DERIVED from the laws and measured by the tb -------
//
// All four are slot counts BETWEEN a record's last painted pixel and the next
// mask's pixel 0, so 0 means "adjacent pixels".
//
//   RUN -> MASK        0.  The header paints pixel 0 in its own slot, and a RUN
//                      long enough to cover a fetch has the header banked.
//                      (compositor_tb MASKB_SPRITE, measured 0.)
//   MASK -> MASK       0.  The pixel-15 edge captures the next header one slot
//                      early, so it is staged with a terminal count during the
//                      last pixel and fires on the very next edge.  A cursor of
//                      any width is a plain run of records.
//                      (compositor_tb MASKB_CHAIN32, measured 0.)
//   MASK -> SET -> MASK
//                      2.  The SET is the record the pixel-15 edge captured, so
//                      it owns the slot after the mask; the next header is only
//                      READ during that slot, captured at its end and consumed
//                      one edge later — one HOLD slot in between.  That is the
//                      ordinary 2-slot cadence, not a mask penalty.
//                      (compositor_tb MASKB_RECOLOR, measured 2.)
//   MASK -> SET,SET -> MASK
//                      4.  DERIVED HERE, and it is NOT 3.  The two SETs cannot
//                      be a banked pair: the mask holds staged_word, so only ONE
//                      word is parked on Q while it plays, and the pixel-15 edge
//                      captures that one.  SET 1 therefore executes alone, /RE
//                      falls on its edge, SET 2 is captured one slot later and
//                      executes one slot after that — 2 + 2.  A pair needs a
//                      record that DOES release the buffer early, which is what
//                      the RUN case below is.
//   RUN -> SET,SET -> MASK
//                      3, and the third slot is NOT obvious.  Behind a RUN the
//                      fetch banks both SETs (one staged, one parked), so the
//                      pair law does put them on CONSECUTIVE slots — but the
//                      park held /RE LOW, so /RE only RISES on the edge the
//                      first SET is consumed and cannot FALL again until the
//                      edge after.  The header is therefore read one edge later
//                      than a naive count expects, captured on the edge after
//                      that, and consumed on the edge after THAT: one HOLD slot
//                      behind the pair.  This is not a mask rule at all — it is
//                      the same shape compositor_tb's PAIR_LOCAL already pins
//                      (RUN(4), SETs on slots 4 and 5, HOLD on 6, the next
//                      record on 7), and it is why recolouring in the
//                      BACKGROUND RUN costs 3 while recolouring between two
//                      mask groups costs 4.
inline constexpr uint32_t MASK_GAP_AFTER_RUN          = 0;
inline constexpr uint32_t MASK_GAP_AFTER_MASK         = 0;
inline constexpr uint32_t MASK_GAP_AFTER_MASK_SET     = 2;
inline constexpr uint32_t MASK_GAP_AFTER_MASK_SET_SET = 4;
inline constexpr uint32_t MASK_GAP_AFTER_RUN_SET_SET  = 3;

constexpr uint16_t vidcmd_set(uint32_t target, uint32_t value)
{
    return static_cast<uint16_t>(0x8000u | ((target & 0x7) << 12) | (value & 0xFFF));
}

constexpr uint32_t vidcmd_set_target(uint16_t w) { return (w >> 12) & 0x7; }
constexpr uint32_t vidcmd_set_value(uint16_t w)  { return w & 0xFFF; }

// ---------------------------------------------------------------------------
// One palette entry, as the three words it really is
// ---------------------------------------------------------------------------
//
// SET clut_index, SET clut_data, and a SAME-VALUE SET to the filler target.
// The filler is not optional and it is not decoration: COMPOSITOR re-registers
// set_pix_value from every staged word, so without it the RAM stores whatever
// the next word carries (clut_pair_tb.v S1a/S1b).  Authors emit entries through
// this helper and never hand-roll the pair, which is the whole point of it
// living here.
//
// THE THREE WORDS MUST BE CONSECUTIVE IN THE STREAM, and the four pixel slots
// starting at the data SET's own slot must be blanking or covered — see
// "THE PALETTE" above.
struct PaletteEntryWords
{
    uint16_t w[3];

    constexpr const uint16_t *begin() const { return w; }
    constexpr const uint16_t *end() const { return w + 3; }
    static constexpr uint32_t size() { return PALETTE_WORDS_PER_ENTRY; }
};

constexpr PaletteEntryWords vidcmd_palette_entry(uint32_t index, Rgb444 colour)
{
    return PaletteEntryWords{{
        vidcmd_set(SET_PIX_CLUT_INDEX, index & 0xFu),
        vidcmd_set(SET_PIX_CLUT_DATA, colour),
        vidcmd_set(SET_FILLER, colour),
    }};
}

// Words this record occupies in the stream: one, except MASK's header + data.
constexpr uint32_t vidcmd_record_words(uint16_t lead)
{
    return (vidcmd_type_of(lead) == VidcmdType::MASK) ? MASK_RECORD_WORDS : 1u;
}

// Active slots this record occupies WHEN IT EXECUTES.  A RUN contributes its
// count exactly (compositor.v loads ~count and adds 1 for the slot the record
// is consumed in, so the terminal is reached after `count` active slots); a SET
// is one slot; a MASK is sixteen, its header's own slot (pixel 0) included —
// the header loads 12'hFF0 with no +1 and the fifteen active-slot increments
// walk it to the terminal.  This is playback cost only — what the record costs
// to fetch is the cadence's business, and a line's real occupancy comes from
// vidcmd_plan_line() below.
constexpr uint32_t vidcmd_record_slots(uint16_t lead)
{
    switch (vidcmd_type_of(lead))
    {
        // RUN_COLOR steals three of RUN's count bits for its colour, so its
        // count has to be read out of the narrower field.
        case VidcmdType::RUN:  return (vidcmd_run_src(lead) == RUN_SRC_COLOR)
                                          ? vidcmd_run_color_count(lead)
                                          : vidcmd_run_count(lead);
        case VidcmdType::MASK: return MASK_SLOTS;
        default:               return 1;
    }
}

// ---------------------------------------------------------------------------
// Measured pipeline constants — from cpld/compositor/compositor_tb.v
// ---------------------------------------------------------------------------
//
// These are the testbench's numbers, not this suite's guesses.  Most of them
// are UNIFORM pipeline latencies: every pixel is delayed by the same amount, so
// a frame image is unaffected and the sync generator absorbs the shift (see
// pixel.v's DAC_LEAD discussion).  They live here so the emulator and any
// future TB comparison have one place to read them from.  The 2026-08-19
// registered-/RE rework did NOT move any of the K constants below; it changed
// the FETCH cadence only.
//
// The one that is NOT uniform, and therefore the one the renderer actually
// implements, is the slot rule: a record's effects land on the edge that ENDS
// its own slot, so a SET is visible in the pixel of the slot it occupies and a
// RUN emits its first pixel in the slot in which it is consumed.  The TB pins
// that with NORMATIVE_M0 ("SET lands on pixel 1" for RUN(pt,1) then SET) and
// with POSITIONAL_EAGER ("SET executes as slot 4" behind a RUN(4)).

// H_ACTIVE rise to the matching RGB_OUT, in PIXEL_CLKs.  Uniform.
inline constexpr uint32_t COMPOSITOR_OUT_LEAD = 2;

// A SET occupies one slot and is visible in that same slot: offset 0.  The TB
// prints this as "SET becomes visible at slot 1" for the m0 arrangement, where
// slot 1 is the SET's own slot.
inline constexpr uint32_t SET_VISIBLE_SLOT_OFFSET = 0;

// Pop to set_pix_commit while blanking, in PIXEL_CLKs.  Uniform; matters only
// for the RTL handshake, not for which pixel a value lands on.
inline constexpr uint32_t SET_PIX_COMMIT_BLANK_CLOCKS = 3;

// set_pix_commit pulses this many clocks before its own slot's RGB_OUT.
inline constexpr uint32_t SET_PIX_COMMIT_LEAD = 1;

// Sustained fetch: one VIDCMD word per TWO slots (registered /RE, resolved
// 2026-08-18 in griffin.yml interfaces "VIDCMD FIFO read port", measured by
// compositor_tb's BACK_TO_BACK_SETS/SUSTAINED_2SLOT).  Fall at edge k, capture
// at edge k+1, fall again at edge k+2.
inline constexpr uint32_t VIDCMD_SLOTS_PER_WORD = 2;

// How far a parked record trails the record it was banked with.  The bank is
// two deep — staged_word plus a word held on the FIFO's Q while /RE stays low —
// and the park moves into staged_word on the very edge the first record is
// consumed, so a banked PAIR executes on CONSECUTIVE slots (compositor_tb's
// PAIR_LOCAL / PAIR_PIX).  The third record of a burst is back on the cadence.
inline constexpr uint32_t VIDCMD_PAIR_SLOT_GAP = 1;

// The on-chip bank's depth, in records: staged_word plus the parked Q.  This is
// the whole cushion a line gets for free at pixel 0 no matter how deep the FIFO
// behind it is, which is why an exact-640 line's arithmetic is a simulation and
// not a sum.
inline constexpr uint32_t VIDCMD_BANK_DEPTH = 2;

// PIXEL's own consumed-group -> registered IDX stage, from pixel.v's lead
// discussion.  The palette RAM is combinational, so it adds nothing.
inline constexpr uint32_t PIXEL_OUT_LEAD = 1;

// ---------------------------------------------------------------------------
// The per-line VIDCMD word cap the suite budgets against
// ---------------------------------------------------------------------------
//
// This is a DELIVERY number, not a playback one: ENGINE puts one word on the
// bus every ENGINE_CYCLES_PER_WORD SYSCLK, so a line can only carry as many
// words as the share of its SYSCLK the display list is allowed to spend.
//
// The share is 66%, and it is not a round number picked here — it is where the
// existing cases sit.  The tempest density sweep's last CLEAN point (32
// one-pixel stress spans, pixel stream split 20 words either side of the VIDCMD
// group) costs 294 SYSCLK of the 445-SYSCLK line; the next point up starves
// PIXEL.  294 / 445 = 66%, and 294 SYSCLK at 2 SYSCLK/word is 147 words.
//
// A PURE-VIDCMD screen — no PIXELS descriptors at all, the display list IS the
// framebuffer — has no pixel stream to share the bus with, so the whole 66% is
// its own and the cap is the honest one to hold it to.  (It could in principle
// spend more, since nothing else wants the bus; holding the screen cases to the
// same figure as everything else is what makes the numbers comparable.)
inline constexpr uint32_t VIDCMD_BUS_BUDGET_PERCENT = 66;
inline constexpr uint32_t VIDCMD_LINE_SYSCLK_BUDGET =
    (LINE_SYSCLK * VIDCMD_BUS_BUDGET_PERCENT + 50) / 100;                  // 294
inline constexpr uint32_t VIDCMD_WORDS_PER_LINE_CAP =
    static_cast<uint32_t>(VIDCMD_LINE_SYSCLK_BUDGET / ENGINE_CYCLES_PER_WORD);   // 147

// ---------------------------------------------------------------------------
// Cross-chip SET skew — a named knob over a KNOWN-BROKEN interface
// ---------------------------------------------------------------------------
//
// A SET aimed at COMPOSITOR's own held colours commits inside COMPOSITOR.  A
// SET aimed at PIXEL crosses a chip boundary on the dedicated 12-bit
// set_pix_value bus, whose value/valid/target/commit all live in ONE pipeline
// stage (compositor.v).  PIXEL applies the bus at the commit pulse, and the
// commit leads its own slot's RGB_OUT by SET_PIX_COMMIT_LEAD — a pipeline
// constant, not a pixel offset.  So both targets land on the slot the SET
// occupies and both knobs measure zero.
//
// The knobs stay named rather than deleted: main.cpp re-runs a case at a
// nonzero, asymmetric skew and requires the identical image, so the list
// builder's compensation path stays exercised rather than becoming vacuous.
inline constexpr uint32_t SKEW_PIX_TARGET = 0;   // measured: value bus, same slot
inline constexpr uint32_t SKEW_CMP_TARGET = 0;   // measured: SET lands in its own slot

constexpr uint32_t vidcmd_set_skew(uint32_t target, uint32_t skew_pix, uint32_t skew_cmp)
{
    return vidcmd_set_targets_pixel(target) ? skew_pix : skew_cmp;
}

// ============================================================================
// The fetch engine, as a line-planning simulation
// ============================================================================
//
// THE CADENCE-AWARE PREFETCH INVARIANT.  The 1-word-per-clock era retired the
// old playback(k) >= words(k+1) rule (main.cpp finding 17) because it was
// satisfied by construction.  The registered-/RE fetch brings it back, and it
// is no longer a closed form, so this is a simulation rather than a rule.
//
// compositor.v's engine, verbatim (its header calls these fall / capture /
// park, and the testbench derives every expectation from them):
//
//   fall     /RE may go low at an edge only if it was HIGH for the whole cycle
//            ending at that edge and the FIFO has data.  The 7200 advances its
//            read pointer on that falling edge and presents the word on Q.
//   capture  when /RE has been low for a full cycle the word is valid at the
//            ending edge; if staged_word is free — or is being freed by that
//            same edge's consume — the word lands in staged_word and /RE
//            registers high.
//   park     otherwise /RE stays LOW and Q holds the word until a later edge
//            frees staged_word.  /RE's own level carries the parked state, so
//            the bank is exactly two deep and a parked word cannot be
//            overwritten.
//
// L4 falls out as 2 slots per word sustained; L5 (the banked-pair law) falls
// out as "a parked record executes on the slot after the one it was banked
// with".  A record therefore has to average two slots of playback to keep the
// stream fed, with the two-deep bank as the line's entire free credit.
//
// This simulation assumes the CUSHION discipline — every word already in the
// FIFO when HBLANK starts — because that is the discipline whose slot sums have
// to close.  Whether a word ARRIVES in time is the render model's job (it
// drives the same engine against real deposit cycles); this one answers "given
// that they are all there, which slot does each record land on".
//
// Bare-metal safe: no allocation, no recursion, bounded loop.

struct VidcmdSlotPlan
{
    uint32_t slots       = 0;   // active slots the list occupies, holds included
    uint32_t last_slot   = 0;   // slot in which the final record executed
    uint32_t records     = 0;   // records that executed in active video
    uint32_t blank_sets  = 0;   // records consumed eagerly in HBLANK, zero slots
    uint32_t stretch     = 0;   // worst slots the cadence pushed any record back by
    uint32_t starved     = 0;   // records that executed later than their authored slot
    uint32_t masks       = 0;   // MASK records that executed
    uint32_t mask_stalls = 0;   // slots a mask spent waiting for its data word
};

inline VidcmdSlotPlan vidcmd_plan_line(std::span<const uint16_t> words,
                                       uint32_t hblank_clocks = H_BLANK)
{
    VidcmdSlotPlan plan;

    uint32_t next        = 0;       // next word the FIFO would hand over
    bool     re_low      = false;   // nVIDCMD_RE: a read is in progress
    bool     q_full      = false;   // ef_at_pop: that read found a word
    uint16_t q_word      = 0;
    bool     staged      = false;
    uint16_t staged_word = 0;

    uint32_t run_remaining = 0;
    bool     mask_active   = false;
    uint32_t slot          = 0;     // active slots elapsed
    uint32_t authored      = 0;     // where the authored sum puts the next record

    // Four times the line is far beyond the worst stretch a 640-slot list can
    // suffer (2 slots per word, 640 words maximum), and it keeps a malformed
    // list from spinning.
    const uint32_t limit = hblank_clocks + 4u * H_ACTIVE;
    for (uint32_t clock = 0; clock < limit; clock++)
    {
        const bool active     = clock >= hblank_clocks;
        const bool word_on_q  = re_low && q_full;
        const bool had_staged = staged;
        bool       consumed   = false;

        // Mask playback, from the pre-edge count.  run_remaining is this
        // model's form of compositor.v's run_count — remaining == 0xFFF -
        // run_count — so the RTL's low-nibble compares become plain counts:
        // pos 7 is the slot that paints pixel MASK_RELOAD_PIXEL and pos 14 the
        // slot that paints the last pixel.  Both are gated by H_ACTIVE in the
        // RTL, which is what freezes a mask across the line boundary and keeps
        // its data word parked through HBLANK.
        const bool terminal    = (run_remaining == 0);
        const bool mask_pos_7  = active && mask_active &&
                                 (run_remaining == MASK_SLOTS - MASK_RELOAD_PIXEL);
        const bool mask_pos_14 = active && mask_active && (run_remaining == 1);
        const bool mask_reload = mask_pos_7 && word_on_q;
        const bool mask_stall  = mask_pos_7 && !word_on_q;
        bool       load_mask   = false;

        // Playback: consume_active = H_ACTIVE & have_staged & terminal, or the
        // eager blank-region SET.  A MASK header PAINTS, so it is playback-class
        // and is never eager in blanking.
        if (staged)
        {
            const VidcmdType t = vidcmd_type_of(staged_word);
            if (active ? terminal : (t == VidcmdType::SET))
            {
                consumed = true;
                staged   = false;
                if (active)
                {
                    const uint32_t duration = vidcmd_record_slots(staged_word);
                    // A record that executes past its authored slot has starved
                    // the beam by that much.  A long RUN later on lets playback
                    // fall back behind the fetch, so the delay can come back
                    // down — the worst one is what an author needs to see.
                    if (slot > authored)
                    {
                        plan.starved++;
                        if (slot - authored > plan.stretch)
                        {
                            plan.stretch = slot - authored;
                        }
                    }
                    plan.last_slot = slot;
                    plan.slots     = slot + duration;
                    plan.records++;
                    authored      += duration;
                    run_remaining  = duration;
                    if (t == VidcmdType::MASK)
                    {
                        load_mask = true;
                        plan.masks++;
                    }
                }
                else
                {
                    plan.blank_sets++;
                }
            }
        }

        // mask_next is literally mask_active's next state, and mask_holds is
        // "staged_word belongs to the mask across this edge" — the term that
        // parks the next word on Q instead of letting the fetch overwrite the
        // dibits.  The two borrow-back edges punch through it.
        const bool mask_next  = load_mask || (mask_active && !terminal);
        const bool mask_holds = mask_next && !mask_pos_7 && !mask_pos_14;

        // Fetch, from the registers as they stood when the cycle began.
        const bool buffer_frees = (!had_staged || consumed) && !mask_holds;
        const bool capture_now  = word_on_q && buffer_frees;
        const bool q_parked     = word_on_q && !capture_now;
        const bool re_fall      = !re_low && (next < words.size());
        const bool re_rise      = re_low && !q_parked;

        if (capture_now)
        {
            staged_word = q_word;
            // The mask's own data word is captured with have_staged LOW: it is
            // DATA, it feeds the shifter only, and nothing can ever decode it.
            staged      = !mask_reload;
        }
        if (re_fall)
        {
            re_low = true;
            q_word = words[next];
            q_full = true;
            next++;
        }
        if (re_rise)
        {
            re_low = false;
        }

        mask_active = mask_next;

        if (active)
        {
            // count_inc = load_run | (H_ACTIVE & ~terminal & ~mask_stall): a
            // starved mask freezes whole, so pixel 7's colour stretches.
            if (run_remaining > 0 && !mask_stall)
            {
                run_remaining--;
            }
            if (mask_stall)
            {
                plan.mask_stalls++;
            }
            slot++;
        }

        if (next >= words.size() && !staged && !re_low && !mask_active)
        {
            break;
        }
    }

    return plan;
}

// ============================================================================
// AUDIO — unchanged by the 12-bit colour move
// ============================================================================
//
// One deposited word is a stereo pair {L[15:8], R[7:0]}, unsigned 8-bit per
// channel.  TIMING toggles AUDIO_TICK once per line and PORTS pops one pair
// on each FALLING edge, so the sample rate is exactly half the 31.46875 kHz
// line rate — 15734.375 Hz, which is what griffin.yml rounds to
// AUDIO_SAMPLES_PER_SECOND.  The /2 phase is TIMING's free-running toggle
// (not frame-locked), so it must be carried across frames here too:
// V_TOTAL is odd, so a frame alternately consumes 263 and 262 pairs.
inline constexpr uint32_t AUDIO_FIFO_PAIRS   = Griffin::AUDIO_FIFO_DEPTH;          // 256
inline constexpr uint32_t AUDIO_SAMPLE_RATE  = Griffin::AUDIO_SAMPLES_PER_SECOND;  // 15734
inline constexpr uint32_t AUDIO_LINES_PER_SAMPLE = 2;

constexpr uint16_t audio_pair_word(uint8_t left, uint8_t right)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(left) << 8) | right);
}

constexpr uint8_t audio_pair_left(uint16_t w)  { return static_cast<uint8_t>(w >> 8); }
constexpr uint8_t audio_pair_right(uint16_t w) { return static_cast<uint8_t>(w & 0xFF); }

}  // namespace SuperEngine
