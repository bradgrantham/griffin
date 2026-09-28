// audio_chip.cpp — see audio_chip.h.

#include "audio_chip.h"

namespace griffin::audio
{

void AudioChip::reset()
{
    for (int c = 0; c < CHANNELS; ++c)
    {
        fifo_[c].reset();
        period_[c] = 0;
        volume_[c] = 0;
        count_[c] = 0;
        held_[c] = SILENCE;
        popped_[c] = false;
    }
    control_.reset();
    control_state_ = ControlState::Idle;
    control_have_register_ = false;
    control_register_ = 0;
    steps_ = 0;
    stats = ChipStats{};
}

bool AudioChip::deposit_sample(int channel, uint8_t byte)
{
    if (!fifo_[channel].push(byte))
    {
        ++stats.deposits_dropped[channel];
        return false;
    }
    if (fifo_[channel].count > stats.max_fill[channel])
    {
        stats.max_fill[channel] = fifo_[channel].count;
    }
    return true;
}

bool AudioChip::deposit_control(uint8_t byte)
{
    if (!control_.push(byte))
    {
        ++stats.control_dropped;
        return false;
    }
    if (control_.count > stats.max_control_fill)
    {
        stats.max_control_fill = control_.count;
    }
    return true;
}

void AudioChip::run_control_clocks(int clocks)
{
    for (int k = 0; k < clocks; ++k)
    {
        switch (control_state_)
        {
            case ControlState::Idle:
            {
                if (!control_.empty())
                {
                    control_state_ = ControlState::Read0;      // /R falls
                }
                break;
            }
            case ControlState::Read0:
            {
                control_state_ = ControlState::Read1;
                break;
            }
            case ControlState::Read1:
            {
                // The byte is latched and /R rises: the 7200 advances.
                const uint8_t byte = control_.front();
                control_.pop();
                if (!control_have_register_)
                {
                    control_register_ = static_cast<uint8_t>(byte & 7);
                    control_have_register_ = true;
                }
                else
                {
                    const int channel = control_register_ & 3;
                    if (control_register_ < REG_VOLUME)
                    {
                        period_[channel] = byte;
                    }
                    else
                    {
                        volume_[channel] = static_cast<uint8_t>(byte & VOLUME_MAX);
                    }
                    control_have_register_ = false;
                    ++stats.control_records;
                }
                control_state_ = ControlState::Advance;
                break;
            }
            case ControlState::Advance:
            default:
            {
                control_state_ = ControlState::Idle;
                break;
            }
        }
    }
}

void AudioChip::step()
{
    // The reader is idle almost always; only spend the clocks when it has
    // something to do.  Its writes land before the step's counter update
    // whenever the record was complete in the FIFO at the step's first clock
    // (8 of the 16 clocks suffice).
    if (control_state_ != ControlState::Idle || !control_.empty())
    {
        run_control_clocks(STEP_CLOCKS);
    }

    for (int c = 0; c < CHANNELS; ++c)
    {
        if (count_[c] == 0)
        {
            popped_[c] = true;
            ++stats.pops[c];
            if (!fifo_[c].pop())
            {
                ++stats.pops_empty[c];
            }
            count_[c] = period_[c];
        }
        else
        {
            popped_[c] = false;
            --count_[c];
        }
        if (!fifo_[c].empty())
        {
            held_[c] = fifo_[c].front();
        }
    }
    ++steps_;
}

uint8_t AudioChip::sample(int channel) const
{
    return held_[channel];
}

int AudioChip::gate_ticks_on(int channel) const
{
    if (fifo_[channel].empty())
    {
        return 0;
    }
    // The step just completed covered frame ticks 2s and 2s+1 of its frame.
    const uint8_t tick0 = static_cast<uint8_t>(((steps_ - 1) % STEPS_PER_FRAME) * TICKS_PER_STEP);
    int on = 0;
    on += rate_multiplier_on(volume_[channel], tick0) ? 1 : 0;
    on += rate_multiplier_on(volume_[channel], static_cast<uint8_t>(tick0 + 1)) ? 1 : 0;
    return on;
}

void AnalogRender::configure(double corner_hz)
{
    const double dt = static_cast<double>(STEP_CLOCKS) / static_cast<double>(CLK_HZ);
    const double rc = 1.0 / (6.283185307179586 * corner_hz);
    alpha = dt / (rc + dt);
}

void AnalogRender::step(const AudioChip& chip)
{
    double l = 0.0;
    double r = 0.0;
    for (int c = 0; c < CHANNELS; ++c)
    {
        const double v = static_cast<int8_t>(chip.sample(c)) *
                         (static_cast<double>(chip.gate_ticks_on(c)) / TICKS_PER_STEP);
        if (c < 2)
        {
            l += v;
        }
        else
        {
            r += v;
        }
    }
    left += alpha * (l - left);
    right += alpha * (r - right);
}

}   // namespace griffin::audio
