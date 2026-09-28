// audio_chip.h — host model of the AUDIO CPLD (cpld/audio/audio.v) and its
// five IDT7200 FIFOs.  This is THE SPEC of what the chip does, kept
// equation-for-equation with the RTL (the video chain's render.h rule); the
// testbench and the emulator are checked against it, never the other way.
//
// Bare metal capable: <cstdint>, <cstddef>, <array> only; no allocation, I/O,
// exceptions or floating point in this header or audio_chip.cpp.
//
// ---------------------------------------------------------------------------
// THE LAWS (audio.v header, griffin.log 2026-09-27)
//
// Time.  CLK is the 25.175 MHz pixel clock.  A 9-bit timebase counts CLK:
// bits 3:0 make a STEP every 16 clocks (1,573,437.5 Hz), bits 8:3 are the
// 64-tick volume FRAME (a tick every 8 clocks, a frame every 512 = 49.17
// kHz).  PIXEL's HBLANK rising edge loads the low nibble once per line so
// that the step edges fall at h_cnt 0 mod 16: 50 steps per 800-clock line,
// locked to the raster (a no-op once in phase, so the frame counter is
// undisturbed).  Two lines are 100 steps.
//
// Channels 0..3.  Each has an 8-bit PERIOD register and an 8-bit down counter
// COUNT.  At every step: if COUNT == 0 the channel POPS (its FIFO's /R rises
// for one clock: the 7200 advances to the next byte, or ignores the read if
// it is empty) and COUNT <= PERIOD; otherwise COUNT <= COUNT - 1.  So a
// channel's sample rate is 1,573,437.5 / (PERIOD + 1) Hz; PERIOD 99 is the
// 15,734.375 Hz tick exactly, PERIOD 49 is 31.47 kHz, PERIOD 255 is 6.15 kHz.
// The FIFO's Q bus, held valid with /R low, IS the channel's current sample
// (two's complement, 0x00 = silence), driving that channel's ladder through
// a 74HC04 on bit 7, since the ladder wants offset binary.
// Reset: COUNT 0 and PERIOD 0, so an unconfigured channel pops every step;
// set PERIOD before depositing samples.
//
// Volume.  Each channel has a 6-bit VOLUME.  GATE[n] closes the CD4066 switch
// between ladder n and its side's summing node (channels 0,1 left; 2,3
// right).  On frame tick t (0..63) the gate is on when any VOLUME bit b is set
// and t is in window b:
//     window 5: t odd            (32 ticks)     window 2: t mod 16 == 8  (4)
//     window 4: t mod 4 == 2     (16)           window 1: t mod 32 == 16 (2)
//     window 3: t mod 8 == 4     (8)            window 0: t == 32        (1)
// The windows are disjoint and tick 0 is in none, so the on-count per frame
// is exactly VOLUME (of 64) and 63 is the loudest.  The gate is forced off
// while the channel's FIFO is empty and during its pop clock (Q floats then).
// The analog stage averages the gate, so the channel's contribution is
// sample * VOLUME / 64 plus components at multiples of 49.17 kHz.
//
// Control.  A fifth byte-wide FIFO carries two-byte records {register,
// value}: registers 0..3 are PERIOD of channel n, 4..7 VOLUME of channel n
// (low six bits).  The reader consumes a byte in four clocks whenever the
// FIFO is not empty (/R low two clocks, high one), so a complete record
// deposited at clock 0 of a step is written by clock 8 and is in force at
// that step's counter update.  A record split by an empty FIFO waits.
//
// Deposits.  ENGINE writes a word per FIFO byte (the low byte; the high byte
// is ignored) to the FIFO its descriptor's destination code names.  A write to a full 7200 is
// ignored (samples lost); a pop of an empty one is ignored (the channel
// holds its Q while /R is low, but the gate is off).  Nothing is read back.
// ---------------------------------------------------------------------------

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace griffin::audio
{

inline constexpr uint32_t CLK_HZ            = 25175000;
inline constexpr int      CHANNELS          = 4;
inline constexpr int      FIFO_DEPTH        = 256;
inline constexpr int      STEP_CLOCKS       = 16;
inline constexpr int      FRAME_TICK_CLOCKS = 8;
inline constexpr int      FRAME_TICKS       = 64;
inline constexpr int      TICKS_PER_STEP    = STEP_CLOCKS / FRAME_TICK_CLOCKS;  // 2
inline constexpr int      STEPS_PER_FRAME   = FRAME_TICKS / TICKS_PER_STEP;     // 32
inline constexpr int      CLOCKS_PER_LINE   = 800;
inline constexpr int      STEPS_PER_LINE    = CLOCKS_PER_LINE / STEP_CLOCKS;    // 50
inline constexpr int      LINES_PER_TICK    = 2;                                // AUDIO_TICK
inline constexpr int      PERIOD_MAX        = 255;
inline constexpr int      PERIOD_TICK       = 99;      // 15,734.375 Hz
inline constexpr int      VOLUME_MAX        = 63;
inline constexpr uint8_t  SILENCE           = 0x00;    // two's complement
inline constexpr uint8_t  REG_PERIOD        = 0;       // + channel
inline constexpr uint8_t  REG_VOLUME        = 4;       // + channel
inline constexpr int      CONTROL_CLOCKS_PER_BYTE = 4;

// Steps per second, and a channel's sample rate, as exact rationals.
inline constexpr uint32_t STEP_HZ_NUMERATOR   = CLK_HZ;      // / STEP_CLOCKS
inline constexpr uint32_t STEP_HZ_DENOMINATOR = STEP_CLOCKS; // 1,573,437.5 Hz

constexpr bool rate_multiplier_on(uint8_t volume, uint8_t tick)
{
    const uint8_t t = tick & 63;
    const bool window5 = (t & 1) != 0;
    const bool window4 = (t & 3) == 2;
    const bool window3 = (t & 7) == 4;
    const bool window2 = (t & 15) == 8;
    const bool window1 = (t & 31) == 16;
    const bool window0 = t == 32;
    return ((volume & 32) && window5) || ((volume & 16) && window4) ||
           ((volume & 8) && window3) || ((volume & 4) && window2) ||
           ((volume & 2) && window1) || ((volume & 1) && window0);
}

// One IDT7200, byte wide, 256 deep.  Writes when full and reads when empty
// are ignored, as the part does.
struct ByteFifo
{
    std::array<uint8_t, FIFO_DEPTH> data{};
    uint16_t head = 0;      // next byte to read
    uint16_t tail = 0;      // next byte to write
    uint16_t count = 0;

    void reset()
    {
        head = tail = count = 0;
    }

    bool empty() const
    {
        return count == 0;
    }

    bool full() const
    {
        return count == FIFO_DEPTH;
    }

    bool push(uint8_t byte)
    {
        if (full())
        {
            return false;
        }
        data[tail] = byte;
        tail = static_cast<uint16_t>((tail + 1) & (FIFO_DEPTH - 1));
        ++count;
        return true;
    }

    bool pop()
    {
        if (empty())
        {
            return false;
        }
        head = static_cast<uint16_t>((head + 1) & (FIFO_DEPTH - 1));
        --count;
        return true;
    }

    // Q while /R is low: the byte at the head (undefined when empty; the
    // model returns the byte last read so a render has something to hold).
    uint8_t front() const
    {
        return data[head];
    }
};

struct ChipStats
{
    uint64_t pops[CHANNELS]{};
    uint64_t pops_empty[CHANNELS]{};          // underruns: pop with nothing to pop
    uint64_t deposits_dropped[CHANNELS]{};    // writes to a full sample FIFO
    uint64_t control_dropped = 0;
    uint64_t control_records = 0;
    uint16_t max_fill[CHANNELS]{};
    uint16_t max_control_fill = 0;
};

class AudioChip
{
public:
    AudioChip() { reset(); }

    void reset();

    // ENGINE deposits: the low byte of a word into sample FIFO `channel`, or
    // one byte of a control record.  Return false if the FIFO was full.
    bool deposit_sample(int channel, uint8_t byte);
    bool deposit_control(uint8_t byte);

    // Advance 16 clocks: the control reader runs, then the counters step.
    void step();

    // Observation (the pins, and the registers a DUMP-less chip never shows).
    uint8_t  sample(int channel) const;          // the Q bus (held value when empty)
    bool     fifo_empty(int channel) const { return fifo_[channel].empty(); }
    int      fill(int channel) const { return fifo_[channel].count; }
    int      control_fill() const { return control_.count; }
    uint8_t  period(int channel) const { return period_[channel]; }
    uint8_t  volume(int channel) const { return volume_[channel]; }
    uint8_t  count(int channel) const { return count_[channel]; }
    bool     popped_this_step(int channel) const { return popped_[channel]; }
    uint64_t steps() const { return steps_; }

    // The gate as the analog stage sees it over the step just taken: the
    // number of its two frame ticks the switch was closed (0, 1 or 2), zero
    // while the FIFO is empty; the pop clock is 1 of 16 and is not modelled
    // here (it is a fixed -0.06% at the pop rate).
    int gate_ticks_on(int channel) const;

    ChipStats stats{};

private:
    void run_control_clocks(int clocks);

    std::array<ByteFifo, CHANNELS> fifo_{};
    ByteFifo control_{};
    std::array<uint8_t, CHANNELS> period_{};
    std::array<uint8_t, CHANNELS> volume_{};
    std::array<uint8_t, CHANNELS> count_{};
    std::array<uint8_t, CHANNELS> held_{};        // last byte seen on Q, for renders
    std::array<bool, CHANNELS> popped_{};
    uint64_t steps_ = 0;

    // Control reader, as audio.v: IDLE, READ_0, READ_1, ADVANCE.
    enum class ControlState : uint8_t { Idle, Read0, Read1, Advance };
    ControlState control_state_ = ControlState::Idle;
    bool    control_have_register_ = false;
    uint8_t control_register_ = 0;
};

// Analog side: two summing nodes through the output stage's RC corner,
// rendered per step.  Host only (floating point); not part of the spec, a
// listening aid.  Units: one 8-bit LSB at VOLUME 63 of one channel = 1.0.
struct AnalogRender
{
    double left = 0.0;
    double right = 0.0;
    double alpha = 1.0;     // one-pole coefficient per step

    // corner_hz: the output stage's single-pole RC (reminders.md: ~9.7 kHz).
    void configure(double corner_hz);
    void step(const AudioChip& chip);
};

}   // namespace griffin::audio
