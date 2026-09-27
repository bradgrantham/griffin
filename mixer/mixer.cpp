// MIXER shared model — see mixer.h for the laws.

#include "mixer.h"

namespace griffin::mixer
{

namespace
{

// Saturating add into an 8-bit two's complement accumulator.  The accumulator
// is carried in an int so that "- AMP" of -128 is simply +128 before the clamp.
inline int saturate_add(int acc, int term, uint32_t& saturations)
{
    int sum = acc + term;
    if (sum > 127)
    {
        sum = 127;
        ++saturations;
    }
    else if (sum < -128)
    {
        sum = -128;
        ++saturations;
    }
    return sum;
}

// xorshift32: the "undefined" power-on contents, reproducible run to run.
inline uint32_t next_random(uint32_t& state)
{
    uint32_t x = state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    state = x;
    return x;
}

}   // namespace

bool WordFifo::push(uint16_t word)
{
    if (count_ == words_.size())
    {
        return false;
    }
    words_[(head_ + count_) % words_.size()] = word;
    ++count_;
    return true;
}

bool WordFifo::pop(uint16_t& word)
{
    if (count_ == 0)
    {
        return false;
    }
    word = words_[head_];
    head_ = (head_ + 1) % words_.size();
    --count_;
    return true;
}

Mixer::Mixer(int voices)
{
    if (voices < 1)
    {
        voices = 1;
    }
    if (voices > MAX_VOICES)
    {
        voices = MAX_VOICES;
    }
    voices_ = voices;
    reset_power_on();
}

void Mixer::reset_power_on(uint32_t seed)
{
    uint32_t state = seed ? seed : 1u;
    for (uint8_t& byte : ram_)
    {
        byte = static_cast<uint8_t>(next_random(state) >> 24);
    }
    stats = Stats{};
    reset();
}

void Mixer::reset()
{
    wait_ = 0;
    lfsr_ = LFSR_SEED;
    audcmd_.clear();
    samples_.clear();
}

bool Mixer::push_audcmd(uint16_t word)
{
    if (!audcmd_.push(word))
    {
        ++stats.audcmd_overflows;
        return false;
    }
    return true;
}

bool Mixer::push_samples(uint16_t word)
{
    if (!samples_.push(word))
    {
        ++stats.samples_overflows;
        return false;
    }
    return true;
}

int Mixer::lfsr_voice() const
{
    const int v = ram_[LFSR_VOICE_SELECT] & LFSR_VOICE_MASK;
    return (v < voices_) ? v : -1;
}

uint16_t Mixer::inc(int voice) const
{
    const int base = voice_addr(voice, 0);
    return static_cast<uint16_t>(ram_[base + V_INC_LO] | (ram_[base + V_INC_HI] << 8));
}

uint16_t Mixer::phase(int voice) const
{
    const int base = voice_addr(voice, 0);
    return static_cast<uint16_t>(ram_[base + V_PHASE_LO] | (ram_[base + V_PHASE_HI] << 8));
}

void Mixer::execute_set(uint8_t addr, uint8_t value)
{
    addr &= SET_WINDOW_BYTES - 1;
    ++stats.sets;
    if (addr < CONTROL_BASE && (addr & (VOICE_BYTES - 1)) == V_CTL && (value & CTL_RESET_PHASE))
    {
        const int base = addr & ~(VOICE_BYTES - 1);
        ram_[base + V_PHASE_LO] = 0;
        ram_[base + V_PHASE_HI] = 0;
        if (base / VOICE_BYTES == lfsr_voice())
        {
            lfsr_ = LFSR_SEED;
        }
        value = static_cast<uint8_t>(value & ~CTL_RESET_PHASE);
    }
    ram_[addr] = value;
}

void Mixer::consume_records()
{
    if (wait_ > 0)
    {
        --wait_;
    }
    uint32_t records = 0;
    uint32_t sets = 0;
    while (wait_ == 0)
    {
        if (records == MAX_RECORDS_PER_BOUNDARY)
        {
            ++stats.limited_boundaries;     // the rest slide to the next boundary
            break;
        }
        uint16_t word = 0;
        if (!audcmd_.pop(word))
        {
            ++stats.empty_boundaries;
            break;
        }
        ++records;
        if (word & AUDCMD_SET_BIT)
        {
            ++sets;
            execute_set(static_cast<uint8_t>(word >> 8), static_cast<uint8_t>(word));
        }
        else
        {
            ++stats.waits;
            wait_ = static_cast<uint16_t>(word & WAIT_MAX_TICKS);
        }
    }
    if (records > stats.max_records_per_boundary)
    {
        stats.max_records_per_boundary = records;
    }
    if (sets > stats.max_sets_per_boundary)
    {
        stats.max_sets_per_boundary = sets;
    }
}

void Mixer::shift_lfsr(bool white)
{
    const uint16_t feedback = white ? static_cast<uint16_t>((lfsr_ ^ (lfsr_ >> 1)) & 1u)
                                    : static_cast<uint16_t>(lfsr_ & 1u);
    lfsr_ = static_cast<uint16_t>((lfsr_ >> 1) | (feedback << 14));
    ++stats.lfsr_shifts;
}

StereoSample Mixer::tick()
{
    ++stats.ticks;

    // 1. AUDCMD at the boundary.
    consume_records();

    // 2. PCM passthrough seeds the accumulators.
    int left = 0;
    int right = 0;
    uint16_t pcm = 0;
    if (samples_.pop(pcm))
    {
        ++stats.samples_popped;
        left = static_cast<int8_t>(pcm >> 8);
        right = static_cast<int8_t>(pcm & 0xFF);
    }
    else
    {
        ++stats.samples_empty;
    }

    // 3. Voices in index order through the one adder.  Every voice reads the
    // LFSR as it stood when the walk began; the LFSR voice's carry is only
    // latched here and the shift happens in step 4.
    const int lfsr_v = lfsr_voice();
    bool lfsr_carry = false;
    bool lfsr_white = false;
    for (int v = 0; v < voices_; ++v)
    {
        uint8_t *const voice = &ram_[v * VOICE_BYTES];
        const uint32_t inc = static_cast<uint32_t>(voice[V_INC_LO] | (voice[V_INC_HI] << 8));
        const uint32_t old_phase = static_cast<uint32_t>(voice[V_PHASE_LO] | (voice[V_PHASE_HI] << 8));
        const uint32_t sum = old_phase + inc;
        const bool carry = (sum & 0x10000u) != 0;
        const uint16_t phase = static_cast<uint16_t>(sum);
        voice[V_PHASE_LO] = static_cast<uint8_t>(phase);
        voice[V_PHASE_HI] = static_cast<uint8_t>(phase >> 8);

        const uint8_t ctl_byte = voice[V_CTL];
        const uint8_t mode = static_cast<uint8_t>((ctl_byte & CTL_MODE_MASK) >> CTL_MODE_SHIFT);
        const int amp = static_cast<int8_t>(voice[V_AMP]);

        bool positive = false;
        if (mode == MODE_SQUARE)
        {
            positive = (phase & 0x8000u) != 0;
        }
        else if (mode == MODE_NOISE_WHITE || mode == MODE_NOISE_PERIODIC)
        {
            if (v == lfsr_v && carry)
            {
                lfsr_carry = true;
                lfsr_white = (mode == MODE_NOISE_WHITE);
            }
            positive = (lfsr_ & 1u) != 0;
        }
        else
        {
            // MODE_WAVETABLE: rung v, not in v0.  SRAM[WAVE_PAGE*256 + phase[15:8]]
            // will go here; until then the voice is silent.
            ++stats.wavetable_ticks;
            continue;
        }

        const int term = positive ? amp : -amp;
        if (ctl_byte & CTL_PAN_L)
        {
            left = saturate_add(left, term, stats.saturations);
        }
        if (ctl_byte & CTL_PAN_R)
        {
            right = saturate_add(right, term, stats.saturations);
        }
    }

    // 4. Latch, then the one LFSR shift this tick earned (heard next tick).
    if (lfsr_carry)
    {
        shift_lfsr(lfsr_white);
    }
    return StereoSample{ static_cast<int8_t>(left), static_cast<int8_t>(right) };
}

}   // namespace griffin::mixer
