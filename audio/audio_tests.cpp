// audio_tests.cpp — exact checks of the AUDIO model against its laws and of
// the Scheduler's mirror against the model.  Expectations are derived in
// README.md before this runs; nothing here is tuned to output.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "audio_chip.h"
#include "psg_channels.h"
#include "psg_tables.h"

#include <algorithm>
#include <cmath>
#include "frame_scheduler.h"
#include "scheduler.h"

#include "frame_audio.h"
#include "tests/trace_io.h"

using namespace griffin::audio;

namespace
{

int failures = 0;

#define CHECK(cond, ...)                                              \
    do                                                                \
    {                                                                 \
        if (!(cond))                                                  \
        {                                                             \
            ++failures;                                               \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);               \
            printf(__VA_ARGS__);                                      \
            printf("\n");                                             \
        }                                                             \
    } while (0)

void run_line(AudioChip& chip)
{
    for (int s = 0; s < STEPS_PER_LINE; ++s)
    {
        chip.step();
    }
}

void set_register(AudioChip& chip, uint8_t reg, uint8_t value)
{
    chip.deposit_control(reg);
    chip.deposit_control(value);
}

// T1: the rate multiplier's on-count per frame is the volume, the windows
// are disjoint and tick 0 is in none.
void test_rate_multiplier()
{
    for (int v = 0; v <= VOLUME_MAX; ++v)
    {
        int on = 0;
        for (int t = 0; t < FRAME_TICKS; ++t)
        {
            on += rate_multiplier_on(static_cast<uint8_t>(v), static_cast<uint8_t>(t)) ? 1 : 0;
        }
        CHECK(on == v, "volume %d is on %d ticks of 64", v, on);
    }
    for (int t = 0; t < FRAME_TICKS; ++t)
    {
        int windows = 0;
        for (int b = 0; b < 6; ++b)
        {
            windows += rate_multiplier_on(static_cast<uint8_t>(1 << b), static_cast<uint8_t>(t)) ? 1 : 0;
        }
        CHECK(windows <= 1, "tick %d is in %d windows", t, windows);
        if (t == 0)
        {
            CHECK(windows == 0, "tick 0 is in a window");
        }
    }
    printf("T1 rate multiplier: %s\n", failures ? "see FAIL lines" : "on-count == volume, windows disjoint");
}

// T2: the closed form line_pops() is the chip's counter law, including a
// PERIOD record landing at the line's start (two records per line at most).
void test_closed_form()
{
    AudioChip chip;
    std::mt19937 rng(12345);
    int period[CHANNELS] = { 0, 0, 0, 0 };
    int count[CHANNELS] = { 0, 0, 0, 0 };
    uint64_t pops_before[CHANNELS] = {};
    uint64_t lines_checked = 0;
    const int failures_before = failures;

    for (int line = 0; line < 200000; ++line)
    {
        // Up to two channels change period this line.
        const int changes = static_cast<int>(rng() % 3);
        for (int k = 0; k < changes; ++k)
        {
            const int c = static_cast<int>(rng() % CHANNELS);
            const int p = static_cast<int>(rng() % 256);
            set_register(chip, static_cast<uint8_t>(REG_PERIOD + c), static_cast<uint8_t>(p));
            period[c] = p;
        }
        LinePops predicted[CHANNELS];
        for (int c = 0; c < CHANNELS; ++c)
        {
            predicted[c] = line_pops(count[c], period[c]);
            pops_before[c] = chip.stats.pops[c];
        }
        run_line(chip);
        for (int c = 0; c < CHANNELS; ++c)
        {
            const int pops = static_cast<int>(chip.stats.pops[c] - pops_before[c]);
            CHECK(pops == predicted[c].pops, "line %d ch %d: chip popped %d, closed form %d (count %d period %d)",
                  line, c, pops, predicted[c].pops, count[c], period[c]);
            CHECK(chip.count(c) == predicted[c].count_after, "line %d ch %d: chip count %d, closed form %d",
                  line, c, chip.count(c), predicted[c].count_after);
            count[c] = predicted[c].count_after;
            if (failures > failures_before + 20)
            {
                printf("T2 aborted after 20 failures\n");
                return;
            }
        }
        ++lines_checked;
    }
    printf("T2 closed form: %llu lines, %s\n", static_cast<unsigned long long>(lines_checked),
           failures == failures_before ? "pops and counts identical" : "see FAIL lines");
}

// T3: an empty FIFO forces the gate off; with samples queued the gate's
// on-count over a frame is the volume.
void test_gate()
{
    AudioChip chip;
    set_register(chip, REG_PERIOD + 0, 255);
    set_register(chip, REG_VOLUME + 0, 63);
    run_line(chip);
    int on = 0;
    for (int s = 0; s < STEPS_PER_FRAME; ++s)
    {
        chip.step();
        on += chip.gate_ticks_on(0);
    }
    CHECK(on == 0, "empty FIFO: gate on %d ticks", on);
    for (int i = 0; i < 200; ++i)
    {
        chip.deposit_sample(0, 0xFF);
    }
    // Align to a frame start, then sum a frame.
    while (chip.steps() % STEPS_PER_FRAME != 0)
    {
        chip.step();
    }
    on = 0;
    for (int s = 0; s < STEPS_PER_FRAME; ++s)
    {
        chip.step();
        on += chip.gate_ticks_on(0);
    }
    CHECK(on == 63, "volume 63: gate on %d ticks of 64", on);
    set_register(chip, REG_VOLUME + 0, 20);
    run_line(chip);
    while (chip.steps() % STEPS_PER_FRAME != 0)
    {
        chip.step();
    }
    on = 0;
    for (int s = 0; s < STEPS_PER_FRAME; ++s)
    {
        chip.step();
        on += chip.gate_ticks_on(0);
    }
    CHECK(on == 20, "volume 20: gate on %d ticks of 64", on);
    CHECK(chip.sample(0) == 0xFF, "Q bus shows the queued byte");
    printf("T3 gate: %s\n", failures ? "see FAIL lines" : "off when empty, on-count == volume");
}

// T4: Scheduler keeps a channel fed and its mirror equals the chip's fill.
void test_scheduler_feed()
{
    const int failures_before = failures;
    for (int period : { 99, 49, 255, 12 })
    {
        AudioChip chip;
        Scheduler sched;
        std::vector<uint8_t> table(32);
        for (size_t i = 0; i < table.size(); ++i)
        {
            table[i] = (i < 16) ? 0xFF : 0x01;
        }
        sched.set_period(0, static_cast<uint8_t>(period));
        sched.set_volume(0, 63);
        sched.set_stream(0, table.data(), 32, 0);
        LineDeposits dep;
        uint64_t underruns = 0;
        int max_fill = 0;
        int mirror_mismatch = 0;
        for (int line = 0; line < 20000; ++line)
        {
            sched.line(dep);
            for (int i = 0; i < dep.count; ++i)
            {
                if (dep.items[i].destination == DEST_CONTROL)
                {
                    chip.deposit_control(dep.items[i].byte);
                }
                else
                {
                    chip.deposit_sample(dep.items[i].destination, dep.items[i].byte);
                }
            }
            const uint64_t empty_before = chip.stats.pops_empty[0];
            run_line(chip);
            if (line >= 16)
            {
                underruns += chip.stats.pops_empty[0] - empty_before;
            }
            if (chip.fill(0) != sched.channel(0).fill)
            {
                ++mirror_mismatch;
            }
            if (chip.fill(0) > max_fill)
            {
                max_fill = chip.fill(0);
            }
        }
        const int pops_per_burst = (STEPS_PER_LINE * sched.config().burst_lines) / (period + 1) + 1;
        CHECK(underruns == 0, "period %d: %llu underruns", period, static_cast<unsigned long long>(underruns));
        CHECK(mirror_mismatch == 0, "period %d: mirror != chip fill on %d lines", period, mirror_mismatch);
        CHECK(max_fill <= pops_per_burst + sched.config().margin + 1, "period %d: max fill %d, bound %d",
              period, max_fill, pops_per_burst + sched.config().margin + 1);
        CHECK(chip.stats.deposits_dropped[0] == 0, "period %d: deposits dropped", period);
        printf("T4 feed period %3d: max fill %d, underruns %llu, mirror mismatches %d\n", period, max_fill,
               static_cast<unsigned long long>(underruns), mirror_mismatch);
    }
    printf("T4 scheduler feed: %s\n", failures == failures_before ? "fed, mirror exact" : "see FAIL lines");
}

// T5: a PERIOD change lands when the old table's samples have drained: at
// most one line of old samples at the new rate or new samples at the old.
void test_period_alignment()
{
    const int failures_before = failures;
    AudioChip chip;
    Scheduler sched;
    std::vector<uint8_t> a(4, 0xFF);
    std::vector<uint8_t> b(4, 0x01);
    sched.set_period(0, 99);
    sched.set_volume(0, 63);
    sched.set_stream(0, a.data(), 4, 0);
    LineDeposits dep;
    auto deposit = [&]()
    {
        for (int i = 0; i < dep.count; ++i)
        {
            if (dep.items[i].destination == DEST_CONTROL)
            {
                chip.deposit_control(dep.items[i].byte);
            }
            else
            {
                chip.deposit_sample(dep.items[i].destination, dep.items[i].byte);
            }
        }
    };
    for (int line = 0; line < 2000; ++line)
    {
        sched.line(dep);
        deposit();
        run_line(chip);
    }
    sched.set_stream(0, b.data(), 4, 0);
    sched.set_period(0, 49);
    int old_at_new_rate = 0;    // 0xFF popped while PERIOD is 49
    int new_at_old_rate = 0;    // 0x01 popped while PERIOD is 99
    int change_line = -1;
    for (int line = 0; line < 200; ++line)
    {
        sched.line(dep);
        deposit();
        for (int s = 0; s < STEPS_PER_LINE; ++s)
        {
            chip.step();
            if (chip.popped_this_step(0) && !chip.fifo_empty(0))
            {
                if (chip.period(0) == 49 && chip.sample(0) == 0xFF)
                {
                    ++old_at_new_rate;
                }
                if (chip.period(0) == 99 && chip.sample(0) == 0x01)
                {
                    ++new_at_old_rate;
                }
            }
        }
        if (change_line < 0 && chip.period(0) == 49)
        {
            change_line = line;
        }
    }
    CHECK(change_line >= 0, "period record never landed");
    CHECK(old_at_new_rate <= 2, "%d old samples played at the new rate", old_at_new_rate);
    CHECK(new_at_old_rate <= 2, "%d new samples played at the old rate", new_at_old_rate);
    printf("T5 period alignment: record at line %d, %d old at new rate, %d new at old rate\n", change_line,
           old_at_new_rate, new_at_old_rate);
    printf("T5: %s\n", failures == failures_before ? "aligned within a line" : "see FAIL lines");
}

// T6: the generated tables.  Every tone period the reference makes audible
// (N >= 15) has a (PERIOD, LENGTH) and the band-limited ones have none;
// PERIOD and LENGTH are in range; the noise entries keep PERIOD in range with
// REPEAT >= 1; the volume ladder falls from 63 to 0.
void test_tables()
{
    namespace pt = griffin::audio::psg_tables;
    const int failures_before = failures;
    double worst = 0.0;
    int over_goal = 0;
    for (int n = 1; n <= 1024; ++n)
    {
        const pt::ToneEntry& t = pt::TONE[n & 1023];
        const uint64_t numerator = 65536ull * 3579545ull * 1600ull;
        const uint64_t denominator = 16ull * 25175000ull * static_cast<uint64_t>(n);
        const bool limited = (numerator + denominator / 2) / denominator >= 0x10000ull;
        CHECK(limited == (t.length == 0), "period %d: band limit %d but length %u", n, limited, t.length);
        if (t.length == 0)
        {
            continue;
        }
        CHECK(t.length >= 2 && t.length <= 512 && t.period >= 12, "period %d: PERIOD %u LENGTH %u", n, t.period, t.length);
        const double cents = 1200.0 * std::log2((1573437.5 / (t.period + 1) / t.length) / (3579545.0 / (32.0 * n)));
        worst = std::max(worst, std::fabs(cents));
        over_goal += (std::fabs(cents) > 1.0) ? 1 : 0;
    }
    for (int r = 1; r <= 1024; ++r)
    {
        CHECK(pt::NOISE[r].repeat >= 1 && pt::NOISE[r].period >= 12, "noise reload %d", r);
    }
    for (int a = 1; a < 16; ++a)
    {
        CHECK(pt::VOLUME[a] <= pt::VOLUME[a - 1], "volume ladder rises at %d", a);
    }
    CHECK(pt::VOLUME[0] == 63 && pt::VOLUME[15] == 0, "volume ends");
    printf("T6 tables: worst tone error %.3f cents, %d periods over 1 cent\n", worst, over_goal);
    printf("T6: %s\n", failures == failures_before ? "ok" : "see FAIL lines");
}

// T7: lines lost to a missed list.  Feed normally, skip 40 lines with
// idle_line() while the chip runs without deposits, then feed again: the
// mirror equals the chip's fill on every line afterwards.
void test_idle_lines()
{
    const int failures_before = failures;
    AudioChip chip;
    Scheduler sched;
    std::vector<uint8_t> table(20, 0x40);
    sched.set_period(0, 49);
    sched.set_volume(0, 63);
    sched.set_stream(0, table.data(), 20, 0);
    LineDeposits dep;
    int mismatches = 0;
    for (int line = 0; line < 3000; ++line)
    {
        if (line >= 1000 && line < 1040)
        {
            sched.idle_line();
        }
        else
        {
            sched.line(dep);
            for (int i = 0; i < dep.count; ++i)
            {
                if (dep.items[i].destination == DEST_CONTROL)
                {
                    chip.deposit_control(dep.items[i].byte);
                }
                else
                {
                    chip.deposit_sample(dep.items[i].destination, dep.items[i].byte);
                }
            }
        }
        run_line(chip);
        if (chip.fill(0) != sched.channel(0).fill)
        {
            ++mismatches;
        }
    }
    CHECK(mismatches == 0, "mirror differs from the chip on %d lines around a 40-line gap", mismatches);
    printf("T7 idle lines: %s\n", failures == failures_before ? "mirror exact across a 40-line gap" : "see FAIL lines");
}

// ---------------------------------------------------------------------------
// T8 and T9: FrameScheduler (frame_scheduler.h) against the per-line Scheduler
// and against the chip.  T9 proves the arithmetic; T8 proves the equivalence.
// ---------------------------------------------------------------------------

// T9: the per-region pop table against line_pops() summed over the region, for
// every PERIOD and every accumulator state the mirror can hold (COUNT 0..255
// covers the PERIOD-decrease reseed, where COUNT is larger than the new PERIOD
// and the accumulator starts negative).
void test_region_table()
{
    const int failures_before = failures;
    unsigned long long checked = 0;
    unsigned wrong = 0;
    for (int R : { 8, 16, 32, 64, 128, 176, 256 })
    {
        FrameSchedulerConfig cfg;
        cfg.region_lines = R;
        cfg.field_lines = 525;
        FrameScheduler fs(cfg);
        const int last = fs.regions_per_field() - 1;
        for (int which = 0; which < 2; ++which)
        {
            const int region = (which == 0) ? 0 : last;
            const int lines = fs.lines_of_region(region);
            if (lines <= 0)
            {
                continue;
            }
            for (int period = 0; period <= 255; ++period)
            {
                for (int count = 0; count <= 255; ++count)
                {
                    int c = count;
                    int total = 0;
                    for (int l = 0; l < lines; ++l)
                    {
                        const LinePops lp = line_pops(c, period);
                        total += lp.pops;
                        c = lp.count_after;
                    }
                    const FrameScheduler::RegionPops rp =
                        fs.region_pops(region, period, period - count);
                    ++checked;
                    if (rp.pops != total || rp.acc != period - c)
                    {
                        if (wrong == 0)
                        {
                            printf("  R %d region %d period %d count %d: table {%d, acc %d}, "
                                   "line_pops {%d, count %d -> acc %d}\n", R, region, period,
                                   count, rp.pops, rp.acc, total, c, period - c);
                            ++failures;
                        }
                        ++wrong;
                    }
                }
            }
        }
    }
    printf("T9 region table: %llu (R, region, period, count) cases, %s\n", checked,
           wrong ? "DISAGREES with line_pops" : "exact against line_pops summed over the region");
    printf("T9: %s\n", failures == failures_before ? "ok" : "see FAIL lines");
}

// ---- T8 -------------------------------------------------------------------

namespace t8
{

// One side of the comparison: a driver, a scheduler and a chip, plus the byte
// stream each destination received.
struct Recorder
{
    std::vector<uint8_t> sample_bytes[CHANNELS];
    std::vector<uint8_t> control_bytes;
    uint64_t underruns_active = 0;

    void note_sample(int channel, uint8_t byte)
    {
        sample_bytes[channel].push_back(byte);
    }

    void note_control(uint8_t byte)
    {
        control_bytes.push_back(byte);
    }
};

struct LineSide
{
    PsgChannels  driver;
    Scheduler    sched;
    AudioChip    chip;
    LineDeposits dep;
    Recorder     rec;

    explicit LineSide(int margin) : sched(SchedulerConfig{ 8, margin }) {}

    void emit() { driver.emit(sched); }

    // Deposit and step one line.
    void line()
    {
        sched.line(dep);
        for (int i = 0; i < dep.count; ++i)
        {
            const uint8_t d = dep.items[i].destination;
            const uint8_t b = dep.items[i].byte;
            if (d == DEST_CONTROL)
            {
                chip.deposit_control(b);
                rec.note_control(b);
            }
            else
            {
                chip.deposit_sample(d, b);
                rec.note_sample(d, b);
            }
        }
    }
};

struct FrameSide
{
    PsgChannels    driver;
    FrameScheduler fs;
    AudioChip      chip;
    RegionDeposits dep;
    Recorder       rec;
    // The region's staging window, the size the port gives it.
    uint16_t       window[32] = {};
    uint64_t       runs = 0;
    uint64_t       staged = 0;
    unsigned       max_runs_per_region = 0;
    unsigned       max_runs_per_channel = 0;

    explicit FrameSide(const FrameSchedulerConfig& cfg) : fs(cfg) {}

    void emit()
    {
        driver.emit(fs.inbox());
        fs.adopt_requests();
    }

    // The region due at this line, if any.
    void region()
    {
        fs.region(dep, window, 32);
        for (int i = 0; i < dep.control_bytes; ++i)
        {
            chip.deposit_control(dep.control[i]);
            rec.note_control(dep.control[i]);
        }
        for (int c = 0; c < CHANNELS; ++c)
        {
            if (dep.runs_of[c] > max_runs_per_channel)
            {
                max_runs_per_channel = dep.runs_of[c];
            }
            for (unsigned r = 0; r < dep.runs_of[c]; ++r)
            {
                const SampleRun& run = dep.runs[c][r];
                for (unsigned w = 0; w < run.count; ++w)
                {
                    const uint8_t byte = static_cast<uint8_t>(run.source[w] & 0xFF);
                    chip.deposit_sample(c, byte);
                    rec.note_sample(c, byte);
                }
            }
        }
        runs += static_cast<uint64_t>(dep.run_count);
        staged += static_cast<uint64_t>(dep.staged_words);
        if (static_cast<unsigned>(dep.run_count) > max_runs_per_region)
        {
            max_runs_per_region = static_cast<unsigned>(dep.run_count);
        }
    }
};

struct Result
{
    uint64_t fields = 0;
    uint64_t steps = 0;
    uint64_t line_deposits = 0;
    uint64_t frame_deposits = 0;
    uint64_t frame_runs = 0;
    uint64_t frame_staged = 0;
    unsigned max_runs_per_region = 0;
    unsigned max_runs_per_channel = 0;
    uint64_t line_underruns = 0;
    uint64_t frame_underruns = 0;
    uint64_t line_drops = 0;
    uint64_t frame_drops = 0;
    uint64_t line_control_drops = 0;
    uint64_t frame_control_drops = 0;
    uint64_t byte_stream_diff[CHANNELS] = {};     // value mismatches in the common prefix
    uint64_t byte_stream_short[CHANNELS] = {};    // bytes one side has and the other does not
    uint64_t byte_stream_len[CHANNELS] = {};
    uint64_t control_stream_diff = 0;
    uint64_t output_diff[CHANNELS] = {};
    uint64_t first_output_diff_step = 0;
    int      first_output_diff_channel = -1;
    uint32_t staged_short = 0;
    uint32_t runs_dropped = 0;
    uint32_t rings_built = 0;
    int      min_safe_period = 0;
    int      max_fill_gap = 0;          // |mirror fill| between the two schedulers
    int      max_phase_gap = 0;         // samples of stream phase between them
    uint64_t first_byte_diff_field = 0;
    uint32_t retargets_at_first_diff = 0;
    bool     any_byte_diff = false;
    uint32_t stream_decisions_differ = 0;   // fields where the two drivers disagreed
    uint32_t stream_changes = 0;            // set_stream events seen on the line side
    uint64_t first_stream_change_field = 0;
    int      max_deposit_gap = 0;           // per channel, bytes one side is ahead
    uint32_t retargets = 0;             // tone retargets + noise retargets
};

// Run both sides over the same sequence of PSG byte groups, one 525-line field
// per group, and compare.  `bytes[first[i] .. first[i]+count[i])` is group i.
void run(const std::vector<uint8_t>& bytes, const std::vector<uint32_t>& first,
         const std::vector<uint32_t>& count, const FrameSchedulerConfig& cfg, Result& out)
{
    LineSide line_side(cfg.margin);
    FrameSide frame_side(cfg);
    const uint8_t *last_table[CHANNELS] = {};
    uint32_t last_length[CHANNELS] = {};

    for (size_t g = 0; g < first.size(); ++g)
    {
        // The region grid is the scheduler's own, not R times the index: a
        // short remainder region is merged into the one before it.
        int next_region_line = 0;
        int next_region = 0;
        (void)next_region;
        for (uint32_t i = 0; i < count[g]; ++i)
        {
            line_side.driver.write(bytes[first[g] + i]);
            frame_side.driver.write(bytes[first[g] + i]);
        }
        line_side.emit();
        frame_side.emit();

        for (int l = 0; l < cfg.field_lines; ++l)
        {
            uint64_t empty_before_a[CHANNELS];
            uint64_t empty_before_b[CHANNELS];
            for (int c = 0; c < CHANNELS; ++c)
            {
                empty_before_a[c] = line_side.chip.stats.pops_empty[c];
                empty_before_b[c] = frame_side.chip.stats.pops_empty[c];
            }
            line_side.line();
            if (l == next_region_line && next_region < frame_side.fs.regions_per_field())
            {
                next_region_line += frame_side.fs.lines_of_region(next_region);
                ++next_region;
                frame_side.region();
            }
            for (int s = 0; s < STEPS_PER_LINE; ++s)
            {
                line_side.chip.step();
                frame_side.chip.step();
                for (int c = 0; c < CHANNELS; ++c)
                {
                    const bool same = line_side.chip.sample(c) == frame_side.chip.sample(c) &&
                                      line_side.chip.gate_ticks_on(c) ==
                                          frame_side.chip.gate_ticks_on(c);
                    if (!same)
                    {
                        if (out.first_output_diff_channel < 0)
                        {
                            out.first_output_diff_channel = c;
                            out.first_output_diff_step = out.steps;
                        }
                        out.output_diff[c]++;
                    }
                }
                out.steps++;
            }
            for (int c = 0; c < CHANNELS; ++c)
            {
                if (line_side.sched.channel(c).active)
                {
                    out.line_underruns +=
                        line_side.chip.stats.pops_empty[c] - empty_before_a[c];
                }
                if (frame_side.fs.channel(c).active)
                {
                    out.frame_underruns +=
                        frame_side.chip.stats.pops_empty[c] - empty_before_b[c];
                }
            }
        }
        for (int c = 0; c < CHANNELS; ++c)
        {
            const ChannelState& a = line_side.sched.channel(c);
            const FrameChannelState& b = frame_side.fs.channel(c);
            // The two drivers see the same PSG bytes, so they must reach the
            // same stream every field; only the PHASE inside it can differ.
            if (a.table != b.table || a.length != b.length || a.repeat != b.repeat ||
                a.active != b.active)
            {
                out.stream_decisions_differ++;
            }
            {
                const long long da = static_cast<long long>(line_side.rec.sample_bytes[c].size());
                const long long db = static_cast<long long>(frame_side.rec.sample_bytes[c].size());
                const int gap = static_cast<int>((da > db) ? (da - db) : (db - da));
                if (gap > out.max_deposit_gap)
                {
                    out.max_deposit_gap = gap;
                }
            }
            if (a.table != last_table[c] || a.length != last_length[c])
            {
                last_table[c] = a.table;
                last_length[c] = a.length;
                out.stream_changes++;
                if (out.stream_changes == 1)
                {
                    out.first_stream_change_field = out.fields;
                }
            }
            const int gap = (a.fill > b.fill) ? (a.fill - b.fill) : (b.fill - a.fill);
            if (gap > out.max_fill_gap)
            {
                out.max_fill_gap = gap;
            }
            if (a.active && b.active && a.table == b.table && a.length == b.length &&
                a.length != 0)
            {
                const uint32_t len = a.length;
                const uint32_t pa = a.position % len;
                const uint32_t pb = b.position % len;
                uint32_t d = (pa > pb) ? (pa - pb) : (pb - pa);
                if (d > len - d)
                {
                    d = len - d;
                }
                if (static_cast<int>(d) > out.max_phase_gap)
                {
                    out.max_phase_gap = static_cast<int>(d);
                }
            }
            if (!out.any_byte_diff)
            {
                const std::vector<uint8_t>& sa = line_side.rec.sample_bytes[c];
                const std::vector<uint8_t>& sb = frame_side.rec.sample_bytes[c];
                const size_t n = std::min(sa.size(), sb.size());
                for (size_t i = 0; i < n; ++i)
                {
                    if (sa[i] != sb[i])
                    {
                        out.any_byte_diff = true;
                        out.first_byte_diff_field = out.fields;
                        out.retargets_at_first_diff =
                            frame_side.driver.tone_retargets + frame_side.driver.noise_retargets;
                        break;
                    }
                }
            }
        }
        out.fields++;
    }

    out.retargets = frame_side.driver.tone_retargets + frame_side.driver.noise_retargets;
    for (int c = 0; c < CHANNELS; ++c)
    {
        out.line_drops += line_side.chip.stats.deposits_dropped[c];
        out.frame_drops += frame_side.chip.stats.deposits_dropped[c];
        const std::vector<uint8_t>& a = line_side.rec.sample_bytes[c];
        const std::vector<uint8_t>& b = frame_side.rec.sample_bytes[c];
        out.byte_stream_len[c] = a.size();
        out.line_deposits += a.size();
        out.frame_deposits += b.size();
        const size_t n = std::min(a.size(), b.size());
        for (size_t i = 0; i < n; ++i)
        {
            if (a[i] != b[i])
            {
                out.byte_stream_diff[c]++;
            }
        }
        out.byte_stream_short[c] = (a.size() > b.size()) ? (a.size() - n) : (b.size() - n);
    }
    out.line_control_drops = line_side.chip.stats.control_dropped;
    out.frame_control_drops = frame_side.chip.stats.control_dropped;
    {
        const std::vector<uint8_t>& a = line_side.rec.control_bytes;
        const std::vector<uint8_t>& b = frame_side.rec.control_bytes;
        const size_t n = std::min(a.size(), b.size());
        for (size_t i = 0; i < n; ++i)
        {
            if (a[i] != b[i])
            {
                out.control_stream_diff++;
            }
        }
        out.control_stream_diff += (a.size() > n) ? (a.size() - n) : (b.size() - n);
    }
    out.frame_runs = frame_side.runs;
    out.frame_staged = frame_side.staged;
    out.max_runs_per_region = frame_side.max_runs_per_region;
    out.max_runs_per_channel = frame_side.max_runs_per_channel;
    out.staged_short = frame_side.fs.staged_short();
    out.runs_dropped = frame_side.fs.runs_dropped();
    out.rings_built = frame_side.fs.rings_built();
    out.min_safe_period = frame_side.fs.min_safe_period();
}

void report(const char *what, const FrameSchedulerConfig& cfg, const Result& r)
{
    const double fields = (r.fields != 0) ? static_cast<double>(r.fields) : 1.0;
    printf("  %-28s R %3d, %llu fields (%.1f s)\n", what, cfg.region_lines,
           static_cast<unsigned long long>(r.fields), r.fields / 60.0);
    printf("    underruns  line %llu / frame %llu %s\n",
           static_cast<unsigned long long>(r.line_underruns),
           static_cast<unsigned long long>(r.frame_underruns),
           (r.line_underruns == 0 && r.frame_underruns == 0) ? "ok" : "FAIL");
    printf("    drops      line %llu / frame %llu, control %llu / %llu %s\n",
           static_cast<unsigned long long>(r.line_drops),
           static_cast<unsigned long long>(r.frame_drops),
           static_cast<unsigned long long>(r.line_control_drops),
           static_cast<unsigned long long>(r.frame_control_drops),
           (r.line_drops == 0 && r.frame_drops == 0 && r.line_control_drops == 0 &&
            r.frame_control_drops == 0) ? "ok" : "FAIL");
    printf("    deposits   line %llu / frame %llu (mean %.1f a field)\n",
           static_cast<unsigned long long>(r.line_deposits),
           static_cast<unsigned long long>(r.frame_deposits), r.frame_deposits / fields);
    printf("    runs       %llu (mean %.1f a field, max %u a region, max %u a channel), "
           "staged words %llu (%.2f%%), short %u, dropped %u\n",
           static_cast<unsigned long long>(r.frame_runs), r.frame_runs / fields,
           r.max_runs_per_region, r.max_runs_per_channel,
           static_cast<unsigned long long>(r.frame_staged),
           r.frame_deposits ? 100.0 * static_cast<double>(r.frame_staged) /
                                  static_cast<double>(r.frame_deposits) : 0.0,
           r.staged_short, r.runs_dropped);
    uint64_t bytes_diff = 0;
    uint64_t bytes_short = 0;
    for (int c = 0; c < CHANNELS; ++c)
    {
        bytes_diff += r.byte_stream_diff[c];
        bytes_short += r.byte_stream_short[c];
    }
    printf("    byte streams: %llu value mismatches in the common prefix of %llu, one side "
           "%llu bytes further on (%.3f%%), control records %llu differ %s\n",
           static_cast<unsigned long long>(bytes_diff),
           static_cast<unsigned long long>(r.line_deposits),
           static_cast<unsigned long long>(bytes_short),
           r.line_deposits ? 100.0 * static_cast<double>(bytes_short) /
                                 static_cast<double>(r.line_deposits) : 0.0,
           static_cast<unsigned long long>(r.control_stream_diff),
           (bytes_diff == 0) ? "(streams identical as far as both go)"
                             : "(a retarget carried a different phase; see below)");
    uint64_t out_diff = 0;
    for (int c = 0; c < CHANNELS; ++c)
    {
        out_diff += r.output_diff[c];
    }
    printf("    chip output differs on %llu of %llu channel-steps (%.4f%%)",
           static_cast<unsigned long long>(out_diff),
           static_cast<unsigned long long>(r.steps * CHANNELS),
           r.steps ? 100.0 * static_cast<double>(out_diff) /
                         static_cast<double>(r.steps * CHANNELS) : 0.0);
    if (r.first_output_diff_channel >= 0)
    {
        printf(", first at step %llu channel %d",
               static_cast<unsigned long long>(r.first_output_diff_step),
               r.first_output_diff_channel);
    }
    printf("\n");
    printf("    mirror fill gap max %d samples, deposit gap max %d bytes, stream phase gap max "
           "%d samples, retargets %u, stream changes %u%s\n", r.max_fill_gap,
           r.max_deposit_gap, r.max_phase_gap, r.retargets, r.stream_changes,
           r.any_byte_diff ? "" : " (no byte ever differed)");
    printf("    stream decisions differ on %u channel-fields %s\n", r.stream_decisions_differ,
           r.stream_decisions_differ ? "FAIL" : "ok");
    if (r.any_byte_diff)
    {
        printf("    first byte-stream divergence at field %llu, after %u retargets\n",
               static_cast<unsigned long long>(r.first_byte_diff_field),
               r.retargets_at_first_diff);
    }
}

}   // namespace t8

// T8: over the recorded Smurf PSG stream and over a synthetic sweep of every
// PSG period, the FrameScheduler and the Scheduler each drive a chip model.
// The gates: no underrun of an active channel and no dropped deposit on either
// side.  The byte streams and the chip outputs are reported, not asserted
// identical: a PERIOD record lands at a REGION boundary rather than a line
// boundary, which is by design (frame_scheduler.h, "POLICY") and moves the
// `need` of the bursts that follow it.
void test_frame_scheduler(const char *trace_path, double sweep_seconds, int region_lines)
{
    const int failures_before = failures;

    printf("T8 FrameScheduler equivalence\n");

    // ---- the recorded Smurf trace ----------------------------------------
    Trace trace;
    if (read_trace(trace_path, trace))
    {
        std::vector<uint8_t> bytes;
        std::vector<uint32_t> first;
        std::vector<uint32_t> count;
        for (size_t f = 0; f < trace.frame_bytes.size(); ++f)
        {
            const std::vector<uint8_t>& fb = trace.frame_bytes[f];
            const std::vector<uint16_t>& groups = trace.frame_ticks[f];
            const int fields = griffin::psg::frame_fields(groups.size(), 1);
            for (int g = 0; g < fields; ++g)
            {
                size_t begin = 0;
                size_t end = 0;
                if (static_cast<size_t>(g) < groups.size())
                {
                    begin = (g == 0) ? size_t{ 0 } : groups[g];
                    end = (static_cast<size_t>(g) + 1 < groups.size()) ? groups[g + 1]
                                                                       : fb.size();
                }
                first.push_back(static_cast<uint32_t>(bytes.size()));
                count.push_back(static_cast<uint32_t>(end - begin));
                for (size_t i = begin; i < end; ++i)
                {
                    bytes.push_back(fb[i]);
                }
            }
        }
        for (int R : { 8, region_lines })
        {
            FrameSchedulerConfig cfg;
            cfg.region_lines = R;
            cfg.field_lines = 525;
            cfg.margin = 2;
            t8::Result r;
            t8::run(bytes, first, count, cfg, r);
            t8::report("Smurf trace", cfg, r);
            CHECK(r.line_underruns == 0 && r.frame_underruns == 0,
                  "Smurf trace R %d: underruns line %llu frame %llu", R,
                  static_cast<unsigned long long>(r.line_underruns),
                  static_cast<unsigned long long>(r.frame_underruns));
            CHECK(r.line_drops == 0 && r.frame_drops == 0 && r.frame_control_drops == 0,
                  "Smurf trace R %d: dropped deposits", R);
            CHECK(r.runs_dropped == 0 && r.staged_short == 0,
                  "Smurf trace R %d: %u runs dropped, %u staged runs short", R,
                  r.runs_dropped, r.staged_short);
            // Both drivers must pick the same stream every field; only the
            // phase inside it may differ (see the report).
            CHECK(r.stream_decisions_differ == 0,
                  "Smurf trace R %d: the two drivers picked different streams on %u "
                  "channel-fields", R, r.stream_decisions_differ);
            CHECK(!r.any_byte_diff || r.first_byte_diff_field >= r.first_stream_change_field,
                  "Smurf trace R %d: bytes differed at field %llu, before the first stream "
                  "change at %llu", R,
                  static_cast<unsigned long long>(r.first_byte_diff_field),
                  static_cast<unsigned long long>(r.first_stream_change_field));
        }
    }
    else
    {
        printf("  (no trace at %s: the Smurf half of T8 was skipped)\n", trace_path);
    }

    // ---- the synthetic sweep ---------------------------------------------
    //
    // Every PSG period 1..1023 for a tone and for both noise sequences (rate 3
    // takes its reload from tone 2's period, which is how every one of the
    // 1,024 NOISE entries is reached).
    struct Case
    {
        const char *what;
        int kind;       // 0 tone on channel 0, 1 tone on channel 2, 2 white noise, 3 periodic
    };
    const Case cases[4] = { { "tone channel 0", 0 },
                            { "tone channel 2", 1 },
                            { "white noise (rate 3)", 2 },
                            { "periodic noise (rate 3)", 3 } };
    const int fields_per_case = static_cast<int>(sweep_seconds * 60.0) + 1;
    unsigned driver_defect_cases = 0;
    unsigned frame_defect_cases = 0;
    int first_defect_period = -1;
    for (const Case& kase : cases)
    {
        t8::Result total;
        unsigned excluded = 0;
        for (int n = 1; n <= 1023; ++n)
        {
            std::vector<uint8_t> bytes;
            std::vector<uint32_t> first;
            std::vector<uint32_t> count;
            const int tone = (kase.kind == 1 || kase.kind >= 2) ? 2 : 0;
            first.push_back(0);
            bytes.push_back(static_cast<uint8_t>(0x80 | (tone * 2) << 4 | (n & 0x0F)));
            bytes.push_back(static_cast<uint8_t>((n >> 4) & 0x3F));
            if (kase.kind < 2)
            {
                bytes.push_back(static_cast<uint8_t>(0x80 | ((tone * 2 + 1) << 4) | 0));
            }
            else
            {
                // tone 2 silent, noise on rate 3 with tone 2's period as reload.
                bytes.push_back(static_cast<uint8_t>(0x80 | ((tone * 2 + 1) << 4) | 15));
                bytes.push_back(
                    static_cast<uint8_t>(0x80 | (6 << 4) | ((kase.kind == 2) ? 0x07 : 0x03)));
                bytes.push_back(static_cast<uint8_t>(0x80 | (7 << 4) | 0));
            }
            count.push_back(static_cast<uint32_t>(bytes.size()));
            for (int f = 1; f < fields_per_case; ++f)
            {
                first.push_back(static_cast<uint32_t>(bytes.size()));
                count.push_back(0);
            }
            FrameSchedulerConfig cfg;
            cfg.region_lines = region_lines;
            cfg.field_lines = 525;
            cfg.margin = 2;
            t8::Result r;
            t8::run(bytes, first, count, cfg, r);
            // The shipped Scheduler must keep every swept case fed.  (Until
            // 2026-09-29 psg_channels.h's Shadow::period used 0xFF as "no
            // PERIOD yet", so a stream whose chosen PERIOD really was 255
            // never got its control record and the chip stayed at the reset
            // PERIOD 0; 128 cases underran on both schedulers.  The sentinel
            // is now out of PERIOD's range and this counts the regression.)
            if (r.line_underruns != 0)
            {
                ++driver_defect_cases;
                ++excluded;
                if (first_defect_period < 0)
                {
                    first_defect_period = n;
                }
                CHECK(false, "sweep %s N %d: the shipped Scheduler underran %llu times",
                      kase.what, n, static_cast<unsigned long long>(r.line_underruns));
                continue;
            }
            if (r.frame_underruns != 0)
            {
                ++frame_defect_cases;
                CHECK(r.frame_underruns == 0,
                      "sweep %s N %d: FrameScheduler underran %llu times where the Scheduler "
                      "did not", kase.what, n,
                      static_cast<unsigned long long>(r.frame_underruns));
            }
            total.fields += r.fields;
            total.steps += r.steps;
            total.line_deposits += r.line_deposits;
            total.frame_deposits += r.frame_deposits;
            total.frame_runs += r.frame_runs;
            total.frame_staged += r.frame_staged;
            total.line_underruns += r.line_underruns;
            total.frame_underruns += r.frame_underruns;
            total.line_drops += r.line_drops;
            total.frame_drops += r.frame_drops;
            total.line_control_drops += r.line_control_drops;
            total.frame_control_drops += r.frame_control_drops;
            total.control_stream_diff += r.control_stream_diff;
            for (int c = 0; c < CHANNELS; ++c)
            {
                total.byte_stream_diff[c] += r.byte_stream_diff[c];
                total.output_diff[c] += r.output_diff[c];
            }
            total.max_runs_per_region =
                std::max(total.max_runs_per_region, r.max_runs_per_region);
            total.max_runs_per_channel =
                std::max(total.max_runs_per_channel, r.max_runs_per_channel);
            total.max_fill_gap = std::max(total.max_fill_gap, r.max_fill_gap);
            total.max_phase_gap = std::max(total.max_phase_gap, r.max_phase_gap);
            total.retargets += r.retargets;
            total.staged_short += r.staged_short;
            total.runs_dropped += r.runs_dropped;
            if (r.any_byte_diff && !total.any_byte_diff)
            {
                total.any_byte_diff = true;
                total.first_byte_diff_field = r.first_byte_diff_field;
            }
            if (total.first_output_diff_channel < 0 && r.first_output_diff_channel >= 0)
            {
                total.first_output_diff_channel = r.first_output_diff_channel;
                total.first_output_diff_step = r.first_output_diff_step;
            }
        }
        FrameSchedulerConfig cfg;
        cfg.region_lines = region_lines;
        cfg.field_lines = 525;
        t8::report(kase.what, cfg, total);
        printf("    %u of 1023 periods excluded: the shipped Scheduler underruns them too\n",
               excluded);
        CHECK(total.frame_drops == 0 && total.frame_control_drops == 0,
              "sweep %s: %llu dropped deposits", kase.what,
              static_cast<unsigned long long>(total.frame_drops));
        CHECK(total.runs_dropped == 0 && total.staged_short == 0,
              "sweep %s: %u runs dropped, %u staged runs short", kase.what,
              total.runs_dropped, total.staged_short);
        uint64_t sweep_diff = 0;
        uint64_t sweep_out = 0;
        for (int c = 0; c < CHANNELS; ++c)
        {
            sweep_diff += total.byte_stream_diff[c];
            sweep_out += total.output_diff[c];
        }
        // No retarget happens in a sweep case (one period, held), so the two
        // schedulers must be byte-identical and the chips bit-identical.
        CHECK(sweep_diff == 0, "sweep %s: %llu byte-stream mismatches with no retarget",
              kase.what, static_cast<unsigned long long>(sweep_diff));
        CHECK(sweep_out == 0, "sweep %s: %llu channel-steps of chip output differ",
              kase.what, static_cast<unsigned long long>(sweep_out));
    }

    printf("  Scheduler underrun cases: %u of the 4092 swept (channel type, period) cases "
           "(first: tone period %d; 0 expected since the PERIOD-255 sentinel fix)\n",
           driver_defect_cases, first_defect_period);
    printf("  FrameScheduler-only underrun cases: %u\n", frame_defect_cases);
    printf("  rings: %u of %u pool words used (%u bytes)\n", frame_ring_pool_words(),
           frame_ring_pool_capacity(), 2u * frame_ring_pool_words());
    printf("T8: %s\n", failures == failures_before
                           ? "no underrun and no drop anywhere the shipped Scheduler has none"
                           : "see FAIL lines");
}

}   // namespace

int main(int argc, char **argv)
{
    const char *trace = "../../griffin-ports/smurf/work/smurf-trace.io";
    double sweep_seconds = 0.25;
    int region_lines = 64;
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--trace") == 0 && i + 1 < argc)
        {
            trace = argv[++i];
        }
        else if (strcmp(argv[i], "--sweep-seconds") == 0 && i + 1 < argc)
        {
            sweep_seconds = atof(argv[++i]);
        }
        else if (strcmp(argv[i], "--region-lines") == 0 && i + 1 < argc)
        {
            region_lines = atoi(argv[++i]);
        }
        else
        {
            fprintf(stderr, "usage: audio_tests [--trace f] [--sweep-seconds s] "
                            "[--region-lines n]\n");
            return 2;
        }
    }
    test_rate_multiplier();
    test_closed_form();
    test_gate();
    test_scheduler_feed();
    test_period_alignment();
    test_tables();
    test_idle_lines();
    test_region_table();
    test_frame_scheduler(trace, sweep_seconds, region_lines);
    printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
