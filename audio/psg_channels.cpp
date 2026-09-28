// psg_channels.cpp — see psg_channels.h.  Bare metal.

#include "psg_channels.h"

#include "psg_tables.h"

namespace griffin::audio
{

namespace pt = psg_tables;

namespace
{

constexpr uint16_t NOISE_RELOAD[3] = { 0x10, 0x20, 0x40 };
constexpr uint8_t  HIGH = 0x7F;
constexpr uint8_t  LOW  = 0x81;      // -127

struct SquareBuffer
{
    uint8_t v[512];
};

consteval SquareBuffer make_square()
{
    SquareBuffer b{};
    for (int i = 0; i < 512; ++i)
    {
        b.v[i] = (i < 256) ? HIGH : LOW;
    }
    return b;
}

constinit const SquareBuffer SQUARE = make_square();

uint8_t white_samples[pt::WHITE_LENGTH];
uint8_t periodic_samples[pt::PERIODIC_LENGTH];
bool    noise_built = false;

void build_noise()
{
    for (uint32_t i = 0; i < pt::WHITE_LENGTH; ++i)
    {
        white_samples[i] = ((pt::WHITE_BITS[i / 8] >> (i % 8)) & 1) ? HIGH : LOW;
    }
    for (uint32_t i = 0; i < pt::PERIODIC_LENGTH; ++i)
    {
        periodic_samples[i] = (i == pt::PERIODIC_LENGTH - 1) ? HIGH : LOW;   // seed 0x4000: fourteen 0s, then the 1
    }
    noise_built = true;
}

// A new table's starting point: the old stream's phase fraction carried over,
// in sample units (table index x repeat + copies made).
struct Start
{
    uint32_t position;
    uint32_t repeat_count;
};

Start carry_phase(const ChannelState& old, uint32_t old_samples, uint32_t new_samples, uint32_t new_repeat)
{
    const uint64_t at = static_cast<uint64_t>(old.position) * old.repeat + old.repeat_count;
    const uint32_t sample = static_cast<uint32_t>((at * new_samples) / old_samples);
    return Start{ sample / new_repeat, sample % new_repeat };
}

}   // namespace

const uint8_t *square_table(uint16_t length)
{
    return &SQUARE.v[256 - length / 2];
}

const uint8_t *noise_samples(bool white)
{
    if (!noise_built)
    {
        build_noise();
    }
    return white ? white_samples : periodic_samples;
}

uint32_t noise_bits(bool white, uint8_t repeat)
{
    const uint32_t bits = white ? pt::WHITE_LENGTH : pt::PERIODIC_LENGTH;
    const uint32_t cap = NOISE_TABLE_CAP / repeat;
    return (bits < cap) ? bits : cap;
}

void PsgChannels::reset()
{
    for (int c = 0; c < 3; ++c)
    {
        period_reg[c] = 0;
    }
    for (int c = 0; c < 4; ++c)
    {
        atten[c] = 15;
        shadow_[c] = Shadow{};
    }
    noise_control = 0;
    latch = 0;
    noise_reseed_ = true;
}

void PsgChannels::write(uint8_t byte)
{
    // sn76489.cpp Psg::write, register decode only.
    if (byte & 0x80)
    {
        latch = static_cast<uint8_t>((byte >> 4) & 7);
        const uint8_t data = static_cast<uint8_t>(byte & 0x0F);
        switch (latch)
        {
            case 0:
            case 2:
            case 4:
            {
                const int c = latch >> 1;
                period_reg[c] = static_cast<uint16_t>((period_reg[c] & 0x3F0) | data);
                break;
            }
            case 1:
            case 3:
            case 5:
            {
                atten[latch >> 1] = data;
                break;
            }
            case 6:
            {
                noise_control = static_cast<uint8_t>(data & 7);
                noise_reseed_ = true;
                break;
            }
            default:
            {
                atten[3] = data;
                break;
            }
        }
    }
    else if ((latch & 1) == 0 && latch <= 4)
    {
        const int c = latch >> 1;
        period_reg[c] = static_cast<uint16_t>(((byte & 0x3F) << 4) | (period_reg[c] & 0x0F));
    }
}

void PsgChannels::emit(Scheduler& scheduler)
{
    // Tones.
    for (int c = 0; c < 3; ++c)
    {
        Shadow& sh = shadow_[c];
        const uint16_t n = effective_period(c);
        const pt::ToneEntry& tone = pt::TONE[n & 1023];
        const bool audible = atten[c] < 15 && tone.length != 0;
        const uint8_t volume = audible ? pt::VOLUME[atten[c] & 15] : 0;

        if (audible && (!sh.active || sh.psg_period != n))
        {
            periods_used[(n & 1023) / 8] = static_cast<uint8_t>(periods_used[(n & 1023) / 8] | (1u << (n % 8)));
            Start start{ 0, 0 };
            if (sh.active && sh.length != 0)
            {
                start = carry_phase(scheduler.channel(c), sh.length, tone.length, 1);
                ++tone_retargets;
            }
            scheduler.set_stream(c, square_table(tone.length), tone.length, start.position);
            if (tone.period != sh.period)
            {
                scheduler.set_period(c, tone.period);
                sh.period = tone.period;
            }
            sh.active = true;
            sh.psg_period = n;
            sh.length = tone.length;
        }
        else if (!audible && sh.active)
        {
            scheduler.stop(c);
            sh.active = false;
        }
        if (volume != sh.volume)
        {
            scheduler.set_volume(c, volume);
            sh.volume = volume;
        }
    }

    // Noise.
    {
        Shadow& sh = shadow_[NOISE_CHANNEL];
        const bool audible = atten[3] < 15;
        const uint8_t volume = audible ? pt::VOLUME[atten[3] & 15] : 0;
        const bool white = (noise_control & 4) != 0;
        const int rate = noise_control & 3;
        const uint16_t reload = (rate == 3) ? effective_period(2) : NOISE_RELOAD[rate];
        const pt::NoiseEntry& choice = pt::NOISE[reload];

        if (audible && (noise_reseed_ || !sh.active || sh.white != white || sh.repeat != choice.repeat
                        || sh.period != choice.period))
        {
            const uint32_t bits = noise_bits(white, choice.repeat);
            const uint32_t samples = bits * choice.repeat;
            Start start{ 0, 0 };
            if (!noise_reseed_ && sh.active && sh.length != 0)
            {
                start = carry_phase(scheduler.channel(NOISE_CHANNEL), sh.length, samples, choice.repeat);
                ++noise_retargets;
            }
            else
            {
                ++noise_restarts;
            }
            scheduler.set_stream(NOISE_CHANNEL, noise_samples(white), bits, start.position, choice.repeat,
                                 start.repeat_count);
            if (choice.period != sh.period)
            {
                scheduler.set_period(NOISE_CHANNEL, choice.period);
                sh.period = choice.period;
            }
            sh.active = true;
            sh.white = white;
            sh.repeat = choice.repeat;
            sh.length = samples;
        }
        else if (!audible && sh.active)
        {
            scheduler.stop(NOISE_CHANNEL);
            sh.active = false;
        }
        if (volume != sh.volume)
        {
            scheduler.set_volume(NOISE_CHANNEL, volume);
            sh.volume = volume;
        }
        noise_reseed_ = false;
    }
}

}   // namespace griffin::audio
