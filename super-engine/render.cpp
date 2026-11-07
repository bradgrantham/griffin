// render.cpp — PIXEL and COMPOSITOR state machines, plus the suite's two
// drivers over them.
//
// ORDER WITHIN AN ACTIVE PIXEL CLOCK — this is the whole ballgame, and it is
// compositor.v's, not an invention:
//
//   1. retire any skew-delayed register commits whose effect pixel has arrived
//   2. sample the fetch engine's pre-edge state (is a word sitting on Q, is
//      staged_word occupied) — compositor.v decides every fetch term from the
//      registers as they stand when the cycle begins
//   3. COMPOSITOR consumes this slot's staged word if the run count is terminal;
//      a SET commits here, a RUN loads its source and count here
//   4. sample PIXEL — so a PIXEL-target SET committed in step 3 is already
//      visible in this very slot
//   5. mux the slot's source against PIXEL's colour
//   6. fetch edge: capture-or-park, then the /RE fall/rise decision
//
// Hence RUN(passthrough,1), SET(pix_pal_fg,C2), RUN(passthrough,637) over
// all-foreground bits renders pixel 0 as C1 and pixels 1..639 as C2, which is
// compositor_tb's NORMATIVE_M0: the RUN and the SET are a banked pair, so the
// SET still lands on pixel 1, and the tail RUN follows the 2-slot cadence onto
// slot 3 with slot 2 holding the same passthrough behind it.

#include "render.h"

#include "audio_chip.h"

#include <cstdio>

namespace SuperEngine
{

void rgb444_to_rgb888(Rgb444 c, uint8_t &r, uint8_t &g, uint8_t &b)
{
    r = rgb444_channel_to_8(rgb444_r(c));
    g = rgb444_channel_to_8(rgb444_g(c));
    b = rgb444_channel_to_8(rgb444_b(c));
}

// ---------------------------------------------------------------------------
// PaletteUnit — the external 16 x 12 RAM and its write sequencer
// ---------------------------------------------------------------------------
//
// pixel.v drives the address (IDX) and the pulse (nCLUT_WE); COMPOSITOR drives
// the twelve data inputs from set_pix_value.  Everything below is
// clut_pair_tb.v's measured sequence, counted in clocks from the clock in which
// set_pix_commit is high for the clut_data SET:
//
//   phase 1   IDX = clut_index                  RAM drives the entry being
//                                               written: WRONG, but defined
//   phase 2   nCLUT_WE low, one clock           RAM outputs high-Z: UNDEFINED
//   phase 3   nCLUT_WE high, address still held RAM recovering: UNDEFINED
//   phase 4   IDX back to the pixel index       correct again; the authoring
//                                               guard for write recovery
//
// In the model's screen space those four phases are the four SLOTS after the
// clut_data SET's own slot, which is the same window measured in clocks after
// its commit (render.h).
//
// THE STORED VALUE IS WHAT IS ON THE BUS AT THE PULSE, and that is the criterion
// clut_pair_tb.v settled on after its second harness bug: "the data was stable"
// is not "the data was correct", because an illegal third word leaves the bus
// perfectly stable at its own payload.  So a missing or wrong-valued filler
// stores the wrong colour here exactly as it does on the bench (S1a/S1b), and
// the model flags it rather than fixing it up.

void PaletteUnit::reset_power_on(uint32_t seed)
{
    // Nothing on the board clears this RAM, so the model must not either.  A
    // xorshift keeps it reproducible: the same seed gives the same "undefined"
    // contents every run, so a suite case that forgets an entry fails the same
    // way every time.
    uint32_t x = seed | 1u;
    for (uint32_t i = 0; i < PALETTE_ENTRIES; i++)
    {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        entry_[i]   = static_cast<Rgb444>(x & 0x0FFFu);
        written_[i] = false;
    }
    phase_            = 0;
    wr_index_         = 0;
    wr_value_         = 0;
    wr_flagged_       = false;
    bad_filler_       = 0;
    unwritten_reads_  = 0;
}

void PaletteUnit::frame_reset()
{
    // /RS reaches PIXEL, not the RAM: the entries survive, the sequencer does
    // not (pixel.v clears wr1/wr2 and drives nCLUT_WE high on reset).
    phase_      = 0;
    wr_flagged_ = false;
}

void PaletteUnit::begin_write(uint32_t index, uint32_t value)
{
    wr_index_   = index & 0xFu;
    wr_value_   = value & 0xFFFu;
    wr_flagged_ = false;
    phase_      = 0;   // this clock is clock 0; phase 1 is the next one
    pending_    = true;
}

void PaletteUnit::clock(uint32_t bus)
{
    if (phase_ == 2)
    {
        // The pulse clock: the RAM latches whatever the bus carries.
        entry_[wr_index_]   = static_cast<Rgb444>(bus & 0x0FFFu);
        written_[wr_index_] = true;
        if (stats_ != nullptr && counting_)
        {
            stats_->palette_writes++;
        }
        if ((bus & 0x0FFFu) != wr_value_ && !wr_flagged_)
        {
            wr_flagged_ = true;
            bad_filler_++;
            if (stats_ != nullptr && counting_)
            {
                stats_->palette_bad_filler++;
            }
        }
    }

    if (phase_ > 0)
    {
        phase_++;
        if (phase_ > PALETTE_WRITE_DISTURBED_SLOTS)
        {
            phase_ = 0;
        }
    }

    if (pending_)
    {
        pending_ = false;
        phase_   = 1;
    }
}

void PaletteUnit::note_read(uint32_t index)
{
    if (!written_[index & 0xFu])
    {
        unwritten_reads_++;
        if (stats_ != nullptr && counting_)
        {
            stats_->palette_unwritten_reads++;
        }
    }
}

// ---------------------------------------------------------------------------
// PixelUnit — pixel.v's indexed decoder, register for register
// ---------------------------------------------------------------------------

void PixelUnit::reset()
{
    // /RS at vsync.  pixel.v's async reset block, verbatim: the SET registers,
    // the word engine, the output stage.  The FIFO's own /RS empties it.
    head_       = 0;
    count_      = 0;
    last_word_  = 0;

    mode_       = 0;
    pix_base_   = 0;
    pixel_skip_ = 0;
    clut_index_ = 0;
    cidx_set_at_pulse_ = false;

    shift_reg_        = 0;
    bit_pos_          = 0;
    align_cnt_        = 0xF;
    half_phase_       = 0;
    fifo_loading_     = false;
    line_skipped_     = false;
    extra_fetch_done_ = false;

    attr_colour_  = 0;
    col_transfer_ = false;

    grp_           = 0;
    pix_consume_d_ = false;
    idx_           = 0;

    h_phase_         = 0;
    in_line_         = false;
    preload_armed_   = false;
    line_words_      = 0;
    last_line_words_ = 0;

    value_bus_       = 0;
    disturbed_pixel_ = false;

    // THE PALETTE RAM IS NOT PART OF THIS.  /RS does not reach it.
    palette_.frame_reset();
}

void PixelUnit::power_on_reset()
{
    reset();
    palette_.reset_power_on();
}

bool PixelUnit::push_word(uint16_t w)
{
    if (count_ >= fifo_.size())
    {
        if (counting_ && stats_ != nullptr)
        {
            stats_->pixels_overflows++;
        }
        return false;
    }
    fifo_[(head_ + count_) % fifo_.size()] = w;
    count_++;
    return true;
}

// A 7200 ignores a read while empty and holds Q, so an exhausted FIFO simply
// re-delivers the last word.  That makes a short PIXELS fill a bandwidth
// compressor rather than an error — pixel.v has no empty flag to notice with.
uint16_t PixelUnit::fifo_read()
{
    uint16_t w = last_word_;
    if (count_ > 0)
    {
        w = fifo_[head_];
        head_ = (head_ + 1) % static_cast<uint32_t>(fifo_.size());
        count_--;
        last_word_ = w;
    }
    else if (counting_ && stats_ != nullptr)
    {
        stats_->pixels_tiled_words++;
    }
    line_words_++;
    return w;
}

// pixel.v's fetch_due, written the same way: the compares the step size makes
// sufficient.  Attribute mode's cell is eight groups, so it compares the low
// three bits of bit_pos only.
bool PixelUnit::fetch_due() const
{
    const bool half = half_rate();
    switch (depth())
    {
        case PIXEL_DEPTH_ATTR:
            return ((bit_pos_ >> 1) & 3u) == 3u && (bit_pos_ & 1u) == (half ? 1u : 0u);
        case PIXEL_DEPTH_4BPP:
            return ((bit_pos_ >> 2) & 3u) == (half ? 3u : 2u);
        case PIXEL_DEPTH_2BPP:
            return ((bit_pos_ >> 1) & 7u) == (half ? 7u : 6u);
        default:
            return ((bit_pos_ >> 1) & 7u) == 7u && (bit_pos_ & 1u) == (half ? 1u : 0u);
    }
}

// pixel.v's idx_value, off the REGISTERED group.  The model resolves the group
// consumed on this very clock instead (screen space, render.h), which is the
// same equation applied one clock earlier.
uint32_t PixelUnit::idx_value() const
{
    switch (depth())
    {
        case PIXEL_DEPTH_ATTR:
            return ((grp_ >> 3) & 1u) != 0 ? ((attr_colour_ >> 4) & 0xFu)
                                           : (attr_colour_ & 0xFu);
        case PIXEL_DEPTH_4BPP:
            return grp_ & 0xFu;
        case PIXEL_DEPTH_2BPP:
            return (pix_base_ & 0xCu) | ((grp_ >> 2) & 3u);
        default:
            return (pix_base_ & 0xEu) | ((grp_ >> 3) & 1u);
    }
}

// ---------------------------------------------------------------------------
// ONE PIXEL CLOCK OF THE DECODER
// ---------------------------------------------------------------------------
//
// pixel.v's always blocks, in one function, all of them reading the state as it
// stands at the start of the clock (non-blocking assignment) and writing it at
// the end.  The priority chains are the RTL's, and their order is load bearing:
// the engine's `fifo_loading` branch preempts its `step` branch, and the shifter
// is deliberately OUTSIDE that chain.
//
// Returns the index this clock's consumed group resolves to.
uint32_t PixelUnit::clock(bool pix_consume, bool preload, bool pix_last)
{
    const uint32_t sf           = skip_fine();
    const bool     skip_nonzero = sf != 0;
    const bool     aligning     = align_cnt_ != 0xF;
    const bool     half         = half_rate();
    const bool     step         = pix_consume && (!half || half_phase_ == 0);
    const bool     attr         = depth() == PIXEL_DEPTH_ATTR;
    const uint32_t w            = bits_per_group();
    const bool     due          = fetch_due();
    const bool     cutoff       = pix_last && (extra_fetch_done_ || !line_skipped_);

    // The state this clock reads.
    const uint32_t old_shift        = shift_reg_;
    const bool     old_loading      = fifo_loading_;
    const bool     old_col_transfer = col_transfer_;
    const bool     old_consume_d    = pix_consume_d_;

    // The group this clock consumes, i.e. what the RTL latches into grp at the
    // end of it: {attr ? shift_reg[7] : shift_reg[15], shift_reg[14:12]}.
    const uint32_t group = (((attr ? (old_shift >> 7) : (old_shift >> 15)) & 1u) << 3) |
                           ((old_shift >> 12) & 7u);

    // --- the word engine ----------------------------------------------------
    if (preload)
    {
        fifo_loading_ = true;
        align_cnt_    = (~sf) & 0xFu;
        line_skipped_ = skip_nonzero;
    }
    else if (old_loading)
    {
        fifo_loading_ = false;
    }
    else if (aligning)
    {
        align_cnt_ = (align_cnt_ + 1u) & 0xFu;
    }
    else if (step)
    {
        if (due && !cutoff)
        {
            fifo_loading_ = true;
        }
    }

    // --- the shifter, outside the chain above -------------------------------
    if (old_loading)
    {
        shift_reg_ = fifo_read();
    }
    else if (aligning && !preload)
    {
        shift_reg_ = (old_shift << 1) & 0xFFFFu;
    }
    else if (step)
    {
        shift_reg_ = (old_shift << w) & 0xFFFFu;
    }

    // --- bit_pos ------------------------------------------------------------
    if (preload)
    {
        bit_pos_ = sf;
    }
    else if (step)
    {
        bit_pos_ = (bit_pos_ + w) & 0xFu;
    }

    // --- half_phase ---------------------------------------------------------
    if (preload)
    {
        half_phase_ = 0;
    }
    else if (pix_consume)
    {
        half_phase_ ^= 1u;
    }

    // --- the skipped line's one-word allowance ------------------------------
    extra_fetch_done_ = pix_last && (extra_fetch_done_ || (step && due));

    // --- the attribute cell's colour byte -----------------------------------
    //
    // col_transfer fires one clock AFTER the load, which is the only edge
    // attr_colour may change on: any earlier and the previous cell's last pixel,
    // resolved off the registered group, gets the new cell's colour.  In screen
    // space that means the update happens BEFORE this clock's pixel resolves,
    // which is why it sits here and not below.
    col_transfer_ = attr && old_loading;
    if (old_col_transfer)
    {
        attr_colour_ = (old_shift >> 8) & 0xFFu;
    }

    // --- the output stage ---------------------------------------------------
    pix_consume_d_ = pix_consume;
    if (step)
    {
        grp_ = group;
    }

    // IDX: clut_index while the write sequence owns the bus, 0 outside a line,
    // the pixel's index otherwise.  Kept for observability; the colour this
    // function returns is resolved in screen space below.
    const uint32_t seq = palette_.write_phase();
    idx_ = (seq >= 1 && seq <= 3) ? clut_index_ :
           (!old_consume_d)       ? 0u
                                  : idx_value();

    // pixel.v: clut_index <= clut_index + 1 on the edge that ends the pulse
    // clock (wr2 high), unless a clut_index SET coincides — the SET wins.  IDX
    // is registered from the OLD value on that same edge, so the hold clock
    // still carries the written address.  seq == 3 at the top of this call IS
    // that edge (seq counts the clock the RTL's clut_seq term belongs to), so
    // the increment follows idx_ above and the next clut_data SET — clock 4 at
    // the earliest — begins its write at the new index.
    if (seq == 3)
    {
        if (!cidx_set_at_pulse_)
        {
            clut_index_ = (clut_index_ + 1u) & 0xFu;
        }
        cidx_set_at_pulse_ = false;
    }

    // --- the palette's own clock, on COMPOSITOR's bus as it stands now ------
    palette_.clock(value_bus_);

    // The index this clock's pixel resolves to: idx_value() off grp, which was
    // just updated if this was a step clock and holds its old group if it was
    // the second clock of a half-rate pair.  THE HOLD IS grp's, NOT THE
    // SHIFTER'S — the shifter has already moved on by the time the RTL's index
    // mux reads it, which is why the model reads the register too.
    return idx_value();
}

void PixelUnit::arm_preload()
{
    preload_armed_ = true;
}

void PixelUnit::blank_clock()
{
    const bool preload = preload_armed_;
    preload_armed_ = false;

    if (preload)
    {
        // The line's first read: roll the per-line word counter over here,
        // because the preload's own read belongs to the line that follows.
        last_line_words_ = line_words_;
        line_words_      = 0;
        in_line_         = false;
    }

    (void)clock(false, preload, false);
    disturbed_pixel_ = false;
}

void PixelUnit::begin_line()
{
    // The raster generator's business, not a register: the next clock is this
    // line's first consumption clock.  Nothing is reset here — everything the
    // line needs was set up at PIX_PRELOAD, PIXEL_FETCH_LEAD clocks ago.
    in_line_ = true;
    h_phase_ = 0;

    if (counting_ && stats_ != nullptr)
    {
        if (count_ < stats_->pixels_line_start_min)
        {
            stats_->pixels_line_start_min = count_;
        }
        if (count_ > stats_->pixels_line_start_max)
        {
            stats_->pixels_line_start_max = count_;
        }
    }
}

Rgb444 PixelUnit::next_pixel()
{
    // PIX_LAST is the last eight consumption clocks of the line: the
    // end-of-line fetch cutoff window (timing.v, h_cnt[9:3] == 79).
    const bool pix_last = in_line_ && h_phase_ + PIXEL_LAST_CLOCKS >= H_ACTIVE;
    const uint32_t index = clock(true, false, pix_last);
    h_phase_++;

    // What the RAM drives on this clock.  A write steals the address for three
    // clocks and needs a fourth to recover, so the pixel that should have been
    // painted here is lost: clut_pair_tb.v S6-neg measures exactly this reaching
    // RGB_OUT when the slot is passthrough.
    disturbed_pixel_ = palette_.bus_disturbed();

    if (palette_.bus_undefined())
    {
        return RGB444_UNDEFINED;
    }
    if (palette_.write_phase() == 1)
    {
        // The entry being written, read back before the pulse: wrong, defined.
        return palette_.entry(clut_index_);
    }

    palette_.note_read(index);
    return palette_.entry(index);
}

void PixelUnit::set_register(uint32_t target, uint32_t value)
{
    switch (target)
    {
        case SET_PIX_CLUT_INDEX:
            clut_index_        = value & 0xF;
            cidx_set_at_pulse_ = (palette_.write_phase() == 2);   // the pulse clock
            break;
        case SET_PIX_BASE:       pix_base_   = value & 0xF; break;
        case SET_PIX_MODE:       mode_       = value & PIXEL_MODE_BITS; break;
        case SET_PIX_PIXEL_SKIP: pixel_skip_ = value & 0xF; break;

        // clut_data writes no register: it starts the RAM write sequence, whose
        // DATA comes from COMPOSITOR's bus two clocks later and not from here.
        case SET_PIX_CLUT_DATA:  palette_.begin_write(clut_index_, value & 0xFFF); break;

        default:                 break;
    }
}

// ---------------------------------------------------------------------------
// CompositorUnit
// ---------------------------------------------------------------------------

void CompositorUnit::reset()
{
    head_          = 0;
    count_         = 0;
    held_fg_       = VIDCMD_RESET_HELD_FG;
    held_bg_       = VIDCMD_RESET_HELD_BG;
    staged_valid_  = false;
    staged_word_   = 0;
    set_pix_value_ = 0;
    re_low_        = false;                 // /RE idles high
    ef_at_pop_     = false;
    q_word_        = 0;
    ef_meta_       = false;
    ef_sync_       = false;
    run_remaining_ = 0;                     // count already terminal
    cur_src_       = RUN_SRC_PASSTHROUGH;   // source back to passthrough, never KEEP
    streaming_     = false;                 // the streaming flag is cleared
    lit_active_    = false;                 // and a literal's bank hold abandoned
    rgb_out_       = RGB444_BLACK;          // RGB_OUT resets to black
    mask_active_   = false;                 // mask playback abandoned
    sav_src_       = RUN_SRC_PASSTHROUGH;
    pending_n_     = 0;
}

// POWER-ON RESET, and it is load bearing rather than cosmetic.  On the board
// COMPOSITOR's only reset input is PIXEL's /RS, which pulses once per frame at
// the top of the vsync line — NOTHING resets it at power-up except the
// ATF1508AS's own POWER_RESET feature (the fitter's power_reset strategy flag).
// Whatever does not model that runs its whole first frame with undefined state.
//
// POWER_RESET clears the macrocell registers, so the power-on state is NOT the
// /RS state: the held colours come up 0x000/0x000, not 0xFFF/0x000.  A list that
// relies on cmp_color1 being white without ever setting it therefore works after
// the first vsync and not before it, which is the honest model of the board.
void CompositorUnit::power_on_reset()
{
    reset();
    held_fg_ = RGB444_BLACK;
    held_bg_ = RGB444_BLACK;
}

bool CompositorUnit::push_word(uint16_t w)
{
    if (count_ >= fifo_.size())
    {
        if (counting_ && stats_ != nullptr)
        {
            stats_->vidcmd_overflows++;
        }
        return false;
    }
    fifo_[(head_ + count_) % fifo_.size()] = w;
    count_++;
    return true;
}

bool CompositorUnit::pop(uint16_t &w)
{
    if (count_ == 0)
    {
        return false;
    }
    w = fifo_[head_];
    head_ = (head_ + 1) % static_cast<uint32_t>(fifo_.size());
    count_--;
    if (counting_ && stats_ != nullptr)
    {
        stats_->vidcmd_words_popped++;
    }
    return true;
}

// /EF, asymmetrically guarded exactly as compositor.v guards it: the RISE is
// asynchronous (an ENGINE-domain write) and has to cross two synchronizer
// stages before the fall rule will believe it, so a word deposited this clock
// cannot start a read for two more.  The FALL is our own read emptying the
// FIFO and the raw flag governs it.
bool CompositorUnit::fifo_has_data() const
{
    return count_ > 0 && ef_sync_ && ef_meta_;
}

// The fetch half of one clock edge — registered /RE, two pixel clocks per word,
// park-on-Q banking (griffin.yml interfaces "VIDCMD FIFO read port", resolved
// 2026-08-18; compositor.v implements it and compositor_tb.v pins the timing
// semantics).  All three arguments are the PRE-edge state, because that is what
// the RTL's combinational terms see:
//
//   word_on_q   /RE was low for the whole cycle ending here, and the read it
//               started found a word, so Q is readable at this edge.
//   had_staged  staged_word was occupied when the cycle began.
//   consumed    this edge's playback freed staged_word.
//   mask_holds  staged_word belongs to a playing MASK across this edge, so the
//               buffer is NOT free however the first three read — that is what
//               protects the dibits and parks the next record on Q.
//   mask_reload this edge is the mask's pixel-8 boundary, so the word captured
//               here is the mask's DATA and must land with have_staged LOW.
//   load_literal this edge consumed a LITERAL, which keeps staged_word (LL).
//
// The two laws the suite has to reproduce come straight out of these five
// lines: a word captured here cannot execute before the NEXT slot (2 slots per
// word sustained), and a word PARKED on Q moves into staged_word on the very
// edge the record ahead of it is consumed, so a banked pair executes on
// consecutive slots.  The mask's two borrow-back edges are the third: they punch
// through mask_holds, which is what makes mask-to-mask chaining gapless.
void CompositorUnit::fetch_edge(bool word_on_q, bool had_staged, bool consumed,
                                bool mask_holds, bool mask_reload, bool load_literal)
{
    // A literal's consume edge does not free the buffer either (law LL): the
    // literal's colour is read out of staged_word for one more clock, so the
    // next word parks on Q and is captured one edge later.
    const bool buffer_frees = (!had_staged || consumed) && !mask_holds && !load_literal;
    const bool capture_now  = word_on_q && buffer_frees;
    const bool q_parked     = word_on_q && !capture_now;
    const bool re_fall      = !re_low_ && fifo_has_data();
    const bool re_rise      = re_low_ && !q_parked;

    // Sampled before the pop below, like the flag itself.
    ef_sync_ = ef_meta_;
    ef_meta_ = count_ > 0;

    if (capture_now)
    {
        staged_word_  = q_word_;
        staged_valid_ = !mask_reload;
    }

    // Fall and rise are mutually exclusive by construction: one needs /RE high,
    // the other /RE low.  The 7200 advances its read pointer on the fall, and
    // ef_at_pop remembers whether that read was answered at all.
    if (re_fall)
    {
        re_low_    = true;
        ef_at_pop_ = pop(q_word_);
    }
    if (re_rise)
    {
        re_low_ = false;
    }
}

// SET target 0 writes cmp_color1 and target 1 writes cmp_color0.  The
// number/colour mismatch is deliberate and deferred (descriptor.h): the wire
// encoding is frozen, and renumbering the targets is a contract change to be
// made on its own.
void CompositorUnit::commit(uint32_t target, uint32_t value, PixelUnit &pix)
{
    if (target == SET_CMP_HELD_FG)
    {
        held_fg_ = static_cast<Rgb444>(value);
    }
    else if (target == SET_CMP_HELD_BG)
    {
        held_bg_ = static_cast<Rgb444>(value);
    }
    else
    {
        pix.set_register(target, value);
    }
}

void CompositorUnit::apply_pending(PixelUnit &pix)
{
    uint32_t kept = 0;
    for (uint32_t i = 0; i < pending_n_; i++)
    {
        if (pending_[i].effect_pixel <= abs_pixel_)
        {
            commit(pending_[i].target, pending_[i].value, pix);
        }
        else
        {
            pending_[kept] = pending_[i];
            kept++;
        }
    }
    pending_n_ = kept;
}

// Playback is frozen while H_ACTIVE is low: no slots, no counting.  Fetch
// continues, and a staged SET executes immediately at pop, in stream order, so
// it lands before the next line's pixel 0.  A staged RUN — or a staged MASK
// HEADER, which paints pixel 0 and is therefore playback, not setup — waits for
// H_ACTIVE and blocks everything behind it: eagerness is positional, not
// temporal.  So does a staged LITERAL, HOLD or STREAM RUN — streaming is an
// active-video rule, so a SET behind a staged stream run is not eager either.
void CompositorUnit::blank_clock(PixelUnit &pix)
{
    const bool word_on_q  = re_low_ && ef_at_pop_;
    const bool had_staged = staged_valid_;
    bool       consumed   = false;

    // set_pix_value as it stands during THIS clock: compositor.v registers it
    // from the staged word every clock, so it is one clock behind staging.  The
    // palette RAM's data inputs are on it.
    pix.set_value_bus(set_pix_value_);

    if (staged_valid_ && vidcmd_type_of(staged_word_) == VidcmdType::SET)
    {
        // Skew is unobservable here: nothing is being displayed.  Only the weak
        // property is modelled — the write has landed before pixel 0.
        commit(vidcmd_set_target(staged_word_), vidcmd_set_value(staged_word_), pix);
        staged_valid_ = false;
        consumed      = true;
    }

    // A mask caught by the H_ACTIVE fall freezes rather than being abandoned,
    // and mask_pos_7/pos_14 are gated by H_ACTIVE in the RTL, so mask_holds is
    // unconditionally true here: the parked data word is NOT taken during
    // blanking.  (compositor_tb MASKB_RESUME asserts exactly that.)
    const bool mask_holds = mask_active_ && run_remaining_ > 0;

    // PIXEL advances after this clock's commit, so a clut_data SET committed
    // here starts its write sequence on this very clock — the same order the
    // RTL has, where set_cdata is live in the commit clock.
    pix.blank_clock();

    // RGB_OUT is forced black outside H_ACTIVE, so KEEP at the next line's
    // first slot shows black; lit_active <= load_literal, and no literal loads
    // here (it is playback-class).
    rgb_out_    = RGB444_BLACK;
    lit_active_ = false;

    // Blanking does not exempt the fetch from the cadence: a run of eager SETs
    // still executes at one per two clocks (a pair when one is parked).  With
    // H_BLANK = 160 clocks that is invisible to a cushioned list and exactly
    // what a starved one runs out of.
    fetch_edge(word_on_q, had_staged, consumed, mask_holds, false, false);
    set_pix_value_ = static_cast<uint32_t>(staged_word_) & 0xFFFu;
}

Rgb444 CompositorUnit::active_slot(PixelUnit &pix)
{
    apply_pending(pix);

    // The palette RAM's data inputs, one clock behind staging (see blank_clock).
    pix.set_value_bus(set_pix_value_);

    // The fetch engine's pre-edge state, sampled before playback can disturb
    // it: compositor.v evaluates every fetch term from the registers plus this
    // cycle's H_ACTIVE, so no edge ever has to guess the next slot.
    const bool word_on_q  = re_low_ && ef_at_pop_;
    const bool had_staged = staged_valid_;
    bool       consumed   = false;

    // MASK playback, all of it derived from the pre-edge count.  run_remaining_
    // is this model's form of compositor.v's run_count — remaining == 0xFFF -
    // run_count — so the RTL's two low-nibble compares become plain counts:
    //
    //   pos 7   the slot that paints pixel MASK_RELOAD_PIXEL.  The header is
    //           spent (d7 was read on the previous edge), so the data word comes
    //           out of the park HERE and d8 is read straight off Q.  Playback
    //           does not pause.  If the data word has not arrived the record
    //           STALLS whole: no count, no shift, no source change, so pixel 7's
    //           colour stretches.
    //   pos 14  the slot that paints the last pixel.  The shifter is dead
    //           afterwards, so the NEXT RECORD is captured one slot early —
    //           which is exactly what makes mask-to-mask chaining gapless.
    const bool terminal    = (run_remaining_ == 0);
    const bool mask_pos_7  = mask_active_ &&
                             (run_remaining_ == MASK_SLOTS - MASK_RELOAD_PIXEL);
    const bool mask_pos_14 = mask_active_ && (run_remaining_ == 1);
    const bool mask_reload = mask_pos_7 && word_on_q;
    const bool mask_stall  = mask_pos_7 && !word_on_q;
    const bool mask_step   = mask_active_ && !terminal && !mask_stall;
    const bool mask_end    = mask_active_ && terminal;
    bool       load_mask   = false;

    // WRITTEN IN compositor.v's PRIORITY ORDER, and the order is load-bearing.
    // A mask's end edge and the next record's load edge are the SAME edge when
    // records chain, so the restore is written first and any load overrides it.
    if (mask_end)
    {
        cur_src_ = sav_src_;
    }

    // consume_active = H_ACTIVE & have_staged & (terminal | (streaming & w_is_set)).
    // A SET staged behind a STREAM RUN does not wait for the terminal (LS2);
    // everything else does (LS3), and the count runs on untouched underneath.
    const bool staged_is_set  = staged_valid_ && vidcmd_type_of(staged_word_) == VidcmdType::SET;
    const bool consume_active = staged_valid_ && (terminal || (streaming_ && staged_is_set));
    bool       load_literal   = false;

    if (consume_active)
    {
        const uint16_t   w = staged_word_;
        const VidcmdType t = vidcmd_type_of(w);
        staged_valid_ = false;
        consumed      = true;

        if (t == VidcmdType::SET)
        {
            // LS5: a SET still staged at its stream run's terminal executes as
            // an ordinary SET and steals its slot from the line.
            if (terminal && streaming_ && counting_ && stats_ != nullptr)
            {
                stats_->vidcmd_stream_leftovers++;
            }
            const uint32_t target = vidcmd_set_target(w);
            const uint32_t skew   = vidcmd_set_skew(target, skew_pix_, skew_cmp_);
            if (skew == 0)
            {
                commit(target, vidcmd_set_value(w), pix);
            }
            else if (pending_n_ < PENDING_MAX)
            {
                pending_[pending_n_].effect_pixel = abs_pixel_ + skew;
                pending_[pending_n_].target       = target;
                pending_[pending_n_].value        = vidcmd_set_value(w);
                pending_n_++;
            }
            apply_pending(pix);
        }
        else if (t == VidcmdType::RUN)
        {
            // load_run: cur_src <= w_run_src (a HOLD forces KEEP, a literal's
            // src is KEEP already), streaming <= w_streaming.  A literal loads
            // no count — the count was terminal on its consume edge and stays
            // terminal — and sets lit_active for its own slot.
            const VidcmdRunKind kind = vidcmd_run_kind(w);
            cur_src_   = vidcmd_run_load_src(w);
            streaming_ = (kind == VidcmdRunKind::STREAM);
            if (kind == VidcmdRunKind::LITERAL)
            {
                load_literal = true;
                if (counting_ && stats_ != nullptr)
                {
                    stats_->vidcmd_literals++;
                }
            }
            else
            {
                run_remaining_ = vidcmd_run_count(w);
            }
        }
        else
        {
            // MASK.  The header PAINTS: its own slot is the record's pixel
            // 0, an implicit opaque cmp_color0.  sav_src_ is only taken when
            // a mask is not already playing, so a chained mask does not
            // overwrite the source the FIRST one borrowed.
            if (!mask_active_)
            {
                sav_src_ = cur_src_;
            }
            load_mask      = true;
            run_remaining_ = MASK_SLOTS;
            if (counting_ && stats_ != nullptr)
            {
                stats_->vidcmd_mask_records++;
            }
        }
    }
    else if (terminal && !lit_active_ && counting_ && stats_ != nullptr)
    {
        // HOLD: terminal count, nothing staged.  Keep the current source and
        // keep trying.  First-class line framing, not underrun mercy — but
        // it arises for two unrelated reasons and the checker cares which.
        // If a word is on Q or still in the FIFO the hold is the second
        // slot of the 2-clock fetch, which is structural.  If there is
        // nothing anywhere, the stream really has run dry.  The slot right
        // after a literal (lit_active, pre-edge) is NEITHER: it is the
        // literal's own KEEP slot, part of its two-slot width by law LL, so it
        // is not counted at all.
        if (word_on_q || count_ > 0)
        {
            stats_->vidcmd_cadence_slots++;
        }
        else
        {
            stats_->vidcmd_hold_slots++;
        }
    }
    lit_active_ = load_literal;

    // THE MASK DRIVES cur_src_ FOR ITS OWN SLOT, over anything a load above
    // wrote.  ALIGNMENT, per compositor.v: both halves are read at [13:12],
    // because the shift and the read are the same edge — the header's d1 is
    // already there when the record loads, and after seven shifts d7 is too.
    // d8 has to be read on the very edge that captures the data word, so it
    // comes straight off Q.  Pixel 0 has no dibit anywhere.
    if (load_mask || mask_step)
    {
        uint32_t dibit = MASK_PIXEL0_DIBIT;
        if (!load_mask)
        {
            dibit = mask_reload ? ((q_word_ >> 14) & 3u)
                                : ((staged_word_ >> 12) & 3u);
        }
        cur_src_ = vidcmd_mask_dibit_src(dibit);
    }

    const bool mask_next  = load_mask || (mask_active_ && !terminal);
    const bool mask_holds = mask_next && !mask_pos_7 && !mask_pos_14;
    mask_active_          = mask_next;

    // The shifter and the fetch capture DO collide, on exactly the two
    // borrow-back edges, and the capture must win — so the shift is written
    // first and fetch_edge() below overwrites it.  Both reads above used the
    // pre-edge value, so the collision is benign.
    if (mask_step)
    {
        staged_word_ = static_cast<uint16_t>(staged_word_ << 2);
    }

    // PIXEL advances every active clock regardless of what the compositor is
    // doing, and is sampled AFTER this slot's commit.
    const Rgb444 rgb_in = pix.next_pixel();

    Rgb444 out = rgb_in;
    if (cur_src_ == RUN_SRC_HELD_FG)
    {
        out = held_fg_;
    }
    else if (cur_src_ == RUN_SRC_HELD_BG)
    {
        out = held_bg_;
    }
    else if (cur_src_ == RUN_SRC_KEEP)
    {
        // The literal's own slot reads staged_word[11:0] (still the literal:
        // its consume edge did not free the buffer); every other KEEP slot is
        // RGB_OUT fed back — the last colour shown on this line, black at its
        // first slot.  Neither reads RGB_IN, so KEEP covers a palette write.
        out = lit_active_ ? vidcmd_literal_colour(staged_word_) : rgb_out_;
    }
    else if (pix.pixel_disturbed())
    {
        // A palette write's four-clock window landed on a PASSTHROUGH slot, so
        // the wrong colour (or nothing at all) reaches the DAC.  That is the
        // authoring rule of vidcmd-spec.md §3.2 being broken, and it is a
        // measured hardware behaviour, not a model assertion: clut_pair_tb.v's
        // S6-neg sees exactly this and its S6 sees the legal arrangement pass.
        if (counting_ && stats_ != nullptr)
        {
            stats_->palette_disturbed_visible++;
        }
    }

    // count_inc = load_run | (H_ACTIVE & ~terminal & ~mask_stall).
    if (run_remaining_ > 0 && !mask_stall)
    {
        run_remaining_--;
    }
    if (mask_stall && counting_ && stats_ != nullptr)
    {
        stats_->vidcmd_mask_stalls++;
    }

    fetch_edge(word_on_q, had_staged, consumed, mask_holds, mask_reload, load_literal);
    set_pix_value_ = static_cast<uint32_t>(staged_word_) & 0xFFFu;
    rgb_out_       = out;
    abs_pixel_++;
    return out;
}

void CompositorUnit::end_of_line()
{
    // Nothing re-frames at the boundary: compositor.v freezes run_count while
    // H_ACTIVE is low and resumes on the next line.  An overrun is therefore a
    // diagnostic, not a repair point — it is legal under the JIT discipline and
    // a bug under the cushion one, and only the checker knows which.
    if (run_remaining_ > 0 && counting_ && stats_ != nullptr)
    {
        stats_->vidcmd_overruns++;
    }
}

// ---------------------------------------------------------------------------
// Per-line helpers
// ---------------------------------------------------------------------------

void run_blanking(PixelUnit &pix, CompositorUnit &cmp, bool next_line_visible)
{
    for (uint32_t h = 0; h < H_BLANK; h++)
    {
        // PIX_PRELOAD: one clock, PIXEL_FETCH_LEAD clocks before the first
        // consumption clock of a VISIBLE line, and nowhere else.  Arming it in
        // vertical blanking would read one PIXELS word per blank line that the
        // chip never reads — which is exactly the kind of drift this model now
        // exists to catch, so the caller has to say.
        if (next_line_visible && h == H_BLANK - PIXEL_FETCH_LEAD)
        {
            pix.arm_preload();
        }
        cmp.blank_clock(pix);
    }
}

void render_active_line(PixelUnit &pix, CompositorUnit &cmp, Rgb444 *out)
{
    pix.begin_line();
    for (uint32_t h = 0; h < H_ACTIVE; h++)
    {
        out[h] = cmp.active_slot(pix);
    }
    cmp.end_of_line();
}

// ---------------------------------------------------------------------------
// Drivers
// ---------------------------------------------------------------------------

namespace
{

// The AUDIO chip's own model (audio/audio_chip.h), so the suite measures the
// FIFOs the hardware has.  The stereo stream is channel 0 (left) and channel 2
// (right) at PERIOD 99; channels 1 and 3 are never fed.
struct AudioModel
{
    griffin::audio::AudioChip chip;

    void deposit(uint8_t destination, uint16_t word, RenderResult &r, bool keep)
    {
        if (destination == DEST_AUDCTL_FIFO_W)
        {
            chip.deposit_control(static_cast<uint8_t>(word & 0xFF));
            return;
        }
        const int channel = destination - DEST_SAMPLES0_FIFO_W;
        if (channel < 0 || channel >= griffin::audio::CHANNELS)
        {
            return;
        }
        const bool ok = chip.deposit_sample(channel, audio_channel_byte(channel, word));
        if (keep)
        {
            if (!ok)
            {
                r.audio_overflows++;
            }
            else if (channel == 0)
            {
                r.audio_pairs_deposited++;
            }
        }
    }

    // One step of the period counters (every 16 pixel clocks).
    void step(RenderResult &r, bool keep)
    {
        const uint64_t empty0 = chip.stats.pops_empty[0];
        const uint64_t empty2 = chip.stats.pops_empty[2];
        const uint64_t pops0  = chip.stats.pops[0];
        chip.step();
        if (keep)
        {
            r.audio_underruns += static_cast<uint32_t>((chip.stats.pops_empty[0] - empty0) +
                                                       (chip.stats.pops_empty[2] - empty2));
            r.audio_pairs_consumed += static_cast<uint32_t>((chip.stats.pops[0] - pops0) -
                                                            (chip.stats.pops_empty[0] - empty0));
        }
    }

    // What the two ladders present, once per tick (offset binary, as WAV).
    void sample(RenderResult &r)
    {
        r.audio.push_back(chip.sample(0));
        r.audio.push_back(chip.sample(2));
        const uint32_t count = static_cast<uint32_t>(chip.fill(0));
        if (count > r.audio_fifo_high)
        {
            r.audio_fifo_high = count;
        }
        if (count < r.audio_fifo_low)
        {
            r.audio_fifo_low = count;
        }
    }
};

void finish(RenderResult &r)
{
    if (r.stats.pixels_fifo_low > PIXELS_FIFO_WORDS)
    {
        r.stats.pixels_fifo_low = 0;
    }
    if (r.stats.pixels_line_start_min > PIXELS_FIFO_WORDS)
    {
        r.stats.pixels_line_start_min = 0;
    }
    if (r.stats.vidcmd_fifo_low > VIDCMD_FIFO_WORDS)
    {
        r.stats.vidcmd_fifo_low = 0;
    }
    if (r.audio_fifo_low > AUDIO_FIFO_SAMPLES)
    {
        r.audio_fifo_low = 0;
    }
}

}  // namespace

RenderResult render(const std::vector<DepositEvent> &events, const RenderParams &p)
{
    RenderResult r;
    r.frames.resize(p.frame_count);
    for (FrameImage &f : r.frames)
    {
        f.pixels.assign(static_cast<size_t>(H_ACTIVE) * V_ACTIVE, 0);
    }

    PixelUnit      pix;
    CompositorUnit cmp;
    AudioModel     audio;

    pix.attach_stats(&r.stats);
    cmp.attach_stats(&r.stats);
    cmp.set_skew(p.skew_pix, p.skew_cmp);

    // POWER-ON, before any /RS: the palette RAM comes up with undefined
    // contents and COMPOSITOR is held sane only by the ATF1508's POWER_RESET.
    pix.power_on_reset();
    cmp.power_on_reset();

    size_t         ev           = 0;
    const uint32_t total_frames = p.first_frame + p.frame_count;

    for (uint32_t frame = 0; frame < total_frames; frame++)
    {
        const bool     keep      = (frame >= p.first_frame);
        const uint32_t out_index = keep ? (frame - p.first_frame) : 0;
        pix.set_counting(keep);
        cmp.set_counting(keep);

        for (uint32_t line = 0; line < V_TOTAL; line++)
        {
            const uint64_t abs_line    = static_cast<uint64_t>(frame) * V_TOTAL + line;
            const uint64_t line_pixel0 = abs_line * H_TOTAL;
            // PIX_PRELOAD is armed for the line that FOLLOWS this one.  On the
            // very last line of the run there is no such line — the raster just
            // stops — so arming it would read a PIXELS word for a line nobody
            // ever deposits, which the stats would (rightly) call a tiled word.
            const bool last_of_run   = (frame + 1 == total_frames) && (line + 1 == V_TOTAL);
            const bool next_visible  = !last_of_run && ((line + 1) % V_TOTAL) < V_ACTIVE;

            for (uint32_t h = 0; h < H_TOTAL; h++)
            {
                const uint64_t abs_pixel = line_pixel0 + h;
                const uint64_t cycle     = sysclk_of_pixel(abs_pixel);

                while (ev < events.size() && events[ev].cycle <= cycle)
                {
                    const DepositEvent &e = events[ev];
                    if (e.destination == DEST_PIXELS_FIFO_W)
                    {
                        pix.push_word(e.word);
                    }
                    if (e.destination == DEST_VIDCMD_FIFO_W)
                    {
                        cmp.push_word(e.word);
                        if (keep && line < V_ACTIVE && h < H_ACTIVE)
                        {
                            // Arrived after its line had already started: under
                            // the JIT discipline this is the deadline miss that
                            // makes a packet resume at the wrong x.
                            r.stats.vidcmd_late_words++;
                        }
                    }
                    if (e.destination >= DEST_AUDCTL_FIFO_W)
                    {
                        audio.deposit(e.destination, e.word, r, keep);
                    }
                    ev++;
                }

                // AUDIO's period counters step every 16 pixel clocks, at
                // h_cnt 0 mod 16 (its divider is HBLANK-locked).
                if (p.audio_enabled && (h % 16) == 0)
                {
                    audio.step(r, keep);
                }

                if (h == 0)
                {
                    // TIMING pulses /RS on PIXELS and VIDCMD during vertical
                    // sync: both machines clear, held colours return to
                    // 0xFFF/0x000 and the source returns to passthrough.  The
                    // AUDIO FIFOs are not on /RS and are not part of that reset.
                    if (line == V_SYNC_START)
                    {
                        pix.reset();
                        cmp.reset();
                    }

                    if (p.audio_enabled && keep && (abs_line % AUDIO_LINES_PER_SAMPLE) == 0)
                    {
                        audio.sample(r);
                    }

                    if (line < V_ACTIVE)
                    {
                        cmp.set_pixel_position(abs_pixel);
                        pix.begin_line();
                    }
                }

                if (h == H_ACTIVE && line < V_ACTIVE)
                {
                    cmp.end_of_line();
                }

                // PIX_PRELOAD, at the one h_cnt timing.v emits it on.
                if (h == H_TOTAL - PIXEL_FETCH_LEAD && next_visible)
                {
                    pix.arm_preload();
                }

                if (keep)
                {
                    if (pix.fifo_count() > r.stats.pixels_fifo_high)
                    {
                        r.stats.pixels_fifo_high = pix.fifo_count();
                    }
                    if (cmp.fifo_count() > r.stats.vidcmd_fifo_high)
                    {
                        r.stats.vidcmd_fifo_high = cmp.fifo_count();
                    }
                }

                if (line < V_ACTIVE && h < H_ACTIVE)
                {
                    const Rgb444 out = cmp.active_slot(pix);
                    if (keep)
                    {
                        r.frames[out_index].pixels[line * H_ACTIVE + h] = out;
                        if (pix.fifo_count() < r.stats.pixels_fifo_low)
                        {
                            r.stats.pixels_fifo_low = pix.fifo_count();
                        }
                        if (cmp.fifo_count() < r.stats.vidcmd_fifo_low)
                        {
                            r.stats.vidcmd_fifo_low = cmp.fifo_count();
                        }
                    }
                }
                else
                {
                    cmp.blank_clock(pix);
                }
            }
        }
    }

    finish(r);
    return r;
}

RenderResult render_line_at_a_time(const std::vector<DepositEvent> &events,
                                   const RenderParams &p)
{
    RenderResult r;
    r.frames.resize(p.frame_count);
    for (FrameImage &f : r.frames)
    {
        f.pixels.assign(static_cast<size_t>(H_ACTIVE) * V_ACTIVE, 0);
    }

    PixelUnit      pix;
    CompositorUnit cmp;
    AudioModel     audio;

    pix.attach_stats(&r.stats);
    cmp.attach_stats(&r.stats);
    cmp.set_skew(p.skew_pix, p.skew_cmp);

    pix.power_on_reset();
    cmp.power_on_reset();

    size_t         ev           = 0;
    const uint32_t total_frames = p.first_frame + p.frame_count;

    for (uint32_t frame = 0; frame < total_frames; frame++)
    {
        const bool     keep      = (frame >= p.first_frame);
        const uint32_t out_index = keep ? (frame - p.first_frame) : 0;
        pix.set_counting(keep);
        cmp.set_counting(keep);

        for (uint32_t line = 0; line < V_TOTAL; line++)
        {
            const uint64_t abs_line = static_cast<uint64_t>(frame) * V_TOTAL + line;
            const uint64_t line_end = (abs_line + 1) * H_TOTAL;

            if (line == V_SYNC_START)
            {
                pix.reset();
                cmp.reset();
            }
            if (p.audio_enabled && keep && (abs_line % AUDIO_LINES_PER_SAMPLE) == 0)
            {
                audio.sample(r);
            }

            // The emulator's documented simplification: a line's deposits are
            // lumped at its boundary rather than spread across the bus cycles
            // that carried them.  Lumping is strictly EARLIER than reality, so
            // it cannot manufacture a fill that the cycle-accurate driver would
            // have missed — which is why the two agree on a well-fed list and
            // would diverge on a starved one.
            while (ev < events.size() && events[ev].cycle < sysclk_of_pixel(line_end))
            {
                const DepositEvent &e = events[ev];
                if (e.destination == DEST_PIXELS_FIFO_W)
                {
                    pix.push_word(e.word);
                }
                if (e.destination == DEST_VIDCMD_FIFO_W)
                {
                    cmp.push_word(e.word);
                }
                if (e.destination >= DEST_AUDCTL_FIFO_W)
                {
                    audio.deposit(e.destination, e.word, r, keep);
                }
                ev++;
            }
            if (p.audio_enabled)
            {
                for (uint32_t k = 0; k < H_TOTAL / 16; k++)
                {
                    audio.step(r, keep);
                }
            }

            if (line < V_ACTIVE)
            {
                cmp.set_pixel_position(abs_line * H_TOTAL);
                if (keep)
                {
                    render_active_line(pix, cmp, &r.frames[out_index].pixels[line * H_ACTIVE]);
                }
                else
                {
                    Rgb444 scratch[H_ACTIVE];
                    render_active_line(pix, cmp, scratch);
                }
            }
            // This blanking precedes the NEXT line, so that is the line whose
            // visibility gates PIX_PRELOAD — and on the run's last line there is
            // no next line to preload for.
            const bool last_of_run = (frame + 1 == total_frames) && (line + 1 == V_TOTAL);
            run_blanking(pix, cmp, !last_of_run && ((line + 1) % V_TOTAL) < V_ACTIVE);
        }
    }

    finish(r);
    return r;
}

}  // namespace SuperEngine
