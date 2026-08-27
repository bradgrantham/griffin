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
// PixelUnit
// ---------------------------------------------------------------------------

void PixelUnit::reset()
{
    head_        = 0;
    count_       = 0;
    last_word_   = 0;
    pal_fg_      = RGB444_WHITE;
    pal_bg_      = RGB444_BLACK;
    ham_held_    = RGB444_WHITE;
    mode_        = PIXEL_MODE_DIRECT_1BPP;
    pixel_skip_  = 0;
    bitbuf_      = 0;
    bits_avail_  = 0;
    ham_pending_ = 0;
    ham_type_    = 0;
    half_phase_  = 0;
    idx_code_    = 0;
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
void PixelUnit::refill(uint32_t need)
{
    while (bits_avail_ < need)
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
        bitbuf_ = (bitbuf_ << 16) | w;
        bits_avail_ += 16;
    }
}

uint32_t PixelUnit::peek(uint32_t n) const
{
    return static_cast<uint32_t>((bitbuf_ >> (bits_avail_ - n)) & ((1u << n) - 1u));
}

uint32_t PixelUnit::take(uint32_t n)
{
    const uint32_t v = peek(n);
    bits_avail_ -= n;
    bitbuf_ &= (bits_avail_ == 0) ? 0ull : ((1ull << bits_avail_) - 1ull);
    return v;
}

// held_init in pixel.v is `~pix_consume & ~mode_idx2`, i.e. a write that
// happens on EVERY blank clock, not once at line start.  Modelling it per clock
// is what makes the SET ORDERING RULE real here: in 1bpp and micro-HAM held
// tracks pal_fg through blanking so every visible line begins at fg with no
// cross-line state, while in indexed mode BOTH writers drop out and ham_held —
// that mode's third palette entry — survives from its SET to the end of the
// frame.  SET ham_held before SET mode and these clocks are what eat it.
void PixelUnit::blank_clock()
{
    half_phase_ = 0;   // preload clears the pairing phase; every line pairs alike

    if (!mode_idx2())
    {
        ham_held_ = pal_fg_;
    }
}

void PixelUnit::begin_line()
{
    bitbuf_      = 0;
    bits_avail_  = 0;
    ham_pending_ = 0;
    half_phase_  = 0;

    if (!mode_idx2())
    {
        ham_held_ = pal_fg_;   // held reloads from pal_fg at every line start
    }

    // Hardware clamp (pixel.v, decided 2026-08-13, inherited by the indexed
    // mode 2026-08-24): skip bit 0 is ignored in BOTH two-bit modes, applied at
    // consumption so SET ordering cannot smuggle a stale odd bit in.  An odd
    // two-bit skip would shift every code across its boundary and mis-parse the
    // whole line.  Half rate needs no extra clamp — skip is in stream bits and
    // a stream bit is still exactly one group.
    const uint32_t effective_skip = pixels_skip_effective(mode_, pixel_skip_);
    if (effective_skip > 0)
    {
        refill(effective_skip);
        (void)take(effective_skip);
    }

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

Rgb444 PixelUnit::idx_colour() const
{
    switch (idx_code_)
    {
        case IDX2_CODE_PAL_BG:   return pal_bg_;
        case IDX2_CODE_PAL_FG:   return pal_fg_;
        case IDX2_CODE_HAM_HELD: return ham_held_;
        default:                 return RGB444_BLACK;   // code 11 is a constant
    }
}

Rgb444 PixelUnit::next_pixel()
{
    // HALF RATE, pixel.v's one gate rather than a second cadence.  half_phase
    // splits the 640 consumption clocks into pairs and `step` is the FIRST
    // clock of each pair, which is what keeps PIXEL_OUT_LEAD at 1: pixel 0
    // still reaches RGB_OUT on the clock it would at full rate.  A held clock
    // re-reads the output mux and consumes nothing, so a line costs half the
    // stream at half the horizontal resolution.  In micro-HAM half_rate is
    // masked off in hardware, so `hold` is false there by construction.
    const bool hold = half_rate() && half_phase_ != 0;
    half_phase_ ^= 1u;

    if (hold)
    {
        // What the output register would show on the second clock of a group.
        // Indexed re-evaluates the mux, because idx_colour is combinational off
        // the three colour registers; 1bpp shows the registered ham_held, which
        // the stream is not writing this clock.
        return mode_idx2() ? idx_colour() : ham_held_;
    }

    // 2BPP INDEXED.  Two stream bits per clock like micro-HAM — same stream
    // rate, same fetch cadence, same odd-skip clamp — but the dibit is a DIRECT
    // palette index with no arithmetic and no multi-clock codes.  ham_held is
    // NOT written here: it is this mode's third palette entry and SET is its
    // only writer (pixel.v's load_pal and held_init both drop out), which is
    // exactly what lets code 10 show the SET-loaded value.
    //
    // UNDERRUN: an exhausted FIFO re-shifts the last word.  From reset that
    // word is zero, so every dibit reads 00 and the whole line renders pal_bg —
    // the same substrate the pure-VIDCMD screens spend as a settable third
    // colour, and finding 22's caveat applies here unchanged.
    if (mode_idx2())
    {
        refill(2);
        idx_code_ = take(2);
        return idx_colour();
    }

    if (!mode_ham())
    {
        refill(1);
        ham_held_ = take(1) != 0 ? pal_fg_ : pal_bg_;
        return ham_held_;
    }

    // Exactly two bits leave the stream every clock.
    //
    // PAIR ORDERING, per pixel.v: the prefix pair (1x) and the chroma pair
    // (g,r / g,b) land in different clocks, so the decoder cannot see g until
    // the second.  The first pixel of a 4-bit code therefore shows the OLD held
    // colour and the second shows both channels updated together.  video.v's
    // serial decoder staggered the channels one pixel apart; that behaviour is
    // not reproducible here and this matches the chip that exists.
    if (ham_pending_ != 0)
    {
        ham_pending_ = 0;
        (void)take(2);
        if (ham_type_ == 0)
        {
            ham_held_ = rgb444(ham_v_, ham_g_, rgb444_b(ham_held_));   // 10_g_r
        }
        else
        {
            ham_held_ = rgb444(rgb444_r(ham_held_), ham_g_, ham_v_);   // 11_g_b
        }
        return ham_held_;
    }

    refill(1);
    if (peek(1) == 0)
    {
        refill(2);
        const uint32_t code = take(2);
        ham_held_ = ((code & 1u) != 0) ? pal_fg_ : pal_bg_;
        return ham_held_;
    }

    refill(4);
    const uint32_t code = peek(4);   // look ahead at the whole code...
    (void)take(2);                   // ...but only eat this clock's two bits
    ham_type_    = (code >> 2) & 1u;
    ham_g_       = ((code >> 1) & 1u) != 0 ? 0xFu : 0x0u;
    ham_v_       = (code & 1u) != 0 ? 0xFu : 0x0u;
    ham_pending_ = 1;
    return ham_held_;   // the OLD held colour, per pixel.v
}

void PixelUnit::set_register(uint32_t target, uint32_t value)
{
    switch (target)
    {
        case SET_PIX_PAL_FG:     pal_fg_ = static_cast<Rgb444>(value); break;
        case SET_PIX_PAL_BG:     pal_bg_ = static_cast<Rgb444>(value); break;
        case SET_PIX_HAM_HELD:   ham_held_ = static_cast<Rgb444>(value); break;
        case SET_PIX_MODE:       mode_ = value & PIXEL_MODE_BITS; break;
        case SET_PIX_PIXEL_SKIP: pixel_skip_ = value & 0xF; break;
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
    re_low_        = false;                 // /RE idles high
    ef_at_pop_     = false;
    q_word_        = 0;
    ef_meta_       = false;
    ef_sync_       = false;
    run_remaining_ = 0;                     // count already terminal
    cur_src_       = RUN_SRC_PASSTHROUGH;   // source back to passthrough
    cur_colour_    = RGB444_BLACK;
    mask_active_   = false;                 // mask playback abandoned
    sav_src_       = RUN_SRC_PASSTHROUGH;
    pending_n_     = 0;
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
//
// The two laws the suite has to reproduce come straight out of these five
// lines: a word captured here cannot execute before the NEXT slot (2 slots per
// word sustained), and a word PARKED on Q moves into staged_word on the very
// edge the record ahead of it is consumed, so a banked pair executes on
// consecutive slots.  The mask's two borrow-back edges are the third: they punch
// through mask_holds, which is what makes mask-to-mask chaining gapless.
void CompositorUnit::fetch_edge(bool word_on_q, bool had_staged, bool consumed,
                                bool mask_holds, bool mask_reload)
{
    const bool buffer_frees = (!had_staged || consumed) && !mask_holds;
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
// temporal.
void CompositorUnit::blank_clock(PixelUnit &pix)
{
    const bool word_on_q  = re_low_ && ef_at_pop_;
    const bool had_staged = staged_valid_;
    bool       consumed   = false;

    // PIXEL's own blank-clock behaviour: held_init reloads ham_held from pal_fg
    // on every clock PIX_CONSUME is low, unless the mode register reads
    // indexed.  Running it BEFORE this clock's SET commit is what gives the
    // ordering rule its teeth — a SET(ham_held) that arrives while mode is
    // still 1bpp is overwritten on the very next blank clock.
    pix.blank_clock();

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

    // Blanking does not exempt the fetch from the cadence: a run of eager SETs
    // still executes at one per two clocks (a pair when one is parked).  With
    // H_BLANK = 160 clocks that is invisible to a cushioned list and exactly
    // what a starved one runs out of.
    fetch_edge(word_on_q, had_staged, consumed, mask_holds, false);
}

Rgb444 CompositorUnit::active_slot(PixelUnit &pix)
{
    apply_pending(pix);

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

    // consume_active = H_ACTIVE & have_staged & terminal
    if (terminal)
    {
        if (staged_valid_)
        {
            const uint16_t     w = staged_word_;
            const VidcmdType   t = vidcmd_type_of(w);
            staged_valid_ = false;
            consumed      = true;

            if (t == VidcmdType::SET)
            {
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
                cur_src_ = vidcmd_run_src(w);
                if (cur_src_ == RUN_SRC_COLOR)
                {
                    cur_colour_ = run_colour_to_rgb444(vidcmd_run_color_code(w));
                    if (counting_ && stats_ != nullptr)
                    {
                        stats_->vidcmd_color_runs++;
                    }
                }
                run_remaining_ = vidcmd_record_slots(w);
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
        else if (counting_ && stats_ != nullptr)
        {
            // HOLD: terminal count, nothing staged.  Keep the current source and
            // keep trying.  First-class line framing, not underrun mercy — but
            // it arises for two unrelated reasons and the checker cares which.
            // If a word is on Q or still in the FIFO the hold is the second
            // slot of the 2-clock fetch, which is structural.  If there is
            // nothing anywhere, the stream really has run dry.
            if (word_on_q || count_ > 0)
            {
                stats_->vidcmd_cadence_slots++;
            }
            else
            {
                stats_->vidcmd_hold_slots++;
            }
        }
    }

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
    else if (cur_src_ == RUN_SRC_COLOR)
    {
        out = cur_colour_;
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

    fetch_edge(word_on_q, had_staged, consumed, mask_holds, mask_reload);
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

void run_blanking(PixelUnit &pix, CompositorUnit &cmp)
{
    for (uint32_t h = 0; h < H_BLANK; h++)
    {
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

struct AudioModel
{
    std::vector<uint16_t> fifo = std::vector<uint16_t>(AUDIO_FIFO_PAIRS, 0);
    uint32_t head   = 0;
    uint32_t count  = 0;
    uint8_t  last_l = 0x80;
    uint8_t  last_r = 0x80;

    bool push(uint16_t w)
    {
        if (count >= fifo.size())
        {
            return false;
        }
        fifo[(head + count) % fifo.size()] = w;
        count++;
        return true;
    }

    bool pop(uint16_t &w)
    {
        if (count == 0)
        {
            return false;
        }
        w = fifo[head];
        head = (head + 1) % static_cast<uint32_t>(fifo.size());
        count--;
        return true;
    }

    void tick(RenderResult &r)
    {
        uint16_t w = 0;
        if (pop(w))
        {
            last_l = audio_pair_left(w);
            last_r = audio_pair_right(w);
            r.audio_pairs_consumed++;
        }
        else
        {
            r.audio_underruns++;   // hardware holds the last sample
        }
        r.audio.push_back(last_l);
        r.audio.push_back(last_r);
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
    if (r.audio_fifo_low > AUDIO_FIFO_PAIRS)
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

            for (uint32_t h = 0; h < H_TOTAL; h++)
            {
                const uint64_t abs_pixel = line_pixel0 + h;
                const uint64_t cycle     = sysclk_of_pixel(abs_pixel);

                while (ev < events.size() && events[ev].cycle <= cycle)
                {
                    const DepositEvent &e = events[ev];
                    if ((e.signal_mask & SIGNAL_PIXELS_FIFO_W) != 0)
                    {
                        pix.push_word(e.word);
                    }
                    if ((e.signal_mask & SIGNAL_VIDCMD_FIFO_W) != 0)
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
                    if ((e.signal_mask & SIGNAL_AUDIO_FIFO_W) != 0)
                    {
                        if (!audio.push(e.word))
                        {
                            if (keep)
                            {
                                r.audio_overflows++;
                            }
                        }
                        else if (keep)
                        {
                            r.audio_pairs_deposited++;
                        }
                    }
                    ev++;
                }

                if (h == 0)
                {
                    // TIMING pulses /RS on PIXELS and VIDCMD during vertical
                    // sync: both machines clear, held colours return to
                    // 0xFFF/0x000 and the source returns to passthrough.  The
                    // audio FIFO lives in PORTS and is not part of that reset.
                    if (line == V_SYNC_START)
                    {
                        pix.reset();
                        cmp.reset();
                    }

                    if (p.audio_enabled && keep && (abs_line % AUDIO_LINES_PER_SAMPLE) == 0)
                    {
                        audio.tick(r);
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
                audio.tick(r);
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
                if ((e.signal_mask & SIGNAL_PIXELS_FIFO_W) != 0)
                {
                    pix.push_word(e.word);
                }
                if ((e.signal_mask & SIGNAL_VIDCMD_FIFO_W) != 0)
                {
                    cmp.push_word(e.word);
                }
                if ((e.signal_mask & SIGNAL_AUDIO_FIFO_W) != 0)
                {
                    if (!audio.push(e.word))
                    {
                        if (keep)
                        {
                            r.audio_overflows++;
                        }
                    }
                    else if (keep)
                    {
                        r.audio_pairs_deposited++;
                    }
                }
                ev++;
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
            run_blanking(pix, cmp);
        }
    }

    finish(r);
    return r;
}

}  // namespace SuperEngine
