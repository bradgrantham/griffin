// psg_driver.h — SN76489AN port bytes in, MIXER SET words out.
//
// The 68000 app runs this in place of the software SN76489 once MIXER exists
// (wave-table-design.md §3 step 8); the A/B tool runs the same source on the
// host.  Same constraints as mixer.h: no allocation, I/O, exceptions or
// floating point, and no division at run time — the INC table and the AMP
// ladder are built by the compiler (consteval), as sn76489.cpp builds STEP.
//
// ---------------------------------------------------------------------------
// THE MAPPING (wave-table-design.md §2, "PSG driver")
//
//   tone c (0..2)  -> voice c,          MODE square
//   noise          -> voice NOISE_VOICE (3), MODE 1 (white, control bit 2
//                     set) or 3 (periodic); the driver makes it the LFSR voice
//                     by SETting LFSR_VOICE_SELECT (the chip has no default:
//                     the select byte is unreset SRAM)
//   every voice    PAN_L | PAN_R (the PSG is mono)
//
//   period register N (10 bits, 0 means 1024)
//                  -> INC = round(3579545 / (32 N) * 65536 / 15734.375),
//                     table INC[N & 0x3FF], 1024 entries
//   attenuation a  -> AMP = round(PEAK * 10^(-a/10)), a = 15 -> 0; PEAK 31 =
//                     the reference's DEFAULT_PEAK_PER_CHANNEL, and the ladder
//                     is built from the reference's own Q32 2 dB ratio, so the
//                     A/B is at equal loudness
//   noise control  -> noise voice INC: reloads 0x10/0x20/0x40, or tone 2's
//                     effective period for rate 3 (re-emitted when tone 2's
//                     period changes); MODE from bit 2; and CTL with
//                     RESET_PHASE on EVERY control write, because the SN76489
//                     reseeds its shift register on every control write
//
// PERIODS 1..7 (decided during step 1).  f(N) >= fs there, so the true INC is
// 65536 or more and does not fit in 16 bits.
//   * A tone voice stores INC mod 65536.  That is exact, not an approximation:
//     phase[15] depends only on phase mod 65536, so the square is the same
//     point-sampled alias the reference renders.
//   * The noise voice cannot do that: its LFSR shifts once per CARRY, and mod
//     65536 would throw carries away.  It stores 0xFFFF instead (one shift on
//     all but one tick in 65536), the fastest rate a 16-bit INC can express.
//     That is a real deviation from the SN76489, which shifts up to about 1.8
//     times a sample at rate 3 with tone 2 at period 4 (Smurf's jump).
//
// DIFF EMISSION.  write() only updates the PSG registers.  emit() compares the
// voice bytes those registers imply against a shadow of what MIXER already
// holds and writes a SET for each byte that differs, so a frame's nine PSG
// bytes become a handful of SETs.  After reset() the shadow is invalid and the
// first emit() writes LFSR_VOICE_SELECT = NOISE_VOICE FIRST (so the noise
// voice's RESET_PHASE below reseeds the LFSR), then every byte of all four
// voices with RESET_PHASE (phase 0, LFSR seeded), which is the SN76489's
// power-on state: all attenuations 15, periods 1024, noise control 0.  All SETs of one emit() belong in one AUDCMD
// boundary (no WAIT between them): the two INC bytes then change together.
//
// Voices 4 .. 7 are not the driver's; silence_voices() writes AMP 0 and CTL 0
// for a range of them so SRAM power-on garbage stays quiet.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "mixer.h"

namespace griffin::mixer::psg
{

inline constexpr uint32_t PSG_CLOCK_HZ            = 3579545;
inline constexpr uint32_t SAMPLE_RATE_NUMERATOR   = 25175000;
inline constexpr uint32_t SAMPLE_RATE_DENOMINATOR = 1600;

// griffin-ports/smurf/app/audio/sn76489.h DEFAULT_PEAK_PER_CHANNEL, 31, the
// only value at which four voices cannot saturate (4 x 31 = 124).  The build
// may override it (-DMIXER_PSG_PEAK=60, `make test-peak60`) to measure the
// smurf app's louder clamp; the ladder is still built by the compiler.
#ifndef MIXER_PSG_PEAK
#define MIXER_PSG_PEAK 31
#endif
inline constexpr int PEAK_PER_CHANNEL = MIXER_PSG_PEAK;
static_assert(PEAK_PER_CHANNEL > 0 && PEAK_PER_CHANNEL <= 127);

inline constexpr int PERIODS     = 1024;   // INC table entries, indexed by the raw 10-bit register
inline constexpr int NOISE_VOICE = 3;       // the LFSR voice, by the first emit's select SET
inline constexpr int PSG_VOICES  = 4;

// The most SETs one emit() can produce: LFSR_VOICE_SELECT, then every byte a
// voice's registers imply (INC_LO, INC_HI, AMP, CTL) for all four voices.
inline constexpr size_t MAX_SETS_PER_EMIT = 1 + PSG_VOICES * 4;

// The increment of period N before the 16-bit store: the
// compile-time formula, exposed so tests and the A/B can see the unwrapped
// value.  N = 1..1024.
constexpr uint32_t true_inc(uint32_t period)
{
    const uint64_t numerator = 65536ull * PSG_CLOCK_HZ * SAMPLE_RATE_DENOMINATOR;
    const uint64_t denominator = 32ull * SAMPLE_RATE_NUMERATOR * period;
    return static_cast<uint32_t>((numerator + denominator / 2) / denominator);
}

// Below this period the true INC is 65536 or more (see PERIODS 1..7 above).
inline constexpr uint32_t FIRST_REPRESENTABLE_PERIOD = 8;
static_assert(true_inc(FIRST_REPRESENTABLE_PERIOD - 1) >= 0x10000u);
static_assert(true_inc(FIRST_REPRESENTABLE_PERIOD) < 0x10000u);

// The stored tables.  INC is indexed by the raw period register (0 = 1024).
uint16_t inc_for_register(uint16_t period_register);
int8_t   amp_for_attenuation(uint8_t attenuation);

// SETs that silence voices [first, last): AMP 0 and CTL 0 each.  Returns the
// words written; out must hold 2 * (last - first).
size_t silence_voices(int first, int last, std::span<uint16_t> out);

class PsgDriver
{
public:
    PsgDriver() { reset(); }

    // SN76489 power-on registers; the next emit() writes everything.
    void reset();

    // One byte of the PSG port stream, decoded as sn76489.cpp's Psg::write().
    void write(uint8_t byte);

    // SET words for every voice byte that differs from what MIXER holds.
    // Returns the count.  If `out` is shorter than MAX_SETS_PER_EMIT the rest
    // stays pending and the next emit() writes it.
    size_t emit(std::span<uint16_t> out);

    // PSG registers, public for tests (the same names as sn76489.h).
    uint16_t period_reg[3]{};
    uint8_t  atten[4]{};
    uint8_t  noise_control{};
    uint8_t  latch{};

    // True when the noise voice's rate was clamped (rate 3, tone 2 below
    // FIRST_REPRESENTABLE_PERIOD) at the last emit().
    bool noise_clamped() const { return noise_clamped_; }

private:
    uint16_t noise_inc() const;

    // What MIXER holds for voices 0..3: INC_LO, INC_HI, AMP, CTL.
    bool    select_valid_ = false;          // LFSR_VOICE_SELECT holds NOISE_VOICE
    uint8_t shadow_[PSG_VOICES][4]{};
    bool    shadow_valid_[PSG_VOICES][4]{};
    bool    reset_pending_[PSG_VOICES]{};
    bool    noise_clamped_ = false;
};

}   // namespace griffin::mixer::psg
