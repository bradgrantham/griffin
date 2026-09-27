// mixer.h — MIXER, the time-shared voice chip of wave-table-design.md §2, as an
// object the A/B tool, the suite and the emulator can hold and tick.
//
// ONE IMPLEMENTATION.  This is the shared model in the sense of
// super-engine/render.h: the RTL (cpld/mixer/, later) is written against it and
// its testbench must agree with it tick for tick.  Where §2 is silent this file
// decides, says so with "(decided during step 1)", and the same decision is in
// wave-table-design.md §2.
//
// Bare-metal constraints, because the 68000 app and the emulator both compile
// it: no allocation, no I/O, no exceptions, no floating point, nothing beyond
// <cstdint>, <cstddef> and <array>.
//
// ---------------------------------------------------------------------------
// THE TICK.  One AUDIO_TICK (TIMING, 25175000/1600 = 15,734.375 Hz) is one call
// of tick(), which does, in this order:
//
//   1. AUDCMD.  If the WAIT counter is nonzero it counts down by one.  If it is
//      then zero, records are consumed until a WAIT n (n > 0) or an empty
//      queue:
//        SET  {1, addr[6:0], v[7:0]}   SET-window byte addr = v, no time
//        WAIT {0, ticks[14:0]}         counter = ticks; WAIT 0 is a no-op
//      So a WAIT n consumed at the boundary of tick k lets ticks k .. k+n-1
//      play, and the next record is consumed at the boundary of tick k+n
//      (decided during step 1: "n more ticks elapse" counts the tick the WAIT
//      was consumed in).  An empty queue changes nothing: voices keep sounding.
//      BOUNDED (amended 2026-09-26): at most MAX_RECORDS_PER_BOUNDARY records
//      (SETs and WAITs alike; each is one FIFO read in the idle gap between
//      ticks) are consumed at one boundary.  If the limit is reached with the
//      counter still zero, the remaining records slide to the next boundary,
//      a tick later, and Stats::limited_boundaries counts it.
//   2. SAMPLES.  One word {L[15:8], R[7:0]} is popped if the FIFO is not empty,
//      else both channels take 0.  It seeds the two accumulators.
//   3. VOICES, in index order 0 .. voices()-1, one at a time (the hardware has
//      one adder):  phase += inc (16 bits, wraps; the carry out is kept);
//      then the voice's sample by MODE; then the sample is added into each
//      channel its PAN bits enable, saturating to -128..+127 on every add.
//      The LFSR voice's carry is LATCHED here, not acted on.
//   4. The two accumulators are the tick's {L, R}, latched into the DACs.
//      Then, if the latched carry is set, the LFSR shifts once (amended
//      2026-09-26: the shift takes effect at the END of the tick).
//
// THE HARDWARE IS ONE ACCUMULATOR, TWO PASSES (amended 2026-09-26).  The RTL
// (cpld/mixer/) is stereo with ONE saturating accumulator walked over the
// voices twice per tick: pass 0 advances every phase, sums the PAN_L voices
// and latches the left DAC; pass 1 re-reads PHASE_HI, CTL and AMP, sums the
// PAN_R voices and latches the right DAC.  This model keeps two accumulators
// and one pass, and the two are bit-identical, because:
//   * the channels never interact: each is its own PCM seed plus its own
//     ordered, saturating sum, and saturation is per channel, so the order of
//     the two sums against each other cannot matter;
//   * pass 1 reads the phase pass 0 wrote, which is the NEW phase this model
//     samples, and CTL/AMP cannot change between the passes (SETs execute only
//     at the boundary);
//   * the LFSR shifts only after both passes (step 4), so both passes read the
//     same lfsr[0];
//   * the PCM seed is one SAMPLES word either way: the hardware pops the
//     pair's left chip at the start of pass 0 and the right chip at the start
//     of pass 1, and the two chips advance together.
// What the model does NOT represent: the right latch follows the left by
// about 100 SYSCLK, about 7 us (§2's figure for the two-pass form).  The
// model latches both at once; the skew is a constant sub-sample delay on R, inaudible and
// invisible at the tick granularity every consumer of this model works in.
//
// THE SET WINDOW (amended 2026-09-26, provisional for step 5 to confirm).  A
// SET's 7-bit address reaches 128 bytes:
//
//   0x00 .. 0x77  voices 0 .. 14, 8 bytes each (addr = voice*8 + i); all are
//                 addressable whatever voices() is, only 0 .. voices()-1 play
//   0x78          LFSR_VOICE_SELECT: bits 3:0 name the LFSR voice; a value
//                 naming a voice that is not played (>= voices(), so 15
//                 always) means NO voice drives the LFSR and it never shifts.
//                 Bits 7:4 are stored and ignored (decided in the model).
//   0x79 .. 0x7F  MIXER control, reserved: stored, no effect
//
// So at most 15 voices (MAX_VOICES).  The select byte is SRAM like the rest
// and NOT reset: power-on leaves it garbage, and whoever owns the chip must
// SET it before relying on noise (psg_driver does so in its first emit).
//
// HARDWARE-PRIVATE STATE (amended 2026-09-26).  The SRAM is 8K; the SET window
// is only its first 128 bytes.  State the RTL keeps for itself lives at SRAM
// addresses >= 0x80, which no SET can reach: in particular the WAIT counter
// is a 16-bit SRAM field there, walked by the phase adder toward all-ones
// (loaded as ~n, terminal = carry out) rather than a flip-flop counter, and
// wave table pages (MODE 2) sit there too.  This model's ram_ is ONLY the
// 128-byte SET window; it keeps the WAIT counter as the plain member wait_
// with the contract of THE TICK step 1, which the SRAM field implements
// unchanged, and it does not model the private addresses or their layout.
// Consequently /RS "clears the WAIT counter" in the model; the RTL must give
// the SRAM field the same effect (a counter that reads as expired).
//
// A VOICE (8 bytes):
//
//   +0 INC_LO  +1 INC_HI   phase increment per tick
//   +2 AMP                 signed 8-bit amplitude
//   +3 CTL                 bit0 PAN_L, bit1 PAN_R, bits 3:2 MODE, bit4 RESET_PHASE
//   +4 PHASE_LO +5 PHASE_HI  hardware-owned, but a SET is honoured
//   +6 WAVE_PAGE           MODE 2 only
//   +7 reserved            stored, no effect
//
// MODE (bits 3:2 of CTL):
//   0 square     sample = phase[15] ? +AMP : -AMP
//   1 noise      sample = lfsr[0] ? +AMP : -AMP; when this voice is the LFSR
//                voice, WHITE feedback
//   2 wavetable  RUNG v, NOT IN v0.  The phase still advances; the voice adds
//                nothing and is counted in Stats::wavetable_ticks.
//   3 noise      as 1, but when this voice is the LFSR voice, PERIODIC feedback
//                (decided during step 1: §2 gives the LFSR two feedback modes
//                and no bit to choose between them; MODE 3 was unassigned)
//
// "±AMP" is an add or a subtract of the signed AMP byte into the accumulator,
// not an 8-bit negate: -AMP of -128 is +128 and saturates like any other add
// (decided during step 1).
//
// RESET_PHASE.  A SET of CTL with bit 4 set stores CTL with bit 4 clear and
// zeroes the voice's PHASE at that instant, so the first add after it gives
// phase = inc.  When the voice is the LFSR voice (per LFSR_VOICE_SELECT as it
// stands when the SET executes) it also reseeds the LFSR to
// LFSR_SEED (decided during step 1: this is the SN76489's "a noise control
// write reseeds the shift register", and §2 had no other way to reseed).  SETs
// in one boundary take effect in queue order, so a PHASE SET after the
// RESET_PHASE wins.
//
// THE LFSR.  One in the chip: 15 bits, the SN76489AN's.  It shifts right once
// per tick in which the LFSR voice's phase add carried out, provided that
// voice's MODE is 1 or 3; white feeds lfsr[0] ^ lfsr[1] into bit 14, periodic
// feeds lfsr[0] (the 15-bit ring).  The shift takes effect at the END of the
// tick (amended 2026-09-26, replacing step 1's "a later voice sees this tick's
// shift"): the carry is latched during the voice walk and the register shifts
// once after it, so EVERY voice in a tick, before or after the LFSR voice and
// the LFSR voice itself, reads ONE lfsr[0], and a carry in tick k is first
// heard in tick k+1.  (In the two-pass hardware this is what lets pass 1
// re-read the same value pass 0 did.)  WHICH voice drives it is the control
// byte LFSR_VOICE_SELECT (0x78), read every tick.  A 16-bit INC gives at most
// one carry, so at most one shift,
// per tick: a shift rate above fs is not expressible (see psg_driver.h).
//
// POWER-ON AND /RS (decided during step 1).  The SET window is an SRAM that
// nothing resets, so reset_power_on() fills it with a SEEDED pseudo-random
// pattern, as super-engine's PaletteUnit does: software that forgets a voice,
// or LFSR_VOICE_SELECT, hears garbage here as it would on the bench.  /RS
// clears the WAIT counter to 0 (an SRAM field in the RTL, see above) and
// seeds the LFSR to LFSR_SEED, and the 7200s' /RS empties both FIFOs.
//
// FIFOs.  AUDCMD and SAMPLES are each a 7200 pair, 256 words.  A push into a
// full FIFO is lost and counted (the 7200 ignores /W when full).
//
// AMPLITUDE BUDGETING is the driver's business: nothing here scales.  PSG
// content at peak 31 is 4 x 31 = 124 and cannot saturate; at the smurf app's
// peak 60 it can (README.md, "Peak 60").

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace griffin::mixer
{

inline constexpr int MAX_VOICES       = 15;
inline constexpr int DEFAULT_VOICES   = 8;
inline constexpr int VOICE_BYTES      = 8;
inline constexpr int SET_WINDOW_BYTES = 128;   // the 7-bit SET address space

// MIXER control bytes, after the last voice.
inline constexpr uint8_t CONTROL_BASE      = MAX_VOICES * VOICE_BYTES;   // 0x78
inline constexpr uint8_t LFSR_VOICE_SELECT = CONTROL_BASE + 0;           // bits 3:0
inline constexpr uint8_t LFSR_VOICE_MASK   = 0x0F;
static_assert(CONTROL_BASE == 0x78 && CONTROL_BASE + 8 == SET_WINDOW_BYTES);

// The most AUDCMD records one tick boundary consumes (see THE TICK, step 1).
// PROVISIONAL UNTIL RUNG (ii) MEASURES IT: the RTL executes records only in
// the idle gap between ticks (about 890 - 268 SYSCLK in the two-pass form,
// less the FIFO read cost per record); 64 is a placeholder, not a derivation.
inline constexpr uint32_t MAX_RECORDS_PER_BOUNDARY = 64;

inline constexpr size_t AUDCMD_FIFO_WORDS  = 256;
inline constexpr size_t SAMPLES_FIFO_WORDS = 256;
static_assert(AUDCMD_FIFO_WORDS == 256 && SAMPLES_FIFO_WORDS == 256, "WordFifo is a 7200 pair");

// Byte offsets inside a voice.
inline constexpr uint8_t V_INC_LO    = 0;
inline constexpr uint8_t V_INC_HI    = 1;
inline constexpr uint8_t V_AMP       = 2;
inline constexpr uint8_t V_CTL       = 3;
inline constexpr uint8_t V_PHASE_LO  = 4;
inline constexpr uint8_t V_PHASE_HI  = 5;
inline constexpr uint8_t V_WAVE_PAGE = 6;
inline constexpr uint8_t V_RESERVED  = 7;

// CTL bits.
inline constexpr uint8_t CTL_PAN_L       = 0x01;
inline constexpr uint8_t CTL_PAN_R       = 0x02;
inline constexpr uint8_t CTL_MODE_SHIFT  = 2;
inline constexpr uint8_t CTL_MODE_MASK   = 0x0C;
inline constexpr uint8_t CTL_RESET_PHASE = 0x10;

inline constexpr uint8_t MODE_SQUARE         = 0;
inline constexpr uint8_t MODE_NOISE_WHITE    = 1;
inline constexpr uint8_t MODE_WAVETABLE      = 2;
inline constexpr uint8_t MODE_NOISE_PERIODIC = 3;

constexpr uint8_t ctl(uint8_t mode, bool pan_l, bool pan_r, bool reset_phase = false)
{
    return static_cast<uint8_t>((mode << CTL_MODE_SHIFT) | (pan_l ? CTL_PAN_L : 0)
                                | (pan_r ? CTL_PAN_R : 0) | (reset_phase ? CTL_RESET_PHASE : 0));
}

// The SN76489AN register: 15 bits, seeded with a single 1 in the top bit.
inline constexpr uint16_t LFSR_SEED = 0x4000;

inline constexpr uint32_t POWER_ON_SEED = 0x6D495845;   // "MIXE"

// AUDCMD records.
inline constexpr uint16_t AUDCMD_SET_BIT   = 0x8000;
inline constexpr uint16_t WAIT_MAX_TICKS   = 0x7FFF;

constexpr uint16_t wait_word(uint16_t ticks)
{
    return static_cast<uint16_t>(ticks & WAIT_MAX_TICKS);
}

constexpr uint16_t set_word(uint8_t addr, uint8_t value)
{
    return static_cast<uint16_t>(AUDCMD_SET_BIT | ((addr & 0x7F) << 8) | value);
}

constexpr uint8_t voice_addr(int voice, uint8_t offset)
{
    return static_cast<uint8_t>((voice * VOICE_BYTES + offset) & (SET_WINDOW_BYTES - 1));
}

struct StereoSample
{
    int8_t left  = 0;
    int8_t right = 0;
};

// Counters.  Nothing here is an error by itself; the caller decides.
struct Stats
{
    uint32_t ticks            = 0;
    uint32_t sets             = 0;   // SET records executed
    uint32_t waits            = 0;   // WAIT records consumed, WAIT 0 included
    uint32_t empty_boundaries = 0;   // a boundary that wanted a record and found none
    uint32_t limited_boundaries = 0; // a boundary stopped by MAX_RECORDS_PER_BOUNDARY
    uint32_t max_records_per_boundary = 0;
    uint32_t max_sets_per_boundary    = 0;
    uint32_t audcmd_overflows = 0;
    uint32_t samples_popped   = 0;
    uint32_t samples_empty    = 0;   // ticks with no SAMPLES word (PCM term 0)
    uint32_t samples_overflows = 0;
    uint32_t saturations      = 0;   // adds that clipped, either channel
    uint32_t lfsr_shifts      = 0;
    uint32_t wavetable_ticks  = 0;   // voice-ticks spent in the rung-v stub
};

// A 7200 pair: 256 words, first in first out.
class WordFifo
{
public:
    bool push(uint16_t word);
    bool pop(uint16_t& word);
    size_t size() const { return count_; }
    bool empty() const { return count_ == 0; }
    void clear() { head_ = 0; count_ = 0; }

private:
    std::array<uint16_t, 256> words_{};   // a 7200 pair; AUDCMD and SAMPLES are the same part
    size_t head_  = 0;
    size_t count_ = 0;
};

class Mixer
{
public:
    // voices is clamped to 1 .. MAX_VOICES.
    explicit Mixer(int voices = DEFAULT_VOICES);

    // SRAM garbage (seeded), then /RS.
    void reset_power_on(uint32_t seed = POWER_ON_SEED);

    // /RS alone: WAIT counter 0, LFSR seeded, both FIFOs empty.  The voice RAM
    // is untouched.
    void reset();

    // ENGINE's writes.  false = the FIFO was full and the word is lost.
    bool push_audcmd(uint16_t word);
    bool push_samples(uint16_t word);

    // One AUDIO_TICK.
    StereoSample tick();

    // Inspection, for tests and the A/B tool.
    int      voices() const { return voices_; }
    // The voice LFSR_VOICE_SELECT names, or -1 when it names none played.
    int      lfsr_voice() const;
    uint8_t  ram(uint8_t addr) const { return ram_[addr & 0x7F]; }
    uint16_t inc(int voice) const;
    uint16_t phase(int voice) const;
    uint16_t lfsr() const { return lfsr_; }
    uint16_t wait_remaining() const { return wait_; }
    size_t   audcmd_size() const { return audcmd_.size(); }
    size_t   samples_size() const { return samples_.size(); }

    Stats stats{};

private:
    void execute_set(uint8_t addr, uint8_t value);
    void consume_records();
    void shift_lfsr(bool white);

    std::array<uint8_t, SET_WINDOW_BYTES> ram_{};
    WordFifo audcmd_{};
    WordFifo samples_{};
    int      voices_     = DEFAULT_VOICES;
    uint16_t wait_       = 0;
    uint16_t lfsr_       = LFSR_SEED;
};

}   // namespace griffin::mixer
