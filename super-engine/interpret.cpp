// interpret.cpp — EngineWalker, plus the whole-run driver the suite uses.
//
// The walker is the shared implementation; interpret() is one of its two
// drivers (emulator.cpp will be the other).  Nothing about the descriptor state
// machine lives outside EngineWalker.

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

// Start of group g, which is the HBLANK rising edge of raster line g-1.
uint64_t hblank_edge_cycle(uint64_t group)
{
    return sysclk_of_pixel(group * H_TOTAL - H_BLANK);
}

namespace
{
constexpr uint32_t MAX_REPORTED_VIOLATIONS = 24;
}

// ---------------------------------------------------------------------------
// EngineWalker
// ---------------------------------------------------------------------------

void EngineWalker::note(const char *fmt, ...)
{
    if (violations_.size() >= MAX_REPORTED_VIOLATIONS)
    {
        suppressed_++;
        return;
    }
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    violations_.emplace_back(buf);
}

void EngineWalker::reset()
{
    armed_       = false;
    irq_pending_ = false;
    desc_ptr_    = 0;
    cycle_       = 0;
    pending_     = false;
    pending_desc_ = Descriptor{};
}

// Fetch the four descriptor words and charge for them.  Returns false if the
// pointer is outside the table window, which disarms.
bool EngineWalker::fetch_descriptor(Memory ram, Descriptor &d, WalkResult &out)
{
    if (desc_ptr_ < DESC_TABLE_BASE ||
        desc_ptr_ + DESC_BYTES > DESC_TABLE_BASE + DESC_TABLE_BYTES)
    {
        note("descriptor pointer 0x%06X is outside the 64K table window", desc_ptr_);
        armed_ = false;
        return false;
    }
    if ((desc_ptr_ & (DESC_BYTES - 1)) != 0)
    {
        note("descriptor pointer 0x%06X is not 4-word aligned", desc_ptr_);
        armed_ = false;
        return false;
    }

    if (desc_ptr_ < table_low_)
    {
        table_low_ = desc_ptr_;
    }
    if (desc_ptr_ + DESC_BYTES > table_high_)
    {
        table_high_ = desc_ptr_ + DESC_BYTES;
    }

    const uint16_t w0 = ram[desc_ptr_ + 0];
    const uint16_t w1 = ram[desc_ptr_ + 2];
    const uint16_t w2 = ram[desc_ptr_ + 4];
    const uint16_t w3 = ram[desc_ptr_ + 6];
    const uint16_t raw[DESC_WORDS] = {w0, w1, w2, w3};
    d = decode_descriptor(raw);

    if ((w0 & DESC_RESERVED_MASK) != 0 || (w1 & 0xFF80) != 0 || (w2 & 0x0001) != 0 || w3 != 0)
    {
        note("descriptor @0x%06X: reserved bits are not zero (%04X %04X %04X %04X)",
             desc_ptr_, w0, w1, w2, w3);
    }
    if (d.src + static_cast<uint32_t>(d.count) * 2 > ram.size_bytes())
    {
        note("descriptor @0x%06X: source 0x%06X + %u words runs off RAM",
             desc_ptr_, d.src, d.count);
    }

    const uint64_t start = cycle_;
    cycle_ += arbitration_ + ENGINE_ASSERT_CYCLES + ENGINE_FETCH_CYCLES;
    out.busy_cycles += cycle_ - start;
    return true;
}

void EngineWalker::run_payload(Memory ram, const Descriptor &d, DepositSink &sink,
                               WalkResult &out)
{
    for (uint32_t i = 0; i < d.count; i++)
    {
        cycle_ += ENGINE_CYCLES_PER_WORD;
        sink.deposit(cycle_, d.signal_mask, ram[d.src + i * 2]);
    }
    cycle_ += ENGINE_RELEASE_CYCLES;   // RELEASE, or STOP for the last one

    out.words += d.count;
    out.busy_cycles += static_cast<uint64_t>(d.count) * ENGINE_CYCLES_PER_WORD +
                       ENGINE_RELEASE_CYCLES;
    if (d.signal_mask == SIGNAL_NONE)
    {
        out.pacing_words += d.count;
    }

    total_words_ += d.count;
    if (d.signal_mask == SIGNAL_NONE)
    {
        total_pacing_words_ += d.count;
    }
    total_descriptors_++;
    out.descriptors++;
}

// Run descriptors from the current pointer until a wait_hblank parks or
// stop_after fires.  Assumes cycle_ is already positioned.
void EngineWalker::run_until_wait(Memory ram, DepositSink &sink, WalkResult &out)
{
    while (armed_)
    {
        if (out.descriptors >= max_per_call_)
        {
            note("runaway list — %u descriptors in one call with no stop_after",
                 out.descriptors);
            armed_ = false;
            break;
        }

        Descriptor d;
        if (!fetch_descriptor(ram, d, out))
        {
            break;
        }
        desc_ptr_ += DESC_BYTES;

        if (d.wait_hblank)
        {
            // STATE_HBLANK_RELEASE, then park: the payload runs at the next
            // edge, which is the whole point of the split.
            cycle_ += 1;
            out.busy_cycles += 1;
            pending_      = true;
            pending_desc_ = d;
            break;
        }

        run_payload(ram, d, sink, out);

        if (d.stop_after)
        {
            irq_pending_ = true;
            irq_count_++;
            armed_    = false;
            out.irq   = true;
            break;
        }
    }
}

WalkResult EngineWalker::arm(Memory ram, uint32_t desc_byte_address, uint64_t cycle,
                             DepositSink &sink)
{
    WalkResult out;
    armed_       = true;
    irq_pending_ = false;   // the DESC write clears a pending IRQ
    desc_ptr_    = desc_byte_address;
    cycle_       = cycle;
    pending_     = false;

    run_until_wait(ram, sink, out);

    out.end_cycle = cycle_;
    total_busy_cycles_ += out.busy_cycles;
    return out;
}

WalkResult EngineWalker::advance(Memory ram, uint64_t edge_cycle, DepositSink &sink)
{
    WalkResult out;
    if (!armed_)
    {
        out.end_cycle = cycle_;
        return out;
    }

    // edma3.v samples HBLANK through a 2-FF synchronizer and looks for the
    // rising edge of the synchronized signal.
    cycle_ = edge_cycle + ENGINE_HBLANK_SYNC_CYCLES;

    if (pending_)
    {
        const Descriptor d = pending_desc_;
        pending_ = false;

        const uint64_t start = cycle_;
        cycle_ += arbitration_ + ENGINE_ASSERT_CYCLES;
        out.busy_cycles += cycle_ - start;

        run_payload(ram, d, sink, out);

        if (d.stop_after)
        {
            irq_pending_ = true;
            irq_count_++;
            armed_  = false;
            out.irq = true;
            out.end_cycle = cycle_;
            total_busy_cycles_ += out.busy_cycles;
            return out;
        }
    }

    run_until_wait(ram, sink, out);

    out.end_cycle = cycle_;
    total_busy_cycles_ += out.busy_cycles;
    return out;
}

// ---------------------------------------------------------------------------
// Whole-run driver
// ---------------------------------------------------------------------------

namespace
{

// Collects the walker's deposits into the suite's event list and charges busy
// cycles to raster line groups.
struct RecordingSink final : DepositSink
{
    InterpretResult *result = nullptr;

    void deposit(uint64_t cycle, uint8_t signal_mask, uint16_t word) override
    {
        DepositEvent e;
        e.cycle       = cycle;
        e.word        = word;
        e.signal_mask = signal_mask;
        result->events.push_back(e);
    }
};

constexpr uint64_t group_start_pixel(uint64_t group)
{
    return group * H_TOTAL - H_BLANK;
}

constexpr uint64_t group_active_pixel(uint64_t group)
{
    return group * H_TOTAL;
}

// Charge a busy interval to the groups it covers, splitting at group
// boundaries so a burst that spills out of HBLANK is still counted against the
// line it belongs to.
void mark_busy(InterpretResult &r, uint64_t from, uint64_t len)
{
    r.busy_cycles += len;
    uint64_t c    = from;
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
}

}  // namespace

InterpretResult interpret(Memory ram, const InterpretParams &params)
{
    InterpretResult r;

    const uint64_t group_count = group_of_cycle(params.end_cycle) + 2;
    r.groups.resize(static_cast<size_t>(group_count));

    RecordingSink sink;
    sink.result = &r;

    EngineWalker walker(params.arbitration_cycles);

    r.first_cycle = params.start_cycle;

    size_t   arm_index    = 0;
    uint64_t next_arm_at  = params.start_cycle;
    bool     want_arm     = !params.arm_addresses.empty();
    uint64_t last_end     = params.start_cycle;

    // Walk edge by edge from the arm point to the end of the run, exactly as
    // the emulator will: one advance() per HBLANK, arming when the ISR would.
    const uint64_t first_group = group_of_cycle(params.start_cycle);
    const uint64_t last_group  = group_of_cycle(params.end_cycle);

    for (uint64_t g = first_group; g <= last_group; g++)
    {
        const uint64_t edge = hblank_edge_cycle(g + 1);

        // Arm before the edge if the ISR's re-arm has come due.
        if (want_arm && next_arm_at <= edge)
        {
            const uint64_t before = r.events.size();
            (void)before;
            const uint64_t arm_start = next_arm_at;
            const WalkResult w = walker.arm(ram, params.arm_addresses[arm_index], arm_start, sink);
            mark_busy(r, arm_start, w.busy_cycles);
            last_end = w.end_cycle;
            arm_index++;
            want_arm = false;
        }

        if (!walker.armed())
        {
            continue;
        }

        const WalkResult w = walker.advance(ram, edge, sink);
        mark_busy(r, edge + ENGINE_HBLANK_SYNC_CYCLES, w.busy_cycles);
        last_end = w.end_cycle;

        if (w.irq)
        {
            // The VBLANK ISR's latency between nENGINE_IRQ and the next DESC
            // write.
            if (arm_index < params.arm_addresses.size())
            {
                next_arm_at = w.end_cycle + params.rearm_latency_cycles;
                want_arm    = true;
            }
        }
    }

    r.last_cycle       = last_end;
    r.descriptor_count = walker.total_descriptors();
    r.payload_words    = walker.total_words();
    r.pacing_words     = walker.total_pacing_words();
    r.irq_count        = walker.irq_count();
    r.table_low        = walker.table_low();
    r.table_high       = walker.table_high();
    r.violations       = walker.violations();

    if (r.irq_count != params.arm_addresses.size())
    {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "IRQ count %u does not match the %zu armings — a list failed to stop",
                 r.irq_count, params.arm_addresses.size());
        r.violations.emplace_back(buf);
    }

    return r;
}

}  // namespace SuperEngine
