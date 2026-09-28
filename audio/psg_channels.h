// psg_channels.h — SN76489AN port bytes in, AUDIO channel streams out.
//
// The successor of mixer/psg_driver.h for the Paula-form chip: instead of
// SETs it produces tables, periods and volumes for the Scheduler.
//
//   tone c (0..2) -> channel c: a square table of LENGTH samples played at
//                    PERIOD, chosen so 1,573,437.5 / (PERIOD+1) / LENGTH is
//                    the tone's 3,579,545 / (32 N) Hz within a cent
//                    (choose_tone); tables are cached by LENGTH.  Tones the
//                    reference band-limits to silence (N <= 14, fundamental
//                    at or above 7,867 Hz) are silenced here too, for the A/B.
//   noise         -> channel 3: the shift register's output sequence as a
//                    byte table (white: the 15-bit LFSR, taps 0 and 1, period
//                    32767; periodic: 15), each bit repeated DUP times when
//                    the shift rate is below the chip's 6.15 kHz floor,
//                    truncated to NOISE_TABLE_CAP samples.  A noise control
//                    write restarts the table (the chip reseeds); a rate-3
//                    tone-2 change re-rates it in place.
//   attenuation a -> VOLUME round(63 * 10^(-a/10)), 15 -> 0.  Channel level is
//                    then (sample - 128) * VOLUME / 64, and the mono mixdown
//                    of four channels at VOLUME 63 is 4 x 125, i.e. the
//                    reference's peak 31 per voice times four, scaled by 4.
//
// On a frequency change the new table starts at the old table's phase
// fraction, and the Scheduler lands the PERIOD record when the old samples
// have drained.  Host side today (std::map caches, double for cents); the
// 68000 version prepares the same tables at load time.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "scheduler.h"

namespace griffin::audio
{

inline constexpr uint32_t PSG_CLOCK_HZ    = 3579545;
inline constexpr int      NOISE_CHANNEL   = 3;
inline constexpr uint32_t NOISE_TABLE_CAP = 65536;
inline constexpr uint16_t MAX_TONE_LENGTH = 512;
inline constexpr uint8_t  PREFERRED_MIN_PERIOD = 49;     // rates up to 31.5 kHz
inline constexpr uint8_t  ABSOLUTE_MIN_PERIOD  = 12;     // 121 kHz, 3.85 words per line
inline constexpr double   TONE_CENTS_GOAL = 1.0;

struct ToneChoice
{
    uint8_t  period = 0;
    uint16_t length = 0;
    double   cents = 0.0;       // achieved minus wanted
    bool     audible = false;
};

// The reference's band limit: STEP[N] >= 1 flip per sample.
bool tone_band_limited(uint16_t effective_period);
double tone_hz(uint16_t effective_period);
ToneChoice choose_tone(uint16_t effective_period);

// Noise: the shift rate for control bits 1:0 and tone 2's effective period,
// and the (PERIOD, DUP) that plays a table at it.
double noise_shift_hz(uint8_t noise_control, uint16_t tone2_effective_period);
struct NoiseChoice
{
    uint8_t  period = 0;
    uint32_t dup = 1;
};
NoiseChoice choose_noise(double shift_hz);

uint8_t volume_for_attenuation(uint8_t attenuation);

class PsgChannels
{
public:
    PsgChannels() { reset(); }

    void reset();
    void write(uint8_t byte);
    void emit(Scheduler& scheduler);

    const std::vector<uint8_t>& square_table(uint16_t length);
    const std::vector<uint8_t>& noise_table(bool white, uint32_t dup);
    size_t table_bytes() const;
    size_t table_count() const { return squares_.size() + noises_.size(); }

    // Registers, as sn76489.h names them.
    uint16_t period_reg[3]{};
    uint8_t  atten[4]{};
    uint8_t  noise_control{};
    uint8_t  latch{};

    uint16_t effective_period(int c) const { return period_reg[c] ? period_reg[c] : 1024; }

    // For the A/B report.
    std::map<uint16_t, ToneChoice> tone_choices;    // by effective period, audible tones only
    uint64_t tone_retargets = 0;
    uint64_t noise_restarts = 0;
    uint64_t noise_retargets = 0;

private:
    struct Shadow
    {
        bool     active = false;
        uint16_t psg_period = 0;
        uint8_t  period = 0xFF;
        uint32_t length = 0;
        uint8_t  volume = 0xFF;
        bool     white = false;
        uint32_t dup = 0;
    };
    Shadow shadow_[CHANNELS];
    bool noise_reseed_ = false;
    std::map<uint16_t, std::vector<uint8_t>> squares_;
    std::map<uint32_t, std::vector<uint8_t>> noises_;   // key: dup << 1 | white
};

}   // namespace griffin::audio
