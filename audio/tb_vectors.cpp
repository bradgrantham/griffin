// tb_vectors — a random stimulus through the AUDIO model, written as the
// vectors cpld/audio/audio_tb.v checks the RTL against.  The model is the
// spec, so whatever it does with these deposits, the chip must do too.
//
//   stim.hex  one event per line, 10 hex digits: line[23:0] type[3:0]
//             channel[3:0] byte[7:0]; type 0 = sample deposit, 1 = control
//             byte.  Events of a line are applied, in order, right after the
//             clock edge that starts the line (edge 800 x line since reset).
//   pops.hex  one pop per line, 10 hex digits: step[31:0] channel[7:0]; the
//             counter update of step s is the edge 16 (s + 1), and
//             nSAMPLE_R[channel] is high for the one clock after it.
//   regs.hex  one line per scanline, 16 hex digits: PERIOD0..3 VOLUME0..3 as
//             the chip holds them after the edge that ends the line.
//
// The stimulus mixes: control records complete in one line (up to three, so
// the third lands one step late in both model and chip), records split
// across lines (the reader waits), sample bursts that keep a channel fed,
// channels left to run dry (EF gating), and a burst that overfills (writes
// dropped).  Periods are biased upward so the pop list stays a few hundred
// thousand entries.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "audio_chip.h"

using namespace griffin::audio;

int main(int argc, char **argv)
{
    const std::string dir = (argc > 1) ? argv[1] : ".";
    const int lines = (argc > 2) ? atoi(argv[2]) : 1200;
    std::mt19937 rng(20260927);

    FILE *stim = fopen((dir + "/stim.hex").c_str(), "w");
    FILE *pops = fopen((dir + "/pops.hex").c_str(), "w");
    FILE *regs = fopen((dir + "/regs.hex").c_str(), "w");
    if (!stim || !pops || !regs)
    {
        fprintf(stderr, "tb_vectors: cannot write into %s\n", dir.c_str());
        return 1;
    }

    AudioChip chip;
    uint64_t events = 0;
    uint64_t pop_count = 0;
    bool half_record_pending = false;

    auto event = [&](int line, int type, int channel, uint8_t byte)
    {
        fprintf(stim, "%06x%x%x%02x\n", line & 0xFFFFFF, type & 15, channel & 15, byte);
        ++events;
    };

    for (int line = 0; line < lines; ++line)
    {
        // Control.  A half record left from the previous line completes first.
        if (half_record_pending)
        {
            const uint8_t value = static_cast<uint8_t>(rng() & 0xFF);
            event(line, 1, 0, value);
            chip.deposit_control(value);
            half_record_pending = false;
        }
        const uint32_t dice = rng() % 100;
        int records = (dice < 60) ? 0 : (dice < 85) ? 1 : (dice < 95) ? 2 : 3;
        if (line < 4)
        {
            records = 2;    // get periods and volumes in early
        }
        for (int r = 0; r < records; ++r)
        {
            const uint8_t reg = static_cast<uint8_t>(rng() & 7);
            uint8_t value;
            if (reg < REG_VOLUME)
            {
                const uint32_t d = rng() % 100;
                value = static_cast<uint8_t>((d < 5) ? (rng() % 8) : (d < 20) ? (8 + rng() % 40) : (48 + rng() % 208));
            }
            else
            {
                value = static_cast<uint8_t>(rng() & 0xFF);     // bits 7:6 must be ignored
            }
            event(line, 1, 0, reg);
            chip.deposit_control(reg);
            if (r == records - 1 && (rng() % 10) == 0)
            {
                half_record_pending = true;     // the value byte arrives next line
            }
            else
            {
                event(line, 1, 0, value);
                chip.deposit_control(value);
            }
        }

        // Samples.
        for (int c = 0; c < CHANNELS; ++c)
        {
            const uint32_t d = rng() % 100;
            int n = 0;
            if (d < 35)
            {
                n = static_cast<int>(rng() % 40);
            }
            else if (d < 37)
            {
                n = 300;    // overfill
            }
            for (int i = 0; i < n; ++i)
            {
                const uint8_t byte = static_cast<uint8_t>(rng() & 0xFF);
                event(line, 0, c, byte);
                chip.deposit_sample(c, byte);
            }
        }

        for (int s = 0; s < STEPS_PER_LINE; ++s)
        {
            const uint64_t step = chip.steps();
            chip.step();
            for (int c = 0; c < CHANNELS; ++c)
            {
                if (chip.popped_this_step(c))
                {
                    fprintf(pops, "%08llx%02x\n", static_cast<unsigned long long>(step), c);
                    ++pop_count;
                }
            }
        }
        fprintf(regs, "%02x%02x%02x%02x%02x%02x%02x%02x\n", chip.period(0), chip.period(1), chip.period(2),
                chip.period(3), chip.volume(0), chip.volume(1), chip.volume(2), chip.volume(3));
    }
    fclose(stim);
    fclose(pops);
    fclose(regs);
    printf("tb_vectors: %d lines, %llu events, %llu pops, %llu control records, dropped %llu/%llu/%llu/%llu samples, %llu control\n",
           lines, static_cast<unsigned long long>(events), static_cast<unsigned long long>(pop_count),
           static_cast<unsigned long long>(chip.stats.control_records),
           static_cast<unsigned long long>(chip.stats.deposits_dropped[0]),
           static_cast<unsigned long long>(chip.stats.deposits_dropped[1]),
           static_cast<unsigned long long>(chip.stats.deposits_dropped[2]),
           static_cast<unsigned long long>(chip.stats.deposits_dropped[3]),
           static_cast<unsigned long long>(chip.stats.control_dropped));
    return 0;
}
