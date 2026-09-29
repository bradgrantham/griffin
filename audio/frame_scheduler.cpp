// frame_scheduler.cpp — see frame_scheduler.h.  Bare metal.

#include "frame_scheduler.h"

namespace griffin::audio
{

// ---------------------------------------------------------------------------
// The ring pool: static, shared by every FrameScheduler, never freed.  A ring
// is keyed by (table pointer, length), which is the identity psg_channels.h
// hands over, and holds the table expanded into WORDS with the sample in the
// low byte so an ENGINE descriptor can name it.
// ---------------------------------------------------------------------------

namespace
{

struct RingEntry
{
    const uint8_t *table = nullptr;
    uint32_t       length = 0;
    uint32_t       offset = 0;
    uint32_t       words = 0;
    bool           nowrap = false;
};

RingEntry g_rings[FRAME_RING_MAX_RINGS];
unsigned  g_ring_count = 0;
uint16_t  g_ring_pool[FRAME_RING_POOL_WORDS];
uint32_t  g_ring_used = 0;

// Build time, not the per-frame path: one division per ring.
const RingEntry *ring_lookup(const uint8_t *table, uint32_t length)
{
    for (unsigned i = 0; i < g_ring_count; ++i)
    {
        if (g_rings[i].table == table && g_rings[i].length == length)
        {
            return &g_rings[i];
        }
    }
    return nullptr;
}

const RingEntry *ring_build(const uint8_t *table, uint32_t length)
{
    uint32_t words = length;
    bool nowrap = false;
    if (length <= FRAME_RING_PATTERN_MAX)
    {
        const uint32_t want = length + FRAME_MAX_RUN_WORDS;
        const uint32_t copies = (want + length - 1) / length;
        words = copies * length;
        nowrap = true;
    }
    if (g_ring_count >= FRAME_RING_MAX_RINGS || g_ring_used + words > FRAME_RING_POOL_WORDS)
    {
        return nullptr;
    }
    RingEntry& e = g_rings[g_ring_count];
    e.table = table;
    e.length = length;
    e.offset = g_ring_used;
    e.words = words;
    e.nowrap = nowrap;
    uint32_t source = 0;
    for (uint32_t i = 0; i < words; ++i)
    {
        g_ring_pool[e.offset + i] = table[source];
        if (++source == length)
        {
            source = 0;
        }
    }
    g_ring_used += words;
    ++g_ring_count;
    return &e;
}

}   // namespace

const uint16_t *frame_ring_pool_base()
{
    return g_ring_pool;
}

uint32_t frame_ring_pool_words()
{
    return g_ring_used;
}

uint32_t frame_ring_pool_capacity()
{
    return FRAME_RING_POOL_WORDS;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

void FrameScheduler::build_windows()
{
    if (config_.region_lines < 6)
    {
        config_.region_lines = 6;        // 50*R must exceed the widest COUNT (255)
    }
    if (config_.field_lines < config_.region_lines)
    {
        config_.field_lines = config_.region_lines;
    }
    regions_ = (config_.field_lines + config_.region_lines - 1) / config_.region_lines;
    last_region_lines_ = config_.field_lines - (regions_ - 1) * config_.region_lines;
    // A window has to be wider than the widest COUNT (255 steps) or a region
    // can pass with no pop at all, and then the accumulator's modular form is
    // not the chip's COUNT any more (frame_scheduler.h).  A short remainder
    // region is merged into the one before it rather than left at risk.
    while (regions_ > 1 && last_region_lines_ * STEPS_PER_LINE <= 255 + STEPS_PER_LINE)
    {
        --regions_;
        last_region_lines_ += config_.region_lines;
    }

    const int lines[2] = { config_.region_lines, last_region_lines_ };
    for (int w = 0; w < 2; ++w)
    {
        const int steps = STEPS_PER_LINE * lines[w];
        window_[w].steps = steps;
        for (int p = 0; p < 256; ++p)
        {
            const int d = p + 1;
            window_[w].qr[p] = (static_cast<uint32_t>(steps / d) << 8) |
                               static_cast<uint32_t>(steps % d);
        }
    }

    // The smallest PERIOD a region can keep fed: the run a region asks for is
    // pops + margin and it is capped by the FIFO's room.
    const int steps = (window_[0].steps > window_[1].steps) ? window_[0].steps : window_[1].steps;
    min_safe_period_ = 255;
    for (int p = 0; p <= 255; ++p)
    {
        if (steps / (p + 1) + config_.margin + 1 <= FIFO_DEPTH)
        {
            min_safe_period_ = p;
            break;
        }
    }
}

void FrameScheduler::reset()
{
    for (int c = 0; c < CHANNELS; ++c)
    {
        channels_[c] = FrameChannelState{};
        shadow_[c] = InboxShadow{};
    }
    inbox_.reset();
    region_ = 0;
    control_records_ = 0;
    staged_short_ = 0;
    runs_dropped_ = 0;
    rings_built_ = 0;
    ring_pool_used_ = 0;
    ring_pool_failures_ = 0;
    build_windows();
}

// ---------------------------------------------------------------------------
// Requests
// ---------------------------------------------------------------------------

void FrameScheduler::bind_ring(FrameChannelState& s)
{
    s.ring = nullptr;
    s.ring_words = 0;
    s.ring_nowrap = false;
    if (!s.active || s.table == nullptr || s.length == 0 || s.repeat != 1)
    {
        return;
    }
    const RingEntry *e = ring_lookup(s.table, s.length);
    if (e == nullptr)
    {
        e = ring_build(s.table, s.length);
        if (e == nullptr)
        {
            ++ring_pool_failures_;
            return;
        }
        ++rings_built_;
    }
    s.ring = &g_ring_pool[e->offset];
    s.ring_words = e->words;
    s.ring_nowrap = e->nowrap;
    ring_pool_used_ = g_ring_used;
}

void FrameScheduler::set_stream(int channel, const uint8_t *table, uint32_t length,
                                uint32_t position, uint32_t repeat, uint32_t repeat_count)
{
    FrameChannelState& s = channels_[channel];
    s.table = table;
    s.length = length;
    s.repeat = (repeat != 0) ? repeat : 1;
    // scheduler.h's normalisation, so the two hold the same phase.
    s.position = (length != 0) ? position % length : 0;
    s.repeat_count = (repeat_count < s.repeat) ? repeat_count : 0;
    s.active = (table != nullptr) && (length != 0);
    bind_ring(s);
    s.ring_pos = s.position;
}

void FrameScheduler::set_period(int channel, uint8_t period)
{
    FrameChannelState& s = channels_[channel];
    if (!s.period_pending && s.period == period)
    {
        return;
    }
    s.period_pending = true;
    s.pending_period = period;
    s.pending_pops_left = s.fill;
}

void FrameScheduler::set_volume(int channel, uint8_t volume)
{
    FrameChannelState& s = channels_[channel];
    const uint8_t v = static_cast<uint8_t>(volume & VOLUME_MAX);
    if (s.volume != v)
    {
        s.volume_dirty = true;
    }
    s.volume = v;
}

void FrameScheduler::stop(int channel)
{
    channels_[channel].active = false;
    channels_[channel].ring = nullptr;
}

// Everything PsgChannels::emit() left in the inbox, and the phase back to it.
void FrameScheduler::adopt_requests()
{
    for (int c = 0; c < CHANNELS; ++c)
    {
        const ChannelState& in = inbox_.channel(c);
        InboxShadow& sh = shadow_[c];
        FrameChannelState& s = channels_[c];

        // A stream request: set_stream() is the only thing that moves these
        // fields and the last value written here is known exactly.
        if (in.table != sh.table || in.length != sh.length || in.repeat != sh.repeat ||
            in.position != sh.position || in.repeat_count != sh.repeat_count ||
            in.active != sh.active)
        {
            if (in.active)
            {
                set_stream(c, in.table, in.length, in.position, in.repeat, in.repeat_count);
            }
            else
            {
                stop(c);
            }
        }

        // A PERIOD request.  The inbox's flag is never cleared (its line() is
        // never called), so the value is the edge.
        if (in.period_pending && (!sh.period_pending || in.pending_period != sh.pending_period))
        {
            set_period(c, in.pending_period);
        }

        // A VOLUME request.
        if (in.volume != sh.volume)
        {
            set_volume(c, in.volume);
        }

        // Publish this scheduler's phase.  For a ring stream the hot path
        // advanced `ring_pos`; reduce it to the stream's phase here — the one
        // remainder in the per-field path, 16 bits wide, and skipped when the
        // ring never wrapped past the table.  An unreduced index would in fact
        // carry through carry_phase() unchanged (set_stream() reduces modulo
        // the new length and the excess is a multiple of it), but the shadow
        // has to hold what set_stream() will store.
        if (s.active && s.length != 0)
        {
            if (s.ring != nullptr)
            {
                uint32_t p = s.ring_pos;
                if (p >= s.length)
                {
                    p = static_cast<uint32_t>(static_cast<uint16_t>(p) %
                                              static_cast<uint16_t>(s.length));
                }
                s.position = p;
            }
            inbox_.set_stream(c, s.table, s.length, s.position, s.repeat, s.repeat_count);
        }

        const ChannelState& now = inbox_.channel(c);
        sh.table = now.table;
        sh.length = now.length;
        sh.position = now.position;
        sh.repeat = now.repeat;
        sh.repeat_count = now.repeat_count;
        sh.active = now.active;
        sh.period_pending = now.period_pending;
        sh.pending_period = now.pending_period;
        sh.period = now.period;
        sh.volume = now.volume;
        sh.volume_seen = true;
    }
}

// ---------------------------------------------------------------------------
// One region
// ---------------------------------------------------------------------------

void FrameScheduler::emit_run(RegionDeposits& out, int channel, FrameChannelState& s, int need,
                              uint16_t *window, unsigned window_words)
{
    if (need <= 0)
    {
        return;
    }
    const int room = FIFO_DEPTH - s.fill;
    if (need > room)
    {
        need = room;
    }
    if (need > static_cast<int>(FRAME_MAX_RUN_WORDS))
    {
        need = static_cast<int>(FRAME_MAX_RUN_WORDS);
    }
    if (need <= 0)
    {
        return;
    }

    int placed = 0;
    if (s.ring != nullptr)
    {
        uint32_t index = s.ring_pos;
        int left = need;
        while (left > 0)
        {
            if (out.runs_of[channel] >= FRAME_MAX_RUNS_PER_CHANNEL)
            {
                ++runs_dropped_;
                break;
            }
            int chunk = left;
            const int to_end = static_cast<int>(s.ring_words - index);
            if (chunk > to_end)
            {
                chunk = to_end;
            }
            if (chunk > static_cast<int>(MAX_DESCRIPTOR_WORDS))
            {
                chunk = static_cast<int>(MAX_DESCRIPTOR_WORDS);
            }
            SampleRun& r = out.runs[channel][out.runs_of[channel]];
            r.source = s.ring + index;
            r.count = static_cast<uint16_t>(chunk);
            r.channel = static_cast<uint8_t>(channel);
            r.staged = 0;
            ++out.runs_of[channel];
            ++out.run_count;
            index += static_cast<uint32_t>(chunk);
            if (index >= s.ring_words)
            {
                index -= s.ring_words;
            }
            left -= chunk;
            placed += chunk;
        }
        s.ring_pos = index;
    }
    else
    {
        // Staged: repeat > 1, or a table the pool had no room for.  The words
        // go into the caller's window and the run points at them.
        const int room_in_window = static_cast<int>(window_words) - out.staged_words;
        int take = need;
        if (window == nullptr || room_in_window <= 0)
        {
            ++staged_short_;
            return;
        }
        if (take > room_in_window)
        {
            take = room_in_window;
            ++staged_short_;
        }
        uint16_t *at = window + out.staged_words;
        for (int i = 0; i < take; ++i)
        {
            at[i] = s.table[s.position];
            if (++s.repeat_count >= s.repeat)
            {
                s.repeat_count = 0;
                if (++s.position >= s.length)
                {
                    s.position = 0;
                }
            }
        }
        int left = take;
        int offset = 0;
        while (left > 0)
        {
            if (out.runs_of[channel] >= FRAME_MAX_RUNS_PER_CHANNEL)
            {
                ++runs_dropped_;
                break;
            }
            int chunk = (left > static_cast<int>(MAX_DESCRIPTOR_WORDS))
                            ? static_cast<int>(MAX_DESCRIPTOR_WORDS)
                            : left;
            SampleRun& r = out.runs[channel][out.runs_of[channel]];
            r.source = at + offset;
            r.count = static_cast<uint16_t>(chunk);
            r.channel = static_cast<uint8_t>(channel);
            r.staged = 1;
            ++out.runs_of[channel];
            ++out.run_count;
            offset += chunk;
            left -= chunk;
            placed += chunk;
        }
        out.staged_words += placed;
    }

    if (placed > 0)
    {
        s.fill += placed;
        s.deposited += static_cast<uint32_t>(placed);
        if (s.fill > s.max_fill)
        {
            s.max_fill = s.fill;
        }
        out.words += placed;
    }
}

void FrameScheduler::advance_region(int lines_index)
{
    const Window& w = window_[lines_index];
    for (int c = 0; c < CHANNELS; ++c)
    {
        FrameChannelState& s = channels_[c];
        const Pops p = window_pops_of(w, s.period, s.acc);
        s.acc = p.acc;
        if (s.fill >= p.pops)
        {
            s.fill -= p.pops;
        }
        else
        {
            if (s.active)
            {
                s.predicted_underruns += static_cast<uint32_t>(p.pops - s.fill);
            }
            s.fill = 0;
        }
        if (s.period_pending)
        {
            s.pending_pops_left -= p.pops;
        }
    }
}

void FrameScheduler::region(RegionDeposits& out, uint16_t *window, unsigned window_words)
{
    out.clear();
    const int index = (region_ + 1 == regions_) ? 1 : 0;
    const Window& w = window_[index];

    // Control first: PERIOD records whose old samples have drained, then
    // VOLUME changes.  A PERIOD write does not touch COUNT, so the accumulator
    // is reseeded at the same COUNT (frame_scheduler.h).
    for (int c = 0; c < CHANNELS; ++c)
    {
        FrameChannelState& s = channels_[c];
        if (s.period_pending && s.pending_pops_left <= 0)
        {
            out.control[out.control_bytes++] = static_cast<uint8_t>(REG_PERIOD + c);
            out.control[out.control_bytes++] = s.pending_period;
            s.acc += static_cast<int>(s.pending_period) - static_cast<int>(s.period);
            s.period = s.pending_period;
            s.period_pending = false;
            ++control_records_;
        }
    }
    for (int c = 0; c < CHANNELS; ++c)
    {
        FrameChannelState& s = channels_[c];
        if (s.volume_dirty)
        {
            out.control[out.control_bytes++] = static_cast<uint8_t>(REG_VOLUME + c);
            out.control[out.control_bytes++] = s.volume;
            s.volume_dirty = false;
            ++control_records_;
        }
    }

    // One top-up per active channel, to this region's pops plus the margin, and
    // the region's own pops, in ONE pass: the channels are independent, and when
    // no faster PERIOD is queued the prediction and the advance are the same
    // window from the same accumulator, so the law is evaluated once.
    for (int c = 0; c < CHANNELS; ++c)
    {
        FrameChannelState& s = channels_[c];
        const Pops p = window_pops_of(w, s.period, s.acc);
        if (s.active)
        {
            int pred = p.pops;
            if (s.period_pending && s.pending_period < s.period)
            {
                pred = window_pops_of(w, s.pending_period,
                                      s.acc + static_cast<int>(s.pending_period) -
                                          static_cast<int>(s.period))
                           .pops;
            }
            emit_run(out, c, s, pred + config_.margin - s.fill, window, window_words);
        }
        s.acc = p.acc;
        if (s.fill >= p.pops)
        {
            s.fill -= p.pops;
        }
        else
        {
            if (s.active)
            {
                s.predicted_underruns += static_cast<uint32_t>(p.pops - s.fill);
            }
            s.fill = 0;
        }
        if (s.period_pending)
        {
            s.pending_pops_left -= p.pops;
        }
    }

    if (++region_ >= regions_)
    {
        region_ = 0;
    }
}

void FrameScheduler::note_replay(int channel, int words)
{
    FrameChannelState& s = channels_[channel];
    s.fill += words;
    if (s.fill > FIFO_DEPTH)
    {
        s.fill = FIFO_DEPTH;
    }
    if (s.fill > s.max_fill)
    {
        s.max_fill = s.fill;
    }
}

void FrameScheduler::idle_regions(int regions)
{
    for (int i = 0; i < regions; ++i)
    {
        const int index = (region_ + 1 == regions_) ? 1 : 0;
        advance_region(index);
        if (++region_ >= regions_)
        {
            region_ = 0;
        }
    }
}

}   // namespace griffin::audio
