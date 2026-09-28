// scheduler.h — the 68000's side of AUDIO: keep four sample FIFOs fed from
// looped tables with per-line deposits, and land control records.
//
// This is the code a Griffin application runs (step 3 makes it 68000-clean;
// today it is host C++23 with the same shape): no floating point, no
// division except by small constants, fixed-capacity output.  It mirrors the
// chip's pop law (audio_chip.h) with the closed form line_pops(), so it knows
// each FIFO's fill without any readback, which the hardware does not have.
//
// Policy (parameters in SchedulerConfig):
//   * Channel k deposits on every BURST_LINES-th line, staggered by channel
//     (and at the very next line when a stream starts), enough bytes that the FIFO still holds MARGIN samples when the next
//     burst arrives: n = pops over the next BURST_LINES lines + MARGIN - fill,
//     capped by the 256-byte FIFO; a queued faster PERIOD is assumed for
//     the prediction so a sweep upward cannot drain the FIFO.  Latency from a table change to the
//     speaker is therefore about (pops per burst + MARGIN) samples.
//   * (Corrected before the first full run: a line is 800 clocks = 50 steps;
//     the first draft said 1600.)
//   * A PERIOD change is queued and its control record is deposited at the
//     first line by which every sample that was already in the FIFO has
//     been popped, so the new rate applies to the new table, not to the
//     tail of the old one (within a line, the mirror's resolution).
//   * VOLUME changes are deposited at the next line.
//   * Control records are two deposits (register, value) to DEST_CONTROL.
//   * The mirror assumes a line's deposits land before its first step.  The
//     step edges themselves are raster-locked in hardware (the chip re-phases
//     its divider from HBLANK every line), so the only approximation is that
//     an ENGINE deposit lands wherever its descriptor executes within the
//     line.  That moves a pop across a line boundary at most; the fill error
//     is bounded by one sample and MARGIN covers it.
//
// Deposits are what the display list carries: a run of bytes to one
// destination is one ENGINE descriptor of at most MAX_DESCRIPTOR_WORDS words
// (SuperEngine::DESC_MAX_COUNT), the byte in each word's low half.

#pragma once

#include <cstddef>
#include <cstdint>

#include "audio_chip.h"

namespace griffin::audio
{

inline constexpr uint8_t DEST_CONTROL         = 4;
inline constexpr int     MAX_DESCRIPTOR_WORDS = 32;
inline constexpr int     MAX_LINE_DEPOSITS    = CHANNELS * FIFO_DEPTH + 4 * CHANNELS;

struct Deposit
{
    uint8_t destination;    // 0..3 sample FIFO, DEST_CONTROL
    uint8_t byte;
};

struct LineDeposits
{
    Deposit items[MAX_LINE_DEPOSITS];
    int count = 0;
    int descriptors = 0;    // runs of one destination, split at MAX_DESCRIPTOR_WORDS

    void clear()
    {
        count = 0;
        descriptors = 0;
        run_destination_ = 0xFF;
        run_words_ = 0;
    }

    void add(uint8_t destination, uint8_t byte)
    {
        if (count >= MAX_LINE_DEPOSITS)
        {
            return;
        }
        items[count++] = Deposit{ destination, byte };
        if (destination != run_destination_ || run_words_ == MAX_DESCRIPTOR_WORDS)
        {
            run_destination_ = destination;
            run_words_ = 0;
            ++descriptors;
        }
        ++run_words_;
    }

private:
    uint8_t run_destination_ = 0xFF;
    int run_words_ = 0;
};

// The chip's law over one line (STEPS_PER_LINE steps) from COUNT `count` and
// PERIOD `period` in force at the line's first step: how many pops, and the
// COUNT left for the next line.
struct LinePops
{
    int pops;
    int count_after;
};

constexpr LinePops line_pops(int count, int period)
{
    if (count >= STEPS_PER_LINE)
    {
        return LinePops{ 0, count - STEPS_PER_LINE };
    }
    const int spacing = period + 1;
    const int pops = 1 + (STEPS_PER_LINE - 1 - count) / spacing;
    const int last = count + (pops - 1) * spacing;
    return LinePops{ pops, period - (STEPS_PER_LINE - 1 - last) };
}

struct SchedulerConfig
{
    int burst_lines = 8;
    int margin = 2;
};

struct ChannelState
{
    const uint8_t *table = nullptr;
    uint32_t length = 0;
    uint32_t position = 0;
    uint32_t repeat = 1;            // each table byte is deposited this many times
    uint32_t repeat_count = 0;      // deposits of table[position] already made
    bool     active = false;
    bool     burst_now = false;     // deposit at the next line, not the stagger slot

    uint8_t  period = 0;            // what the chip holds from this line on
    int      count = 0;             // the chip's COUNT at the start of the line
    int      fill = 0;              // bytes queued in the FIFO (mirror)

    bool     period_pending = false;
    uint8_t  pending_period = 0;
    int      pending_pops_left = 0;

    bool     volume_dirty = false;
    uint8_t  volume = 0;

    uint64_t deposited = 0;
    uint64_t predicted_underruns = 0;
    int      max_fill = 0;
};

class Scheduler
{
public:
    explicit Scheduler(SchedulerConfig config = SchedulerConfig{}) : config_(config) { reset(); }

    void reset();

    // The channel plays `table` (two's complement bytes) as a loop of `length`,
    // each byte deposited `repeat` times (a slow sequence at a rate the period
    // counter cannot reach, without a stretched copy of the table), starting
    // at `position` with `repeat_count` of that byte's copies already made.
    void set_stream(int channel, const uint8_t *table, uint32_t length, uint32_t position,
                    uint32_t repeat = 1, uint32_t repeat_count = 0);
    void set_period(int channel, uint8_t period);
    void set_volume(int channel, uint8_t volume);
    void stop(int channel);         // no more deposits; the FIFO drains to silence

    // One scanline of the display list.
    void line(LineDeposits& out);

    // A scanline that passed with none of this scheduler's deposits (a list
    // that missed its frame): the mirror still pops, nothing is queued, the
    // streams do not advance.  Call it once for each line lost, in order, so
    // the mirror stays in step with the chip; the channels will have run dry.
    void idle_line();

    const ChannelState& channel(int channel) const { return channels_[channel]; }
    const SchedulerConfig& config() const { return config_; }
    uint64_t lines() const { return line_; }
    uint64_t control_records() const { return control_records_; }

private:
    int predicted_pops(const ChannelState& s, int lines) const;
    void mirror_line();

    SchedulerConfig config_;
    ChannelState channels_[CHANNELS];
    uint64_t line_ = 0;
    uint64_t control_records_ = 0;
};

}   // namespace griffin::audio
