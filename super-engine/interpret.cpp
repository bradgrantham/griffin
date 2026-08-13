// interpret.cpp — walk a descriptor table exactly as edma3.v's state machine
// would, on a SYSCLK-cycle timeline.
//
// The cost model is one SYSCLK per state (see descriptor.h's
// engine_descriptor_cycles):
//
//   RELEASE(1) + [ REQUEST/WAIT_FREE = arbitration ] + ASSERT(1)
//     + 4 descriptor words x (SETTLE+STROBE)
//     + count payload words x (SETTLE+STROBE)
//
// with wait_hblank inserting HBLANK_RELEASE(1), an unbounded wait for the
// synchronized HBLANK rising edge, and a second arbitration round-trip before
// the payload.  stop_after replaces RELEASE with STOP: nENGINE_IRQ asserts and
// dma_en clears.

#include "interpret.h"

#include <cstdarg>
#include <cstdio>

namespace SuperEngine
{

// Inverse of sysclk_of_pixel: the last pixel clock that has started by `cycle`.
// floor(p*NUM/DEN) <= c  <=>  p <= (DEN*(c+1) - 1)/NUM.
uint64_t pixel_of_cycle(uint64_t cycle)
{
    return (PIXEL_SYSCLK_DEN * (cycle + 1) - 1) / PIXEL_SYSCLK_NUM;
}

uint64_t group_of_cycle(uint64_t cycle)
{
    return (pixel_of_cycle(cycle) + H_BLANK) / H_TOTAL;
}

namespace
{

constexpr uint64_t group_start_pixel(uint64_t group)
{
    return group * H_TOTAL - H_BLANK;
}

// Start of active video for the display line this group feeds — i.e. the cycle
// past which the group's work is "spilling" out of HBLANK.
constexpr uint64_t group_active_pixel(uint64_t group)
{
    return group * H_TOTAL;
}

constexpr uint32_t MAX_REPORTED_VIOLATIONS = 24;

struct Reporter
{
    std::vector<std::string> &out;
    uint32_t suppressed = 0;

    void add(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
};

void Reporter::add(const char *fmt, ...)
{
    if (out.size() >= MAX_REPORTED_VIOLATIONS)
    {
        suppressed++;
        return;
    }
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    out.emplace_back(buf);
}

}  // namespace

InterpretResult interpret(Memory ram, const InterpretParams &params)
{
    InterpretResult r;
    Reporter report{r.violations};

    const uint32_t arb = params.arbitration_cycles;

    // Size the per-group table from the run's time bound so the renderer and
    // the report can index it without a second pass.
    const uint64_t group_count = group_of_cycle(params.end_cycle) + 2;
    r.groups.resize(static_cast<size_t>(group_count));

    // Charge a busy interval to the groups it covers, splitting at group
    // boundaries so a burst that spills out of HBLANK is still counted against
    // the line it belongs to.
    auto mark_busy = [&](uint64_t from, uint64_t len)
    {
        r.busy_cycles += len;
        uint64_t c = from;
        uint64_t left = len;
        while (left > 0)
        {
            const uint64_t g = group_of_cycle(c);
            const uint64_t next_group_cycle = sysclk_of_pixel(group_start_pixel(g + 1));
            uint64_t take = left;
            if (next_group_cycle > c && next_group_cycle - c < take)
            {
                take = next_group_cycle - c;
            }
            if (g < r.groups.size())
            {
                GroupStat &s = r.groups[static_cast<size_t>(g)];
                s.used = true;
                s.busy_cycles += static_cast<uint32_t>(take);
                s.last_busy_end = c + take;

                // Anything past the start of this line's active video is
                // stealing bus bandwidth from the CPU during display, and is
                // also the part that races the PIXELS FIFO drain.
                const uint64_t active = sysclk_of_pixel(group_active_pixel(g));
                if (c + take > active)
                {
                    const uint64_t overlap_start = (c > active) ? c : active;
                    s.active_busy_cycles += static_cast<uint32_t>(c + take - overlap_start);
                }
            }
            c += take;
            left -= take;
        }
    };

    // Next HBLANK rising edge strictly after `t`, as the engine sees it (two
    // SYSCLK of input synchronizer, edma3.v's hblank_meta/hblank_sync pair).
    auto next_hblank_resume = [&](uint64_t t)
    {
        uint64_t g = group_of_cycle(t) + 1;
        uint64_t edge = sysclk_of_pixel(group_start_pixel(g));
        while (edge <= t)
        {
            g++;
            edge = sysclk_of_pixel(group_start_pixel(g));
        }
        return edge + ENGINE_HBLANK_SYNC_CYCLES;
    };

    uint64_t t = params.start_cycle;
    r.first_cycle = t;

    for (size_t arm_index = 0; arm_index < params.arm_addresses.size(); arm_index++)
    {
        uint32_t desc_ptr = params.arm_addresses[arm_index];
        bool armed = true;
        uint32_t in_this_arm = 0;

        while (armed)
        {
            if (t > params.end_cycle)
            {
                report.add("arm %zu: ran past end_cycle %llu with the list still armed",
                           arm_index, static_cast<unsigned long long>(params.end_cycle));
                armed = false;
                break;
            }

            if (in_this_arm >= params.max_descriptors_per_arm)
            {
                report.add("arm %zu: runaway list — %u descriptors with no stop_after",
                           arm_index, in_this_arm);
                armed = false;
                break;
            }

            // --- structural checks on the descriptor's own address ---
            if (desc_ptr < DESC_TABLE_BASE || desc_ptr + DESC_BYTES > DESC_TABLE_BASE + DESC_TABLE_BYTES)
            {
                report.add("arm %zu desc %u: pointer 0x%06X outside the 64K table window",
                           arm_index, in_this_arm, desc_ptr);
                armed = false;
                break;
            }
            if ((desc_ptr & (DESC_BYTES - 1)) != 0)
            {
                report.add("arm %zu desc %u: pointer 0x%06X is not 4-word aligned",
                           arm_index, in_this_arm, desc_ptr);
                armed = false;
                break;
            }
            if (desc_ptr < r.table_low)
            {
                r.table_low = desc_ptr;
            }
            if (desc_ptr + DESC_BYTES > r.table_high)
            {
                r.table_high = desc_ptr + DESC_BYTES;
            }

            const uint16_t w0 = ram[desc_ptr + 0];
            const uint16_t w1 = ram[desc_ptr + 2];
            const uint16_t w2 = ram[desc_ptr + 4];
            const uint16_t w3 = ram[desc_ptr + 6];
            const uint16_t raw[DESC_WORDS] = {w0, w1, w2, w3};
            const Descriptor d = decode_descriptor(raw);

            if ((w0 & DESC_RESERVED_MASK) != 0 || (w1 & 0xFF80) != 0 || (w2 & 0x0001) != 0 || w3 != 0)
            {
                report.add("arm %zu desc %u @0x%06X: reserved bits are not zero (%04X %04X %04X %04X)",
                           arm_index, in_this_arm, desc_ptr, w0, w1, w2, w3);
            }
            if (d.src + static_cast<uint32_t>(d.count) * 2 > ram.size_bytes())
            {
                report.add("arm %zu desc %u @0x%06X: source 0x%06X + %u words runs off RAM",
                           arm_index, in_this_arm, desc_ptr, d.src, d.count);
            }

            // --- the state machine ---
            const uint64_t fetch_start = t;
            t += arb + ENGINE_ASSERT_CYCLES + ENGINE_FETCH_CYCLES;

            uint64_t payload_start = fetch_start;
            if (d.wait_hblank)
            {
                t += 1;   // STATE_HBLANK_RELEASE
                mark_busy(fetch_start, t - fetch_start);
                t = next_hblank_resume(t);
                payload_start = t;
                t += arb + ENGINE_ASSERT_CYCLES;
            }

            for (uint32_t i = 0; i < d.count; i++)
            {
                t += ENGINE_CYCLES_PER_WORD;
                DepositEvent e;
                e.cycle       = t;   // strobe rising edge at the end of PAYLOAD_STROBE
                e.word        = ram[d.src + i * 2];
                e.signal_mask = d.signal_mask;
                r.events.push_back(e);
            }

            t += ENGINE_RELEASE_CYCLES;   // RELEASE, or STOP for the last one
            mark_busy(payload_start, t - payload_start);

            r.payload_words += d.count;
            if (d.signal_mask == SIGNAL_NONE)
            {
                r.pacing_words += d.count;
            }
            r.descriptor_count++;
            in_this_arm++;
            desc_ptr += DESC_BYTES;

            if (d.stop_after)
            {
                r.irq_count++;
                armed = false;
            }
        }

        r.last_cycle = t;

        // The VBLANK ISR's latency between nENGINE_IRQ and the next DESC write.
        t += params.rearm_latency_cycles;
    }

    if (r.irq_count != params.arm_addresses.size())
    {
        report.add("IRQ count %u does not match the %zu armings — a list failed to stop",
                   r.irq_count, params.arm_addresses.size());
    }

    if (report.suppressed > 0)
    {
        char buf[96];
        snprintf(buf, sizeof(buf), "... and %u further violations suppressed", report.suppressed);
        r.violations.emplace_back(buf);
    }

    return r;
}

}  // namespace SuperEngine
