// interpret.h — a cycle-accurate interpreter for edma3.v's display-list DMA.
//
// Host-only (it uses std::vector/std::string for its report) but deliberately
// dependency-free otherwise: no SDL, no POSIX, no project headers beyond
// descriptor.h.  The intended next rung is to include this file straight into
// emulator/emulator.cpp as the ENGINE model, so nothing here may acquire a
// dependency the emulator would have to unpick.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "descriptor.h"

namespace SuperEngine
{

// One nSIGNAL strobe: the cycle its rising edge latches D[15:0] into whichever
// consumers the mask selects.  Pacing descriptors (mask == 0) are emitted too,
// so the event stream is a complete record of what the engine put on the bus.
struct DepositEvent
{
    uint64_t cycle       = 0;
    uint16_t word        = 0;
    uint8_t  signal_mask = 0;
};

// Per *line group*, not per raster line.  A group is all the work anchored to
// one HBLANK edge, i.e. everything that feeds display line g; it starts at
// pixel g*H_TOTAL - H_BLANK and normally spills a little past pixel g*H_TOTAL
// into that line's active video.  Charging by group is what makes "per-line
// SYSCLK" mean what the budget conversation means by it.
struct GroupStat
{
    uint32_t busy_cycles        = 0;
    uint32_t active_busy_cycles = 0;   // of which fell inside active video
    uint64_t last_busy_end      = 0;   // absolute cycle, exclusive
    bool     used               = false;
};

struct InterpretParams
{
    uint32_t arbitration_cycles = ENGINE_ARBITRATION_CYCLES;
    uint64_t start_cycle        = 0;   // when the CPU's DESC write arms the engine
    uint64_t end_cycle          = 0;   // hard stop; a list still running here is a fault

    // One entry per arming.  Entry 0 is the initial arm; each subsequent entry
    // is what the VBLANK ISR writes after the previous list's stop+IRQ, delayed
    // by rearm_latency_cycles.  Double buffering is just two addresses.
    std::vector<uint32_t> arm_addresses;
    uint64_t              rearm_latency_cycles = 0;

    // Runaway detection: a list with no stop_after would otherwise walk the
    // whole 64K table and then off the end of it.
    uint32_t max_descriptors_per_arm = 1u << 16;
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

InterpretResult interpret(Memory ram, const InterpretParams &params);

// Raster helpers shared with the renderer.  A "group" is the HBLANK-anchored
// unit described above: group g covers pixels [g*H_TOTAL - H_BLANK,
// (g+1)*H_TOTAL - H_BLANK).
uint64_t pixel_of_cycle(uint64_t cycle);
uint64_t group_of_cycle(uint64_t cycle);

}  // namespace SuperEngine
