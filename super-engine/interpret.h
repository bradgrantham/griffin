// interpret.h — the ENGINE display-list walker, as an object that can be
// advanced one line at a time.
//
// Host-only in that it builds std::vector/std::string for its report, but
// deliberately dependency-free otherwise: no SDL, no POSIX, nothing from the
// project beyond descriptor.h.  emulator.cpp includes this file verbatim and
// drives EngineWalker from VideoState's per-line seam; main.cpp drives the same
// object across a whole run.  ONE IMPLEMENTATION, TWO DRIVERS — if a behaviour
// only exists in the whole-frame path it is a bug, not a convenience.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "descriptor.h"

namespace SuperEngine
{

// Where a deposited word goes.  The engine strobes; the sink decides what that
// means.  The suite's sink timestamps into an event list; the emulator's sink
// pushes straight into the PIXEL/VIDCMD/AUDIO models.
struct DepositSink
{
    virtual void deposit(uint64_t cycle, uint8_t signal_mask, uint16_t word) = 0;

protected:
    ~DepositSink() = default;
};

// What one call to arm()/advance() did.  `stall_cycles` is what the emulator
// charges the 68000 for the bus the engine took.
struct WalkResult
{
    uint32_t descriptors  = 0;
    uint32_t words        = 0;
    uint32_t pacing_words = 0;   // of which went to signal mask 0
    uint64_t busy_cycles  = 0;
    uint64_t end_cycle    = 0;
    bool     irq          = false;   // stop_after fired during this call
};

// The ENGINE state machine of cpld/engine/edma3.v, walked at SYSCLK
// granularity.  Cost model in descriptor.h (engine_descriptor_cycles):
//
//   [ REQUEST/WAIT_FREE = arbitration ] + ASSERT(1) + 4 descriptor words x2
//     + count payload words x2 + RELEASE(1)
//
// wait_hblank splits a descriptor in two: the four-word fetch and
// HBLANK_RELEASE happen when the descriptor is reached, then the engine parks
// until the next HBLANK rising edge, re-arbitrates, and runs the payload.  That
// is why the walker holds a `pending` descriptor across a line boundary.
class EngineWalker
{
public:
    explicit EngineWalker(uint32_t arbitration_cycles = ENGINE_ARBITRATION_CYCLES)
        : arbitration_(arbitration_cycles)
    {
    }

    // Power-on / CTRL disable.
    void reset();

    // The CPU's DESC write: arms the engine and clears a pending IRQ.  Runs
    // immediately, exactly as the hardware does, until it parks on the first
    // wait_hblank descriptor or stops.
    WalkResult arm(Memory ram, uint32_t desc_byte_address, uint64_t cycle, DepositSink &sink);

    // One HBLANK rising edge: release the parked descriptor's payload, then run
    // descriptors until the next wait_hblank parks or stop_after fires.
    WalkResult advance(Memory ram, uint64_t hblank_edge_cycle, DepositSink &sink);

    bool armed() const { return armed_; }
    bool irq_pending() const { return irq_pending_; }
    void clear_irq() { irq_pending_ = false; }
    uint32_t descriptor_pointer() const { return desc_ptr_; }

    // Structural findings, capped so a runaway list cannot flood stdout.
    const std::vector<std::string> &violations() const { return violations_; }

    // Cumulative totals over the walker's life.
    uint32_t total_descriptors() const { return total_descriptors_; }
    uint32_t total_words() const { return total_words_; }
    uint32_t total_pacing_words() const { return total_pacing_words_; }
    uint32_t irq_count() const { return irq_count_; }
    uint32_t table_low() const { return table_low_; }
    uint32_t table_high() const { return table_high_; }
    uint64_t total_busy_cycles() const { return total_busy_cycles_; }

    // Runaway guard: a list with no stop_after would walk the whole 64K table.
    void set_max_descriptors_per_call(uint32_t n) { max_per_call_ = n; }

private:
    void run_until_wait(Memory ram, DepositSink &sink, WalkResult &out);
    bool fetch_descriptor(Memory ram, Descriptor &d, WalkResult &out);
    void run_payload(Memory ram, const Descriptor &d, DepositSink &sink, WalkResult &out);
    void note(const char *fmt, ...) __attribute__((format(printf, 2, 3)));

    uint32_t arbitration_  = ENGINE_ARBITRATION_CYCLES;
    uint32_t max_per_call_ = 1u << 16;

    bool       armed_       = false;
    bool       irq_pending_ = false;
    uint32_t   desc_ptr_    = 0;
    uint64_t   cycle_       = 0;
    bool       pending_     = false;   // a wait_hblank descriptor is parked
    Descriptor pending_desc_{};

    uint32_t total_descriptors_ = 0;
    uint32_t total_words_       = 0;
    uint32_t total_pacing_words_ = 0;
    uint32_t irq_count_         = 0;
    uint32_t table_low_         = 0xFFFFFFFF;
    uint32_t table_high_        = 0;
    uint64_t total_busy_cycles_ = 0;

    std::vector<std::string> violations_;
    uint32_t                 suppressed_ = 0;
};

// ---------------------------------------------------------------------------
// Whole-run driver, for the suite
// ---------------------------------------------------------------------------

// One nSIGNAL strobe: the cycle its rising edge latches D[15:0] into whichever
// consumers the mask selects.  Pacing descriptors (mask == 0) are emitted too,
// so the event stream is a complete record of what the engine put on the bus.
struct DepositEvent
{
    uint64_t cycle       = 0;
    uint16_t word        = 0;
    uint8_t  signal_mask = 0;
};

// Per *line group*: all the work anchored to one HBLANK edge, i.e. everything
// that feeds display line g.  It starts at pixel g*H_TOTAL - H_BLANK and
// normally spills a little past pixel g*H_TOTAL into that line's active video.
// Charging by group is what makes "per-line SYSCLK" mean what the budget
// conversation means by it.
struct GroupStat
{
    uint32_t busy_cycles        = 0;
    uint32_t active_busy_cycles = 0;
    uint64_t last_busy_end      = 0;
    bool     used               = false;
};

struct InterpretParams
{
    uint32_t arbitration_cycles = ENGINE_ARBITRATION_CYCLES;
    uint64_t start_cycle        = 0;
    uint64_t end_cycle          = 0;

    // One entry per arming.  Entry 0 is the initial arm; each subsequent entry
    // is what the VBLANK ISR writes after the previous list's stop+IRQ, delayed
    // by rearm_latency_cycles.  Double buffering is just two addresses.
    std::vector<uint32_t> arm_addresses;
    uint64_t              rearm_latency_cycles = 0;
};

struct InterpretResult
{
    std::vector<DepositEvent> events;
    std::vector<GroupStat>    groups;
    std::vector<std::string>  violations;

    uint64_t busy_cycles      = 0;
    uint64_t first_cycle      = 0;
    uint64_t last_cycle       = 0;
    uint32_t descriptor_count = 0;
    uint32_t irq_count        = 0;
    uint32_t payload_words    = 0;
    uint32_t pacing_words     = 0;
    uint32_t table_low        = 0xFFFFFFFF;
    uint32_t table_high       = 0;
};

// Drives an EngineWalker across the whole run, edge by edge, collecting the
// event stream and the per-group budget.  This is the ONLY thing in the suite
// that knows about "the whole run"; everything it does, it does through the
// same EngineWalker the emulator will hold.
InterpretResult interpret(Memory ram, const InterpretParams &params);

// Raster helpers shared with the renderer.  A "group" is the HBLANK-anchored
// unit described above: group g covers pixels [g*H_TOTAL - H_BLANK,
// (g+1)*H_TOTAL - H_BLANK).
uint64_t pixel_of_cycle(uint64_t cycle);
uint64_t group_of_cycle(uint64_t cycle);
uint64_t hblank_edge_cycle(uint64_t group);

}  // namespace SuperEngine
