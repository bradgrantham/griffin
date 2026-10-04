// gcx_verify.h — a .gcx file run through the REAL suite model, with delivery
// timing
//
// The encoder calls this on its own output before it writes the file, and
// gcx_roundtrip calls it on a file from disk; both get the frame the model
// renders, so "the file shows what the encoder predicted" is one comparison.
//
// It does what a viewer does: put the file in RAM, copy its descriptor table
// into ENGINE's page through GcxTarget::relocate_table() -- the same function
// the 68000 runs -- and arm the list in the vsync line.  interpret() walks the
// list and timestamps every deposit; render() plays those deposits clock by
// clock.  Unlike clut_roundtrip, which pre-pushes each line's words, a word that
// is delivered late here is late.
//
// WHAT THE MODEL DOES NOT CATCH, and this file therefore checks itself:
//
//   a group that outruns its line.  EngineWalker::advance() starts every group
//   at its HBLANK edge whatever the previous group was still doing, where the
//   chip would miss the edge and slip the frame a line.  group_costs() prices
//   every group from the descriptor table with descriptor.h's cost model and the
//   worst must fit the shortest line.
//
// The list is run at each arbitration in VerifyOptions (4 is the cost model's
// figure, 8 the pessimistic one the suite sweeps to) and must render the same
// frame at all of them.

#pragma once

#include "gcx_format.h"

#include "super-engine/interpret.h"
#include "super-engine/render.h"

#include <cstdio>
#include <string>
#include <vector>

namespace Gcx
{

// Where the verification puts things.  The load address is arbitrary and
// deliberately not zero; the table goes where an app's carve starts.
inline constexpr uint32_t VERIFY_LOAD_ADDRESS  = 0x100000;
inline constexpr uint32_t VERIFY_TABLE_ADDRESS = 0x3F2000;
inline constexpr uint32_t VERIFY_TABLE_BYTES   = 0x00E000;

// The vsync ISR's arm point: inside line V_SYNC_START, after its 8-clock /RS.
inline constexpr uint32_t VERIFY_ARM_PIXEL = 400;

// SYSCLK between two HBLANK edges is 444 or 445; a group must fit the shorter,
// less the two-cycle HBLANK synchronizer in front of it.
inline constexpr uint32_t GROUP_SYSCLK_LIMIT =
    static_cast<uint32_t>(se::LINE_SYSCLK) - 1 - se::ENGINE_HBLANK_SYNC_CYCLES;

struct VerifyOptions
{
    std::vector<uint32_t> arbitrations = { se::ENGINE_ARBITRATION_CYCLES, 8 };
};

struct VerifyReport
{
    std::vector<std::string> problems;
    std::vector<se::Rgb444>  frame;          // 640 x 480, from the first arbitration
    uint32_t                 worst_group = 0;   // SYSCLK, at the first arbitration
    uint32_t                 fifo_high   = 0;   // VIDCMD FIFO high water
    uint32_t                 descriptors = 0;

    bool ok() const { return problems.empty(); }
};

// The cost of every HBLANK-anchored group in a table, in SYSCLK from the edge:
// the parked wait descriptor's payload, the descriptors behind it, and the fetch
// of the next wait descriptor, which must park before the next edge.
inline uint32_t worst_group_cost(const uint16_t *table, uint32_t count, uint32_t arbitration)
{
    const uint32_t fetch = arbitration + se::ENGINE_ASSERT_CYCLES + se::ENGINE_FETCH_CYCLES + 1;
    uint32_t       worst = 0;
    uint32_t       cost  = 0;
    bool           open  = false;
    for (uint32_t i = 0; i < count; i++)
    {
        const se::Descriptor d = se::decode_descriptor(&table[i * se::DESC_WORDS]);
        if (d.wait_hblank)
        {
            if (open)
            {
                cost += fetch;
                worst = (cost > worst) ? cost : worst;
            }
            open = true;
            cost = arbitration + se::ENGINE_ASSERT_CYCLES + se::ENGINE_CYCLES_PER_WORD * d.count
                   + se::ENGINE_RELEASE_CYCLES;
        }
        else if (open)
        {
            cost += se::engine_descriptor_cycles(arbitration, d.count);
        }
    }
    return worst;
}

inline VerifyReport verify_file(const std::vector<uint8_t> &data, const VerifyOptions &options = {})
{
    VerifyReport report;
    char         buf[256];
    auto problem = [&](const char *text) { report.problems.emplace_back(text); };

    if ((data.size() & 1u) != 0)
    {
        problem("odd file length");
        return report;
    }
    const std::vector<uint16_t> words = file_words(data);

    GcxTarget::Picture picture;
    const char        *why = nullptr;
    if (!GcxTarget::open_picture(words.data(), static_cast<uint32_t>(words.size()), &picture, &why))
    {
        problem(why);
        return report;
    }
    report.descriptors = picture.desc_count;
    if (picture.table_bytes() > VERIFY_TABLE_BYTES)
    {
        std::snprintf(buf, sizeof(buf), "descriptor table is %u bytes, the app carve is %u",
                      picture.table_bytes(), VERIFY_TABLE_BYTES);
        problem(buf);
        return report;
    }
    if (VERIFY_LOAD_ADDRESS + data.size() > VERIFY_TABLE_ADDRESS)
    {
        problem("file too large for the verification RAM layout");
        return report;
    }

    std::vector<uint16_t> ram_words(se::RAM_BYTES / 2, 0);
    for (size_t i = 0; i < words.size(); i++)
    {
        ram_words[VERIFY_LOAD_ADDRESS / 2 + i] = words[i];
    }
    GcxTarget::relocate_table(&ram_words[VERIFY_TABLE_ADDRESS / 2], picture, VERIFY_LOAD_ADDRESS);
    const se::Memory ram{ std::span<uint16_t>(ram_words) };

    // Arm in the vsync line of frame 0, and again a frame later: the stop IRQ
    // fires just after line 479's HBLANK edge and the vsync ISR's arm comes ten
    // lines on.  Frames 1 and 2 are the ones rendered.
    const uint64_t arm_cycle =
        se::sysclk_of_pixel(static_cast<uint64_t>(se::V_SYNC_START) * se::H_TOTAL + VERIFY_ARM_PIXEL);
    const uint64_t stop_cycle =
        se::sysclk_of_pixel(static_cast<uint64_t>(se::V_ACTIVE - 1) * se::H_TOTAL + se::H_ACTIVE);

    bool first = true;
    for (uint32_t arbitration : options.arbitrations)
    {
        const uint32_t worst =
            worst_group_cost(&ram_words[VERIFY_TABLE_ADDRESS / 2], picture.desc_count, arbitration);
        if (first)
        {
            report.worst_group = worst;
        }
        if (worst > GROUP_SYSCLK_LIMIT)
        {
            std::snprintf(buf, sizeof(buf),
                          "arbitration %u: worst line group costs %u SYSCLK, the limit is %u",
                          arbitration, worst, GROUP_SYSCLK_LIMIT);
            problem(buf);
        }

        se::InterpretParams ip;
        ip.arbitration_cycles   = arbitration;
        ip.start_cycle          = arm_cycle;
        ip.end_cycle            = se::sysclk_of_pixel((3ull * se::V_TOTAL + 1) * se::H_TOTAL);
        ip.rearm_latency_cycles = arm_cycle - stop_cycle - 16;
        ip.arm_addresses        = { VERIFY_TABLE_ADDRESS, VERIFY_TABLE_ADDRESS };
        const se::InterpretResult ir = se::interpret(ram, ip);

        for (const std::string &v : ir.violations)
        {
            std::snprintf(buf, sizeof(buf), "arbitration %u: engine: %s", arbitration, v.c_str());
            problem(buf);
        }
        if (ir.descriptor_count != 2 * picture.desc_count)
        {
            std::snprintf(buf, sizeof(buf), "arbitration %u: walked %u descriptors, expected %u",
                          arbitration, ir.descriptor_count, 2 * picture.desc_count);
            problem(buf);
        }

        se::RenderParams rp;
        rp.first_frame = 1;
        rp.frame_count = 2;
        const se::RenderResult rr = se::render(ir.events, rp);

        for (const std::string &v : rr.violations)
        {
            std::snprintf(buf, sizeof(buf), "arbitration %u: render: %s", arbitration, v.c_str());
            problem(buf);
        }
        struct Counter
        {
            const char *name;
            uint32_t    value;
        };
        const Counter counters[] = {
            { "palette_unwritten_reads",   rr.stats.palette_unwritten_reads },
            { "palette_bad_filler",        rr.stats.palette_bad_filler },
            { "palette_disturbed_visible", rr.stats.palette_disturbed_visible },
            { "pixels_tiled_words",        rr.stats.pixels_tiled_words },
            { "pixels_overflows",          rr.stats.pixels_overflows },
            { "vidcmd_overflows",          rr.stats.vidcmd_overflows },
            { "vidcmd_overruns",           rr.stats.vidcmd_overruns },
            { "vidcmd_hold_slots",         rr.stats.vidcmd_hold_slots },
            { "vidcmd_stream_leftovers",   rr.stats.vidcmd_stream_leftovers },
            { "vidcmd_mask_stalls",        rr.stats.vidcmd_mask_stalls },
        };
        for (const Counter &c : counters)
        {
            if (c.value != 0)
            {
                std::snprintf(buf, sizeof(buf), "arbitration %u: model counter %s = %u, expected 0",
                              arbitration, c.name, c.value);
                problem(buf);
            }
        }

        if (rr.frames.size() != 2 || rr.frames[0].pixels != rr.frames[1].pixels)
        {
            std::snprintf(buf, sizeof(buf), "arbitration %u: the two rendered frames differ",
                          arbitration);
            problem(buf);
        }
        if (first)
        {
            report.fifo_high = rr.stats.vidcmd_fifo_high;
            if (!rr.frames.empty())
            {
                report.frame = rr.frames[0].pixels;
            }
        }
        else if (!rr.frames.empty() && rr.frames[0].pixels != report.frame)
        {
            std::snprintf(buf, sizeof(buf),
                          "arbitration %u renders a different frame from arbitration %u",
                          arbitration, options.arbitrations[0]);
            problem(buf);
        }
        first = false;
    }
    return report;
}

}   // namespace Gcx
