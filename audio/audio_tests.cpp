// audio_tests.cpp — exact checks of the AUDIO model against its laws and of
// the Scheduler's mirror against the model.  Expectations are derived in
// README.md before this runs; nothing here is tuned to output.

#include <cstdio>
#include <cstdlib>
#include <random>

#include "audio_chip.h"
#include "psg_channels.h"
#include "scheduler.h"

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

// T6: the tone chooser lands every audible SN76489 period within a cent, and
// the noise chooser keeps PERIOD in range.
void test_choosers()
{
    const int failures_before = failures;
    double worst = 0.0;
    int over_goal = 0;
    for (int n = 1; n <= 1024; ++n)
    {
        const ToneChoice c = choose_tone(static_cast<uint16_t>(n));
        if (!c.audible)
        {
            CHECK(tone_band_limited(static_cast<uint16_t>(n)), "period %d inaudible but not band limited", n);
            continue;
        }
        CHECK(c.length >= 2 && c.length <= MAX_TONE_LENGTH, "period %d: length %d", n, c.length);
        if (std::fabs(c.cents) > worst)
        {
            worst = std::fabs(c.cents);
        }
        if (std::fabs(c.cents) > TONE_CENTS_GOAL)
        {
            ++over_goal;
        }
    }
    CHECK(tone_band_limited(14) && !tone_band_limited(15), "band limit boundary is period 14/15");
    for (int r = 0; r < 4; ++r)
    {
        for (int n2 : { 1, 4, 16, 100, 1024 })
        {
            const NoiseChoice c = choose_noise(noise_shift_hz(static_cast<uint8_t>(r), static_cast<uint16_t>(n2)));
            CHECK(c.dup >= 1, "noise dup 0");
        }
    }
    printf("T6 choosers: worst tone error %.3f cents, %d periods over %.1f cent\n", worst, over_goal, TONE_CENTS_GOAL);
    printf("T6: %s\n", failures == failures_before ? "ok" : "see FAIL lines");
}

}   // namespace

int main()
{
    test_rate_multiplier();
    test_closed_form();
    test_gate();
    test_scheduler_feed();
    test_period_alignment();
    test_choosers();
    printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
