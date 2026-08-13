// render.cpp — PIXEL, COMPOSITOR and AUDIO models driven by the engine's
// deposit events.
//
// One pass over the raster at pixel-clock granularity.  Every pixel clock:
// apply any deposits whose strobe has landed by that pixel's SYSCLK time, run
// the COMPOSITOR slot for that clock (which may commit a SET), sample PIXEL,
// then combine.  Doing it in pixel order rather than event order is what makes
// a deposit that arrives mid-line show up mid-line.
//
// ORDER WITHIN AN ACTIVE PIXEL CLOCK — this is the whole ballgame, because it
// is what makes the normative slot regression come out right:
//
//   1. retire any skew-delayed register commits whose effect pixel has arrived
//   2. COMPOSITOR takes this slot's instruction; if it is a SET, commit it now
//      (a SET "commits on the clock edge that BEGINS its slot")
//   3. sample PIXEL — so a PIXEL-target SET committed in step 2 with zero skew
//      is already visible in this very slot
//   4. resolve the slot's output source against PIXEL's colour
//
// Hence RUN(passthrough,1), SET(pix_pal_fg,C2), RUN(passthrough,638) over
// all-foreground bits renders pixel 0 as C1 and pixels 1..639 as C2: the SET's
// own slot is pixel 1, and it shows passthrough under the *new* palette.

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

namespace
{

// A plain ring buffer standing in for an IDT7200L pair.  Push fails on full
// (the real part just drops the write and flags), pop fails on empty.
struct Fifo
{
    std::vector<uint16_t> buf;
    uint32_t head  = 0;
    uint32_t count = 0;

    explicit Fifo(uint32_t depth) : buf(depth, 0) {}

    void reset()
    {
        head  = 0;
        count = 0;
    }

    bool push(uint16_t v)
    {
        if (count >= buf.size())
        {
            return false;
        }
        buf[(head + count) % buf.size()] = v;
        count++;
        return true;
    }

    bool pop(uint16_t &v)
    {
        if (count == 0)
        {
            return false;
        }
        v = buf[head];
        head = (head + 1) % static_cast<uint32_t>(buf.size());
        count--;
        return true;
    }
};

// ---------------------------------------------------------------------------
// PIXEL
// ---------------------------------------------------------------------------
//
// Pure pixel bits in, 12-bit RGB out.  Two modes share one bit-serial front
// end; the only difference is how many bits a pixel clock eats and what the
// bits mean.
//
//   direct 1bpp   1 bit  per clock: bit selects pix_pal_fg / pix_pal_bg
//   micro-HAM     2 bits per clock, codes parsed out of that stream:
//                   0 p      1 pixel,  held <- p ? pal_fg : pal_bg
//                   1 0 g r  2 pixels, held.green <- g*0xF, held.red  <- r*0xF
//                   1 1 g b  2 pixels, held.green <- g*0xF, held.blue <- b*0xF
//
// pixel_skip is consumed once, at line start: the leading `skip` bits of the
// line are thrown away.  That is why a line with a nonzero skip needs one more
// word than a line without.
struct Pixel
{
    Fifo     fifo{PIXELS_FIFO_WORDS};
    Rgb444   pal_fg     = RGB444_WHITE;
    Rgb444   pal_bg     = RGB444_BLACK;
    Rgb444   ham_held   = RGB444_WHITE;
    uint32_t mode       = PIXEL_MODE_DIRECT_1BPP;
    uint32_t pixel_skip = 0;

    uint64_t bitbuf      = 0;
    uint32_t bits_avail  = 0;
    uint32_t ham_pending = 0;   // pixels still owed by the current 4-bit code

    void refill(uint32_t need, RenderResult &r, bool counting)
    {
        while (bits_avail < need)
        {
            uint16_t w = 0;
            if (!fifo.pop(w))
            {
                if (counting)
                {
                    r.pixels_underruns++;
                }
                w = 0;   // pretend a zero word arrived rather than spinning
            }
            bitbuf = (bitbuf << 16) | w;
            bits_avail += 16;
        }
    }

    uint32_t peek(uint32_t n) const
    {
        return static_cast<uint32_t>((bitbuf >> (bits_avail - n)) & ((1u << n) - 1u));
    }

    uint32_t take(uint32_t n)
    {
        const uint32_t v = peek(n);
        bits_avail -= n;
        bitbuf &= (bits_avail == 0) ? 0ull : ((1ull << bits_avail) - 1ull);
        return v;
    }

    void line_start(RenderResult &r, bool counting)
    {
        bitbuf      = 0;
        bits_avail  = 0;
        ham_pending = 0;
        ham_held    = pal_fg;   // micro-HAM: held <- pix_pal_fg at every line start

        if (pixel_skip > 0)
        {
            refill(pixel_skip, r, counting);
            (void)take(pixel_skip);
        }

        if (counting)
        {
            if (fifo.count < r.pixels_line_start_min)
            {
                r.pixels_line_start_min = fifo.count;
            }
            if (fifo.count > r.pixels_line_start_max)
            {
                r.pixels_line_start_max = fifo.count;
            }
        }
    }

    Rgb444 pixel(RenderResult &r, bool counting)
    {
        if (mode != PIXEL_MODE_MICRO_HAM)
        {
            refill(1, r, counting);
            return take(1) != 0 ? pal_fg : pal_bg;
        }

        // Exactly two bits leave the stream every clock, including the second
        // clock of a 4-bit code, whose remaining two bits are consumed here.
        if (ham_pending > 0)
        {
            ham_pending--;
            refill(2, r, counting);
            (void)take(2);
            return ham_held;
        }

        refill(1, r, counting);
        if (peek(1) == 0)
        {
            refill(2, r, counting);
            const uint32_t code = take(2);
            ham_held = ((code & 1u) != 0) ? pal_fg : pal_bg;
            return ham_held;
        }

        refill(4, r, counting);
        const uint32_t code = peek(4);      // look ahead at the whole code...
        (void)take(2);                      // ...but only eat this clock's two bits
        const uint32_t chroma_sel = (code >> 2) & 1u;
        const uint32_t g          = ((code >> 1) & 1u) != 0 ? 0xFu : 0x0u;
        const uint32_t v          = (code & 1u) != 0 ? 0xFu : 0x0u;
        if (chroma_sel == 0)
        {
            ham_held = rgb444(v, g, rgb444_b(ham_held));            // 10_g_r
        }
        else
        {
            ham_held = rgb444(rgb444_r(ham_held), g, v);            // 11_g_b
        }
        ham_pending = 1;
        return ham_held;
    }
};

// ---------------------------------------------------------------------------
// COMPOSITOR
// ---------------------------------------------------------------------------

enum class OutputSource : uint8_t
{
    PASSTHROUGH = 0,
    HELD_FG     = 1,
    HELD_BG     = 2,
    RUN_COLOUR  = 3,   // the run's own latched colour, not a held register
};

// A register write in flight across the chip boundary.  With both skew
// constants at zero this list never holds more than the one entry that is
// retired in the same clock it was queued; it exists so that when RTL
// simulation pins a real skew, the compensation in author.cpp is exercised
// rather than assumed.
struct PendingCommit
{
    uint64_t effect_pixel = 0;
    uint32_t target       = 0;
    uint32_t value        = 0;
};

struct Compositor
{
    Fifo   fifo{VIDCMD_FIFO_WORDS};
    Rgb444 held_fg = VIDCMD_RESET_HELD_FG;
    Rgb444 held_bg = VIDCMD_RESET_HELD_BG;

    uint16_t collect[VIDCMD_TILE_WORDS] = {};
    uint32_t collect_n                  = 0;

    bool     stage_valid                = false;
    uint16_t stage[VIDCMD_TILE_WORDS]   = {};

    bool         play_active    = false;
    VidcmdType   play_type      = VidcmdType::RUN;
    uint32_t     play_remaining = 0;
    uint32_t     play_src       = RUN_SRC_PASSTHROUGH;
    uint16_t     play_select    = 0;
    uint16_t     play_mask      = 0;

    // RUN_COLOR's colour has to live in the PLAYBACK registers, not in staging:
    // the fetch machine overwrites staging with the next record while this run
    // is still playing.  See FIT-RISKY ASSUMPTION 5 in descriptor.h — these are
    // three flip-flops the compositor does not currently have to spare.
    Rgb444 play_colour = RGB444_BLACK;

    // "Hold the current output mode" is the defined behaviour both for a SET's
    // own slot and for an exhausted stream, so the last resolved source has to
    // survive between records.
    OutputSource last_source = OutputSource::PASSTHROUGH;

    // Prefetch invariant (spec 5): playback_duration(entry k) must cover
    // word_count(entry k+1); HBLANK credits the line's first entry.
    uint32_t play_duration    = H_BLANK;
    uint32_t words_since_play = 0;

    static constexpr uint32_t PENDING_MAX = 16;
    PendingCommit pending[PENDING_MAX] = {};
    uint32_t      pending_n            = 0;

    void reset()
    {
        fifo.reset();
        held_fg          = VIDCMD_RESET_HELD_FG;
        held_bg          = VIDCMD_RESET_HELD_BG;
        collect_n        = 0;
        stage_valid      = false;
        play_active      = false;
        play_remaining   = 0;
        last_source      = OutputSource::PASSTHROUGH;
        play_duration    = H_BLANK;
        words_since_play = 0;
        pending_n        = 0;
    }

    void credit_hblank()
    {
        play_duration    = H_BLANK;
        words_since_play = 0;
    }

    // Fetch machine: one word per clock whenever staging is free, collecting a
    // complete instruction (1 word, or 3 for a TILE) before staging it.  Unlike
    // the old overlay format, a SET occupies staging too — it has to wait for
    // its slot when it lands in active video.
    void fetch_clock(RenderResult &r, bool counting)
    {
        if (stage_valid)
        {
            return;
        }
        uint16_t w = 0;
        if (!fifo.pop(w))
        {
            return;
        }
        words_since_play++;
        collect[collect_n] = w;
        collect_n++;

        if (vidcmd_type_of(collect[0]) == VidcmdType::TILE && collect_n < VIDCMD_TILE_WORDS)
        {
            return;
        }

        for (uint32_t i = 0; i < collect_n; i++)
        {
            stage[i] = collect[i];
        }
        collect_n   = 0;
        stage_valid = true;

        if (counting && words_since_play > play_duration)
        {
            r.vidcmd_pacing_violations++;
        }
    }

    void queue_commit(uint32_t target, uint32_t value, uint64_t effect_pixel, RenderResult &r,
                      bool counting)
    {
        if (pending_n >= PENDING_MAX)
        {
            if (counting)
            {
                r.vidcmd_pending_overflow++;
            }
            return;
        }
        pending[pending_n].effect_pixel = effect_pixel;
        pending[pending_n].target       = target;
        pending[pending_n].value        = value;
        pending_n++;
    }

    void apply_pending(uint64_t now_pixel, Pixel &pix)
    {
        uint32_t kept = 0;
        for (uint32_t i = 0; i < pending_n; i++)
        {
            if (pending[i].effect_pixel <= now_pixel)
            {
                commit(pending[i].target, pending[i].value, pix);
            }
            else
            {
                pending[kept] = pending[i];
                kept++;
            }
        }
        pending_n = kept;
    }

    void commit(uint32_t target, uint32_t value, Pixel &pix)
    {
        switch (target)
        {
            case SET_CMP_HELD_FG:    held_fg = static_cast<Rgb444>(value); break;
            case SET_CMP_HELD_BG:    held_bg = static_cast<Rgb444>(value); break;
            case SET_PIX_PAL_FG:     pix.pal_fg = static_cast<Rgb444>(value); break;
            case SET_PIX_PAL_BG:     pix.pal_bg = static_cast<Rgb444>(value); break;
            case SET_PIX_HAM_HELD:   pix.ham_held = static_cast<Rgb444>(value); break;
            case SET_PIX_MODE:       pix.mode = value & 1u; break;
            case SET_PIX_PIXEL_SKIP: pix.pixel_skip = value & 0xF; break;
            default:                 break;   // SET_SPARE_7: no effect by design
        }
    }

    // Everything outside active video: playback idles, the fetch machine keeps
    // running, and a staged SET commits immediately at zero slot cost.  A
    // staged RUN/TILE just sits there until H_ACTIVE.
    void blank_clock(Pixel &pix, uint32_t skew_pix, uint32_t skew_cmp, RenderResult &r,
                     bool counting)
    {
        (void)skew_pix;
        (void)skew_cmp;
        if (!stage_valid)
        {
            return;
        }
        if (vidcmd_type_of(stage[0]) != VidcmdType::SET)
        {
            return;
        }
        // Skew is unobservable here — nothing is being displayed — so only the
        // weak property is modelled: the write has landed before pixel 0.
        commit(vidcmd_set_target(stage[0]), vidcmd_set_value(stage[0]), pix);
        stage_valid = false;
        (void)r;
        (void)counting;
    }

    // One active-video slot.  Returns where this pixel's colour comes from,
    // having already committed any SET that owns the slot.
    OutputSource begin_slot(uint64_t abs_pixel, Pixel &pix, uint32_t skew_pix,
                            uint32_t skew_cmp, RenderResult &r, bool counting)
    {
        apply_pending(abs_pixel, pix);

        if (!play_active)
        {
            if (!stage_valid)
            {
                // Exhausted stream: hold the current output mode.
                if (counting)
                {
                    r.vidcmd_underruns++;
                }
                return last_source;
            }

            const uint16_t lead = stage[0];
            const VidcmdType t  = vidcmd_type_of(lead);

            if (t == VidcmdType::SET)
            {
                const uint32_t target = vidcmd_set_target(lead);
                const uint32_t skew   = vidcmd_set_skew(target, skew_pix, skew_cmp);
                queue_commit(target, vidcmd_set_value(lead), abs_pixel + skew, r, counting);
                apply_pending(abs_pixel, pix);
                stage_valid = false;
                // The SET's slot continues the previous mode under the new
                // state; it does not reset the prefetch accounting because it
                // is a one-slot entry that has already been consumed.
                play_duration    = 1;
                words_since_play = 0;
                return last_source;
            }

            play_type      = t;
            play_active    = true;
            play_select    = stage[1];
            play_mask      = stage[2];
            play_remaining = vidcmd_record_slots(lead);
            play_src       = (t == VidcmdType::RUN) ? vidcmd_run_src(lead) : RUN_SRC_PASSTHROUGH;
            stage_valid    = false;

            play_duration    = play_remaining;
            words_since_play = 0;

            if (t == VidcmdType::RUN && play_src == RUN_SRC_COLOR)
            {
                play_colour = run_colour_to_rgb444(vidcmd_run_color_code(lead));
                if (counting)
                {
                    r.vidcmd_color_runs++;
                }
            }
        }

        OutputSource src = OutputSource::PASSTHROUGH;
        if (play_type == VidcmdType::RUN)
        {
            if (play_src == RUN_SRC_HELD_FG)
            {
                src = OutputSource::HELD_FG;
            }
            else if (play_src == RUN_SRC_HELD_BG)
            {
                src = OutputSource::HELD_BG;
            }
            else if (play_src == RUN_SRC_COLOR)
            {
                src = OutputSource::RUN_COLOUR;
            }
        }
        else if (play_remaining <= VIDCMD_TILE_PIXELS)
        {
            const uint32_t bit    = VIDCMD_TILE_PIXELS - play_remaining;
            const bool     opaque = ((play_mask >> (15 - bit)) & 1) != 0;
            const bool     is_fg  = ((play_select >> (15 - bit)) & 1) != 0;
            if (opaque)
            {
                src = is_fg ? OutputSource::HELD_FG : OutputSource::HELD_BG;
            }
        }

        play_remaining--;
        if (play_remaining == 0)
        {
            play_active = false;
        }

        last_source = src;
        return src;
    }

    void end_of_line(RenderResult &r, bool counting)
    {
        // Framing is duration arithmetic: a line whose records do not total
        // exactly H_ACTIVE active slots leaves a record straddling the h=640
        // boundary.  Nothing re-frames it — the stream stays skewed until
        // vsync's /RS — so the error is deliberately NOT repaired here.
        if (play_active && play_remaining > 0 && counting)
        {
            r.vidcmd_straddles++;
        }
    }
};

}  // namespace

RenderResult render(const std::vector<DepositEvent> &events, const RenderParams &p)
{
    RenderResult r;
    r.frames.resize(p.frame_count);
    for (FrameImage &f : r.frames)
    {
        f.pixels.assign(static_cast<size_t>(H_ACTIVE) * V_ACTIVE, 0);
    }

    Pixel      pix;
    Compositor comp;
    Fifo       audio_fifo{AUDIO_FIFO_PAIRS};
    uint8_t    audio_last_l = 0x80;
    uint8_t    audio_last_r = 0x80;

    size_t         ev           = 0;
    const uint32_t total_frames = p.first_frame + p.frame_count;

    for (uint32_t frame = 0; frame < total_frames; frame++)
    {
        const bool     keep      = (frame >= p.first_frame);
        const uint32_t out_index = keep ? (frame - p.first_frame) : 0;

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
                        if (!pix.fifo.push(e.word) && keep)
                        {
                            r.pixels_overflows++;
                        }
                    }
                    if ((e.signal_mask & SIGNAL_VIDCMD_FIFO_W) != 0)
                    {
                        if (!comp.fifo.push(e.word) && keep)
                        {
                            r.vidcmd_overflows++;
                        }
                    }
                    if ((e.signal_mask & SIGNAL_AUDIO_FIFO_W) != 0)
                    {
                        if (!audio_fifo.push(e.word))
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
                    // sync, so both streams re-frame every frame, all machine
                    // state clears and the held colours return to their
                    // defaults.  This is also what contains a slot-arithmetic
                    // desync to the frame it started in.  The audio FIFO lives
                    // in PORTS and is not part of that reset.
                    if (line == V_SYNC_START)
                    {
                        pix.fifo.reset();
                        pix.bits_avail  = 0;
                        pix.ham_pending = 0;
                        comp.reset();
                    }

                    // PORTS pops one stereo pair every second LINE_STROBE.  The
                    // /2 phase is free-running across frames, and V_TOTAL is
                    // odd, so a frame alternately consumes 263 and 262 pairs.
                    if (p.audio_enabled && keep && (abs_line % AUDIO_LINES_PER_SAMPLE) == 0)
                    {
                        uint16_t w = 0;
                        if (audio_fifo.pop(w))
                        {
                            audio_last_l = audio_pair_left(w);
                            audio_last_r = audio_pair_right(w);
                            r.audio_pairs_consumed++;
                        }
                        else
                        {
                            r.audio_underruns++;   // hardware holds the last sample
                        }
                        r.audio.push_back(audio_last_l);
                        r.audio.push_back(audio_last_r);

                        if (audio_fifo.count > r.audio_fifo_high)
                        {
                            r.audio_fifo_high = audio_fifo.count;
                        }
                        if (audio_fifo.count < r.audio_fifo_low)
                        {
                            r.audio_fifo_low = audio_fifo.count;
                        }
                    }

                    if (line < V_ACTIVE)
                    {
                        pix.line_start(r, keep);
                    }
                }

                if (h == H_ACTIVE)
                {
                    if (line < V_ACTIVE)
                    {
                        comp.end_of_line(r, keep);
                    }
                    comp.credit_hblank();
                }

                if (keep)
                {
                    if (pix.fifo.count > r.pixels_fifo_high)
                    {
                        r.pixels_fifo_high = pix.fifo.count;
                    }
                    if (comp.fifo.count > r.vidcmd_fifo_high)
                    {
                        r.vidcmd_fifo_high = comp.fifo.count;
                    }
                }

                if (line < V_ACTIVE && h < H_ACTIVE)
                {
                    const OutputSource src =
                        comp.begin_slot(abs_pixel, pix, p.skew_pix, p.skew_cmp, r, keep);
                    const Rgb444 rgb_in = pix.pixel(r, keep);

                    Rgb444 out = rgb_in;
                    if (src == OutputSource::HELD_FG)
                    {
                        out = comp.held_fg;
                    }
                    else if (src == OutputSource::HELD_BG)
                    {
                        out = comp.held_bg;
                    }
                    else if (src == OutputSource::RUN_COLOUR)
                    {
                        out = comp.play_colour;
                    }

                    if (keep)
                    {
                        r.frames[out_index].pixels[line * H_ACTIVE + h] = out;

                        // Low water is only meaningful where running dry hurts:
                        // inside active video, with the drain running.
                        if (pix.fifo.count < r.pixels_fifo_low)
                        {
                            r.pixels_fifo_low = pix.fifo.count;
                        }
                        if (comp.fifo.count < r.vidcmd_fifo_low)
                        {
                            r.vidcmd_fifo_low = comp.fifo.count;
                        }
                    }
                }
                else
                {
                    comp.blank_clock(pix, p.skew_pix, p.skew_cmp, r, keep);
                }

                comp.fetch_clock(r, keep);
            }
        }
    }

    if (r.pixels_fifo_low > PIXELS_FIFO_WORDS)
    {
        r.pixels_fifo_low = 0;
    }
    if (r.pixels_line_start_min > PIXELS_FIFO_WORDS)
    {
        r.pixels_line_start_min = 0;
    }
    if (r.vidcmd_fifo_low > VIDCMD_FIFO_WORDS)
    {
        r.vidcmd_fifo_low = 0;
    }
    if (r.audio_fifo_low > AUDIO_FIFO_PAIRS)
    {
        r.audio_fifo_low = 0;
    }

    return r;
}

}  // namespace SuperEngine
