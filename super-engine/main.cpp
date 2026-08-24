// main.cpp — drive every validation case, print the budget report, write the
// artifacts, and exit nonzero if anything failed.
//
// First host-side test in this repo, so: no test framework, assert-with-message
// only.  A failing check prints file:line and a sentence and bumps a counter;
// every case still runs, because the point of the suite is the *table*, not the
// first stop.  Everything is integer arithmetic seeded from constants, so two
// runs produce byte-identical stdout and byte-identical artifacts.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "author.h"
#include "descriptor.h"
#include "interpret.h"
#include "render.h"

using namespace SuperEngine;

namespace
{

int g_failures = 0;

#define CHECK(cond, ...)                                              \
    do                                                                \
    {                                                                 \
        if (!(cond))                                                  \
        {                                                             \
            fprintf(stderr, "  FAIL %s:%d: ", __FILE__, __LINE__);     \
            fprintf(stderr, __VA_ARGS__);                             \
            fprintf(stderr, "\n");                                    \
            g_failures++;                                             \
        }                                                             \
    } while (0)

// ---------------------------------------------------------------------------
// RAM map for the suite
// ---------------------------------------------------------------------------
//
// One flat 4 MB image, exactly what the engine sees.  Regions are spaced out
// rather than packed so a descriptor that walks off the end of one lands in
// zeroes rather than in the next region's data.
//
// Note what is NOT here any more: there is no palette table and no register
// scratch area.  Palette, mode and pixel_skip values now live inside the VIDCMD
// SET words themselves, which removed a whole class of aliasing bug — the
// previous round of this suite had two frames' lists sharing one MODE scratch
// word, and frame N+1's pixel_skip leaked into frame N's deposits.

constexpr uint32_t FB_BASE            = 0x001000;
constexpr uint32_t FB_STRIDE_BYTES    = 88;    // 704 pixels: 640 visible + 64 of scroll
constexpr uint32_t FB_WORDS_PER_LINE  = FB_STRIDE_BYTES / 2;
constexpr uint32_t FB_LINES           = 600;   // 480 visible + 120 of vertical scroll

constexpr uint32_t FB_SOLID_BASE      = 0x010000;   // every bit set, for the slot regression
constexpr uint32_t FB_HAM_BASE        = 0x020000;
constexpr uint32_t FB_HAM_STRIDE      = 176;   // 88 words: 80 for the line + scroll headroom
constexpr uint32_t FB_WEB_BASE        = 0x040000;   // the Tempest wireframe, drawn once

// The VIDCMD records are per-frame data — they carry this frame's palette
// ramp, its pixel_skip and its tile masks — so they need double buffering
// exactly as much as the descriptor table does.  Only the table has to live in
// the top 64K; the VIDCMD regions are ordinary RAM, which makes it easy to
// forget that the CPU is authoring frame N+1's records while the engine is
// still reading frame N's.
constexpr uint32_t VIDCMD_BASE          = 0x100000;
constexpr uint32_t VIDCMD_REGION_BYTES  = 0x028000;   // 480 lines x 320 bytes, rounded up
constexpr uint32_t AUDIO_BASE           = 0x060000;
constexpr uint32_t AUDIO_SOURCE_PAIRS = 1024;

// Two 32K halves of the 64K descriptor window: the CPU authors the next frame's
// list into the half the engine is not reading.  M5 asserts the halves really
// are disjoint from every address the running list touches.
constexpr uint32_t TABLE_A            = DESC_TABLE_BASE;
constexpr uint32_t TABLE_B            = DESC_TABLE_BASE + DESC_TABLE_BYTES / 2;
constexpr uint32_t TABLE_HALF_BYTES   = DESC_TABLE_BYTES / 2;

// The audio FIFO starts empty, so the very first list has to prime it as well
// as carry vertical blanking's share.  A real driver does this once at startup
// and then rides the PORTS HALF_FULL status; here it is one bigger preamble on
// list A.  Without it the steady-state low water is 1 pair.
constexpr uint32_t AUDIO_PRIME_PAIRS  = 48;

// ---------------------------------------------------------------------------
// Frame lock
// ---------------------------------------------------------------------------
//
// Every list ends on a wait_hblank/stop_after pacer, so nENGINE_IRQ always
// fires just after raster line 479's HBLANK edge no matter how heavy the frame
// was.  The ISR then re-arms somewhere inside line 480, and the list's leading
// wait_hblank pacers walk it to the top of frame: lines 480..523 is 44 HBLANK
// edges, after which the first *pixel* group's wait lands on line 524's edge
// and its words are consumed by line 0 of the next raster frame.
//
// Consequence: the images this suite renders are raster frames 1 and 2, never
// frame 0, which is the frame during which the engine gets armed.
constexpr uint32_t VBLANK_PACING_LINES = 44;
constexpr uint32_t FIRST_ARM_LINE      = 480;
constexpr uint32_t FIRST_ARM_PIXEL     = 100;   // comfortably before that line's h=640
constexpr uint32_t RENDER_FIRST_FRAME  = 1;
constexpr uint32_t RENDER_FRAMES       = 2;

// 68000 interrupt latency plus the ISR's table swap and DESC write.  200 SYSCLK
// is 14.3 us at 14 MHz; case M5 measures how much more the list can absorb
// before it slips a line.
constexpr uint64_t REARM_LATENCY_CYCLES = 200;

uint64_t first_arm_cycle()
{
    return sysclk_of_pixel(static_cast<uint64_t>(FIRST_ARM_LINE) * H_TOTAL + FIRST_ARM_PIXEL);
}

uint64_t run_end_cycle()
{
    const uint64_t frames = RENDER_FIRST_FRAME + RENDER_FRAMES;
    return sysclk_of_pixel((frames * V_TOTAL + 1) * static_cast<uint64_t>(H_TOTAL));
}

// ---------------------------------------------------------------------------
// Artifact writers
// ---------------------------------------------------------------------------

void write_ppm(const std::string &path, const FrameImage &img)
{
    FILE *f = fopen(path.c_str(), "wb");
    if (f == nullptr)
    {
        fprintf(stderr, "  FAIL cannot open %s for writing\n", path.c_str());
        g_failures++;
        return;
    }
    fprintf(f, "P6\n%u %u\n255\n", H_ACTIVE, V_ACTIVE);
    std::vector<uint8_t> row(static_cast<size_t>(H_ACTIVE) * 3);
    for (uint32_t y = 0; y < V_ACTIVE; y++)
    {
        for (uint32_t x = 0; x < H_ACTIVE; x++)
        {
            uint8_t r = 0;
            uint8_t g = 0;
            uint8_t b = 0;
            rgb444_to_rgb888(img.pixels[y * H_ACTIVE + x], r, g, b);
            row[x * 3 + 0] = r;
            row[x * 3 + 1] = g;
            row[x * 3 + 2] = b;
        }
        fwrite(row.data(), 1, row.size(), f);
    }
    fclose(f);
}

void put_u32_le(FILE *f, uint32_t v)
{
    const uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8),
                          static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)};
    fwrite(b, 1, 4, f);
}

void put_u16_le(FILE *f, uint16_t v)
{
    const uint8_t b[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
    fwrite(b, 1, 2, f);
}

// 8-bit unsigned stereo PCM, the format the deposit word already is.  Audio was
// not touched by the move to 12-bit colour.
void write_wav(const std::string &path, const std::vector<uint8_t> &samples)
{
    FILE *f = fopen(path.c_str(), "wb");
    if (f == nullptr)
    {
        fprintf(stderr, "  FAIL cannot open %s for writing\n", path.c_str());
        g_failures++;
        return;
    }
    const uint32_t data_bytes = static_cast<uint32_t>(samples.size());
    fwrite("RIFF", 1, 4, f);
    put_u32_le(f, 36 + data_bytes);
    fwrite("WAVEfmt ", 1, 8, f);
    put_u32_le(f, 16);                       // PCM fmt chunk size
    put_u16_le(f, 1);                        // PCM
    put_u16_le(f, 2);                        // stereo
    put_u32_le(f, AUDIO_SAMPLE_RATE);
    put_u32_le(f, AUDIO_SAMPLE_RATE * 2);    // byte rate
    put_u16_le(f, 2);                        // block align
    put_u16_le(f, 8);                        // bits per sample
    fwrite("data", 1, 4, f);
    put_u32_le(f, data_bytes);
    fwrite(samples.data(), 1, data_bytes, f);
    fclose(f);
}

// ---------------------------------------------------------------------------
// Static-asset drawing: the Tempest well
// ---------------------------------------------------------------------------
//
// This models the CPU rasterizing a level's wireframe into the 1bpp bitmap
// once, at level start.  Integer Bresenham, no floating point, so the asset is
// byte-identical on every host and between runs.  Nothing here runs per frame —
// that is the entire claim the tempest-web case exists to demonstrate.

void plot_1bpp(Memory ram, uint32_t base, uint32_t stride, int32_t x, int32_t y)
{
    if (x < 0 || y < 0 || x >= static_cast<int32_t>(H_ACTIVE) ||
        y >= static_cast<int32_t>(V_ACTIVE))
    {
        return;
    }
    const uint32_t addr = base + static_cast<uint32_t>(y) * stride +
                          (static_cast<uint32_t>(x) / 16u) * 2u;
    ram[addr] = static_cast<uint16_t>(ram[addr] | (0x8000u >> (static_cast<uint32_t>(x) % 16u)));
}

void draw_line_1bpp(Memory ram, uint32_t base, uint32_t stride,
                    int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    const int32_t dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    const int32_t dy = (y1 > y0) ? (y0 - y1) : (y1 - y0);   // negative magnitude
    const int32_t sx = (x0 < x1) ? 1 : -1;
    const int32_t sy = (y0 < y1) ? 1 : -1;
    int32_t err = dx + dy;

    for (;;)
    {
        plot_1bpp(ram, base, stride, x0, y0);
        if (x0 == x1 && y0 == y1)
        {
            break;
        }
        const int32_t e2 = 2 * err;
        if (e2 >= dy)
        {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            y0 += sy;
        }
    }
}

// 16 lanes: the near (outer) rim polygon, the far (inner) rim polygon, and a
// spoke joining each pair of corresponding vertices.
void draw_tempest_web(Memory ram, uint32_t base, uint32_t stride)
{
    for (uint32_t i = 0; i < TEMPEST_LANES; i++)
    {
        const uint32_t j = (i + 1) % TEMPEST_LANES;
        draw_line_1bpp(ram, base, stride, tempest_outer_x(i), tempest_outer_y(i),
                       tempest_outer_x(j), tempest_outer_y(j));
        draw_line_1bpp(ram, base, stride, tempest_inner_x(i), tempest_inner_y(i),
                       tempest_inner_x(j), tempest_inner_y(j));
        draw_line_1bpp(ram, base, stride, tempest_inner_x(i), tempest_inner_y(i),
                       tempest_outer_x(i), tempest_outer_y(i));
    }
}

// Cheap order-sensitive checksum, used to prove the bitmap is never rewritten.
uint32_t checksum_region(Memory ram, uint32_t base, uint32_t bytes)
{
    uint32_t h = 2166136261u;
    for (uint32_t off = 0; off < bytes; off += 2)
    {
        h = (h ^ ram[base + off]) * 16777619u;
    }
    return h;
}

// ---------------------------------------------------------------------------
// Case plumbing
// ---------------------------------------------------------------------------

struct CaseSpec
{
    std::string name;
    std::string title;
    FrameParams base;
    uint32_t    v_scroll_step = 0;
    uint32_t    h_scroll_step = 0;
};

struct GroupBudget
{
    uint32_t max_busy   = 0;
    double   mean_busy  = 0.0;
    uint32_t max_spill  = 0;
    double   mean_spill = 0.0;
    uint32_t max_active = 0;
};

// Per-line budget over the groups that actually feed displayed lines; the 44
// vblank pacers and the vblank preamble are deliberately excluded so the
// numbers mean "what a visible scanline costs".
GroupBudget group_budget(const InterpretResult &ir)
{
    GroupBudget b;
    uint64_t total_busy  = 0;
    uint64_t total_spill = 0;
    uint32_t n           = 0;

    for (uint32_t frame = RENDER_FIRST_FRAME; frame < RENDER_FIRST_FRAME + RENDER_FRAMES; frame++)
    {
        for (uint32_t line = 0; line < V_ACTIVE; line++)
        {
            const size_t g = static_cast<size_t>(frame) * V_TOTAL + line;
            if (g >= ir.groups.size() || !ir.groups[g].used)
            {
                continue;
            }
            const GroupStat &s = ir.groups[g];
            total_busy += s.busy_cycles;
            n++;
            if (s.busy_cycles > b.max_busy)
            {
                b.max_busy = s.busy_cycles;
            }
            if (s.active_busy_cycles > b.max_active)
            {
                b.max_active = s.active_busy_cycles;
            }

            const uint64_t hblank_end = sysclk_of_pixel(static_cast<uint64_t>(g) * H_TOTAL);
            const uint32_t spill = (s.last_busy_end > hblank_end)
                                       ? static_cast<uint32_t>(s.last_busy_end - hblank_end)
                                       : 0u;
            total_spill += spill;
            if (spill > b.max_spill)
            {
                b.max_spill = spill;
            }
        }
    }

    if (n > 0)
    {
        b.mean_busy  = static_cast<double>(total_busy) / n;
        b.mean_spill = static_cast<double>(total_spill) / n;
    }
    return b;
}

struct CaseResult
{
    AuthorResult    author[RENDER_FRAMES];
    InterpretResult interp;
    RenderResult    rendered;
    GroupBudget     budget;
    uint32_t        vidcmd_words_total = 0;
    uint32_t        vidcmd_words_min   = 0xFFFFFFFF;
    uint32_t        vidcmd_words_max   = 0;
    double          vidcmd_words_mean  = 0.0;
    uint32_t        vidcmd_records_max = 0;
    double          vidcmd_records_mean = 0.0;
    uint32_t        vidcmd_slot_min    = 0xFFFFFFFF;
    uint32_t        vidcmd_slot_max    = 0;
    uint32_t        vidcmd_stretch_max = 0;
    uint32_t        vidcmd_stretch_lines = 0;
    std::vector<std::string> artifacts;
};

// Build both frames' lists and run them back to back through the interpreter.
CaseResult run_case(const CaseSpec &spec, Memory ram, uint64_t rearm_latency)
{
    CaseResult out;

    const uint32_t tables[RENDER_FRAMES] = {TABLE_A, TABLE_B};
    InterpretParams ip;
    ip.arbitration_cycles   = spec.base.arbitration_cycles;
    ip.start_cycle          = first_arm_cycle();
    ip.end_cycle            = run_end_cycle();
    ip.rearm_latency_cycles = rearm_latency;

    std::vector<uint8_t>  line_words(V_ACTIVE, 0);
    std::vector<uint8_t>  line_records(V_ACTIVE, 0);
    std::vector<uint16_t> line_slots(V_ACTIVE, 0);
    std::vector<uint16_t> line_stretch(V_ACTIVE, 0);

    uint32_t audio_pair_cursor = 0;
    for (uint32_t f = 0; f < RENDER_FRAMES; f++)
    {
        FrameParams fp         = spec.base;
        fp.table_base          = tables[f];
        fp.table_bytes         = TABLE_HALF_BYTES;
        fp.vidcmd_base         = spec.base.vidcmd_base + f * VIDCMD_REGION_BYTES;
        fp.frame_index         = spec.base.frame_index + f;
        fp.v_scroll_lines      = spec.base.v_scroll_lines + f * spec.v_scroll_step;
        fp.h_scroll_pixels     = spec.base.h_scroll_pixels + f * spec.h_scroll_step;
        fp.vblank_pacing_lines = VBLANK_PACING_LINES;
        fp.audio_preamble_pairs =
            spec.base.audio_preamble_pairs + ((f == 0) ? AUDIO_PRIME_PAIRS : 0);
        fp.audio_frame_pair_base = audio_pair_cursor;
        audio_pair_cursor += fp.audio_preamble_pairs +
                             (V_ACTIVE / spec.base.audio_burst_interval) * spec.base.audio_burst_pairs;

        write_vidcmd_records(fp, ram, line_words, line_records, line_slots, line_stretch);

        out.author[f] = author_frame(fp, ram, line_words);
        ip.arm_addresses.push_back(tables[f]);

        // Accumulate over BOTH frames: the tempest case moves its objects
        // between them, so frame 2 can be the denser one.
        uint32_t words_sum   = 0;
        uint32_t records_sum = 0;
        for (uint32_t line = 0; line < V_ACTIVE; line++)
        {
            words_sum += line_words[line];
            records_sum += line_records[line];
            if (line_words[line] < out.vidcmd_words_min)   { out.vidcmd_words_min = line_words[line]; }
            if (line_words[line] > out.vidcmd_words_max)   { out.vidcmd_words_max = line_words[line]; }
            if (line_records[line] > out.vidcmd_records_max) { out.vidcmd_records_max = line_records[line]; }
            if (line_slots[line] < out.vidcmd_slot_min)    { out.vidcmd_slot_min = line_slots[line]; }
            if (line_slots[line] > out.vidcmd_slot_max)    { out.vidcmd_slot_max = line_slots[line]; }
            if (line_stretch[line] > out.vidcmd_stretch_max)
            {
                out.vidcmd_stretch_max = line_stretch[line];
            }
            if (line_stretch[line] > 0)
            {
                out.vidcmd_stretch_lines++;
            }
        }
        if (f == 0)
        {
            out.vidcmd_words_total = words_sum;
        }
        out.vidcmd_words_mean   = static_cast<double>(words_sum) / V_ACTIVE;
        out.vidcmd_records_mean = static_cast<double>(records_sum) / V_ACTIVE;
    }

    out.interp = interpret(ram, ip);

    RenderParams rp;
    rp.first_frame   = RENDER_FIRST_FRAME;
    rp.frame_count   = RENDER_FRAMES;
    rp.audio_enabled = spec.base.audio;
    rp.skew_pix      = spec.base.skew_pix;
    rp.skew_cmp      = spec.base.skew_cmp;
    out.rendered     = render(out.interp.events, rp);

    out.budget = group_budget(out.interp);
    return out;
}

void print_report(const CaseSpec &spec, const CaseResult &res)
{
    printf("\n=== %s — %s ===\n", spec.name.c_str(), spec.title.c_str());

    const AuthorResult &a0 = res.author[0];
    const AuthorResult &a1 = res.author[1];
    const InterpretResult &ir = res.interp;
    const RenderResult &rr = res.rendered;

    const uint64_t span = (ir.last_cycle > ir.first_cycle) ? (ir.last_cycle - ir.first_cycle) : 1;
    const double util = 100.0 * static_cast<double>(ir.busy_cycles) / static_cast<double>(span);

    printf("  descriptors/frame   %6u          table bytes %6u / %u per half (%u total window)\n",
           a0.descriptor_count, a0.table_bytes, TABLE_HALF_BYTES, DESC_TABLE_BYTES);
    printf("  payload words/frame %6u          of which pacing %u\n",
           a0.payload_words, a0.pacing_words);
    printf("  PIXELS words/line   %6u          VIDCMD words/line %u..%u (mean %.2f), %u/frame\n",
           pixel_words_for(spec.base.mode, spec.base.h_scroll_pixels % 16),
           res.vidcmd_words_min, res.vidcmd_words_max, res.vidcmd_words_mean,
           res.vidcmd_words_total);
    printf("  VIDCMD records/line max %3u  mean %.2f\n",
           res.vidcmd_records_max, res.vidcmd_records_mean);
    printf("  VIDCMD slots/line   %6u..%-6u framing %s (%s)\n",
           res.vidcmd_slot_min, res.vidcmd_slot_max,
           (spec.base.framing == FramingMode::CUSHION) ? "CUSHION" : "JIT    ",
           (spec.base.framing == FramingMode::CUSHION) ? "occupancy must be exactly 640"
                                                       : "<= 640, hold covers the rest");
    printf("  VIDCMD cadence      stretch max %3u slot(s) on %u of %u lines "
           "(fetch = %u slots/word)\n",
           res.vidcmd_stretch_max, res.vidcmd_stretch_lines, V_ACTIVE * RENDER_FRAMES,
           VIDCMD_SLOTS_PER_WORD);
    printf("  bus utilization     %6.1f%%         over %llu SYSCLK of the 2-frame run\n",
           util, static_cast<unsigned long long>(span));
    printf("  per-line SYSCLK     max %4u  mean %6.1f   (line = %llu SYSCLK, %.0f%% worst)\n",
           res.budget.max_busy, res.budget.mean_busy,
           static_cast<unsigned long long>(LINE_SYSCLK),
           100.0 * res.budget.max_busy / static_cast<double>(LINE_SYSCLK));
    printf("  HBLANK spill        max %4u  mean %6.1f   (HBLANK = %llu SYSCLK)\n",
           res.budget.max_spill, res.budget.mean_spill,
           static_cast<unsigned long long>(HBLANK_SYSCLK));
    printf("  active-video steal  max %4u SYSCLK/line\n", res.budget.max_active);
    printf("  IRQ count           %6u          descriptors executed %u\n",
           ir.irq_count, ir.descriptor_count);
    printf("  table window used   0x%06X..0x%06X\n", ir.table_low, ir.table_high);

    if (a1.descriptor_count != a0.descriptor_count || a1.table_bytes != a0.table_bytes)
    {
        printf("  frame 2 list        %6u descriptors, %u bytes\n",
               a1.descriptor_count, a1.table_bytes);
    }

    printf("  PIXELS FIFO         high %3u  low %3u  tiled words %u  overflows %u  (depth %u)\n",
           rr.stats.pixels_fifo_high, rr.stats.pixels_fifo_low, rr.stats.pixels_tiled_words,
           rr.stats.pixels_overflows, PIXELS_FIFO_WORDS);
    printf("  PIXELS line-start   banked words min %3u  max %3u\n",
           rr.stats.pixels_line_start_min, rr.stats.pixels_line_start_max);
    printf("  VIDCMD FIFO         high %3u  low %3u  popped %u  overflows %u\n",
           rr.stats.vidcmd_fifo_high, rr.stats.vidcmd_fifo_low, rr.stats.vidcmd_words_popped,
           rr.stats.vidcmd_overflows);
    printf("  VIDCMD holds        cadence %u  starved %u   (cadence = the 2-clock fetch, "
           "starved = a dry FIFO)\n",
           rr.stats.vidcmd_cadence_slots, rr.stats.vidcmd_hold_slots);
    printf("  VIDCMD framing      overruns %u  late words %u  RUN_COLOR %u  reserved no-ops %u\n",
           rr.stats.vidcmd_overruns, rr.stats.vidcmd_late_words, rr.stats.vidcmd_color_runs,
           rr.stats.vidcmd_reserved_ops);
    if (spec.base.audio)
    {
        printf("  AUDIO FIFO          high %4u low %4u underruns %u  overflows %u  (depth %u)\n",
               rr.audio_fifo_high, rr.audio_fifo_low, rr.audio_underruns,
               rr.audio_overflows, AUDIO_FIFO_PAIRS);
        printf("  AUDIO pairs         %u deposited inside the %u rendered frames, %u consumed\n",
               rr.audio_pairs_deposited, RENDER_FRAMES, rr.audio_pairs_consumed);
    }

    for (const std::string &v : ir.violations)
    {
        printf("  VIOLATION  %s\n", v.c_str());
    }

    if (!res.artifacts.empty())
    {
        printf("  artifacts          ");
        for (const std::string &s : res.artifacts)
        {
            printf(" %s", s.c_str());
        }
        printf("\n");
    }
}

// Common structural checks every case must satisfy.
void check_case(const CaseSpec &spec, const CaseResult &res)
{
    const AuthorResult &a0 = res.author[0];
    const AuthorResult &a1 = res.author[1];
    const InterpretResult &ir = res.interp;
    const RenderResult &rr = res.rendered;

    CHECK(!a0.table_overflow, "%s: frame 1 list overflowed its %u-byte table half",
          spec.name.c_str(), TABLE_HALF_BYTES);
    CHECK(!a1.table_overflow, "%s: frame 2 list overflowed its %u-byte table half",
          spec.name.c_str(), TABLE_HALF_BYTES);
    CHECK(ir.violations.empty(), "%s: interpreter reported %zu structural violations",
          spec.name.c_str(), ir.violations.size());
    CHECK(ir.irq_count == RENDER_FRAMES, "%s: expected %u IRQs, got %u",
          spec.name.c_str(), RENDER_FRAMES, ir.irq_count);
    CHECK(ir.descriptor_count == a0.descriptor_count + a1.descriptor_count,
          "%s: executed %u descriptors, authored %u",
          spec.name.c_str(), ir.descriptor_count, a0.descriptor_count + a1.descriptor_count);
    CHECK(res.budget.max_busy < LINE_SYSCLK,
          "%s: worst line costs %u SYSCLK, more than the %llu-SYSCLK line",
          spec.name.c_str(), res.budget.max_busy, static_cast<unsigned long long>(LINE_SYSCLK));

    // Framing: the two disciplines make different promises, so the checker
    // asserts different things.  This is the whole point of FramingMode — the
    // hardware rule is one rule, and only the list builder knows which contract
    // it signed up to.
    if (spec.base.framing == FramingMode::CUSHION)
    {
        // Records are buffered ahead in VBLANK, so the OCCUPANCY must close
        // exactly and hold must never engage for want of data.  The occupancy
        // the author reports comes from vidcmd_plan_line(); the render model
        // drives the same fetch engine independently against real deposit
        // cycles, so the overrun counter below is what cross-checks the two —
        // if the planner's arithmetic were wrong the line would overrun here.
        CHECK(res.vidcmd_slot_min == H_ACTIVE && res.vidcmd_slot_max == H_ACTIVE,
              "%s: cushion list occupancy ranges %u..%u, must be exactly %u on every line",
              spec.name.c_str(), res.vidcmd_slot_min, res.vidcmd_slot_max, H_ACTIVE);
        CHECK(rr.stats.vidcmd_hold_slots == 0,
              "%s: cushion list held on an empty FIFO for %u slots — the cushion ran out",
              spec.name.c_str(), rr.stats.vidcmd_hold_slots);
        CHECK(rr.stats.vidcmd_overruns == 0,
              "%s: %u records were still running at the h=%u fall, so a leftover plays at "
              "slot 0 of the next line", spec.name.c_str(), rr.stats.vidcmd_overruns, H_ACTIVE);
    }
    else
    {
        // JIT lists deliberately under-fill and let hold replicate the last
        // source, so hold slots are the mechanism rather than a fault.  What
        // must hold instead is the DELIVERY DEADLINE: every word of a line's
        // packet has to be in the FIFO before that line's pixel 0, or the
        // packet resumes at the wrong x and only self-heals at the next empty
        // boundary (compositor_tb's LATE_FILL).
        CHECK(res.vidcmd_slot_max <= H_ACTIVE,
              "%s: JIT list has a line of %u slots, more than the %u available",
              spec.name.c_str(), res.vidcmd_slot_max, H_ACTIVE);
        CHECK(rr.stats.vidcmd_late_words == 0,
              "%s: %u VIDCMD words arrived after their line had already started — the "
              "hblank delivery deadline was missed",
              spec.name.c_str(), rr.stats.vidcmd_late_words);
    }

    // PIXELS tiling is legal by construction (pixel.v has no empty flag and a
    // 7200 holds Q), so a short fill is a bandwidth compressor.  None of these
    // cases wants one, so a nonzero count means the list is wrong, not the
    // hardware.
    CHECK(rr.stats.pixels_tiled_words == 0,
          "%s: PIXEL tiled its last word %u times — the pixel stream came up short",
          spec.name.c_str(), rr.stats.pixels_tiled_words);
    CHECK(rr.stats.pixels_overflows == 0, "%s: PIXELS FIFO overflowed %u times",
          spec.name.c_str(), rr.stats.pixels_overflows);
    CHECK(rr.stats.vidcmd_overflows == 0, "%s: VIDCMD FIFO overflowed %u times",
          spec.name.c_str(), rr.stats.vidcmd_overflows);
    CHECK(rr.stats.vidcmd_reserved_ops == 0,
          "%s: %u reserved `01` no-ops reached the compositor — nothing should emit them",
          spec.name.c_str(), rr.stats.vidcmd_reserved_ops);

    if (spec.base.audio)
    {
        CHECK(rr.audio_underruns == 0, "%s: audio underran %u times",
              spec.name.c_str(), rr.audio_underruns);
        CHECK(rr.audio_overflows == 0, "%s: audio FIFO overflowed %u times",
              spec.name.c_str(), rr.audio_overflows);
    }
}

void write_artifacts(const CaseSpec &spec, CaseResult &res)
{
    for (uint32_t f = 0; f < RENDER_FRAMES; f++)
    {
        const std::string path = spec.name + "-f" + std::to_string(f + 1) + ".ppm";
        write_ppm(path, res.rendered.frames[f]);
        res.artifacts.push_back(path);
    }
    if (spec.base.audio && !res.rendered.audio.empty())
    {
        const std::string path = spec.name + ".wav";
        write_wav(path, res.rendered.audio);
        res.artifacts.push_back(path);
    }
}

// ---------------------------------------------------------------------------
// Case-specific content checks
// ---------------------------------------------------------------------------

Rgb444 at(const FrameImage &img, uint32_t x, uint32_t y)
{
    return img.pixels[y * H_ACTIVE + x];
}

// The vertical scroll must move the image by exactly `step` lines and nothing
// else, so frame 2 row y has to equal frame 1 row y+step everywhere.
void check_vertical_scroll(const CaseSpec &spec, const CaseResult &res, uint32_t step)
{
    if (step == 0)
    {
        return;
    }
    const FrameImage &f1 = res.rendered.frames[0];
    const FrameImage &f2 = res.rendered.frames[1];
    uint32_t mismatched = 0;
    for (uint32_t y = 0; y + step < V_ACTIVE; y++)
    {
        for (uint32_t x = 0; x < H_ACTIVE; x++)
        {
            if (at(f2, x, y) != at(f1, x, y + step))
            {
                mismatched++;
            }
        }
    }
    printf("  scroll check        frame 2 == frame 1 shifted up %u lines: %u mismatched pixels\n",
           step, mismatched);
    CHECK(mismatched == 0, "%s: vertical scroll of %u lines does not match (%u pixels differ)",
          spec.name.c_str(), step, mismatched);
}

// The sprites must actually be visible.  With TILE gone every painted pixel
// comes from a RUN whose source is a held colour, so counting pixels that equal
// held_fg or held_bg counts exactly the art — and the total is hand-derivable
// from the bitmap, which is what makes it a real check rather than a tautology.
void check_tiles_visible(const CaseSpec &spec, const CaseResult &res, uint32_t expect)
{
    const FrameImage &f1 = res.rendered.frames[0];
    uint32_t painted = 0;
    for (uint32_t y = 0; y < V_ACTIVE; y++)
    {
        for (uint32_t x = 0; x < H_ACTIVE; x++)
        {
            const Rgb444 c = at(f1, x, y);
            if (c == spec.base.held_fg || c == spec.base.held_bg)
            {
                painted++;
            }
        }
    }
    printf("  span coverage       %u pixels painted with a held colour (expected %u)\n",
           painted, expect);
    CHECK(painted == expect, "%s: spans painted %u pixels, expected exactly %u",
          spec.name.c_str(), painted, expect);
}

// The emulator will not drive these models the way make check does: it holds a
// PixelUnit and a CompositorUnit across scanlines and advances them a line at a
// time, lumping each line's deposits into its HBLANK.  Running that driver here
// against the same events is what keeps the two paths from diverging — and it
// is a real check, because the two disagree the moment a list is starved
// enough for delivery ORDER within a line to matter.
void check_incremental_api(const CaseSpec &spec, const CaseResult &res)
{
    RenderParams rp;
    rp.first_frame   = RENDER_FIRST_FRAME;
    rp.frame_count   = RENDER_FRAMES;
    rp.audio_enabled = spec.base.audio;
    rp.skew_pix      = spec.base.skew_pix;
    rp.skew_cmp      = spec.base.skew_cmp;

    const RenderResult inc = render_line_at_a_time(res.interp.events, rp);

    uint32_t differing = 0;
    for (uint32_t f = 0; f < RENDER_FRAMES; f++)
    {
        for (size_t i = 0; i < inc.frames[f].pixels.size(); i++)
        {
            if (inc.frames[f].pixels[i] != res.rendered.frames[f].pixels[i])
            {
                differing++;
            }
        }
    }
    printf("  incremental API     line-at-a-time driver vs clock-accurate: %u differing pixels\n",
           differing);
    CHECK(differing == 0,
          "%s: the per-line driver the emulator will use disagrees with the clock-accurate "
          "one on %u pixels", spec.name.c_str(), differing);
}

// ---------------------------------------------------------------------------
// Fetch-cadence traces — the model against compositor_tb.v's normative ones
// ---------------------------------------------------------------------------
//
// These drive the two units the way the testbench drives the DUT — push a
// line's words, run one HBLANK, then take slots — instead of going through a
// display list, so they pin the COMPOSITOR laws themselves rather than a
// list builder's use of them.  Every expectation below is DERIVED from
// compositor.v's header and matches a named check in compositor_tb.v:
//
//   L1  entry-edge commit: a record's effect lands on the edge ending the slot
//       it occupies, and that slot's pixel shows it.
//   L2  one slot per record when it executes; RUN(N) banked before the line
//       occupies slots 0..N-1.
//   L4  fetch cadence 2 slots per word: a word captured on the edge ending
//       slot k executes no earlier than the edge ending slot k+1.
//   L5  banked pair: a record staged with a second parked on Q executes on
//       slot k and the parked one on slot k+1.
//
// The pixel stream is all ones, so passthrough resolves to pix_pal_fg and a
// passthrough span is distinguishable from every held colour used here.
constexpr Rgb444 TRACE_PASSTHROUGH = rgb444(9, 10, 3);

std::vector<Rgb444> trace_slots(const std::vector<uint16_t> &words, uint32_t slots)
{
    PixelUnit      pix;
    CompositorUnit cmp;
    pix.reset();
    cmp.reset();
    pix.set_register(SET_PIX_PAL_FG, TRACE_PASSTHROUGH);
    for (uint32_t i = 0; i < PIXELS_WORDS_1BPP + 1; i++)
    {
        pix.push_word(0xFFFF);
    }
    for (uint16_t w : words)
    {
        cmp.push_word(w);
    }
    for (uint32_t i = 0; i < H_BLANK; i++)
    {
        cmp.blank_clock(pix);
    }
    pix.begin_line();

    std::vector<Rgb444> out;
    out.reserve(slots);
    for (uint32_t i = 0; i < slots; i++)
    {
        out.push_back(cmp.active_slot(pix));
    }
    return out;
}

void check_cadence_traces()
{
    printf("\n=== compositor-cadence — the model against compositor_tb.v's traces ===\n");

    const Rgb444 reset_fg = VIDCMD_RESET_HELD_FG;

    // NORMATIVE_M0.  RUN(fg,1) and the SET are banked as a pair in HBLANK, so
    // L5 puts the SET on slot 1 — exactly where the 1-word-per-clock design put
    // it.  L4 then puts the tail RUN on slot 3, and slot 2 HOLDs the held_fg
    // the SET just wrote, so the tail is blind to the cadence.
    {
        const std::vector<uint16_t> w = {vidcmd_run(RUN_SRC_HELD_FG, 1),
                                         vidcmd_set(SET_CMP_HELD_FG, 0x0F0),
                                         vidcmd_run(RUN_SRC_HELD_FG, 637)};
        const std::vector<Rgb444> px = trace_slots(w, H_ACTIVE);
        uint32_t bad = 0;
        for (uint32_t i = 1; i < H_ACTIVE; i++)
        {
            if (px[i] != 0x0F0)
            {
                bad++;
            }
        }
        printf("  M0                  px0 0x%03X px1 0x%03X px2 0x%03X, %u tail exceptions\n",
               px[0], px[1], px[2], bad);
        CHECK(px[0] == reset_fg, "trace M0: pixel 0 is 0x%03X, expected the pre-SET held_fg 0x%03X",
              px[0], reset_fg);
        CHECK(px[1] == 0x0F0, "trace M0: the SET must land on pixel 1 (banked pair), got 0x%03X",
              px[1]);
        CHECK(bad == 0, "trace M0: %u pixels of the tail are not the SET value", bad);
    }

    // PAIR_LOCAL.  RUN(fg,4) occupies slots 0..3 (L2) and banks SET(fg) staged
    // with SET(bg) parked while it counts, so the two SETs execute on slots 4
    // and 5 (L5).  Slot 6 is the HOLD behind the pair (L4) and RUN(bg) follows
    // on slot 7.
    {
        const std::vector<uint16_t> w = {vidcmd_run(RUN_SRC_HELD_FG, 4),
                                         vidcmd_set(SET_CMP_HELD_FG, 0x1B2),
                                         vidcmd_set(SET_CMP_HELD_BG, 0x3C4),
                                         vidcmd_run(RUN_SRC_HELD_BG, 600)};
        const std::vector<Rgb444> px = trace_slots(w, 16);
        printf("  pair                slots 0..7 %03X %03X %03X %03X %03X %03X %03X %03X\n",
               px[0], px[1], px[2], px[3], px[4], px[5], px[6], px[7]);
        CHECK(px[0] == reset_fg && px[3] == reset_fg,
              "trace pair: slots 0..3 must be the old held_fg");
        CHECK(px[4] == 0x1B2 && px[5] == 0x1B2,
              "trace pair: the SETs must land on slots 4 and 5 (banked pair), got 0x%03X 0x%03X",
              px[4], px[5]);
        CHECK(px[6] == 0x1B2, "trace pair: slot 6 must HOLD the new held_fg, got 0x%03X", px[6]);
        CHECK(px[7] == 0x3C4, "trace pair: RUN(bg) must start on slot 7, got 0x%03X", px[7]);
    }

    // BACK_TO_BACK_SETS.  L5 gives the first SET slot 1 behind a one-slot RUN;
    // L4 gives every SET after that a slot of its own plus a HOLD slot, so the
    // burst lands on slots 1, 3, 5.
    {
        const std::vector<uint16_t> w = {vidcmd_run(RUN_SRC_HELD_FG, 1),
                                         vidcmd_set(SET_CMP_HELD_FG, 0x111),
                                         vidcmd_set(SET_CMP_HELD_FG, 0x222),
                                         vidcmd_set(SET_CMP_HELD_FG, 0x333),
                                         vidcmd_run(RUN_SRC_HELD_FG, 600)};
        const std::vector<Rgb444> px = trace_slots(w, 16);
        printf("  triple burst        slots 0..6 %03X %03X %03X %03X %03X %03X %03X\n",
               px[0], px[1], px[2], px[3], px[4], px[5], px[6]);
        CHECK(px[0] == reset_fg, "trace burst: slot 0 is the RUN's own pixel");
        CHECK(px[1] == 0x111 && px[3] == 0x222 && px[5] == 0x333,
              "trace burst: the three SETs must land on slots 1, 3, 5 — got 0x%03X 0x%03X 0x%03X "
              "at those slots", px[1], px[3], px[5]);
        CHECK(px[2] == 0x111 && px[4] == 0x222,
              "trace burst: slots 2 and 4 must HOLD the value behind them");
    }

    // ONE_PX_SPANS.  RED is staged and GREEN parked at the line start, so RED
    // takes slot 0 and GREEN slot 1 (L5); slot 2 HOLDs GREEN; BLUE is captured
    // on the edge ending slot 2 and executes on slot 3 (L4) with slot 4 its
    // HOLD; the passthrough RUN then executes on slot 5.
    {
        const std::vector<uint16_t> w = {vidcmd_run_color(RUN_COLOR_RED, 1),
                                         vidcmd_run_color(RUN_COLOR_GREEN, 1),
                                         vidcmd_run_color(RUN_COLOR_BLUE, 1),
                                         vidcmd_run(RUN_SRC_PASSTHROUGH, 637)};
        const std::vector<Rgb444> px = trace_slots(w, H_ACTIVE);
        uint32_t bad = 0;
        for (uint32_t i = 5; i < H_ACTIVE; i++)
        {
            if (px[i] != TRACE_PASSTHROUGH)
            {
                bad++;
            }
        }
        printf("  one-px spans        slots 0..5 %03X %03X %03X %03X %03X %03X, %u tail "
               "exceptions\n", px[0], px[1], px[2], px[3], px[4], px[5], bad);
        CHECK(px[0] == 0xF00 && px[1] == 0x0F0 && px[2] == 0x0F0,
              "trace 1px: R then G on adjacent slots (pair) with slot 2 holding G");
        CHECK(px[3] == 0x00F && px[4] == 0x00F,
              "trace 1px: B is one cadence later, slots 3 and 4");
        CHECK(bad == 0, "trace 1px: passthrough must resume at slot 5 (%u exceptions)", bad);
    }

    // SUSTAINED_2SLOT.  A line of one-slot RUN_COLORs: record 1 paints slot 0 as
    // the staged half of the opening pair, and every record after that paints
    // its own slot plus the HOLD slot behind it, so record k >= 2 covers slots
    // 2k-3 and 2k-2 and N records reach only slot 2N-2.  A full 256-word FIFO
    // therefore covers 511 of the 640 slots — the cadence, not the depth, is
    // what a dense line runs out of.
    {
        constexpr uint32_t RECORDS = VIDCMD_FIFO_WORDS;
        constexpr uint32_t COVERED = 2 * RECORDS - 1;
        std::vector<uint16_t> w;
        for (uint32_t i = 0; i < RECORDS; i++)
        {
            w.push_back(vidcmd_run_color((i % 2) == 0 ? RUN_COLOR_RED : RUN_COLOR_GREEN, 1));
        }
        const std::vector<Rgb444> px = trace_slots(w, COVERED);
        uint32_t off = 0;
        for (uint32_t i = 0; i < COVERED; i++)
        {
            const Rgb444 want = (i == 0) ? 0xF00 : ((((i - 1) / 2) % 2) == 0 ? 0x0F0 : 0xF00);
            if (px[i] != want)
            {
                off++;
            }
        }
        printf("  sustained 2-slot    %u of %u one-slot records reach the screen, %u pixels off "
               "the pattern\n", RECORDS, H_ACTIVE, off);
        CHECK(off == 0,
              "trace sustained: %u pixels do not follow the 2-slot cadence pattern", off);
    }

    // THE CADENCE CEILING, clean-room: 1-px spans at a fixed pitch, with the
    // whole line already in the FIFO so nothing about DELIVERY can confound it.
    // A span alternating with a (pitch-1)-px gap averages pitch/2 slots per
    // record against the fetch's requirement of VIDCMD_SLOTS_PER_WORD, so the
    // tightest pitch that still lands where it was authored is
    // TEMPEST_STRESS_MIN_PITCH = 4.  Hand-derived pixel counts:
    //
    //   pitch >= 4  no HOLD lands inside the band: SPANS spans, SPANS pixels.
    //   pitch 3     half a slot lost per record, so the deficit lands as one
    //               HOLD per span (widening it to 2 px, same count as pitch 2)
    //               AND as a rightward drift of one pixel per span, which the
    //               count cannot see and the pitch-4 case is the guard against.
    //   pitch 2     the bank pays for the first span only, every span after it
    //               carries a HOLD, and a HOLD keeps the span's own source —
    //               so each of those renders 2 px: 1 + 2*(SPANS-1).
    {
        constexpr uint32_t SPANS   = 32;
        constexpr uint32_t FIRST_X = 64;
        printf("  cadence ceiling     pitch  authored px  painted px  verdict\n");
        for (uint32_t pitch : {2u, 3u, 4u, 6u})
        {
            std::vector<uint16_t> w;
            uint32_t x = 0;
            for (uint32_t i = 0; i < SPANS; i++)
            {
                const uint32_t sx = FIRST_X + i * pitch;
                if (sx > x)
                {
                    w.push_back(vidcmd_run(RUN_SRC_PASSTHROUGH, sx - x));
                }
                w.push_back(vidcmd_run_color(RUN_COLOR_MAGENTA, 1));
                x = sx + 1;
            }
            w.push_back(vidcmd_run(RUN_SRC_PASSTHROUGH, H_ACTIVE - x));

            const std::vector<Rgb444> px = trace_slots(w, H_ACTIVE);
            const Rgb444 magenta = run_colour_to_rgb444(RUN_COLOR_MAGENTA);
            uint32_t painted = 0;
            for (Rgb444 c : px)
            {
                if (c == magenta)
                {
                    painted++;
                }
            }
            const bool sustains = pitch >= TEMPEST_STRESS_MIN_PITCH;
            printf("                      %5u  %11u  %10u  %s\n", pitch, SPANS, painted,
                   sustains ? "lands as authored" : "STRETCHED, spans widen");
            if (sustains)
            {
                CHECK(painted == SPANS,
                      "trace cadence: a %u-pixel pitch averages %u slots per record and must "
                      "land as authored — %u pixels painted, expected %u",
                      pitch, pitch / 2, painted, SPANS);
            }
            if (pitch == 2)
            {
                CHECK(painted == 1 + 2 * (SPANS - 1),
                      "trace cadence: at a 2-pixel pitch every span but the banked first one "
                      "must render two pixels wide — %u painted, expected %u",
                      painted, 1 + 2 * (SPANS - 1));
            }
        }
    }
}

}  // namespace

int main()
{
    std::vector<uint16_t> ram_words(RAM_BYTES / 2, 0);
    Memory ram{std::span<uint16_t>(ram_words)};

    // Source data every case shares.
    write_test_pattern_1bpp(ram, FB_BASE, FB_STRIDE_BYTES, FB_WORDS_PER_LINE, FB_LINES);
    write_solid_pattern_1bpp(ram, FB_SOLID_BASE, FB_STRIDE_BYTES, FB_WORDS_PER_LINE, FB_LINES);
    write_test_pattern_microham(ram, FB_HAM_BASE, FB_HAM_STRIDE, FB_LINES);
    write_audio_source(ram, AUDIO_BASE, AUDIO_SOURCE_PAIRS);
    draw_tempest_web(ram, FB_WEB_BASE, FB_STRIDE_BYTES);

    FrameParams common;
    common.fb_base         = FB_BASE;
    common.fb_stride_bytes = FB_STRIDE_BYTES;
    common.vidcmd_base     = VIDCMD_BASE;
    common.audio_base      = AUDIO_BASE;

    printf("Griffin super-engine display-list + VIDCMD validation suite\n");
    printf("  SYSCLK %llu Hz, pixel clock %llu Hz, %ux%u of %ux%u, line %llu SYSCLK, "
           "HBLANK %llu SYSCLK\n",
           static_cast<unsigned long long>(SYSCLK_HZ),
           static_cast<unsigned long long>(PIXEL_CLK_HZ),
           H_ACTIVE, V_ACTIVE, H_TOTAL, V_TOTAL,
           static_cast<unsigned long long>(LINE_SYSCLK),
           static_cast<unsigned long long>(HBLANK_SYSCLK));
    printf("  colour R4G4B4 (12-bit), 3 deposit strobes + 1 spare, VIDCMD slot framing\n");
    printf("  arbitration model %u SYSCLK/descriptor, re-arm latency %llu SYSCLK, "
           "%u vblank pacing lines\n",
           ENGINE_ARBITRATION_CYCLES, static_cast<unsigned long long>(REARM_LATENCY_CYCLES),
           VBLANK_PACING_LINES);
    printf("  skew MEASURED: SET->PIXEL %u px, SET->COMPOSITOR %u px (dedicated value bus, "
           "both land in the SET's own slot)\n",
           SKEW_PIX_TARGET, SKEW_CMP_TARGET);
    printf("  rendering raster frames %u and %u (frame 0 is the arming frame)\n",
           RENDER_FIRST_FRAME, RENDER_FIRST_FRAME + 1);
    printf("  VIDCMD fetch %u slots/word, banked pair gap %u slot, on-chip bank %u records\n",
           VIDCMD_SLOTS_PER_WORD, VIDCMD_PAIR_SLOT_GAP, VIDCMD_BANK_DEPTH);

    // The laws first, directly against the two units, before any list builder
    // gets to interpret them.
    check_cadence_traces();

    // --- Normative slot-arithmetic regression ---------------------------------
    {
        CaseSpec s;
        s.name  = "m0-slot-regression";
        s.title = "NORMATIVE: RUN(pt,1) SET(pix_pal_fg,C2) RUN(pt,637) over all-fg bits";
        s.base  = common;
        s.base.fb_base         = FB_SOLID_BASE;
        s.base.slot_regression = true;
        s.base.regression_c1   = rgb444(15, 0, 0);
        s.base.regression_c2   = rgb444(0, 0, 15);
        CaseResult r = run_case(s, ram, REARM_LATENCY_CYCLES);
        write_artifacts(s, r);
        print_report(s, r);
        check_case(s, r);

        const FrameImage &f1 = r.rendered.frames[0];
        uint32_t bad_first = 0;
        uint32_t bad_rest  = 0;
        for (uint32_t y = 0; y < V_ACTIVE; y++)
        {
            if (at(f1, 0, y) != s.base.regression_c1)
            {
                bad_first++;
            }
            for (uint32_t x = 1; x < H_ACTIVE; x++)
            {
                if (at(f1, x, y) != s.base.regression_c2)
                {
                    bad_rest++;
                }
            }
        }
        printf("  normative result    pixel 0 = 0x%03X on %u/%u lines, pixels 1..639 = 0x%03X "
               "with %u exceptions\n",
               s.base.regression_c1, V_ACTIVE - bad_first, V_ACTIVE, s.base.regression_c2, bad_rest);
        CHECK(bad_first == 0,
              "m0-slot-regression: pixel 0 is not C1 on %u lines — the SET committed too early",
              bad_first);
        CHECK(bad_rest == 0,
              "m0-slot-regression: %u pixels in 1..639 are not C2 — the SET committed too late",
              bad_rest);
    }

    // --- M1: vertical scroll, and the JIT / short-line discipline -------------
    {
        CaseSpec s;
        s.name  = "m1-vscroll-jit";
        s.title = "1bpp vertical scroll with a 3-word-per-FRAME VIDCMD preamble";
        s.base  = common;
        s.base.framing            = FramingMode::JIT;
        s.base.jit_frame_preamble = true;
        // Deliberately NOT the /RS defaults (0xFFF/0x000), so the render proves
        // the preamble's SETs actually landed rather than passing by accident.
        s.base.held_fg = rgb444(13, 13, 15);
        s.base.held_bg = rgb444(0, 0, 2);
        s.v_scroll_step = 16;
        CaseResult r = run_case(s, ram, REARM_LATENCY_CYCLES);
        write_artifacts(s, r);
        print_report(s, r);
        check_case(s, r);
        check_vertical_scroll(s, r, s.v_scroll_step);
        check_incremental_api(s, r);

        // This is the firmware console's list shape: three VIDCMD words buy the
        // whole 480-line frame, because compositor.v holds the last source to
        // the end of a line and a line that gets no fill keeps holding.
        printf("  JIT framing         %u VIDCMD words for the whole frame (%u..%u per line); "
               "hold covered %u slots\n",
               r.vidcmd_words_total, r.vidcmd_words_min, r.vidcmd_words_max,
               r.rendered.stats.vidcmd_hold_slots);
        CHECK(r.vidcmd_words_total == 3,
              "m1-vscroll-jit: expected exactly 3 VIDCMD words per frame, got %u",
              r.vidcmd_words_total);
        CHECK(r.rendered.stats.vidcmd_hold_slots > 0,
              "m1-vscroll-jit: hold never engaged, so the short-line discipline is not "
              "actually being exercised");

        // Every pixel must be one of the preamble's two colours: hold has to
        // carry passthrough across 479 lines that were never filled.
        const FrameImage &f1 = r.rendered.frames[0];
        uint32_t off_palette = 0;
        for (Rgb444 c : f1.pixels)
        {
            if (c != s.base.held_fg && c != s.base.held_bg)
            {
                off_palette++;
            }
        }
        printf("  hold coverage       %u of %u pixels are neither preamble colour\n",
               off_palette, static_cast<uint32_t>(f1.pixels.size()));
        CHECK(off_palette == 0,
              "m1-vscroll-jit: %u pixels are not one of the preamble colours, so hold did "
              "not carry the frame", off_palette);
    }

    // --- M2: per-line colours + horizontal scroll -----------------------------
    GroupBudget m2_budget;
    {
        CaseSpec s;
        s.name  = "m2-perline";
        s.title = "per-line palette / mode / pixel_skip as blank-region VIDCMD SETs";
        s.base  = common;
        s.base.per_line_palette = true;
        s.base.per_line_mode    = true;
        s.h_scroll_step         = 3;
        CaseResult r = run_case(s, ram, REARM_LATENCY_CYCLES);
        write_artifacts(s, r);
        print_report(s, r);
        check_case(s, r);
        m2_budget = r.budget;

        // Frame 2 scrolls 3 pixels: word offset 0, pixel_skip 3.  Under the old
        // 3-bit MODE field this was near the edge of what was representable; a
        // 12-bit SET value now covers a whole word's worth of skip.
        const FrameImage &f1 = r.rendered.frames[0];
        const FrameImage &f2 = r.rendered.frames[1];
        uint32_t matched = 0;
        for (uint32_t x = 0; x + 3 < H_ACTIVE; x++)
        {
            if (at(f2, x, 250) == at(f1, x + 3, 250))
            {
                matched++;
            }
        }
        check_incremental_api(s, r);
        printf("  h-scroll check      frame 2 row 250 == frame 1 shifted left 3 px: %u/%u\n",
               matched, H_ACTIVE - 3);
        CHECK(matched == H_ACTIVE - 3, "m2-perline: horizontal scroll of 3 px matched only %u/%u",
              matched, H_ACTIVE - 3);
    }

    // --- M2b: mid-line palette change, now pixel-exact -------------------------
    {
        CaseSpec s;
        s.name  = "m2-split";
        s.title = "mid-line palette change placed by VIDCMD slot arithmetic";
        s.base  = common;
        s.base.per_line_palette = true;
        s.base.per_line_mode    = true;
        s.base.mid_line_split   = true;
        s.base.split_pixel      = 320;
        s.base.split_fg         = rgb444(15, 0, 0);
        s.base.split_bg         = rgb444(0, 15, 0);
        CaseResult r = run_case(s, ram, REARM_LATENCY_CYCLES);
        write_artifacts(s, r);
        print_report(s, r);
        check_case(s, r);

        // Hand-computed expectation on line 100 of frame 1 (h_scroll 0):
        //   line palette fg = rgb444(3,12,15), bg = rgb444(0,0,5)
        //   pattern bit(x,y) = ((x+y)/16)&1, and line 100 is not a marker row
        //   x=319: (419/16)=26 even -> bg -> line bg
        //   x=320: bg, but SET(pix_pal_bg) owns slot 320 -> split bg
        //   x=336: (436/16)=27 odd  -> fg, SET(pix_pal_fg) owned slot 321 -> split fg
        const FrameImage &f1 = r.rendered.frames[0];
        const Rgb444 line_fg = line_palette_fg(100);
        const Rgb444 line_bg = line_palette_bg(100);
        printf("  split check         x=319 0x%03X (want 0x%03X), x=320 0x%03X (want 0x%03X), "
               "x=336 0x%03X (want 0x%03X)\n",
               at(f1, 319, 100), line_bg, at(f1, 320, 100), s.base.split_bg,
               at(f1, 336, 100), s.base.split_fg);
        CHECK(at(f1, 319, 100) == line_bg,
              "m2-split: pixel 319 is 0x%03X, expected the line's own bg 0x%03X",
              at(f1, 319, 100), line_bg);
        CHECK(at(f1, 320, 100) == s.base.split_bg,
              "m2-split: pixel 320 is 0x%03X, expected the split bg 0x%03X — the SET did not "
              "land on its slot", at(f1, 320, 100), s.base.split_bg);
        CHECK(at(f1, 336, 100) == s.base.split_fg,
              "m2-split: pixel 336 is 0x%03X, expected the split fg 0x%03X",
              at(f1, 336, 100), s.base.split_fg);

        // Nothing before the split may have changed.
        uint32_t early = 0;
        for (uint32_t x = 0; x < 320; x++)
        {
            const Rgb444 c = at(f1, x, 100);
            if (c != line_fg && c != line_bg)
            {
                early++;
            }
        }
        CHECK(early == 0, "m2-split: %u pixels before the split already show a split colour",
              early);
        (void)line_fg;

        // Skew compensation: re-author and re-render with a nonzero, asymmetric
        // cross-chip skew.  The list changes shape (the leading RUN shortens),
        // but the image must not, or the compensation is not doing its job.
        CaseSpec sk = s;
        sk.base.skew_pix = 3;
        sk.base.skew_cmp = 1;
        CaseResult rk = run_case(sk, ram, REARM_LATENCY_CYCLES);
        bool identical = true;
        for (uint32_t f = 0; f < RENDER_FRAMES && identical; f++)
        {
            identical = (rk.rendered.frames[f].pixels == r.rendered.frames[f].pixels);
        }
        printf("  skew compensation   re-run at SET->PIXEL %u px / SET->COMPOSITOR %u px: "
               "image %s\n",
               sk.base.skew_pix, sk.base.skew_cmp, identical ? "IDENTICAL" : "DIFFERS");
        CHECK(identical,
              "m2-split: compensating for a %u-pixel PIXEL-target skew changed the image, so "
              "the list builder's compensation is wrong", sk.base.skew_pix);

        // Restore the case's own authoring so the report and artifacts match.
        run_case(s, ram, REARM_LATENCY_CYCLES);
    }

    // --- micro-HAM, now rasterized ---------------------------------------------
    {
        CaseSpec s;
        s.name  = "m2-microham";
        s.title = "micro-HAM: 80 words/line at 2 bits per pixel clock";
        s.base  = common;
        s.base.mode             = PixelMode::MICRO_HAM;
        s.base.fb_base          = FB_HAM_BASE;
        s.base.fb_stride_bytes  = FB_HAM_STRIDE;
        s.base.per_line_palette = true;
        s.base.per_line_mode    = true;
        CaseResult r = run_case(s, ram, REARM_LATENCY_CYCLES);
        write_artifacts(s, r);
        print_report(s, r);
        check_case(s, r);

        // Hand-computed expectation on line 100 of frame 1:
        //   pix_pal_fg = rgb444(3,12,15), pix_pal_bg = rgb444(0,0,5)
        //   x in   0..159 : code 0_1 -> held <- pal_fg          (1 clock, 1 pixel)
        //   x in 160..319 : code 0_0 -> held <- pal_bg          (1 clock, 1 pixel)
        //   x = 320,321   : the first 4-bit code, kk = 0 + (100>>3) = 12, even ->
        //                   10_g_r with g = (12>>1)&1 = 0, r = (12>>2)&1 = 1.
        //
        // PAIR ORDERING (pixel.v, not video.v): the prefix pair and the chroma
        // pair land in different clocks, so x=320 still shows the OLD held —
        // which is pal_bg, left over from the 0_0 region — and x=321 shows both
        // channels updated together: red <- 0xF, green <- 0x0, blue keeps
        // pal_bg's 5.
        const FrameImage &f1 = r.rendered.frames[0];
        const Rgb444 pal_fg = line_palette_fg(100);
        const Rgb444 pal_bg = line_palette_bg(100);
        const Rgb444 chroma = rgb444(15, 0, rgb444_b(pal_bg));
        printf("  micro-HAM check     x=0 0x%03X x=100 0x%03X | x=200 0x%03X x=300 0x%03X | "
               "x=320 0x%03X x=321 0x%03X\n",
               at(f1, 0, 100), at(f1, 100, 100), at(f1, 200, 100), at(f1, 300, 100),
               at(f1, 320, 100), at(f1, 321, 100));
        printf("  micro-HAM expected  palette-fg 0x%03X, palette-bg 0x%03X, chroma code 0x%03X "
               "on the code's SECOND pixel only\n", pal_fg, pal_bg, chroma);
        CHECK(at(f1, 0, 100) == pal_fg && at(f1, 100, 100) == pal_fg,
              "m2-microham: the 0_1 code region is not pix_pal_fg");
        CHECK(at(f1, 200, 100) == pal_bg && at(f1, 300, 100) == pal_bg,
              "m2-microham: the 0_0 code region is not pix_pal_bg");
        CHECK(at(f1, 320, 100) == pal_bg,
              "m2-microham: the first pixel of a 4-bit code must still show the OLD held "
              "colour 0x%03X (pixel.v pair ordering), got 0x%03X", pal_bg, at(f1, 320, 100));
        CHECK(at(f1, 321, 100) == chroma,
              "m2-microham: the second pixel of the first 4-bit code should be 0x%03X with "
              "both channels committed together, got 0x%03X", chroma, at(f1, 321, 100));
    }

    uint32_t m3_cursor_words = 0;
    uint32_t m3_sprite_words = 0;

    // --- M3: cursor -----------------------------------------------------------
    {
        CaseSpec s;
        s.name  = "m3-cursor";
        s.title = "1bpp plus a 16x16 arrow cursor, decomposed into held-colour spans";
        s.base  = common;
        s.base.sprites    = SpriteStyle::CURSOR;
        s.base.cursor_x = 300;
        s.base.cursor_y = 200;
        // Neither held colour may equal PIXEL's default palette (0xFFF / 0x000)
        // or the visibility count cannot tell composited from not.
        s.base.held_fg = rgb444(15, 0, 0);
        s.base.held_bg = rgb444(0, 0, 15);
        CaseResult r = run_case(s, ram, REARM_LATENCY_CYCLES);
        write_artifacts(s, r);
        print_report(s, r);
        check_case(s, r);
        // The arrow mask has 84 opaque pixels and every one of them is painted
        // with a held colour — plus, since 2026-08-19, the HOLD slots the fetch
        // cadence spends inside the art, because a HOLD keeps the current
        // source and the current source there IS a held colour.
        //
        // Derived per row from L4/L5, not measured.  A row's records are
        // {RUN(pt,gap), seg, seg, ...}; the leading RUN and the first segment
        // are the line's banked pair, so the first segment lands on its
        // authored slot and every later segment lands one slot after the
        // previous segment ENDS if that segment was long enough to cover a
        // fetch, and two slots after it starts otherwise.  Rows 0,1 (single
        // segment) and 5..10 (segments >= 4 px) are exact; rows 2 and 13,15
        // add 2 HOLDs each, rows 3,12,14 add 1, and row 11 — six 1-px segments
        // with a 1-px hole — adds 5.  2+1+0+0+0+0+0+0+0+5+1+2+1+2 = 14.
        check_tiles_visible(s, r, 84 + 14);
        printf("  cadence regression  the arrow's 84 opaque pixels now paint %u: %u rows carry\n"
               "                      1-px fg/bg segments, and at the 2-clock fetch each of\n"
               "                      those costs a HOLD slot that keeps the art's own held\n"
               "                      colour.  Worst row +5 (six 1-px segments).  The line\n"
               "                      still frames exactly %u — the stretch comes out of the\n"
               "                      trailing filler — so the damage stays on its own row.\n",
               84 + 14, r.vidcmd_stretch_lines / RENDER_FRAMES, H_ACTIVE);
        m3_cursor_words = r.vidcmd_words_max;
    }

    // --- M3b: four sprites per line, the worst case ---------------------------
    {
        CaseSpec s;
        s.name  = "m3-sprites";
        s.title = "worst case: four sprites per line as spans, plus covering runs";
        s.base  = common;
        s.base.sprites   = SpriteStyle::FOUR_SPRITES;
        s.base.held_fg = rgb444(15, 15, 0);
        s.base.held_bg = rgb444(8, 0, 8);
        CaseResult r = run_case(s, ram, REARM_LATENCY_CYCLES);
        write_artifacts(s, r);
        print_report(s, r);
        check_case(s, r);
        // 4 sprites x 480 lines; the diamond's popcount over a 16-row cycle is
        // 2*(2+4+6+8+10+12+14+16) = 144, i.e. 9 per row on average.
        check_tiles_visible(s, r, 4 * 144 * (V_ACTIVE / 16));
        m3_sprite_words = r.vidcmd_words_max;
    }

    // --- M4: audio woven into the line cadence --------------------------------
    {
        CaseSpec s;
        s.name  = "m4-audio";
        s.title = "M2 plus 16 audio pairs every 32 lines and a 23-pair vblank preamble";
        s.base  = common;
        s.base.per_line_palette = true;
        s.base.per_line_mode    = true;
        s.base.audio            = true;
        CaseResult r = run_case(s, ram, REARM_LATENCY_CYCLES);
        write_artifacts(s, r);
        print_report(s, r);
        check_case(s, r);
        printf("  audio budget delta  worst line %u SYSCLK vs %u without audio (+%d)\n",
               r.budget.max_busy, m2_budget.max_busy,
               static_cast<int>(r.budget.max_busy) - static_cast<int>(m2_budget.max_busy));
    }

    // --- M5: frame handshake and re-arm latency --------------------------------
    {
        CaseSpec s;
        s.name  = "m5-handshake";
        s.title = "stop+IRQ, double-buffered tables, VBLANK-ISR re-arm latency";
        s.base  = common;
        s.base.per_line_palette = true;
        s.base.per_line_mode    = true;
        CaseResult r = run_case(s, ram, REARM_LATENCY_CYCLES);
        write_artifacts(s, r);
        print_report(s, r);
        check_case(s, r);

        CHECK(TABLE_A + r.author[0].table_bytes <= TABLE_B,
              "m5-handshake: frame 1 list (%u bytes at 0x%06X) runs into table B at 0x%06X",
              r.author[0].table_bytes, TABLE_A, TABLE_B);
        CHECK(TABLE_B + r.author[1].table_bytes <= DESC_TABLE_BASE + DESC_TABLE_BYTES,
              "m5-handshake: frame 2 list runs off the end of the 64K descriptor window");
        printf("  double buffering    A 0x%06X+%u, B 0x%06X+%u, disjoint, %u bytes spare\n",
               TABLE_A, r.author[0].table_bytes, TABLE_B, r.author[1].table_bytes,
               DESC_TABLE_BYTES - (TABLE_B - TABLE_A) - r.author[1].table_bytes);

        const uint64_t sweep[] = {0, 100, 200, 300, 400, 600};
        uint64_t last_ok = 0;
        for (uint64_t lat : sweep)
        {
            CaseResult t = run_case(s, ram, lat);
            const bool ok = t.rendered.stats.pixels_tiled_words == 0 && t.rendered.stats.pixels_overflows == 0 &&
                            t.rendered.stats.vidcmd_overruns == 0 && t.interp.violations.empty();
            if (ok)
            {
                last_ok = lat;
            }
            printf("  re-arm latency      %4llu SYSCLK (%5.1f us): %s\n",
                   static_cast<unsigned long long>(lat),
                   1.0e6 * static_cast<double>(lat) / static_cast<double>(SYSCLK_HZ),
                   ok ? "clean" : "FRAME SLIP");
        }
        printf("  re-arm budget       clean up to %llu SYSCLK (%.1f us)\n",
               static_cast<unsigned long long>(last_ok),
               1.0e6 * static_cast<double>(last_ok) / static_cast<double>(SYSCLK_HZ));
        CHECK(last_ok >= REARM_LATENCY_CYCLES,
              "m5-handshake: the chosen %llu-SYSCLK re-arm latency already slips the frame",
              static_cast<unsigned long long>(REARM_LATENCY_CYCLES));

        run_case(s, ram, REARM_LATENCY_CYCLES);
    }

    // --- Capstone: everything at once -----------------------------------------
    {
        CaseSpec s;
        s.name  = "crazy-demo";
        s.title = "per-line palette + horizontal scroll + 4 sprites/line + audio, 2 frames";
        s.base  = common;
        s.base.per_line_palette = true;
        s.base.per_line_mode    = true;
        s.base.sprites            = SpriteStyle::FOUR_SPRITES;
        s.base.held_fg          = rgb444(15, 15, 0);
        s.base.held_bg          = rgb444(8, 0, 8);
        s.base.audio            = true;
        s.h_scroll_step         = 3;
        s.v_scroll_step         = 16;
        CaseResult r = run_case(s, ram, REARM_LATENCY_CYCLES);
        write_artifacts(s, r);
        print_report(s, r);
        check_case(s, r);

        // The capstone is where the group order and the arbitration estimate
        // interact, and 4 SYSCLK per descriptor is a model, not a measurement.
        // Sweep both orderings against a pessimistic arbitration and assert
        // that the ordering the suite recommends survives all of it.
        //
        // The previous round of this suite concluded the opposite ordering.
        // That conclusion was correct for the old overlay stream and is wrong
        // for VIDCMD; see finding 6.
        //
        // The DRY figures fell on 2026-08-19 (138..181 -> 105..148) and the
        // reason is not that deferring got safer.  Two things moved, both
        // consequences of the 2-clock fetch: the beam now waits two extra
        // clocks at the top of a starved line (the /EF synchronizer has to see
        // the deposit), and — much larger — a compositor that eats one word per
        // two slots empties the FIFO half as fast, so most of the holds that
        // used to be genuine starvation are now cadence holds against a FIFO
        // that has words in it.  Both columns are printed so the reattribution
        // is visible rather than looking like an improvement.
        for (uint32_t defer = 0; defer < 2; defer++)
        {
            for (uint32_t arb = 4; arb <= 8; arb++)
            {
                CaseSpec t = s;
                t.base.arbitration_cycles  = arb;
                t.base.vidcmd_after_pixels = (defer != 0);
                CaseResult tr = run_case(t, ram, REARM_LATENCY_CYCLES);
                printf("  arbitration %u        VIDCMD %-13s line %3u SYSCLK, "
                       "banked at line start %2u, PIXELS underruns %u, VIDCMD dry %u "
                       "(cadence %u)\n",
                       arb, (defer != 0) ? "after pixels" : "before pixels",
                       tr.budget.max_busy, tr.rendered.stats.pixels_line_start_min,
                       tr.rendered.stats.pixels_tiled_words, tr.rendered.stats.vidcmd_hold_slots,
                       tr.rendered.stats.vidcmd_cadence_slots);
                if (defer == 0)
                {
                    CHECK(tr.rendered.stats.pixels_tiled_words == 0 && tr.rendered.stats.vidcmd_hold_slots == 0,
                          "crazy-demo: with VIDCMD ahead of PIXELS, arbitration %u underruns "
                          "(PIXELS %u, VIDCMD %u)",
                          arb, tr.rendered.stats.pixels_tiled_words, tr.rendered.stats.vidcmd_hold_slots);
                }
                else
                {
                    // Recorded, not asserted clean: this is the measurement
                    // behind the reversal, and it must stay visible.
                    CHECK(tr.rendered.stats.vidcmd_hold_slots > 0,
                          "crazy-demo: deferring VIDCMD was expected to starve COMPOSITOR at "
                          "arbitration %u but did not — finding 6 needs re-deriving", arb);
                }
            }
        }
        run_case(s, ram, REARM_LATENCY_CYCLES);
    }

    // --- tempest-web: the hybrid vector-game architecture ----------------------
    {
        CaseSpec s;
        s.name  = "tempest-web";
        s.title = "static wireframe in the bitmap, moving objects as RUN_COLOR spans";
        s.base  = common;
        s.base.fb_base            = FB_WEB_BASE;
        s.base.per_line_palette   = true;
        s.base.per_line_mode      = true;
        s.base.depth_fade_palette = true;
        s.base.tempest_objects    = true;

        const uint32_t web_bytes = FB_LINES * FB_STRIDE_BYTES;
        const uint32_t web_before = checksum_region(ram, FB_WEB_BASE, web_bytes);

        CaseResult r = run_case(s, ram, REARM_LATENCY_CYCLES);
        write_artifacts(s, r);
        print_report(s, r);
        check_case(s, r);

        // (d) part one: authoring two frames of animation must not have written
        // a single bit of the bitmap.
        const uint32_t web_after = checksum_region(ram, FB_WEB_BASE, web_bytes);
        printf("  bitmap immutability web checksum 0x%08X before authoring, 0x%08X after "
               "two animated frames\n", web_before, web_after);
        CHECK(web_before == web_after,
              "tempest-web: authoring the animation modified the pixel bitmap "
              "(0x%08X -> 0x%08X)", web_before, web_after);

        const FrameImage &f1 = r.rendered.frames[0];
        const FrameImage &f2 = r.rendered.frames[1];

        const Rgb444 claw    = run_colour_to_rgb444(TEMPEST_CLAW_COLOR);
        const Rgb444 flipper = run_colour_to_rgb444(TEMPEST_FLIPPER_COLOR);
        const Rgb444 shot    = run_colour_to_rgb444(TEMPEST_SHOT_COLOR);

        // (a)(b) Hand-counted object pixels: the shapes are fixed, so the exact
        // pixel totals are known without consulting the authoring code.
        auto count_colour = [](const FrameImage &img, Rgb444 c)
        {
            uint32_t n = 0;
            for (Rgb444 v : img.pixels)
            {
                if (v == c)
                {
                    n++;
                }
            }
            return n;
        };
        const uint32_t want_claw    = TEMPEST_CLAW_ROWS * TEMPEST_CLAW_PRONGS * TEMPEST_CLAW_WIDTH;
        const uint32_t want_flipper = TEMPEST_FLIPPERS * TEMPEST_FLIPPER_ROWS * TEMPEST_FLIPPER_WIDTH;
        const uint32_t want_shot    = TEMPEST_SHOTS * TEMPEST_SHOT_ROWS * TEMPEST_SHOT_WIDTH;
        printf("  object pixels       claw 0x%03X %u (want %u), flipper 0x%03X %u (want %u), "
               "shot 0x%03X %u (want %u)\n",
               claw, count_colour(f1, claw), want_claw,
               flipper, count_colour(f1, flipper), want_flipper,
               shot, count_colour(f1, shot), want_shot);
        CHECK(count_colour(f1, claw) == want_claw,
              "tempest-web: %u claw-coloured pixels, expected %u",
              count_colour(f1, claw), want_claw);
        CHECK(count_colour(f1, flipper) == want_flipper,
              "tempest-web: %u flipper-coloured pixels, expected %u",
              count_colour(f1, flipper), want_flipper);
        CHECK(count_colour(f1, shot) == want_shot,
              "tempest-web: %u shot-coloured pixels, expected %u",
              count_colour(f1, shot), want_shot);

        // (c) The wireframe shows the line's own faded blue, and nothing on the
        // line is any other colour.
        uint32_t probe_line = 0;
        uint32_t web_px     = 0;
        for (uint32_t y = 380; y < 420 && web_px == 0; y++)
        {
            uint32_t n = 0;
            for (uint32_t x = 0; x < H_ACTIVE; x++)
            {
                if (at(f1, x, y) == tempest_palette_fg(y))
                {
                    n++;
                }
            }
            if (n > 0)
            {
                probe_line = y;
                web_px     = n;
            }
        }
        uint32_t stray = 0;
        for (uint32_t x = 0; x < H_ACTIVE; x++)
        {
            const Rgb444 c = at(f1, x, probe_line);
            if (c != tempest_palette_fg(probe_line) && c != tempest_palette_bg(probe_line) &&
                c != claw && c != flipper && c != shot)
            {
                stray++;
            }
        }
        printf("  depth fade          line %u: %u wireframe pixels at 0x%03X, %u stray colours "
               "(near-rim 0x%03X, far-rim 0x%03X)\n",
               probe_line, web_px, tempest_palette_fg(probe_line), stray,
               tempest_palette_fg(V_ACTIVE - 1), tempest_palette_fg(130));
        CHECK(web_px > 0, "tempest-web: no wireframe pixels found at the faded palette colour");
        CHECK(stray == 0, "tempest-web: %u pixels on line %u are neither wireframe, background "
              "nor an object colour", stray, probe_line);
        CHECK(tempest_palette_fg(V_ACTIVE - 1) != tempest_palette_fg(130),
              "tempest-web: the depth fade is flat");

        // (d) part two: every pixel that changed between the frames is an object
        // pixel in one frame or the other — the animation touched nothing else.
        uint32_t changed     = 0;
        uint32_t non_object  = 0;
        for (size_t i = 0; i < f1.pixels.size(); i++)
        {
            if (f1.pixels[i] == f2.pixels[i])
            {
                continue;
            }
            changed++;
            auto is_object = [&](Rgb444 c) { return c == claw || c == flipper || c == shot; };
            if (!is_object(f1.pixels[i]) && !is_object(f2.pixels[i]))
            {
                non_object++;
            }
        }
        printf("  animation delta     %u pixels differ between frames, %u of them not an "
               "object colour in either frame\n", changed, non_object);
        CHECK(changed > 0, "tempest-web: the objects did not move between frames");
        CHECK(non_object == 0,
              "tempest-web: %u changed pixels are not object pixels in either frame, so the "
              "animation disturbed the wireframe", non_object);

        printf("  vector headline     densest line %u VIDCMD words (%u records) for the whole\n"
               "                      object set; the earlier cases peaked at 19.\n",
               r.vidcmd_words_max, r.vidcmd_records_max);

        // The object budget.  The specified object set is nowhere near the
        // limit, so the interesting question is where the limit actually is:
        // add one-pixel spans at a two-pixel pitch — the densest thing the
        // format can express — until something gives.
        // Two list shapes: the whole VIDCMD stream ahead of the pixels (the rule
        // finding 6 established for short streams), versus a 6-word head ahead
        // and the tail behind.  COMPOSITOR only needs its first record staged by
        // pixel 0, so the tail can ride behind the pixel deposits instead of
        // pushing them past the start of active video.
        uint32_t budget_all_head = 0;
        uint32_t budget_split    = 0;
        for (uint32_t split = 0; split < 2; split++)
        {
            printf("  density sweep  %-9s spans words/ln recs/ln  SYSCLK  PIXbank  PIXtiled  "
                   "VIDCMDhold  overrun\n",
                   (split != 0) ? "PIXhead20" : "VIDCMD1st");
            for (uint32_t spans : {0u, 8u, 16u, 24u, 32u, 48u, 64u})
            {
                CaseSpec t = s;
                t.base.tempest_stress_spans = spans;
                t.base.pixels_head_words    = (split != 0) ? 20u : 0u;
                CaseResult tr = run_case(t, ram, REARM_LATENCY_CYCLES);
                const bool clean = tr.rendered.stats.pixels_tiled_words == 0 &&
                                   tr.rendered.stats.vidcmd_hold_slots == 0 &&
                                   tr.rendered.stats.vidcmd_overruns == 0 &&
                                   tr.vidcmd_slot_max == H_ACTIVE &&
                                   tr.budget.max_busy < LINE_SYSCLK;
                if (clean)
                {
                    if (split != 0)
                    {
                        budget_split = spans;
                    }
                    else
                    {
                        budget_all_head = spans;
                    }
                }
                printf("                          %5u %8u %7u  %6u  %7u  %8u  %9u  %6u  %s\n",
                       spans, tr.vidcmd_words_max, tr.vidcmd_records_max, tr.budget.max_busy,
                       tr.rendered.stats.pixels_line_start_min, tr.rendered.stats.pixels_tiled_words,
                       tr.rendered.stats.vidcmd_hold_slots, tr.rendered.stats.vidcmd_overruns,
                       clean ? "ok" : "OVER BUDGET");
            }
        }
        printf("  object budget       %u one-pixel spans/line with VIDCMD wholly ahead of the\n"
               "                      pixels, %u with the pixel stream split 20 words either\n"
               "                      side of it.  The specified object set uses %u records on\n"
               "                      its densest line.  The reorder is FREE: a 40-word 1bpp\n"
               "                      line already splits into two 20-word descriptors, so\n"
               "                      putting VIDCMD between them costs no extra descriptor —\n"
               "                      both shapes are %u SYSCLK/line at zero stress spans.\n",
               budget_all_head, budget_split, r.vidcmd_records_max, r.budget.max_busy);
        CHECK(budget_split >= budget_all_head,
              "tempest-web: interleaving the pixel stream made the object budget worse "
              "(%u spans vs %u) — the head/tail rule needs re-deriving",
              budget_split, budget_all_head);
        CHECK(budget_split >= 24,
              "tempest-web: only %u one-pixel spans per line fit even with the interleaved "
              "list shape — fewer than a Tempest-class game needs", budget_split);

        // The OTHER ceiling, new on 2026-08-19 and independent of the bus: the
        // COMPOSITOR's own fetch needs every record to average
        // VIDCMD_SLOTS_PER_WORD slots however early the words arrived.  A 1-px
        // span alternating with a (pitch-1)-px gap averages pitch/2, so the
        // tightest pitch that still lands where it was authored is
        // TEMPEST_STRESS_MIN_PITCH.  The sweep above never approaches it, and
        // that is the point: the sweep SPREADS its spans, so even its densest
        // point is a comfortable pitch and the ENGINE's bus delivery is still
        // what runs out first.  The ceiling itself is measured clean-room in
        // the cadence traces, where no deposit schedule can confound it.
        const uint32_t densest_pitch = H_ACTIVE / (TEMPEST_MAX_STRESS_SPANS + 1);
        printf("  cadence headroom    densest sweep point is a %u-pixel pitch (%u spans spread\n"
               "                      over %u pixels) against a %u-pixel cadence floor, so the\n"
               "                      2-clock fetch never binds here — stretch %u on every line.\n",
               densest_pitch, TEMPEST_MAX_STRESS_SPANS, H_ACTIVE, TEMPEST_STRESS_MIN_PITCH,
               r.vidcmd_stretch_max);
        CHECK(densest_pitch >= TEMPEST_STRESS_MIN_PITCH,
              "tempest-web: the density sweep now authors below the %u-pixel cadence floor "
              "(%u spans is a %u-pixel pitch), so its results mix delivery with cadence",
              TEMPEST_STRESS_MIN_PITCH, TEMPEST_MAX_STRESS_SPANS, densest_pitch);
        CHECK(r.vidcmd_stretch_max == 0,
              "tempest-web: the object set stretched %u slots — claw/flipper/shot spans are all "
              "at least %u pixels wide and should not",
              r.vidcmd_stretch_max, VIDCMD_SLOTS_PER_WORD);

        // Re-author with the case's own parameters so the artifacts match.
        run_case(s, ram, REARM_LATENCY_CYCLES);
    }

    // --- Findings --------------------------------------------------------------
    printf("\n=== findings ===\n");
    printf("  1. STILL HOLDS. edma3.v has only wait_hblank — no wait-VBLANK and no\n"
           "     wait-last-vblank-line.  A list armed by the ISR needs %u leading\n"
           "     wait_hblank/mask-0 descriptors (%u bytes, %u SYSCLK) just to walk to the top\n"
           "     of frame, and frame lock depends on the CPU arming inside one scanline.\n",
           VBLANK_PACING_LINES, VBLANK_PACING_LINES * DESC_BYTES,
           VBLANK_PACING_LINES * engine_wait_descriptor_cycles(ENGINE_ARBITRATION_CYCLES, 1));
    printf("  2. RESOLVED by the VIDCMD redesign.  pixel_skip used to be a 3-bit slice of a\n"
           "     MODE word, which could only reach pixel offsets 16n+0..16n+7.  As a 12-bit\n"
           "     SET value it spans a whole FIFO word, so every horizontal scroll offset is\n"
           "     now reachable; the m2-perline 3-pixel scroll check confirms it.\n");
    printf("  3. STILL HOLDS. PORTS pops audio through vertical blanking too, where no\n"
           "     per-line group exists to feed it, so a frame's list must carry vblank's\n"
           "     45/2 = 23 pairs in its preamble on top of the 15x16 the visible lines give.\n");
    printf("  4. SUPERSEDED. Landing a mid-line register change no longer needs mask=0 bus\n"
           "     pacing descriptors.  The old technique cost ~45 wasted payload words per\n"
           "     line, drove the case to 57%% bus utilization, and still only placed the\n"
           "     change to about +-4 pixels.  A VIDCMD SET occupying one active slot is\n"
           "     pixel-exact and costs one word.  Pacing descriptors remain the right tool\n"
           "     for time in the *bus* domain — the vblank walk and the stop pacer still use\n"
           "     them — but not for placing a deposit on a pixel.\n");
    printf("  5. STILL HOLDS. Ending every list on a wait_hblank/mask-0/stop_after descriptor\n"
           "     pins nENGINE_IRQ to line 479's HBLANK edge, so the ISR re-arm budget is a\n"
           "     constant instead of shrinking with the frame's weight.\n");
    printf("  6. REVERSED. The previous round concluded \"put the overlay stream BEHIND the\n"
           "     pixel stream\", because the overlay was small and PIXEL was starving at 2 of\n"
           "     41 words banked.  Under VIDCMD the answer is the opposite, and the sweep\n"
           "     above shows it: VIDCMD ahead of PIXELS is clean through 8 SYSCLK/descriptor\n"
           "     (14 words banked falling to 10), while VIDCMD behind PIXELS leaves\n"
           "     COMPOSITOR dry for 105+ slots at every arbitration value (2026-08-19: it\n"
           "     read 138+ before the cadence rework, and the whole of that drop is\n"
           "     reattribution — a fetch that eats one word per two slots empties the FIFO\n"
           "     half as fast, so those slots are now counted as cadence holds, ~500 of them,\n"
           "     against a FIFO that is not empty).  Two things\n"
           "     changed.  VIDCMD now carries the palette and mode SETs that used to be\n"
           "     their own descriptors, so it MUST be in the FIFO before pixel 0 or the line\n"
           "     draws with the previous line's palette — and it is 17-19 words, not 13.\n"
           "     At the same time, folding those two descriptors into the stream deleted two\n"
           "     14-SYSCLK overheads from ahead of the pixel deposits, which is what took\n"
           "     PIXEL's line-start margin from 2 words to 14.  The redesign paid for the\n"
           "     ordering it requires.\n");
    printf("  7. REFINED BY THE RTL, AND AGAIN ON 2026-08-19. The slot arithmetic is still\n"
           "     the sharpest edge, but it is no longer a SUM.  compositor.v holds on an\n"
           "     empty FIFO — it keeps the current source and keeps trying — and that is\n"
           "     first-class framing, not underrun mercy.  Two contracts come off that one\n"
           "     hardware rule: cushion lists promise exactly %u slots of OCCUPANCY per line\n"
           "     (authored slots plus the HOLD slots the 2-clock fetch spends between\n"
           "     records) and may not hold for want of data; JIT lists promise at most %u\n"
           "     and must instead land every word before their line's pixel 0.  The two\n"
           "     flavours of hold are now counted separately — a cadence hold is structural,\n"
           "     a starvation hold is the cushion running out — and write_vidcmd_records\n"
           "     reports occupancy, not the authored sum, because only occupancy closes.\n",
           H_ACTIVE, H_ACTIVE);
    printf("  8. OVERTURNED BY HOLD. The previous round measured a floor of one VIDCMD\n"
           "     word per line even for a line that wanted nothing, and charged the plain\n"
           "     vertical-scroll case 130 SYSCLK/line for it.  With hold-on-empty modelled\n"
           "     the floor is not per line at all: m1-vscroll-jit paints a whole 480-line\n"
           "     frame with THREE VIDCMD words — {SET fg, SET bg, RUN(passthrough,1)} in the\n"
           "     first hblank — and drops back to 114 SYSCLK/line.  That is the firmware\n"
           "     console's list shape, and it is why the console does not need a per-line\n"
           "     VIDCMD descriptor at all.\n");
    printf("  9. NEW, AND NOW CONDITIONAL (2026-08-19). A multi-register mid-line change\n"
           "     cannot be atomic: each SET owns one pixel slot, so changing both palette\n"
           "     entries takes two pixels and the frame shows one pixel of mixed old/new\n"
           "     state.  m2-split still pins exactly that — background at x=320, foreground\n"
           "     from x=321 — but at the 2-clock fetch cadence it holds only because the two\n"
           "     SETs are a BANKED PAIR: the run ahead of them gives the fetch time to stage\n"
           "     one and park the other, and the park moves in on the very edge the first\n"
           "     executes.  A THIRD SET in the same burst lands two pixels later, not one\n"
           "     (the compositor-cadence traces measure exactly that), so a mid-line restyle\n"
           "     of more than two registers is no longer a contiguous two-pixel seam.\n");
    printf(" 10. NEW, AND IT BIT THIS SUITE TWICE. Losing the VIDEO_PALETTE and VIDEO_MODE\n"
           "     strobes did remove one aliasing hazard: those deposits sourced their value\n"
           "     from a RAM scratch word that both double-buffered lists shared, and the\n"
           "     previous round caught frame N+1's pixel_skip leaking into frame N.  A SET\n"
           "     carries its value inside the instruction, so that particular word is gone.\n"
           "     But the hazard MOVED rather than vanished, and rebuilding this suite walked\n"
           "     straight into it again: the VIDCMD records are themselves per-frame data,\n"
           "     and one shared VIDCMD region gave every line of frame 1 the wrong\n"
           "     pixel_skip — 480 PIXELS underruns per frame.  The rule is that ANY RAM a\n"
           "     descriptor sources from is frame-owned and needs the same double buffering\n"
           "     the descriptor table gets.  The table is easy to remember because it lives\n"
           "     in a special 64K window; the VIDCMD region is ordinary RAM (%u bytes here,\n"
           "     nearly three times the table) and is exactly the thing a driver will forget.\n",
           VIDCMD_REGION_BYTES);
    printf(" 12. NEW. micro-HAM costs 232 SYSCLK/line, 52%% of the line and nearly double the\n"
           "     1bpp case, entirely because 80 pixel words is 160 SYSCLK of payload.  It\n"
           "     splits into THREE descriptors of ~27 words, not four of 20: the 5-bit count\n"
           "     field reaches 32, so the 20-word figure inherited from the current engine's\n"
           "     ENGINE_WORDS_PER_BURST is not a constraint here and buying the fourth\n"
           "     descriptor's 14 SYSCLK back is free.  Note the corollary for the reserved\n"
           "     word0 bits [6:4] freed by the strobe reduction: a 6-bit count would carry a\n"
           "     whole 40-word 1bpp line in one descriptor.\n");
    printf(" 13. NEW. RUN_COLOR is the atomic answer to finding 9.  Painting a span with the\n"
           "     held colours costs two SETs and therefore lands one pixel apart; a RUN_COLOR\n"
           "     is one word, one record, and its colour never touches held_fg/held_bg, so\n"
           "     independent objects on one line cannot clobber each other's colour.  It cost\n"
           "     no new semantics either — it is a playback record like any other RUN and it\n"
           "     counts its slots the same way.  What it does NOT do any more (2026-08-19) is\n"
           "     satisfy the prefetch invariant for free: one word is one FETCH, and a fetch\n"
           "     costs two slots, so a one-pixel RUN_COLOR beside a one-pixel gap is exactly\n"
           "     the shape the cadence cannot sustain.  The price is also in the fit, not the\n"
           "     format: FIT-RISKY ASSUMPTION 5 in descriptor.h.  Note the encoding squeeze —\n"
           "     three colour bits come out of the count, so a RUN_COLOR spans at most %u\n"
           "     pixels where a plain RUN spans 4095.  Nothing in these cases comes close.\n",
           RUN_COLOR_MAX_COUNT);
    printf(" 14. NEW, AND THE HEADLINE FOR VECTOR GAMES. The two FIFO consumers drain at\n"
           "     rates that differ by 8x, and the per-line descriptor order has to respect\n"
           "     that.  One PIXELS word feeds 16 pixel clocks; one VIDCMD word feeds as few\n"
           "     as two (2026-08-19: it was one before the registered-/RE fetch, so the gap\n"
           "     halved and the conclusion did not move).  VIDCMD has no cheap head and must\n"
           "     arrive first, while PIXEL can be made immune for a third of a line by 12-20\n"
           "     words.  With the whole VIDCMD stream ahead of the pixels the well supports\n"
           "     only 8 one-pixel spans per line before PIXEL starves; interleaving — 20 pixel\n"
           "     words, then all of VIDCMD, then the other 20 — supports 32, four times as\n"
           "     many, for ZERO extra descriptors, because a 40-word line already splits\n"
           "     into two 20-word descriptors.  This refines finding 6 rather than\n"
           "     overturning it: VIDCMD still goes ahead of the BULK of the pixels.\n");
    printf(" 15. NEW, AND NOW TWO CEILINGS (2026-08-19). The ceiling above is a delivery-rate\n"
           "     ceiling, not a FIFO-depth one: the engine puts one VIDCMD word on the bus\n"
           "     every 2 SYSCLK, one word per ~3.6 pixel clocks, so a line sustains about one\n"
           "     record per 3.6 pixels averaged across its width and denser BURSTS are fine\n"
           "     only to the extent HBLANK pre-buffered them.  The registered-/RE fetch adds\n"
           "     a SECOND, unconditional ceiling underneath it: the compositor itself eats\n"
           "     one word per %u pixel clocks, so no amount of pre-buffering makes a record\n"
           "     list denser than one record per two slots.  A one-pixel span therefore needs\n"
           "     a %u-pixel pitch to land where it was authored (the cadence traces measure\n"
           "     exactly that), and a burst tighter than that is not late — it is impossible.\n"
           "     The two ceilings do not meet in these cases: the density sweep's densest\n"
           "     point spreads 64 spans over 640 pixels, a 9-pixel pitch, so the bus is still\n"
           "     what runs out first and the sweep's answers are unchanged.\n",
           VIDCMD_SLOTS_PER_WORD, TEMPEST_STRESS_MIN_PITCH);
    printf(" 11. CLOSED 2026-08-19, AND THE FIX WAS A WIRE. The COMPOSITOR->PIXEL SET\n"
           "     conduit used to be a shadow tap of the VIDCMD Q bus, and it did not work:\n"
           "     COMPOSITOR held set_pix_valid/target as levels and pulsed set_pix_commit,\n"
           "     PIXEL captured VIDCMD_Q while valid was high, and Q had moved on by then.\n"
           "     That was the one place this suite could not match a TB measurement.  Two\n"
           "     decisions closed it.  A dedicated 12-bit registered value bus (set_pix_value,\n"
           "     twelve point-to-point traces) carries the payload, so value/valid/target/\n"
           "     commit share one pipeline stage and PIXEL applies the bus straight at the\n"
           "     commit pulse; PIXEL's twelve Q taps came off the shared FIFO bus with it.\n"
           "     And the registered-/RE fetch leaves Q high-Z between reads, so a tap was\n"
           "     never going to work anyway.  A PIXEL-target SET now costs exactly what any\n"
           "     other record costs — the tb measures 2 slots, the same as everything else —\n"
           "     which is what the model always assumed, so the model is now a measurement.\n"
           "     SKEW_PIX_TARGET stays a named zero and m2-split still re-runs at 3/1 px for\n"
           "     the identical image, so the compensation path stays exercised.\n");
    printf(" 16. NEW, FROM THE RTL SYNC, AND IT CUTS BOTH WAYS. Dropping TILE bought back the\n"
           "     two 16-bit mask shifters and cost words instead, but whether that is a good\n"
           "     trade depends entirely on how many HOLES the art has per row.  Measured\n"
           "     here: the four-sprite worst case IMPROVED from 13 words/line to %u, because\n"
           "     a diamond row is one contiguous run and a tile always cost 3 words.  The\n"
           "     cursor got WORSE, 4 words/line to %u, because an arrow row with an eroded\n"
           "     outline is up to five alternating fg/bg/transparent segments and each one\n"
           "     is its own record.  Rule of thumb for the list builder: contiguous art is\n"
           "     cheaper as runs, and art with more than ~2 holes per row is what TILE was\n"
           "     actually for.\n",
           m3_sprite_words, m3_cursor_words);
    printf(" 17. REINSTATED 2026-08-19, IN A CADENCE-AWARE FORM. The 1-word/clock rework\n"
           "     retired the prefetch invariant entirely — playback(k) >= words(k+1) was\n"
           "     satisfied by construction once every record was one word and one slot, and\n"
           "     the check was deleted rather than left to pass vacuously.  The registered-\n"
           "     /RE fetch brings the obligation back: one word per %u slots means a record\n"
           "     list has to AVERAGE %u slots per record, with the two-deep on-chip bank\n"
           "     (staged_word plus the parked Q) as the line's entire free credit.  It is no\n"
           "     longer a closed form — whether a given record is half of a banked pair\n"
           "     depends on the whole prefix — so the rule is a SIMULATION,\n"
           "     descriptor.h's vidcmd_plan_line(), and the list builder runs it over every\n"
           "     line to size that line's filler.  The check is not vacuous either way: the\n"
           "     render model drives the same engine independently against real deposit\n"
           "     cycles, so a planner that got the arithmetic wrong would show up as an\n"
           "     overrun in the CUSHION assertions.\n",
           VIDCMD_SLOTS_PER_WORD, VIDCMD_SLOTS_PER_WORD);
    printf(" 18. NEW, FROM THE RTL SYNC. PIXEL underrun is not an error condition.  pixel.v\n"
           "     has no empty-flag input and a 7200 holds Q while empty, so a short PIXELS\n"
           "     fill simply re-delivers the last word — a bandwidth compressor, not a\n"
           "     fault.  The suite counts tiled words and still requires zero on every case,\n"
           "     because none of these lists intends to be short; but the counter is a\n"
           "     measurement now, not an assertion about the hardware.\n");
    printf(" 19. NEW 2026-08-19, AND IT COST ART. The VIDCMD read port is a registered /RE\n"
           "     driving the 7200 pair directly: two pixel clocks per word, no shaping gate.\n"
           "     The alternative died on the pinned worst-case table, not on a breadboard\n"
           "     (griffin.yml interfaces, \"VIDCMD FIFO read port\", 2026-08-18), so this is\n"
           "     an electrical verdict the format has to live with rather than a design\n"
           "     preference.  What it buys back is +7.7 ns of margin on the data path; what\n"
           "     it costs is one HOLD slot behind every record that is not half of a banked\n"
           "     pair.  Three consequences, all visible above.  (a) An exact-640 line's\n"
           "     filler is now computed, not summed: m0's tail RUN is 637 slots, not 638,\n"
           "     and m2-split's is 317, not 318 — same images, one fewer authored slot,\n"
           "     because the record after a SET pair lands on slot N+2 rather than N+1.\n"
           "     (b) m2-split's whole point survives untouched: a SET pair behind a run of\n"
           "     four or more still commits on ADJACENT pixels, because the pair law is what\n"
           "     the parked-Q bank exists for.  (c) The cursor is a REGRESSION and is\n"
           "     recorded as one: a 16x16 arrow whose eroded outline leaves 1-pixel fg/bg\n"
           "     segments now paints %u pixels instead of 84, because the HOLD slots inside\n"
           "     the art keep the art's own held colour and widen it.  The framing still\n"
           "     closes — the builder takes the stretch out of the trailing filler — so the\n"
           "     damage is confined to the row that authored it.  Rule for art: features and\n"
           "     gaps of one pixel are no longer free, and anything at least two pixels wide\n"
           "     with two-pixel gaps (every sprite, every tempest object) is unaffected.\n",
           84 + 14);

    printf("\n=== summary ===\n");
    if (g_failures == 0)
    {
        printf("  all cases PASS\n");
        return 0;
    }
    printf("  %d check(s) FAILED\n", g_failures);
    return 1;
}
