// psg_channels.cpp — see psg_channels.h.

#include "psg_channels.h"

#include <cmath>

namespace griffin::audio
{

namespace
{

constexpr uint32_t SAMPLE_RATE_NUMERATOR   = 25175000;
constexpr uint32_t SAMPLE_RATE_DENOMINATOR = 1600;

constexpr double step_hz()
{
    return static_cast<double>(STEP_HZ_NUMERATOR) / static_cast<double>(STEP_HZ_DENOMINATOR);
}

double cents(double achieved, double wanted)
{
    return 1200.0 * std::log2(achieved / wanted);
}

}   // namespace

bool tone_band_limited(uint16_t effective_period)
{
    // sn76489.cpp make_step_table: flips per sample in 16.16.
    const uint64_t numerator = 65536ull * PSG_CLOCK_HZ * SAMPLE_RATE_DENOMINATOR;
    const uint64_t denominator = 16ull * SAMPLE_RATE_NUMERATOR * effective_period;
    const uint64_t step = (numerator + denominator / 2) / denominator;
    return step >= 0x10000ull;
}

double tone_hz(uint16_t effective_period)
{
    return static_cast<double>(PSG_CLOCK_HZ) / (32.0 * effective_period);
}

ToneChoice choose_tone(uint16_t effective_period)
{
    ToneChoice best{};
    if (tone_band_limited(effective_period))
    {
        return best;
    }
    const double f = tone_hz(effective_period);
    double best_error = 1e9;
    auto consider = [&](int period_min)
    {
        for (int p = PERIOD_MAX; p >= period_min; --p)
        {
            const double rate = step_hz() / (p + 1);
            const double exact = rate / f;
            const long candidates[2] = { static_cast<long>(std::floor(exact)), static_cast<long>(std::ceil(exact)) };
            for (long l : candidates)
            {
                if (l < 2 || l > MAX_TONE_LENGTH)
                {
                    continue;
                }
                const double err = cents(rate / static_cast<double>(l), f);
                if (std::fabs(err) < std::fabs(best_error))
                {
                    best_error = err;
                    best.period = static_cast<uint8_t>(p);
                    best.length = static_cast<uint16_t>(l);
                    best.cents = err;
                    best.audible = true;
                }
            }
        }
    };
    consider(PREFERRED_MIN_PERIOD);
    if (!best.audible || std::fabs(best_error) > TONE_CENTS_GOAL)
    {
        consider(ABSOLUTE_MIN_PERIOD);
    }
    return best;
}

double noise_shift_hz(uint8_t noise_control, uint16_t tone2_effective_period)
{
    static constexpr uint16_t NOISE_RELOAD[3] = { 0x10, 0x20, 0x40 };
    const int rate = noise_control & 3;
    const uint16_t reload = (rate == 3) ? tone2_effective_period : NOISE_RELOAD[rate];
    // The counter expires every `reload` clock/16 ticks and the shift register
    // advances on every second expiry (sn76489.cpp recompute_noise_step).
    return static_cast<double>(PSG_CLOCK_HZ) / (32.0 * reload);
}

NoiseChoice choose_noise(double shift_hz)
{
    NoiseChoice c{};
    // Smallest DUP with PERIOD in range: rate = DUP * shift_hz <= step/(PERIOD+1).
    double dup = std::ceil(step_hz() / (256.0 * shift_hz));
    if (dup < 1.0)
    {
        dup = 1.0;
    }
    c.dup = static_cast<uint32_t>(dup);
    long p = std::lround(step_hz() / (c.dup * shift_hz)) - 1;
    if (p < ABSOLUTE_MIN_PERIOD)
    {
        p = ABSOLUTE_MIN_PERIOD;
    }
    if (p > PERIOD_MAX)
    {
        p = PERIOD_MAX;
    }
    c.period = static_cast<uint8_t>(p);
    return c;
}

uint8_t volume_for_attenuation(uint8_t attenuation)
{
    if (attenuation >= 15)
    {
        return 0;
    }
    const double v = 63.0 * std::pow(10.0, -0.1 * attenuation);
    return static_cast<uint8_t>(std::lround(v));
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

const std::vector<uint8_t>& PsgChannels::square_table(uint16_t length)
{
    auto it = squares_.find(length);
    if (it == squares_.end())
    {
        std::vector<uint8_t> t(length);
        const uint16_t high = static_cast<uint16_t>(length / 2);
        for (uint16_t i = 0; i < length; ++i)
        {
            t[i] = (i < high) ? 0xFF : 0x01;
        }
        it = squares_.emplace(length, std::move(t)).first;
    }
    return it->second;
}

const std::vector<uint8_t>& PsgChannels::noise_table(bool white, uint32_t dup)
{
    const uint32_t key = (dup << 1) | (white ? 1u : 0u);
    auto it = noises_.find(key);
    if (it == noises_.end())
    {
        // The register's output bit at the seed and after each shift, until
        // the state repeats: sn76489.cpp shift_once with the discrete chip's
        // 15-bit register, seed 0x4000.
        std::vector<uint8_t> bits;
        uint16_t reg = 0x4000;
        const uint16_t seed = reg;
        do
        {
            bits.push_back(static_cast<uint8_t>(reg & 1));
            const uint16_t feedback = white ? static_cast<uint16_t>((reg ^ (reg >> 1)) & 1)
                                            : static_cast<uint16_t>(reg & 1);
            reg = static_cast<uint16_t>(reg >> 1);
            if (feedback)
            {
                reg = static_cast<uint16_t>(reg | 0x4000);
            }
        } while (reg != seed && bits.size() < 70000);

        uint32_t samples = static_cast<uint32_t>(bits.size()) * dup;
        if (samples > NOISE_TABLE_CAP)
        {
            samples = (NOISE_TABLE_CAP / dup) * dup;
        }
        std::vector<uint8_t> t(samples);
        for (uint32_t i = 0; i < samples; ++i)
        {
            t[i] = bits[i / dup] ? 0xFF : 0x01;
        }
        it = noises_.emplace(key, std::move(t)).first;
    }
    return it->second;
}

size_t PsgChannels::table_bytes() const
{
    size_t n = 0;
    for (const auto& kv : squares_)
    {
        n += kv.second.size();
    }
    for (const auto& kv : noises_)
    {
        n += kv.second.size();
    }
    return n;
}

void PsgChannels::emit(Scheduler& scheduler)
{
    // Tones.
    for (int c = 0; c < 3; ++c)
    {
        Shadow& sh = shadow_[c];
        const uint16_t n = effective_period(c);
        const bool audible = atten[c] < 15 && !tone_band_limited(n);
        const uint8_t volume = audible ? volume_for_attenuation(atten[c]) : 0;

        if (audible && (!sh.active || sh.psg_period != n))
        {
            const ToneChoice choice = choose_tone(n);
            tone_choices.emplace(n, choice);
            const std::vector<uint8_t>& table = square_table(choice.length);
            uint32_t position = 0;
            if (sh.active && sh.length != 0)
            {
                const uint32_t old_position = scheduler.channel(c).position;
                position = static_cast<uint32_t>((static_cast<uint64_t>(old_position) * choice.length) / sh.length);
                ++tone_retargets;
            }
            scheduler.set_stream(c, table.data(), choice.length, position);
            if (choice.period != sh.period)
            {
                scheduler.set_period(c, choice.period);
                sh.period = choice.period;
            }
            sh.active = true;
            sh.psg_period = n;
            sh.length = choice.length;
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
        const uint8_t volume = audible ? volume_for_attenuation(atten[3]) : 0;
        const bool white = (noise_control & 4) != 0;
        const NoiseChoice choice = choose_noise(noise_shift_hz(noise_control, effective_period(2)));

        if (audible && (noise_reseed_ || !sh.active || sh.white != white || sh.dup != choice.dup || sh.period != choice.period))
        {
            const std::vector<uint8_t>& table = noise_table(white, choice.dup);
            uint32_t position = 0;
            if (!noise_reseed_ && sh.active && sh.length != 0)
            {
                const uint32_t old_position = scheduler.channel(NOISE_CHANNEL).position;
                position = static_cast<uint32_t>((static_cast<uint64_t>(old_position) * table.size()) / sh.length);
                ++noise_retargets;
            }
            else
            {
                ++noise_restarts;
            }
            scheduler.set_stream(NOISE_CHANNEL, table.data(), static_cast<uint32_t>(table.size()), position);
            if (choice.period != sh.period)
            {
                scheduler.set_period(NOISE_CHANNEL, choice.period);
                sh.period = choice.period;
            }
            sh.active = true;
            sh.white = white;
            sh.dup = choice.dup;
            sh.length = static_cast<uint32_t>(table.size());
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
