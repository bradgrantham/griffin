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
// cpld/compositor/compositor.v, with cpld/compositor/compositor_tb.v as the
// measured spec — its normative traces, not the checked-in compositor_sim.log,
// which is an artifact of an earlier run.  Where a model choice is not the
// RTL's, it says so in place.

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

    // Two flavours of HOLD, and telling them apart is the whole point.  A
    // CADENCE hold is the second slot of the 2-clock fetch — the word exists,
    // it is simply still in flight — and it is structural, not a fault.  A
    // STARVATION hold is a genuinely dry FIFO with no read outstanding, which
    // under the CUSHION discipline means the cushion ran out.
    // A literal's own KEEP slot (law LL) is part of the literal's width and
    // is counted as neither.
    uint32_t vidcmd_cadence_slots = 0;
    uint32_t vidcmd_hold_slots   = 0;   // terminal count, nothing staged, nothing coming
    uint32_t vidcmd_overruns     = 0;   // a record still running at the H_ACTIVE fall
    uint32_t vidcmd_late_words   = 0;   // arrived during active video, not in HBLANK
    uint32_t vidcmd_literals     = 0;   // LITERAL records played (replaces the RUN_COLOR count)
    // A NEW FAULT CLASS with the STREAM RUN (law LS5): a SET still staged when
    // its stream run reaches the terminal executes as an ordinary SET, costing
    // its slot plus the cadence slot and pushing the rest of the line out.
    // The author under-sized the run (it needs 2k slots for k SETs) or the
    // words arrived late; either way the line's geometry is no longer the run's.
    uint32_t vidcmd_stream_leftovers = 0;
    uint32_t vidcmd_mask_records = 0;   // `01` records that played their 16 slots
    uint32_t vidcmd_mask_stalls  = 0;   // slots a mask held waiting for its data word
    uint32_t vidcmd_words_popped = 0;

    // The palette, and the three things a list can get wrong about it.  None of
    // them is a model assertion: each is the hardware doing something defined
    // and undesired, counted so the suite can assert on it.
    uint32_t palette_writes            = 0;   // entries actually stored
    uint32_t palette_bad_filler        = 0;   // the filler was missing or carried another value
    uint32_t palette_disturbed_visible = 0;   // a disturbed pixel reached RGB_OUT through passthrough
    uint32_t palette_unwritten_reads   = 0;   // an entry nobody ever loaded was displayed
};

// ---------------------------------------------------------------------------
// PIXEL and the palette RAM
// ---------------------------------------------------------------------------
//
// Pixel bits in, a FOUR-BIT PALETTE INDEX out, and an external 16 x 12 RAM
// turns that into colour.  Both units are written equation for equation against
// cpld/pixel/pixel.v (the decoder) and cpld/pixel/clut_pair_tb.v (the write
// window); every constant they assert is in the benches' MEASURED CONSTANTS.
//
// WHAT CHANGED IN 2026-09-20, and why it matters for the model: the old
// PixelUnit had NO FETCH CADENCE at all — it kept a bit buffer and refilled it
// on demand — so it could not see how many words a line really read, which is
// how it missed two shipped RTL bugs.  This one is a clocked state machine with
// pixel.v's own registers (shift_reg, bit_pos, align_cnt, half_phase,
// fifo_loading, line_skipped, extra_fetch_done, grp, attr_colour) and it reads
// the FIFO on the same clocks the chip does.
//
// THE THREE RASTER STROBES.  PIXEL has no horizontal counter: PIX_CONSUME,
// PIX_PRELOAD and PIX_LAST come from the raster generator in the same package.
// This model drives them from its own line-relative clock counter, which for the
// 640-in-800 raster is timing.v's law exactly:
//
//   PIX_PRELOAD  one clock, PIXEL_FETCH_LEAD (16) clocks before the line's
//                first consumption clock — so the drivers arm it at blank clock
//                H_BLANK - PIXEL_FETCH_LEAD, and only before a VISIBLE line
//   PIX_CONSUME  the 640 clocks of a visible line (next_pixel())
//   PIX_LAST     the last 8 of them, the end-of-line fetch cutoff window
//
// SCREEN SPACE, NOT CLOCK SPACE.  next_pixel() returns the colour of the pixel
// whose slot COMPOSITOR is handling on this clock, as the old model did: the
// 2-clock IDX lag and COMPOSITOR's own 2 clocks are uniform pipeline latency
// that the sync generator absorbs (PIXEL_OUT_LEAD, descriptor.h).  The palette
// write's disturbed window is therefore expressed in SLOTS after the clut_data
// SET's own slot, which is the same thing measured in clocks after its commit.

// The palette: sixteen 12-bit entries in a separate-I/O RAM chip, written by
// PIXEL's address + pulse out of COMPOSITOR's value bus.
//
// NOTHING RESETS IT.  Not /RS, not power-up.  reset_power_on() therefore fills
// it with a SEEDED pseudo-random pattern — reproducible run to run, never zero —
// so a display list that forgets an entry paints garbage here exactly as it
// would on the bench, instead of reading a convenient black.
class PaletteUnit
{
public:
    // The RAM comes up undefined.  seed is part of the model's contract: the
    // same seed gives the same "undefined" contents every run.
    void reset_power_on(uint32_t seed = PALETTE_POWER_ON_SEED);

    // /RS does NOT touch the entries; this exists only to say so at the call
    // site and to clear the write sequencer, which IS in PIXEL's reset.
    void frame_reset();

    Rgb444 entry(uint32_t index) const { return entry_[index & 0xF]; }
    bool   written(uint32_t index) const { return written_[index & 0xF]; }

    // One pixel clock of the write sequencer.  `bus` is COMPOSITOR's
    // set_pix_value as it stands during this clock.
    void clock(uint32_t bus);

    // A clut_data SET committed in this clock: start the sequence.  `value` is
    // the payload the SET committed — the twelve bits that MUST still be on the
    // bus at the pulse.
    void begin_write(uint32_t index, uint32_t value);

    // Is the pixel bus disturbed during THIS clock, and what does the RAM drive?
    //
    //   phase 1   the entry being written: wrong, but defined
    //   phase 2   undefined — /WE is low and the outputs are high-Z
    //   phase 3   undefined — write recovery
    //   phase 4   correct again, but still inside the authoring guard
    bool     bus_disturbed() const { return phase_ >= 1 && phase_ <= PALETTE_WRITE_DISTURBED_SLOTS; }
    bool     bus_undefined() const { return phase_ == 2 || phase_ == 3; }
    uint32_t write_phase() const { return phase_; }

    // Rule violations the suite asserts on.  Both are "the hardware did
    // something defined and wrong", never model assertions.
    uint32_t bad_filler_writes() const { return bad_filler_; }      // stored the wrong colour
    uint32_t unwritten_reads() const { return unwritten_reads_; }   // read an entry nobody loaded
    void     note_read(uint32_t index);

    void attach_stats(RenderStats *s) { stats_ = s; }
    void set_counting(bool on) { counting_ = on; }

    static constexpr uint32_t PALETTE_POWER_ON_SEED = 0x9E3779B9u;

private:
    Rgb444   entry_[PALETTE_ENTRIES]   = {};
    bool     written_[PALETTE_ENTRIES] = {};

    // The write sequencer, one for one with pixel.v's set_cdata / wr1 / wr2:
    // phase 1..3 are the address window and phase 2 the pulse, with phase 4 the
    // authoring guard for write recovery.
    uint32_t phase_     = 0;   // 0 = idle, 1..4 = clocks after the commit
    bool     pending_   = false;   // a commit landed in THIS clock
    uint32_t wr_index_  = 0;
    uint32_t wr_value_  = 0;   // what the clut_data SET committed
    bool     wr_flagged_      = false;
    uint32_t bad_filler_      = 0;
    uint32_t unwritten_reads_ = 0;

    RenderStats *stats_    = nullptr;
    bool         counting_ = false;
};

// The undefined colour the RAM drives while /WE is low and during recovery.  It
// is deliberately NOT a legal Rgb444 value, so a frame that shows it cannot be
// mistaken for art and a checker can find it.
inline constexpr Rgb444 RGB444_UNDEFINED = 0xF000;

class PixelUnit
{
public:
    void reset();                                 // /RS at vsync
    void power_on_reset();                        // the board's nRESET + the RAM's undefined contents
    bool push_word(uint16_t w);                   // PIXELS FIFO write; false on full
    void blank_clock();                           // one clock with PIX_CONSUME low
    void arm_preload();                           // the next blank clock carries PIX_PRELOAD
    void begin_line();                            // the line's first consumption clock is next
    Rgb444 next_pixel();                          // one active pixel clock
    void set_register(uint32_t target, uint32_t value);

    // COMPOSITOR's set_pix_value, re-registered from every staged word.  The
    // palette's data inputs sit on it, so the model has to carry it clock by
    // clock: it is the whole reason a table entry needs a filler.
    void set_value_bus(uint32_t value) { value_bus_ = value & 0xFFF; }

    uint32_t mode() const { return mode_; }
    uint32_t pix_base() const { return pix_base_; }
    uint32_t pixel_skip() const { return pixel_skip_; }
    uint32_t clut_index() const { return clut_index_; }
    uint32_t idx() const { return idx_; }
    uint32_t fifo_count() const { return count_; }

    // Words this line has read so far, and the last completed line's total:
    // what the RTL's per-line /RE count is, and the number the suite asserts
    // against pixels_words_per_line().
    uint32_t line_words_read() const { return line_words_; }
    uint32_t last_line_words_read() const { return last_line_words_; }

    // True on a clock whose colour came out of a disturbed palette read.
    bool pixel_disturbed() const { return disturbed_pixel_; }

    PaletteUnit       &palette() { return palette_; }
    const PaletteUnit &palette() const { return palette_; }

    void attach_stats(RenderStats *s)
    {
        stats_ = s;
        palette_.attach_stats(s);
    }

    void set_counting(bool on)
    {
        counting_ = on;
        palette_.set_counting(on);
    }

private:
    // pixel.v's own combinational wires, by their RTL names.
    uint32_t depth() const { return pixel_mode_depth(mode_); }
    bool     half_rate() const { return pixel_mode_half_rate(mode_); }
    uint32_t bits_per_group() const { return pixels_bits_per_group(mode_); }
    uint32_t skip_fine() const { return pixels_skip_effective(mode_, pixel_skip_); }
    bool     fetch_due() const;
    uint32_t idx_value() const;

    // One clock of the decoder, with the three raster strobes as inputs — the
    // RTL's always blocks in one place.  Returns the registered IDX.
    uint32_t clock(bool pix_consume, bool preload, bool pix_last);
    uint16_t fifo_read();

    std::vector<uint16_t> fifo_ = std::vector<uint16_t>(PIXELS_FIFO_WORDS, 0);
    uint32_t head_  = 0;
    uint32_t count_ = 0;

    // The last word read, retained so an empty FIFO tiles it rather than
    // producing zeroes: a 7200 ignores a read while empty and holds Q.
    uint16_t last_word_ = 0;

    // SET registers (pixel.v: mode, pix_base, pixel_skip, clut_index).
    uint32_t mode_       = 0;
    uint32_t pix_base_   = 0;
    uint32_t pixel_skip_ = 0;
    uint32_t clut_index_ = 0;

    // A clut_index SET committed on the pulse clock outranks that write's
    // post-increment (pixel.v: `if (set_cidx) ... else if (wr2)`).  Only an
    // illegal filler can put a SET there, but the precedence is the RTL's.
    bool cidx_set_at_pulse_ = false;

    // The word engine (pixel.v: shift_reg, bit_pos, align_cnt, half_phase,
    // fifo_loading, line_skipped, extra_fetch_done).
    uint32_t shift_reg_        = 0;
    uint32_t bit_pos_          = 0;
    uint32_t align_cnt_        = 0xF;
    uint32_t half_phase_       = 0;
    bool     fifo_loading_     = false;
    bool     line_skipped_     = false;
    bool     extra_fetch_done_ = false;

    // The attribute cell's colour byte and its one-clock transfer (pixel.v:
    // attr_colour, col_transfer).
    uint32_t attr_colour_  = 0;
    bool     col_transfer_ = false;

    // The output stage (pixel.v: grp, pix_consume_d, IDX).
    uint32_t grp_           = 0;
    bool     pix_consume_d_ = false;
    uint32_t idx_           = 0;

    // The model's stand-in for the raster generator's counters.
    uint32_t h_phase_        = 0;       // consumption clocks since begin_line()
    bool     in_line_        = false;
    bool     preload_armed_  = false;
    uint32_t line_words_     = 0;
    uint32_t last_line_words_ = 0;

    uint32_t value_bus_       = 0;
    bool     disturbed_pixel_ = false;

    PaletteUnit palette_;

    RenderStats *stats_   = nullptr;
    bool         counting_ = false;
};

// ---------------------------------------------------------------------------
// COMPOSITOR
// ---------------------------------------------------------------------------
//
// Four sources (passthrough, cmp_color1, cmp_color0, KEEP) plus the literal's
// own colour for its one slot, one slot per record, and a two-deep fetch bank
// behind it.  The
// slot rule is compositor.v's: a record's effects land on the edge that ENDS
// its own slot, so a SET is visible in the pixel of the slot it occupies and a
// RUN emits its first pixel in the slot in which it is consumed.
//
// The FETCH is the as-built one (registered /RE, 2 pixel clocks per word,
// park-on-Q banking; griffin.yml interfaces "VIDCMD FIFO read port" 2026-08-18)
// and it is modelled at the same granularity the RTL runs at, because its
// state — /RE's own level plus the word parked on Q — is what decides whether
// two records land on adjacent slots or two apart.
class CompositorUnit
{
public:
    void reset();                          // /RS at vsync
    void power_on_reset();                 // what the ATF1508's POWER_RESET does
    bool push_word(uint16_t w);            // VIDCMD FIFO write; false on full
    void blank_clock(PixelUnit &pix);      // one clock with H_ACTIVE low
    Rgb444 active_slot(PixelUnit &pix);    // one clock with H_ACTIVE high
    void end_of_line();                    // H_ACTIVE falls

    Rgb444   held_fg() const { return held_fg_; }
    Rgb444   held_bg() const { return held_bg_; }
    uint32_t fifo_count() const { return count_; }

    // compositor.v's state by its RTL names, for the bench-trace assertions
    // (cur_src, streaming, lit_active, have_staged).
    uint32_t cur_src() const { return cur_src_; }
    bool     streaming() const { return streaming_; }
    bool     literal_active() const { return lit_active_; }
    bool     has_staged() const { return staged_valid_; }

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
    bool fifo_has_data() const;
    void fetch_edge(bool word_on_q, bool had_staged, bool consumed,
                    bool mask_holds, bool mask_reload, bool load_literal);
    void commit(uint32_t target, uint32_t value, PixelUnit &pix);
    void apply_pending(PixelUnit &pix);

    std::vector<uint16_t> fifo_ = std::vector<uint16_t>(VIDCMD_FIFO_WORDS, 0);
    uint32_t head_  = 0;
    uint32_t count_ = 0;

    Rgb444 held_fg_ = VIDCMD_RESET_HELD_FG;
    Rgb444 held_bg_ = VIDCMD_RESET_HELD_BG;

    // staged_word_ is the on-chip lookahead AND the mask shifter, exactly as in
    // compositor.v: while a mask plays, staged_valid_ is false and the record
    // that would otherwise have been captured parks on Q behind it.
    bool     staged_valid_ = false;
    uint16_t staged_word_  = 0;

    // set_pix_value: compositor.v registers it from the staged word on EVERY
    // clock, whatever the target, so a payload owns the bus for as long as its
    // word stays staged.  The palette RAM's data inputs are on it, which is the
    // mechanism behind the three-word table entry (descriptor.h, "THE PALETTE").
    uint32_t set_pix_value_ = 0;

    // The fetch engine's own registers, one for one with compositor.v.  There
    // is deliberately no "parked" flag: /RE's low level IS the parked state,
    // which is what makes the bank exactly two deep and unclobberable.
    bool     re_low_    = false;   // nVIDCMD_RE low: a read is in progress
    bool     ef_at_pop_ = false;   // raw /EF sampled at the /RE fall
    uint16_t q_word_    = 0;       // what the 7200 is presenting on Q
    bool     ef_meta_   = false;   // 2-FF synchronizer on the asynchronous /EF rise
    bool     ef_sync_   = false;

    // Playback.  compositor.v freezes this across the line boundary — nothing
    // re-frames until /RS — so an overrunning run simply continues.
    uint32_t run_remaining_ = 0;   // slots left; 0 == terminal
    uint32_t cur_src_       = RUN_SRC_PASSTHROUGH;

    // compositor.v's streaming flag (STREAM RUN, LS1..LS5) and lit_active (the
    // literal's bank hold: its slot shows staged_word[11:0]).
    bool streaming_  = false;
    bool lit_active_ = false;

    // RGB_OUT, which KEEP feeds back.  In this model's SCREEN SPACE it is the
    // previous slot's colour on the same line, and black at a line's first slot:
    // compositor.v registers RGB_OUT <= h_active_d ? pixel_mux : 0, so the value
    // KEEP sees during slot 0 is the black registered in blanking.
    Rgb444 rgb_out_ = RGB444_BLACK;

    // MASK playback.  The record BORROWS cur_src_ for its sixteen slots and
    // gives it back at the end (compositor.v does the same, and for the same
    // reason: an override in front of the colour mux does not fit).  Externally
    // that is the modal law — the span in force resumes as if untouched; over a
    // KEEP span the restore is KEEP, which repeats the mask's last pixel.
    bool     mask_active_   = false;
    uint32_t sav_src_       = RUN_SRC_PASSTHROUGH;

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
//
// PIX_PRELOAD lives in here: the raster generator pulses it PIXEL_FETCH_LEAD
// clocks before a VISIBLE line, so this arms it at blank clock
// H_BLANK - PIXEL_FETCH_LEAD when the line that FOLLOWS this blanking is
// visible.  A caller that passes true in vertical blanking makes PIXEL read one
// PIXELS word per blank line that the chip would not read.
void run_blanking(PixelUnit &pix, CompositorUnit &cmp, bool next_line_visible = true);

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
    std::vector<uint8_t>     audio;   // interleaved L,R unsigned 8-bit (channels 0 and 2)
    std::vector<std::string> violations;
    RenderStats              stats;

    uint32_t audio_fifo_high       = 0;
    uint32_t audio_fifo_low        = AUDIO_FIFO_SAMPLES + 1;
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
