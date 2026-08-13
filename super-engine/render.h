// render.h — PIXEL and COMPOSITOR as objects the emulator can hold and advance
// one line (or one clock) at a time.
//
// Host-only in that the whole-frame driver builds std::vectors, but the units
// themselves are plain state machines with no allocation in the hot path and no
// dependency beyond descriptor.h.  emulator.cpp includes this file verbatim and
// drives PixelUnit/CompositorUnit from its per-scanline seam; main.cpp drives
// the same two objects clock by clock so it can interleave deposits at their
// exact cycles.  ONE IMPLEMENTATION, TWO DRIVERS.
//
// Both units model the as-built RTL: cpld/pixel/pixel.v and
// cpld/compositor/compositor.v, with cpld/compositor/compositor_sim.log as the
// measured spec.  Where a model choice is not the RTL's, it says so in place.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "descriptor.h"
#include "interpret.h"

namespace SuperEngine
{

// Counters both units accumulate.  Nothing here is an error by itself — hold
// slots and tiled PIXELS words are legal framing under the JIT discipline — so
// the checker, not the model, decides what is a failure.
struct RenderStats
{
    uint32_t pixels_fifo_high      = 0;
    uint32_t pixels_fifo_low       = PIXELS_FIFO_WORDS + 1;
    uint32_t pixels_tiled_words    = 0;   // reads past the end of a short fill
    uint32_t pixels_overflows      = 0;
    uint32_t pixels_line_start_min = PIXELS_FIFO_WORDS + 1;
    uint32_t pixels_line_start_max = 0;

    uint32_t vidcmd_fifo_high    = 0;
    uint32_t vidcmd_fifo_low     = VIDCMD_FIFO_WORDS + 1;
    uint32_t vidcmd_overflows    = 0;
    uint32_t vidcmd_hold_slots   = 0;   // terminal count, nothing staged
    uint32_t vidcmd_overruns     = 0;   // a record still running at the H_ACTIVE fall
    uint32_t vidcmd_late_words   = 0;   // arrived during active video, not in HBLANK
    uint32_t vidcmd_color_runs   = 0;
    uint32_t vidcmd_reserved_ops = 0;
    uint32_t vidcmd_words_popped = 0;
};

// ---------------------------------------------------------------------------
// PIXEL
// ---------------------------------------------------------------------------
//
// Pure pixel bits in, 12-bit RGB out.  Two modes share one bit-serial front
// end; the only difference is how many bits a pixel clock eats and what they
// mean.  Registers are written by COMPOSITOR forwarding a SET, never by a bus.
class PixelUnit
{
public:
    void reset();                                 // /RS at vsync
    bool push_word(uint16_t w);                   // PIXELS FIFO write; false on full
    void begin_line();                            // line start: reload held, spend pixel_skip
    Rgb444 next_pixel();                          // one active pixel clock
    void set_register(uint32_t target, uint32_t value);

    Rgb444   pal_fg() const { return pal_fg_; }
    Rgb444   pal_bg() const { return pal_bg_; }
    Rgb444   ham_held() const { return ham_held_; }
    uint32_t mode() const { return mode_; }
    uint32_t pixel_skip() const { return pixel_skip_; }
    uint32_t fifo_count() const { return count_; }

    void attach_stats(RenderStats *s) { stats_ = s; }
    void set_counting(bool on) { counting_ = on; }

private:
    void refill(uint32_t need);
    uint32_t peek(uint32_t n) const;
    uint32_t take(uint32_t n);

    std::vector<uint16_t> fifo_ = std::vector<uint16_t>(PIXELS_FIFO_WORDS, 0);
    uint32_t head_  = 0;
    uint32_t count_ = 0;

    // The last word read, retained so an empty FIFO tiles it rather than
    // producing zeroes: a 7200 ignores a read while empty and holds Q.
    uint16_t last_word_ = 0;

    Rgb444   pal_fg_     = RGB444_WHITE;
    Rgb444   pal_bg_     = RGB444_BLACK;
    Rgb444   ham_held_   = RGB444_WHITE;
    uint32_t mode_       = PIXEL_MODE_DIRECT_1BPP;
    uint32_t pixel_skip_ = 0;

    uint64_t bitbuf_      = 0;
    uint32_t bits_avail_  = 0;
    uint32_t ham_pending_ = 0;   // this clock is the chroma half of a 4-bit code
    uint32_t ham_type_    = 0;   // 0 = 10_g_r, 1 = 11_g_b
    uint32_t ham_g_       = 0;
    uint32_t ham_v_       = 0;

    RenderStats *stats_   = nullptr;
    bool         counting_ = false;
};

// ---------------------------------------------------------------------------
// COMPOSITOR
// ---------------------------------------------------------------------------
//
// One word of on-chip lookahead, one word per clock, four sources.  The slot
// rule is compositor.v's: a record's effects land on the edge that ENDS its own
// slot, so a SET is visible in the pixel of the slot it occupies and a RUN
// emits its first pixel in the slot in which it is consumed.
class CompositorUnit
{
public:
    void reset();                          // /RS at vsync
    bool push_word(uint16_t w);            // VIDCMD FIFO write; false on full
    void blank_clock(PixelUnit &pix);      // one clock with H_ACTIVE low
    Rgb444 active_slot(PixelUnit &pix);    // one clock with H_ACTIVE high
    void end_of_line();                    // H_ACTIVE falls

    Rgb444   held_fg() const { return held_fg_; }
    Rgb444   held_bg() const { return held_bg_; }
    uint32_t fifo_count() const { return count_; }

    void attach_stats(RenderStats *s) { stats_ = s; }
    void set_counting(bool on) { counting_ = on; }
    void set_skew(uint32_t skew_pix, uint32_t skew_cmp)
    {
        skew_pix_ = skew_pix;
        skew_cmp_ = skew_cmp;
    }
    void set_pixel_position(uint64_t p) { abs_pixel_ = p; }

private:
    bool pop(uint16_t &w);
    void fetch_clock();
    void commit(uint32_t target, uint32_t value, PixelUnit &pix);
    void apply_pending(PixelUnit &pix);

    std::vector<uint16_t> fifo_ = std::vector<uint16_t>(VIDCMD_FIFO_WORDS, 0);
    uint32_t head_  = 0;
    uint32_t count_ = 0;

    Rgb444 held_fg_ = VIDCMD_RESET_HELD_FG;
    Rgb444 held_bg_ = VIDCMD_RESET_HELD_BG;

    bool     staged_valid_ = false;
    uint16_t staged_word_  = 0;

    // Playback.  compositor.v freezes this across the line boundary — nothing
    // re-frames until /RS — so an overrunning run simply continues.
    uint32_t run_remaining_ = 0;   // slots left; 0 == terminal
    uint32_t cur_src_       = RUN_SRC_PASSTHROUGH;
    Rgb444   cur_colour_    = RGB444_BLACK;

    // Cross-chip SET skew (see descriptor.h): a tiny queue so the compensation
    // path stays real even while both constants are zero.
    struct Pending
    {
        uint64_t effect_pixel = 0;
        uint32_t target       = 0;
        uint32_t value        = 0;
    };
    static constexpr uint32_t PENDING_MAX = 8;
    Pending  pending_[PENDING_MAX] = {};
    uint32_t pending_n_ = 0;
    uint32_t skew_pix_  = SKEW_PIX_TARGET;
    uint32_t skew_cmp_  = SKEW_CMP_TARGET;
    uint64_t abs_pixel_ = 0;

    RenderStats *stats_    = nullptr;
    bool         counting_ = false;
};

// ---------------------------------------------------------------------------
// Per-line helpers — the emulator's entry points
// ---------------------------------------------------------------------------

// One horizontal blanking interval: H_BLANK clocks of fetch with playback
// frozen.  Eager SETs land here; a staged RUN waits and blocks what is behind
// it, which is exactly why eagerness is positional and not temporal.
void run_blanking(PixelUnit &pix, CompositorUnit &cmp);

// One active scanline: H_ACTIVE slots into `out`, which must be H_ACTIVE long.
void render_active_line(PixelUnit &pix, CompositorUnit &cmp, Rgb444 *out);

// ---------------------------------------------------------------------------
// Whole-frame driver, for the suite
// ---------------------------------------------------------------------------

struct FrameImage
{
    std::vector<Rgb444> pixels;   // H_ACTIVE * V_ACTIVE, row major
};

struct RenderParams
{
    uint32_t first_frame   = 1;
    uint32_t frame_count   = 2;
    bool     audio_enabled = false;
    uint32_t skew_pix      = SKEW_PIX_TARGET;
    uint32_t skew_cmp      = SKEW_CMP_TARGET;
};

struct RenderResult
{
    std::vector<FrameImage>  frames;
    std::vector<uint8_t>     audio;   // interleaved L,R unsigned 8-bit
    std::vector<std::string> violations;
    RenderStats              stats;

    uint32_t audio_fifo_high       = 0;
    uint32_t audio_fifo_low        = AUDIO_FIFO_PAIRS + 1;
    uint32_t audio_underruns       = 0;
    uint32_t audio_overflows       = 0;
    uint32_t audio_pairs_deposited = 0;
    uint32_t audio_pairs_consumed  = 0;
};

// `events` must be in nondecreasing cycle order, which interpret() guarantees.
// Drives PixelUnit/CompositorUnit clock by clock so a deposit lands at its
// exact cycle — the difference between this driver and the emulator's, which
// lumps a line's deposits into its HBLANK.
RenderResult render(const std::vector<DepositEvent> &events, const RenderParams &params);

// The emulator's driver, exercised here so it cannot rot: feeds each line's
// deposits at its HBLANK and then renders through render_active_line().
RenderResult render_line_at_a_time(const std::vector<DepositEvent> &events,
                                   const RenderParams &params);

// R4G4B4 -> 8 bits per channel by nibble replication (x17).
void rgb444_to_rgb888(Rgb444 c, uint8_t &r, uint8_t &g, uint8_t &b);

}  // namespace SuperEngine
