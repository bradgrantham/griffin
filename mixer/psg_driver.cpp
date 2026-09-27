// PSG bytes -> MIXER SETs — see psg_driver.h.

#include "psg_driver.h"

namespace griffin::mixer::psg
{

namespace
{

// INC[N & 0x3FF]: entry 0 is period 1024.  Stored mod 65536 (tones below
// period 8 alias exactly; psg_driver.h).  2048 bytes of rodata.
struct IncTable
{
    uint16_t v[PERIODS];
};

consteval IncTable make_inc_table()
{
    IncTable t{};
    for (uint32_t r = 0; r < PERIODS; ++r)
    {
        const uint32_t period = r ? r : static_cast<uint32_t>(PERIODS);
        t.v[r] = static_cast<uint16_t>(true_inc(period) & 0xFFFFu);
    }
    return t;
}

inline constexpr IncTable INC = make_inc_table();

// The reference's 2 dB ladder: repeated multiplication by 10^(-0.1) in Q32,
// rounded to Q16 (sn76489.cpp make_atten_table), then scaled to the peak and
// rounded to an 8-bit AMP.
inline constexpr uint64_t RATIO_2DB_Q32 = 3411613790ull;    // 10^(-0.1) * 2^32

struct AmpLadder
{
    int8_t v[16];
};

consteval AmpLadder make_amp_ladder(int peak)
{
    AmpLadder t{};
    uint64_t acc = 1ull << 32;
    for (int a = 0; a < 15; ++a)
    {
        const uint32_t ratio_q16 = static_cast<uint32_t>((acc + (1ull << 15)) >> 16);
        t.v[a] = static_cast<int8_t>((static_cast<uint32_t>(peak) * ratio_q16 + (1u << 15)) >> 16);
        acc = (acc * RATIO_2DB_Q32) >> 32;
    }
    t.v[15] = 0;
    return t;
}

inline constexpr AmpLadder AMP = make_amp_ladder(PEAK_PER_CHANNEL);

// Noise rate selector 0/1/2 reloads.
inline constexpr uint16_t NOISE_RELOAD[3] = { 0x10, 0x20, 0x40 };

inline constexpr uint16_t NOISE_INC_CLAMP = 0xFFFF;

}   // namespace

uint16_t inc_for_register(uint16_t period_register)
{
    return INC.v[period_register & (PERIODS - 1)];
}

int8_t amp_for_attenuation(uint8_t attenuation)
{
    return AMP.v[attenuation & 15];
}

size_t silence_voices(int first, int last, std::span<uint16_t> out)
{
    size_t n = 0;
    for (int v = first; v < last && n + 2 <= out.size(); ++v)
    {
        out[n++] = set_word(voice_addr(v, V_AMP), 0);
        out[n++] = set_word(voice_addr(v, V_CTL), 0);
    }
    return n;
}

void PsgDriver::reset()
{
    for (int c = 0; c < 3; ++c)
    {
        period_reg[c] = 0;
    }
    for (int c = 0; c < 4; ++c)
    {
        atten[c] = 15;
    }
    noise_control = 0;
    latch = 0;
    noise_clamped_ = false;
    select_valid_ = false;
    for (int v = 0; v < PSG_VOICES; ++v)
    {
        for (int b = 0; b < 4; ++b)
        {
            shadow_valid_[v][b] = false;
        }
        reset_pending_[v] = true;
    }
}

void PsgDriver::write(uint8_t byte)
{
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
            atten[latch >> 1] = data;
            break;
        case 6:
            noise_control = static_cast<uint8_t>(data & 7);
            // The SN76489 reseeds its shift register on every control write,
            // same value or not.
            reset_pending_[NOISE_VOICE] = true;
            break;
        default:    // case 7
            atten[3] = data;
            break;
        }
    }
    else
    {
        // Second byte of a tone period, bits 9-4; after any other latch it is
        // ignored, as sn76489.cpp does.
        if ((latch & 1) == 0 && latch <= 4)
        {
            const int c = latch >> 1;
            period_reg[c] = static_cast<uint16_t>(((byte & 0x3F) << 4) | (period_reg[c] & 0x0F));
        }
    }
}

uint16_t PsgDriver::noise_inc() const
{
    const int rate = noise_control & 3;
    if (rate != 3)
    {
        return INC.v[NOISE_RELOAD[rate]];
    }
    const uint16_t reg = period_reg[2];
    if (reg != 0 && reg < FIRST_REPRESENTABLE_PERIOD)
    {
        return NOISE_INC_CLAMP;
    }
    return INC.v[reg];
}

size_t PsgDriver::emit(std::span<uint16_t> out)
{
    size_t n = 0;
    const int rate = noise_control & 3;
    noise_clamped_ = (rate == 3 && period_reg[2] != 0 && period_reg[2] < FIRST_REPRESENTABLE_PERIOD);

    if (!select_valid_)
    {
        if (n == out.size())
        {
            return n;
        }
        out[n++] = set_word(LFSR_VOICE_SELECT, static_cast<uint8_t>(NOISE_VOICE));
        select_valid_ = true;
    }

    for (int v = 0; v < PSG_VOICES; ++v)
    {
        uint16_t inc = 0;
        uint8_t mode = MODE_SQUARE;
        if (v < 3)
        {
            inc = INC.v[period_reg[v] & (PERIODS - 1)];
        }
        else
        {
            inc = noise_inc();
            mode = (noise_control & 4) ? MODE_NOISE_WHITE : MODE_NOISE_PERIODIC;
        }
        const uint8_t want[4] = {
            static_cast<uint8_t>(inc),
            static_cast<uint8_t>(inc >> 8),
            static_cast<uint8_t>(AMP.v[atten[v] & 15]),
            ctl(mode, true, true),
        };
        static constexpr uint8_t OFFSET[4] = { V_INC_LO, V_INC_HI, V_AMP, V_CTL };

        for (int b = 0; b < 4; ++b)
        {
            const bool is_ctl = (b == 3);
            const bool reset = is_ctl && reset_pending_[v];
            if (shadow_valid_[v][b] && shadow_[v][b] == want[b] && !reset)
            {
                continue;
            }
            if (n == out.size())
            {
                return n;       // the rest stays pending
            }
            const uint8_t value = reset ? static_cast<uint8_t>(want[b] | CTL_RESET_PHASE) : want[b];
            out[n++] = set_word(voice_addr(v, OFFSET[b]), value);
            shadow_[v][b] = want[b];
            shadow_valid_[v][b] = true;
            if (reset)
            {
                reset_pending_[v] = false;
            }
        }
    }
    return n;
}

}   // namespace griffin::mixer::psg
