// scheduler.cpp — see scheduler.h.

#include "scheduler.h"

namespace griffin::audio
{

void Scheduler::reset()
{
    for (int c = 0; c < CHANNELS; ++c)
    {
        channels_[c] = ChannelState{};
    }
    line_ = 0;
    control_records_ = 0;
}

void Scheduler::set_stream(int channel, const uint8_t *table, uint32_t length, uint32_t position,
                           uint32_t repeat, uint32_t repeat_count)
{
    ChannelState& s = channels_[channel];
    s.table = table;
    s.length = length;
    s.position = (length != 0) ? position % length : 0;
    s.repeat = (repeat != 0) ? repeat : 1;
    s.repeat_count = (repeat_count < s.repeat) ? repeat_count : 0;
    const bool active = (table != nullptr) && (length != 0);
    if (active && !s.active)
    {
        s.burst_now = true;
    }
    s.active = active;
}

void Scheduler::set_period(int channel, uint8_t period)
{
    ChannelState& s = channels_[channel];
    if (!s.period_pending && s.period == period)
    {
        return;
    }
    s.period_pending = true;
    s.pending_period = period;
    s.pending_pops_left = s.fill;
}

void Scheduler::set_volume(int channel, uint8_t volume)
{
    ChannelState& s = channels_[channel];
    const uint8_t v = static_cast<uint8_t>(volume & VOLUME_MAX);
    if (s.volume != v || !s.volume_dirty)
    {
        s.volume_dirty = (s.volume != v) || s.volume_dirty;
    }
    s.volume = v;
}

void Scheduler::stop(int channel)
{
    channels_[channel].active = false;
}

int Scheduler::predicted_pops(const ChannelState& s, int lines) const
{
    int count = s.count;
    int total = 0;
    int period = s.period;
    if (s.period_pending && s.pending_period < period)
    {
        period = s.pending_period;
    }
    for (int i = 0; i < lines; ++i)
    {
        const LinePops lp = line_pops(count, period);
        total += lp.pops;
        count = lp.count_after;
    }
    return total;
}

void Scheduler::line(LineDeposits& out)
{
    out.clear();

    // Control first: PERIOD records whose old samples have drained, then
    // VOLUME changes.  The chip reads a record in 8 clocks, so the first two
    // land before this line's first step; the mirror assumes they all do and
    // the margin absorbs the rest.
    for (int c = 0; c < CHANNELS; ++c)
    {
        ChannelState& s = channels_[c];
        if (s.period_pending && s.pending_pops_left <= 0)
        {
            out.add(DEST_CONTROL, static_cast<uint8_t>(REG_PERIOD + c));
            out.add(DEST_CONTROL, s.pending_period);
            s.period = s.pending_period;
            s.period_pending = false;
            ++control_records_;
        }
    }
    for (int c = 0; c < CHANNELS; ++c)
    {
        ChannelState& s = channels_[c];
        if (s.volume_dirty)
        {
            out.add(DEST_CONTROL, static_cast<uint8_t>(REG_VOLUME + c));
            out.add(DEST_CONTROL, s.volume);
            s.volume_dirty = false;
            ++control_records_;
        }
    }

    // Sample bursts, staggered so the four channels do not all burst on the
    // same line.
    for (int c = 0; c < CHANNELS; ++c)
    {
        ChannelState& s = channels_[c];
        const int burst = config_.burst_lines;
        const int stagger = (c * burst) / CHANNELS;
        if (s.active && (s.burst_now || static_cast<int>(line_ % static_cast<uint64_t>(burst)) == stagger))
        {
            s.burst_now = false;
            int need = predicted_pops(s, burst) + config_.margin - s.fill;
            const int room = FIFO_DEPTH - s.fill;
            if (need > room)
            {
                need = room;
            }
            for (int i = 0; i < need; ++i)
            {
                out.add(static_cast<uint8_t>(c), s.table[s.position]);
                if (++s.repeat_count == s.repeat)
                {
                    s.repeat_count = 0;
                    if (++s.position == s.length)
                    {
                        s.position = 0;
                    }
                }
            }
            if (need > 0)
            {
                s.fill += need;
                s.deposited += static_cast<uint64_t>(need);
                if (s.fill > s.max_fill)
                {
                    s.max_fill = s.fill;
                }
            }
        }
    }

    // The chip's pops during this line, on the mirror.
    for (int c = 0; c < CHANNELS; ++c)
    {
        ChannelState& s = channels_[c];
        const LinePops lp = line_pops(s.count, s.period);
        if (s.fill >= lp.pops)
        {
            s.fill -= lp.pops;
        }
        else
        {
            if (s.active)
            {
                s.predicted_underruns += static_cast<uint64_t>(lp.pops - s.fill);
            }
            s.fill = 0;
        }
        s.count = lp.count_after;
        if (s.period_pending)
        {
            s.pending_pops_left -= lp.pops;
        }
    }
    ++line_;
}

}   // namespace griffin::audio
