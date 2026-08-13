// render.cpp — PIXEL and COMPOSITOR state machines, plus the suite's two
// drivers over them.
//
// ORDER WITHIN AN ACTIVE PIXEL CLOCK — this is the whole ballgame, and it is
// compositor.v's, not an invention:
//
//   1. retire any skew-delayed register commits whose effect pixel has arrived
//   2. COMPOSITOR consumes this slot's staged word if the run count is terminal;
//      a SET commits here, a RUN loads its source and count here
//   3. sample PIXEL — so a PIXEL-target SET committed in step 2 is already
//      visible in this very slot
//   4. mux the slot's source against PIXEL's colour
//   5. fetch: one word per clock into the lookahead
//
// Hence RUN(passthrough,1), SET(pix_pal_fg,C2), RUN(passthrough,638) over
// all-foreground bits renders pixel 0 as C1 and pixels 1..639 as C2, which is
// compositor_tb's NORMATIVE_M0.

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

void PixelUnit::begin_line()
{
    bitbuf_      = 0;
    bits_avail_  = 0;
    ham_pending_ = 0;
    ham_held_    = pal_fg_;   // held reloads from pal_fg at every line start

    // Hardware clamp (pixel.v, decided 2026-08-13): skip bit 0 is ignored in
    // micro-HAM mode, applied at consumption so SET ordering cannot smuggle a
    // stale odd bit in.  An odd HAM skip would shift every 2-bit code across
    // its boundary and mis-parse the whole line.
    const uint32_t effective_skip =
        (mode_ == PIXEL_MODE_MICRO_HAM ? (pixel_skip_ & ~1u) : pixel_skip_);
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

Rgb444 PixelUnit::next_pixel()
{
    if (mode_ != PIXEL_MODE_MICRO_HAM)
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
        case SET_PIX_MODE:       mode_ = value & 1u; break;
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
    run_remaining_ = 0;                     // count already terminal
    cur_src_       = RUN_SRC_PASSTHROUGH;   // source back to passthrough
    cur_colour_    = RGB444_BLACK;
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

// One word per clock into the single on-chip lookahead.
void CompositorUnit::fetch_clock()
{
    if (staged_valid_)
    {
        return;
    }
    uint16_t w = 0;
    if (pop(w))
    {
        staged_word_  = w;
        staged_valid_ = true;
    }
}

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
// it lands before the next line's pixel 0.  A staged RUN waits for H_ACTIVE and
// blocks everything behind it — eagerness is positional, not temporal.
void CompositorUnit::blank_clock(PixelUnit &pix)
{
    if (staged_valid_ && vidcmd_type_of(staged_word_) == VidcmdType::SET)
    {
        // Skew is unobservable here: nothing is being displayed.  Only the weak
        // property is modelled — the write has landed before pixel 0.
        commit(vidcmd_set_target(staged_word_), vidcmd_set_value(staged_word_), pix);
        staged_valid_ = false;
    }
    fetch_clock();
}

Rgb444 CompositorUnit::active_slot(PixelUnit &pix)
{
    apply_pending(pix);

    // consume_active = H_ACTIVE & have_staged & terminal
    if (run_remaining_ == 0)
    {
        if (staged_valid_)
        {
            const uint16_t     w = staged_word_;
            const VidcmdType   t = vidcmd_type_of(w);
            staged_valid_ = false;

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
            else if (counting_ && stats_ != nullptr)
            {
                // The ex-TILE `01` prefix: consumes this slot, changes nothing.
                stats_->vidcmd_reserved_ops++;
            }
        }
        else if (counting_ && stats_ != nullptr)
        {
            // HOLD: terminal count, nothing staged.  Keep the current source and
            // keep trying.  First-class line framing, not underrun mercy.
            stats_->vidcmd_hold_slots++;
        }
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

    if (run_remaining_ > 0)
    {
        run_remaining_--;
    }

    fetch_clock();
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
