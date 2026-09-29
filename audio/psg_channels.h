// psg_channels.h — SN76489AN port bytes in, AUDIO channel streams out.
//
// The 68000 side of an SN76489 port on the AUDIO chip.  Bare metal:
// <cstdint>/<cstddef> only, no allocation, no floating point, no I/O; every
// choice that needs a search or a logarithm is a lookup in psg_tables.h,
// generated on the host by psg_tables_gen.cpp.  Memory: this object (a few
// hundred bytes) plus one shared 32 KB white-noise sample table built on
// first use.
//
//   tone c (0..2) -> channel c: a square table of LENGTH samples played at
//                    PERIOD, so 1,573,437.5 / (PERIOD+1) / LENGTH is the
//                    tone's 3,579,545 / (32 N) Hz, within a cent where the
//                    integers allow (TONE[N]).  All square tables are windows
//                    of one 512-byte buffer (256 highs then 256 lows).  Tones
//                    the reference band-limits (N <= 14) are silenced.
//   noise         -> channel 3: the shift register's output sequence (white:
//                    the 15-bit LFSR, 32,767 bits; periodic: 15), each bit
//                    deposited REPEAT times when the shift rate is below the
//                    chip's 6.15 kHz floor (NOISE[reload]); the sequence is
//                    cut at 65,536 samples, as the reference driver's tables
//                    were.  A noise control write restarts it (the chip
//                    reseeds); a rate-3 tone-2 change re-rates it in place.
//   attenuation a -> VOLUME[a], 2 dB steps, 15 -> 0.
//
// Samples are offset binary, 0x80 +/- 127.  On a frequency change the new table
// starts at the old one's phase fraction, and the Scheduler lands the PERIOD
// record when the old samples have drained.
//
// Use: write() every byte the game sends to the PSG port; once per game tick
// (after its bytes) call emit(scheduler); then run scheduler.line() for every
// scanline and turn its deposits into ENGINE descriptors.

#pragma once

#include <cstddef>
#include <cstdint>

#include "scheduler.h"

namespace griffin::audio
{

inline constexpr int      NOISE_CHANNEL   = 3;
inline constexpr uint32_t NOISE_TABLE_CAP = 65536;   // samples, the old driver's table cap
inline constexpr uint32_t PSG_CLOCK_HZ    = 3579545;

class PsgChannels
{
public:
    PsgChannels() { reset(); }

    // SN76489 power-on registers; the next emit() sets every channel.
    void reset();

    // One byte of the PSG port stream, decoded as sn76489.cpp's Psg::write().
    void write(uint8_t byte);

    // Push the registers' implied streams, periods and volumes to the
    // Scheduler.  Call once per game tick, after that tick's writes.
    void emit(Scheduler& scheduler);

    // Registers, as sn76489.h names them.
    uint16_t period_reg[3]{};
    uint8_t  atten[4]{};
    uint8_t  noise_control{};
    uint8_t  latch{};

    uint16_t effective_period(int c) const { return period_reg[c] ? period_reg[c] : 1024; }

    // Counters and a record of which tone periods sounded, for reports.
    uint32_t tone_retargets = 0;
    uint32_t noise_restarts = 0;
    uint32_t noise_retargets = 0;
    uint8_t  periods_used[1024 / 8]{};   // bit N%1024: tone period N sounded

private:
    // What has been sent to the Scheduler for a channel, so emit() only
    // sends a PERIOD or VOLUME record when the value changes.  PERIOD is
    // 8 bits, so NO_PERIOD (0x100) can never equal a chosen one.  VOLUME is
    // 6 bits, so 0xFF is a safe "none yet" there.
    static constexpr uint16_t NO_PERIOD = 0x100;
    struct Shadow
    {
        bool     active = false;
        uint16_t psg_period = 0;         // tones: effective period; noise: reload
        uint16_t period = NO_PERIOD;     // last PERIOD record sent
        uint32_t length = 0;             // table length in samples (length x repeat)
        uint8_t  volume = 0xFF;          // last VOLUME record sent
        bool     white = false;
        uint8_t  repeat = 0;
    };
    Shadow shadow_[CHANNELS];
    bool noise_reseed_ = false;
};

// The square and noise sample tables (static storage, shared by every
// PsgChannels): the square buffer is constant; the noise table is built from
// psg_tables.h on first call.
const uint8_t *square_table(uint16_t length);
const uint8_t *noise_samples(bool white);
uint32_t noise_bits(bool white, uint8_t repeat);    // sequence length in table bytes

}   // namespace griffin::audio
