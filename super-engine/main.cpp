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

// 2bpp indexed shares micro-HAM's stream rate — 80 words a line — so it gets
// the same 88-word stride with scroll headroom.  ONE buffer serves both rates:
// a HALF-RATE indexed line consumes the first 40 words, which is the first 320
// dibits, so the half-rate expectation is the full-rate one indexed by k/2.
constexpr uint32_t FB_IDX2_BASE       = 0x080000;
constexpr uint32_t FB_IDX2_STRIDE     = 176;

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
    std::vector<uint8_t>     line_words;   // frame 2's per-line VIDCMD word count
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
        out.line_words          = line_words;
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
           spec.base.pure_vidcmd
               ? 0u
               : pixel_words_for(spec.base.mode, spec.base.h_scroll_pixels % 16),
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
    printf("  VIDCMD framing      overruns %u  late words %u  RUN_COLOR %u  MASK %u "
           "(mask stalls %u)\n",
           rr.stats.vidcmd_overruns, rr.stats.vidcmd_late_words, rr.stats.vidcmd_color_runs,
           rr.stats.vidcmd_mask_records, rr.stats.vidcmd_mask_stalls);
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
    // 7200 holds Q), so a short fill is a bandwidth compressor.  The bitmap
    // cases do not want one, so a nonzero count means the list is wrong; the
    // pure-VIDCMD screens never fill PIXELS at all and tile on purpose, which is
    // exactly what makes passthrough a settable third colour there.
    if (!spec.base.pure_vidcmd)
    {
        CHECK(rr.stats.pixels_tiled_words == 0,
              "%s: PIXEL tiled its last word %u times — the pixel stream came up short",
              spec.name.c_str(), rr.stats.pixels_tiled_words);
    }
    CHECK(rr.stats.pixels_overflows == 0, "%s: PIXELS FIFO overflowed %u times",
          spec.name.c_str(), rr.stats.pixels_overflows);
    CHECK(rr.stats.vidcmd_overflows == 0, "%s: VIDCMD FIFO overflowed %u times",
          spec.name.c_str(), rr.stats.vidcmd_overflows);
    CHECK(rr.stats.vidcmd_mask_stalls == 0,
          "%s: a MASK stalled for %u slots waiting for its data word — the record's two "
          "words were not delivered together", spec.name.c_str(), rr.stats.vidcmd_mask_stalls);
    if (spec.base.screen == ScreenStyle::NONE && spec.base.sprites != SpriteStyle::MASK_SPRITES)
    {
        CHECK(rr.stats.vidcmd_mask_records == 0,
              "%s: %u stray `01` MASK records reached the compositor — this case emits none",
              spec.name.c_str(), rr.stats.vidcmd_mask_records);
    }

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

// ---------------------------------------------------------------------------
// PIXEL mode traces — the model against cpld/pixel/pixel_tb.v
// ---------------------------------------------------------------------------
//
// These drive PixelUnit ALONE, with no compositor and no display list, the way
// pixel_tb.v drives the DUT: SET the registers during blanking, fill the FIFO
// with one line's stream, then take 640 pixel clocks.  Every case below is a
// named case in that testbench and every expectation is its expectation.
//
// HOW THE TB'S ASSERTIONS MAP ONTO THIS MODEL.  pixel_tb.v checks two things
// per case: the 640 per-pixel colours, and check_reads(N) — the number of /RE
// byte strobes the line spent.  The colours map one to one.  The /RE count does
// not: this model has no byte engine, no shift register and no /RE, it pulls
// 16-bit WORDS out of the FIFO on demand.  Its observable for the same claim is
// therefore WORDS CONSUMED PER LINE, and the mapping is exactly N/2 because the
// rev-1 PIXELS pair is two byte-wide 7200s read EVEN-then-ODD for one bus word.
// So the tb's 80/160/40/80 bytes are 40/80/20/40 words here, which is also
// descriptor.h's pixels_words_per_line() — the number the display list deposits.
// A model that consumed the wrong amount would drift a word per line down the
// frame, which is what the suite cases' zero-tiled-words assertion catches.
//
// THE TWO-LINE CASES (IDX2_2LINE_*, HAM_2LINE_*) map onto the SUITE cases, not
// onto these traces.  Their claim is that a line reads exactly its own record
// and steals no byte from the next one, so line 2 starts where line 1 stopped;
// the model's form of that claim is 480 consecutive lines rendered pixel-exact
// against a reference in which line y comes from framebuffer row y, with zero
// tiled words.  A line that ate one word too many or too few would slide the
// whole rest of the frame and m8/m9 would fail on line 1.
//
// ONE DELIBERATE DIVERGENCE, and it is the tb's declared limit rather than a
// disagreement: with a nonzero pixel_skip the line needs a fraction of one more
// byte at its far end and pixel.v's fetch guard REFUSES it, so the last 1..7
// pixels of a fine-scrolled line re-shift the previous byte.  This model has no
// fetch guard and simply reads the extra word, so its tail is the authored
// data.  pixel_tb.v checks only the LEADING pixels of its skip cases for that
// reason, and so do the skip cases below.
//
// The stream is written as BYTES, in the tb's own order, and packed into words
// here — so a byte list can be diffed against pixel_tb.v by eye.

std::vector<uint16_t> stream_words(const std::vector<uint8_t> &bytes)
{
    std::vector<uint16_t> w;
    w.reserve((bytes.size() + 1) / 2);
    for (size_t i = 0; i < bytes.size(); i += 2)
    {
        const uint16_t lo = (i + 1 < bytes.size()) ? bytes[i + 1] : 0u;
        w.push_back(static_cast<uint16_t>((bytes[i] << 8) | lo));
    }
    return w;
}

void push_fill(std::vector<uint8_t> &b, uint32_t count, uint8_t v)
{
    for (uint32_t i = 0; i < count; i++)
    {
        b.push_back(v);
    }
}

// pixel_tb.v's stream_bit / stream_dibit, verbatim: "MSB first" as the mode
// definitions state it, indexed by stream position rather than by pixel.
uint32_t stream_bit(const std::vector<uint8_t> &b, uint32_t n)
{
    return (b[n / 8] >> (7 - (n % 8))) & 1u;
}

uint32_t stream_dibit(const std::vector<uint8_t> &b, uint32_t n)
{
    return (b[n / 4] >> (6 - 2 * (n % 4))) & 3u;
}

struct PixelLine
{
    std::vector<Rgb444> px;
    uint32_t            words_consumed = 0;
    uint32_t            tiled_words    = 0;
};

// One visible line out of a freshly reset PixelUnit.  `held_first` inverts the
// SET(pix_mode) / SET(pix_ham_held) order so a case can prove the dependence
// rather than only obey it.
PixelLine pixel_line(uint32_t mode, const std::vector<uint8_t> &bytes,
                     Rgb444 fg, Rgb444 bg, Rgb444 held, uint32_t skip,
                     bool held_first = false)
{
    RenderStats stats;
    PixelUnit   pix;
    pix.reset();
    pix.attach_stats(&stats);
    pix.set_counting(true);

    // Each SET is followed by a blank clock, which is where PIXEL's held_init
    // lives: the SETs really are separated by clocks on which pal_fg can be
    // written into ham_held, which is the whole mechanism of the ordering rule.
    auto set_and_tick = [&](uint32_t target, uint32_t value)
    {
        pix.set_register(target, value);
        pix.blank_clock();
    };

    set_and_tick(SET_PIX_PAL_FG, fg);
    set_and_tick(SET_PIX_PAL_BG, bg);
    if (held_first)
    {
        set_and_tick(SET_PIX_HAM_HELD, held);
        set_and_tick(SET_PIX_MODE, mode);
    }
    else
    {
        set_and_tick(SET_PIX_MODE, mode);
        set_and_tick(SET_PIX_HAM_HELD, held);
    }
    set_and_tick(SET_PIX_PIXEL_SKIP, skip);
    for (uint32_t i = 0; i < 8; i++)
    {
        pix.blank_clock();
    }

    for (uint16_t w : stream_words(bytes))
    {
        pix.push_word(w);
    }

    const uint32_t before = pix.fifo_count();
    pix.begin_line();

    PixelLine out;
    out.px.reserve(H_ACTIVE);
    for (uint32_t i = 0; i < H_ACTIVE; i++)
    {
        out.px.push_back(pix.next_pixel());
    }
    out.words_consumed = before - pix.fifo_count();
    out.tiled_words    = stats.pixels_tiled_words;
    return out;
}

uint32_t pixel_mismatches(const PixelLine &line, const std::vector<Rgb444> &want,
                          uint32_t first, uint32_t last)
{
    uint32_t bad = 0;
    for (uint32_t k = first; k <= last; k++)
    {
        if (line.px[k] != want[k])
        {
            bad++;
        }
    }
    return bad;
}

void report_pixel_case(const char *name, const PixelLine &line, uint32_t bad,
                       uint32_t checked, uint32_t want_words)
{
    printf("  %-18s %4u pixels checked, %u wrong, %2u words consumed (want %u)\n",
           name, checked, bad, line.words_consumed, want_words);
    CHECK(bad == 0, "pixel trace %s: %u of %u checked pixels are wrong", name, bad, checked);
    CHECK(line.words_consumed == want_words,
          "pixel trace %s: the line ate %u PIXELS words, pixel_tb.v's /RE count says %u "
          "(%u bytes)", name, line.words_consumed, want_words, want_words * 2);
}

void check_pixel_modes()
{
    printf("\n=== pixel-modes — the model against cpld/pixel/pixel_tb.v ===\n");

    // --- 1BPP_REGRESSION: the guard that the new decode broke nothing --------
    {
        std::vector<uint8_t> b = {0xB2, 0x4D, 0xF0, 0x0F, 0xAA, 0x55};
        push_fill(b, 74, 0xC3);              // 80 bytes on the line
        const Rgb444 fg = 0xF00;
        const Rgb444 bg = 0x00F;
        const PixelLine line = pixel_line(PIXEL_MODE_DIRECT_1BPP, b, fg, bg, 0x789, 0);

        std::vector<Rgb444> want(H_ACTIVE);
        for (uint32_t k = 0; k < H_ACTIVE; k++)
        {
            want[k] = stream_bit(b, k) != 0 ? fg : bg;
        }
        report_pixel_case("1BPP_REGRESSION", line,
                          pixel_mismatches(line, want, 0, H_ACTIVE - 1), H_ACTIVE,
                          PIXELS_WORDS_1BPP);
    }

    // --- HAM_REGRESSION: micro-HAM still decodes ----------------------------
    //
    // The 20 hand-derived pixels are pixel_tb.v's, copied verbatim with its
    // working: held starts the line at pal_fg, the first pixel of a 4-bit code
    // shows the OLD held colour and the second shows both channels updated
    // together.  Codes d15/d16 straddle a byte boundary on purpose.
    const std::vector<Rgb444> HAM_HEAD = {
        0x111,   // d0  "0 0"      held <- bg
        0x888,   // d1  "0 1"      held <- fg
        0x888,   // d2  prefix 10, shows OLD held
        0xF08,   // d3  g=0 r=1
        0xF08,   // d4  prefix 11, shows OLD held
        0xFF0,   // d5  g=1 b=0
        0x888,   // d6  "0 1"
        0x111,   // d7  "0 0"
        0x111,   // d8  prefix 10, shows OLD held
        0xFF1,   // d9  g=1 r=1
        0xFF1,   // d10 prefix 11, shows OLD held
        0xF00,   // d11 g=0 b=0
        0x888,   // d12 "0 1"
        0x888,   // d13 "0 1"
        0x111,   // d14 "0 0"
        0x111,   // d15 prefix 10, shows OLD held
        0x0F1,   // d16 g=1 r=0 — and d15/d16 span bytes 3/4
        0x111,   // d17 "0 0"
        0x888,   // d18 "0 1"
        0x888,   // d19 "0 1"
    };
    std::vector<uint8_t> ham_bytes = {0x19, 0xE4, 0xBC, 0x52, 0x85};
    push_fill(ham_bytes, 155, 0x55);         // 160 bytes on the line: all "0 1"

    std::vector<Rgb444> ham_want(H_ACTIVE, 0x888);
    for (uint32_t k = 0; k < HAM_HEAD.size(); k++)
    {
        ham_want[k] = HAM_HEAD[k];
    }
    {
        const PixelLine line =
            pixel_line(PIXEL_MODE_MICRO_HAM, ham_bytes, 0x888, 0x111, 0x789, 0);
        report_pixel_case("HAM_REGRESSION", line,
                          pixel_mismatches(line, ham_want, 0, H_ACTIVE - 1), H_ACTIVE,
                          PIXELS_WORDS_MICROHAM);
    }

    // --- HALF RATE IS MASKED OFF IN MICRO-HAM -------------------------------
    //
    // pixel.v computes half_rate = mode[2] & ~mode[0], so the flag DOES NOTHING
    // in HAM: same pixels, same 160 bytes.  Modelling the masking rather than
    // the garbage is the point — a half-rate HAM line would mis-pair every
    // 4-bit code, and one literal turns that into a debuggable outcome.
    {
        const PixelLine line = pixel_line(PIXEL_MODE_MICRO_HAM | PIXEL_MODE_HALF_RATE,
                                          ham_bytes, 0x888, 0x111, 0x789, 0);
        report_pixel_case("HAM_HALF_MASKED", line,
                          pixel_mismatches(line, ham_want, 0, H_ACTIVE - 1), H_ACTIVE,
                          PIXELS_WORDS_MICROHAM);
    }

    // --- THE RESERVED 11 ENCODING IS MICRO-HAM ------------------------------
    {
        const PixelLine line = pixel_line(PIXEL_MODE_MICRO_HAM | PIXEL_MODE_INDEXED_2BPP,
                                          ham_bytes, 0x888, 0x111, 0x789, 0);
        report_pixel_case("MODE_RESERVED_11", line,
                          pixel_mismatches(line, ham_want, 0, H_ACTIVE - 1), H_ACTIVE,
                          PIXELS_WORDS_MICROHAM);
    }

    // --- IDX2_ALL_FOUR: the new mode, all four codes ------------------------
    //
    //   00 pal_bg   01 pal_fg   10 ham_held   11 black (exactly 000)
    //
    // The long 10-fill after pixel 16 is the real point: it proves ham_held
    // survives as a palette entry for the whole line instead of being
    // overwritten by the 00s and 01s in front of it.
    const Rgb444 IDX_BG   = 0x123;
    const Rgb444 IDX_FG   = 0x456;
    const Rgb444 IDX_HELD = 0x789;
    const std::vector<Rgb444> IDX_EXPECT = {IDX_BG, IDX_FG, IDX_HELD, RGB444_BLACK};

    std::vector<uint8_t> idx_bytes = {0x1B, 0xE4, 0x1B, 0xE4};
    push_fill(idx_bytes, 156, 0xAA);         // 160 bytes: all "10" -> ham_held

    std::vector<Rgb444> idx_want(H_ACTIVE, IDX_HELD);
    for (uint32_t k = 0; k < 16; k++)
    {
        idx_want[k] = IDX_EXPECT[stream_dibit(idx_bytes, k)];
    }
    {
        const PixelLine line = pixel_line(PIXEL_MODE_INDEXED_2BPP, idx_bytes,
                                          IDX_FG, IDX_BG, IDX_HELD, 0);
        report_pixel_case("IDX2_ALL_FOUR", line,
                          pixel_mismatches(line, idx_want, 0, H_ACTIVE - 1), H_ACTIVE,
                          PIXELS_WORDS_INDEXED2);
        CHECK(idx_want[3] == RGB444_BLACK && idx_want[4] == RGB444_BLACK,
              "pixel trace IDX2_ALL_FOUR did not exercise code 11 as black");
        CHECK(line.px[H_ACTIVE - 1] == IDX_HELD,
              "pixel trace IDX2_ALL_FOUR: the last pixel is 0x%03X, not the SET-loaded "
              "ham_held 0x%03X — the stream is not locked out of it",
              line.px[H_ACTIVE - 1], IDX_HELD);
    }

    // --- THE ORDER DEPENDENCE: SET mode BEFORE SET ham_held -----------------
    //
    // held_init is suppressed by the mode bit, so ham_held only survives
    // blanking once the mode register already reads indexed.  A list that SETs
    // ham_held and THEN SETs mode has its colour overwritten by pal_fg on the
    // intervening blank clocks — and code 10 then paints pal_fg.
    {
        const PixelLine line = pixel_line(PIXEL_MODE_INDEXED_2BPP, idx_bytes,
                                          IDX_FG, IDX_BG, IDX_HELD, 0, true);
        uint32_t as_fg = 0;
        for (Rgb444 c : line.px)
        {
            if (c == IDX_FG)
            {
                as_fg++;
            }
        }
        printf("  %-18s ham_held SET BEFORE mode: code 10 paints 0x%03X on %u pixels "
               "(0x%03X was lost)\n", "IDX2_SET_ORDER", line.px[H_ACTIVE - 1], as_fg,
               IDX_HELD);
        CHECK(line.px[H_ACTIVE - 1] == IDX_FG,
              "pixel trace IDX2_SET_ORDER: SETting ham_held before mode must lose the "
              "colour to the blanking reload — got 0x%03X, expected pal_fg 0x%03X",
              line.px[H_ACTIVE - 1], IDX_FG);
    }

    // --- IDX2 fine scroll, and the odd clamp --------------------------------
    //
    // skip is in STREAM BITS, so in a two-bit mode skip 4 discards two whole
    // dibits.  skip 5 must render identically: bit 0 is clamped away at
    // consumption because an odd shift would split every index across a dibit
    // boundary.  skip 12 adds the whole-byte discard on top and skip 13 clamps
    // back onto it.  Only the LEADING pixels are compared — see the declared
    // tail limit at the top of this section.
    for (uint32_t skip : {4u, 12u})
    {
        const uint32_t dibits_gone = skip / 2;
        std::vector<Rgb444> want(H_ACTIVE);
        for (uint32_t k = 0; k < 10; k++)
        {
            want[k] = IDX_EXPECT[stream_dibit(idx_bytes, k + dibits_gone)];
        }

        const PixelLine even = pixel_line(PIXEL_MODE_INDEXED_2BPP, idx_bytes,
                                          IDX_FG, IDX_BG, IDX_HELD, skip);
        const PixelLine odd  = pixel_line(PIXEL_MODE_INDEXED_2BPP, idx_bytes,
                                          IDX_FG, IDX_BG, IDX_HELD, skip + 1);
        const uint32_t bad_even = pixel_mismatches(even, want, 0, 9);
        uint32_t       differ   = 0;
        for (uint32_t k = 0; k < 10; k++)
        {
            if (even.px[k] != odd.px[k])
            {
                differ++;
            }
        }
        printf("  %-18s skip %2u discards %u dibits: %u wrong; skip %2u differs on %u "
               "of 10 pixels\n", "IDX2_SKIP_CLAMP", skip, dibits_gone, bad_even,
               skip + 1, differ);
        CHECK(bad_even == 0,
              "pixel trace IDX2_SKIP%u: %u of the leading 10 pixels are wrong", skip,
              bad_even);
        CHECK(differ == 0,
              "pixel trace IDX2_SKIP%uODD: an odd skip must clamp to the even one, %u of "
              "10 pixels differ", skip + 1, differ);
    }

    // --- HALF_1BPP: 320 groups across the 640-clock window ------------------
    //
    // The expectation is the full-rate definition with the group index k >> 1,
    // which is precisely the claim "each group is held for exactly two clocks
    // and there are 320 of them".  A group held for one clock, three clocks, or
    // with the pair phase inverted fails immediately.  The word count is the
    // other half of the claim: half the stream.
    {
        std::vector<uint8_t> b = {0xB2, 0x4D, 0xF0, 0x0F, 0xAA, 0x55};
        push_fill(b, 34, 0xC3);              // 40 bytes on a half-rate 1bpp line
        const Rgb444 fg = 0xF00;
        const Rgb444 bg = 0x00F;
        const PixelLine line = pixel_line(PIXEL_MODE_DIRECT_1BPP | PIXEL_MODE_HALF_RATE,
                                          b, fg, bg, 0x789, 0);

        std::vector<Rgb444> want(H_ACTIVE);
        for (uint32_t k = 0; k < H_ACTIVE; k++)
        {
            want[k] = stream_bit(b, k / 2) != 0 ? fg : bg;
        }
        report_pixel_case("HALF_1BPP", line,
                          pixel_mismatches(line, want, 0, H_ACTIVE - 1), H_ACTIVE,
                          PIXELS_WORDS_HALF_1BPP);
    }

    // --- HALF_IDX2 ----------------------------------------------------------
    {
        std::vector<uint8_t> b = {0x1B, 0xE4, 0x1B, 0xE4};
        push_fill(b, 76, 0x6C);              // 80 bytes: 01 10 11 00, all four codes
        const PixelLine line = pixel_line(PIXEL_MODE_INDEXED_2BPP | PIXEL_MODE_HALF_RATE,
                                          b, IDX_FG, IDX_BG, IDX_HELD, 0);

        std::vector<Rgb444> want(H_ACTIVE);
        for (uint32_t k = 0; k < H_ACTIVE; k++)
        {
            want[k] = IDX_EXPECT[stream_dibit(b, k / 2)];
        }
        report_pixel_case("HALF_IDX2", line,
                          pixel_mismatches(line, want, 0, H_ACTIVE - 1), H_ACTIVE,
                          PIXELS_WORDS_HALF_INDEXED2);
    }

    // --- INDEXED UNDERRUN, and what the model does --------------------------
    //
    // There is no empty flag: /RE keeps firing, a 7200 ignores a read while
    // empty and holds Q, so the last word pattern repeats.  From reset that
    // word is zero, every dibit reads 00, and the whole line renders pal_bg —
    // which makes the third-colour trick the pure-VIDCMD screens spend
    // available in indexed mode too.  Finding 22's caveat applies unchanged:
    // THIS IS WHAT THE MODEL DOES, not what a 7200 pair is known to present on
    // Q after /RS with no write at all.
    {
        const PixelLine line = pixel_line(PIXEL_MODE_INDEXED_2BPP, {},
                                          IDX_FG, IDX_BG, IDX_HELD, 0);
        uint32_t off = 0;
        for (Rgb444 c : line.px)
        {
            if (c != IDX_BG)
            {
                off++;
            }
        }
        printf("  %-18s empty FIFO re-shifts zeroes: %u of %u pixels are pal_bg 0x%03X, "
               "%u tiled words\n", "IDX2_UNDERRUN", H_ACTIVE - off, H_ACTIVE, IDX_BG,
               line.tiled_words);
        CHECK(off == 0,
              "pixel trace IDX2_UNDERRUN: %u pixels are not pal_bg — an empty PIXELS FIFO "
              "must re-shift its held word", off);
        CHECK(line.tiled_words == PIXELS_WORDS_INDEXED2,
              "pixel trace IDX2_UNDERRUN: %u tiled words, expected the line's whole %u",
              line.tiled_words, PIXELS_WORDS_INDEXED2);
    }

    printf("  MEASURED            full rate 40 words 1bpp / 80 words two-bit; "
           "half rate 20 / 40; HAM ignores the flag\n");
}

// ---------------------------------------------------------------------------
// Reference rasterizers for the two pure-VIDCMD screens
// ---------------------------------------------------------------------------
//
// These draw the screens the ordinary way — straight into a pixel buffer, from
// the same font and the same layout the authoring uses — so the comparison
// against the rendered frame is a test of THE RECORDS AND THE COMPOSITOR, not
// of the font or the text.  If a dibit lands on the wrong pixel, a mask chains
// with a gap, or a SET seam is a different number of slots than the geometry
// was priced at, the two images stop matching.

FrameImage console_reference_frame()
{
    FrameImage img;
    img.pixels.assign(static_cast<size_t>(H_ACTIVE) * V_ACTIVE, 0);

    std::array<InkClass, H_ACTIVE> cls{};
    for (uint32_t y = 0; y < V_ACTIVE; y++)
    {
        console_line_classes(y, cls);
        const uint32_t row = y / CONSOLE_CELL_H;
        const Rgb444   c1  = console_row_color1(row);
        const Rgb444   c0  = console_row_color0(row);
        for (uint32_t x = 0; x < H_ACTIVE; x++)
        {
            const Rgb444 c = (cls[x] == InkClass::INK)   ? c1
                             : (cls[x] == InkClass::ALT) ? CONSOLE_ALT_COLOR
                                                         : c0;
            img.pixels[y * H_ACTIVE + x] = c;
        }
    }
    return img;
}

FrameImage kiosk_reference_frame()
{
    FrameImage img;
    img.pixels.assign(static_cast<size_t>(H_ACTIVE) * V_ACTIVE, 0);

    std::array<InkClass, H_ACTIVE> cls{};
    for (uint32_t y = 0; y < V_ACTIVE; y++)
    {
        const KioskLinePlan plan = kiosk_plan_line(y);
        const Rgb444        bg   = run_colour_to_rgb444(plan.background_color);
        kiosk_line_classes(plan, y, cls);

        for (uint32_t x = 0; x < H_ACTIVE; x++)
        {
            img.pixels[y * H_ACTIVE + x] = bg;
        }
        for (uint32_t s = 0; s < plan.seg_n; s++)
        {
            const KioskSegment &seg = plan.seg[s];
            for (uint32_t i = 0; i < seg.records * KIOSK_CELL_W; i++)
            {
                const uint32_t x = seg.x0 + i;
                img.pixels[y * H_ACTIVE + x] =
                    (cls[x] == InkClass::INK)   ? seg.color1
                    : (cls[x] == InkClass::ALT) ? KIOSK_ALT_COLOR
                                                : seg.color0;
            }
        }
    }
    return img;
}

// ---------------------------------------------------------------------------
// Reference rasterizers for the two new PIXEL modes
// ---------------------------------------------------------------------------
//
// Same discipline as the screen references above: draw the picture the ordinary
// way, straight from the mode definition and the same pattern generator the
// authoring uses, so the comparison tests THE MODE AND THE RECORDS rather than
// the pattern.

// 2bpp indexed, with the mid-line SET pair placed by slot arithmetic.  The two
// SETs are the leading RUN's banked pair, so pix_pal_bg commits on slot
// split_pixel and pix_pal_fg on slot split_pixel + 1 — exactly what m2-split
// pins in 1bpp, restated here in terms of two of the four indices.  Codes 10
// and 11 are untouched by the recolour: ham_held has no mid-line SET and black
// is a constant, so a two-word recolour moves two of the four colours and the
// other two hold.
FrameImage indexed2_reference_frame(const FrameParams &p)
{
    FrameImage img;
    img.pixels.assign(static_cast<size_t>(H_ACTIVE) * V_ACTIVE, 0);

    for (uint32_t y = 0; y < V_ACTIVE; y++)
    {
        const Rgb444 line_fg = line_palette_fg(y);
        const Rgb444 line_bg = line_palette_bg(y);

        for (uint32_t x = 0; x < H_ACTIVE; x++)
        {
            const Rgb444 bg = (p.mid_line_split && x >= p.split_pixel) ? p.split_bg : line_bg;
            const Rgb444 fg =
                (p.mid_line_split && x >= p.split_pixel + 1) ? p.split_fg : line_fg;

            Rgb444 c = RGB444_BLACK;
            switch (idx2_pattern_code(x, y))
            {
                case IDX2_CODE_PAL_BG:   c = bg;                break;
                case IDX2_CODE_PAL_FG:   c = fg;                break;
                case IDX2_CODE_HAM_HELD: c = p.ham_held_color;  break;
                default:                 c = RGB444_BLACK;      break;
            }
            img.pixels[y * H_ACTIVE + x] = c;
        }
    }
    return img;
}

// The 1bpp test pattern's rule, restated so the reference does not have to read
// the framebuffer back.  `b` is a STREAM BIT index, which at half rate is the
// screen x divided by two.
bool test_pattern_bit(uint32_t b, uint32_t y)
{
    if ((y % 32u) < 2u)
    {
        return (b % 8u) < 4u;
    }
    return (((b + y) / 16u) & 1u) != 0;
}

// Half-rate 1bpp with four MASK sprites over it.  THE INTEROP STATEMENT THIS
// ENCODES: a MASK's dibits step once per PIXEL-CLOCK SLOT and COMPOSITOR has no
// idea what rate PIXEL is running at, so the sprites keep full 640-pixel
// horizontal resolution over a playfield whose groups are two pixels wide.
// Half rate is a stream-side gate inside PIXEL and nothing else in the pipeline
// sees it.
FrameImage halfrate_reference_frame(const FrameParams &p)
{
    FrameImage img;
    img.pixels.assign(static_cast<size_t>(H_ACTIVE) * V_ACTIVE, 0);

    for (uint32_t y = 0; y < V_ACTIVE; y++)
    {
        const Rgb444 fg = line_palette_fg(y);
        const Rgb444 bg = line_palette_bg(y);

        for (uint32_t x = 0; x < H_ACTIVE; x++)
        {
            // The playfield: one stream bit covers two pixel clocks.
            img.pixels[y * H_ACTIVE + x] = test_pattern_bit(x / 2u, y) ? fg : bg;
        }

        for (uint32_t s = 0; s < MASK_SPRITE_COUNT; s++)
        {
            const uint32_t x0 = MASK_SPRITE_X[s];
            for (uint32_t c = 0; c < MASK_SLOTS; c++)
            {
                const uint32_t dibit = mask_sprite_dibit(y % MASK_SPRITE_H, c);
                if (dibit == MASK_DIBIT_COLOR0)
                {
                    img.pixels[y * H_ACTIVE + x0 + c] = p.held_bg;
                }
                else if (dibit == MASK_DIBIT_COLOR1)
                {
                    img.pixels[y * H_ACTIVE + x0 + c] = p.held_fg;
                }
                // MASK_DIBIT_PASSTHROUGH keeps the playfield underneath.
            }
        }
    }
    return img;
}

// Is this pixel inside one of the MASK sprite cells?
bool in_mask_sprite(uint32_t x)
{
    for (uint32_t s = 0; s < MASK_SPRITE_COUNT; s++)
    {
        if (x >= MASK_SPRITE_X[s] && x < MASK_SPRITE_X[s] + MASK_SLOTS)
        {
            return true;
        }
    }
    return false;
}

// THE LAYOUT INVARIANT both screens are built on: a record's pixel 0 has no
// dibit, so whatever lands on a 16-pixel boundary comes out cmp_color0.  Both
// cell layouts leave a blank column there; this counts the places they do not.
uint32_t console_pixel0_violations()
{
    std::array<InkClass, H_ACTIVE> cls{};
    uint32_t bad = 0;
    for (uint32_t y = 0; y < V_ACTIVE; y++)
    {
        console_line_classes(y, cls);
        for (uint32_t g = 0; g < H_ACTIVE / MASK_SLOTS; g++)
        {
            std::array<uint32_t, MASK_SLOTS> d{};
            for (uint32_t i = 0; i < MASK_SLOTS; i++)
            {
                d[i] = ink_class_dibit(cls[g * MASK_SLOTS + i]);
            }
            if (!vidcmd_mask_pixel0_ok(d))
            {
                bad++;
            }
        }
    }
    return bad;
}

uint32_t kiosk_pixel0_violations()
{
    std::array<InkClass, H_ACTIVE> cls{};
    uint32_t bad = 0;
    for (uint32_t y = 0; y < V_ACTIVE; y++)
    {
        const KioskLinePlan plan = kiosk_plan_line(y);
        kiosk_line_classes(plan, y, cls);
        for (uint32_t s = 0; s < plan.seg_n; s++)
        {
            for (uint32_t g = 0; g < plan.seg[s].records; g++)
            {
                std::array<uint32_t, MASK_SLOTS> d{};
                for (uint32_t i = 0; i < MASK_SLOTS; i++)
                {
                    d[i] = ink_class_dibit(cls[plan.seg[s].x0 + g * MASK_SLOTS + i]);
                }
                if (!vidcmd_mask_pixel0_ok(d))
                {
                    bad++;
                }
            }
        }
    }
    return bad;
}

// Compares both rendered frames against one reference and reports the worst
// offender, because "17 pixels differ" is useless without an address.
uint32_t compare_frames(const char *name, const CaseResult &res, const FrameImage &ref)
{
    uint32_t differing = 0;
    uint32_t first_x   = 0;
    uint32_t first_y   = 0;
    Rgb444   got       = 0;
    Rgb444   want      = 0;

    for (uint32_t f = 0; f < RENDER_FRAMES; f++)
    {
        for (uint32_t y = 0; y < V_ACTIVE; y++)
        {
            for (uint32_t x = 0; x < H_ACTIVE; x++)
            {
                const Rgb444 a = res.rendered.frames[f].pixels[y * H_ACTIVE + x];
                const Rgb444 b = ref.pixels[y * H_ACTIVE + x];
                if (a != b)
                {
                    if (differing == 0)
                    {
                        first_x = x;
                        first_y = y;
                        got     = a;
                        want    = b;
                    }
                    differing++;
                }
            }
        }
    }

    if (differing == 0)
    {
        printf("  pixel exactness     %s: all %u pixels of both frames match the reference\n",
               name, H_ACTIVE * V_ACTIVE);
    }
    else
    {
        printf("  pixel exactness     %s: %u pixels differ, first at (%u,%u) 0x%03X vs "
               "0x%03X\n", name, differing, first_x, first_y, got, want);
    }
    return differing;
}

// ---------------------------------------------------------------------------
// MASK cadence traces — the model against compositor_tb.v's MASKB_* traces
// ---------------------------------------------------------------------------
//
// compositor_tb.v's numbers are GROUND TRUTH for this model.  Any disagreement
// here is a model bug, not an expectation to retune.  The tb derives all of
// them from five laws on top of L1-L5, and this section re-derives each one in
// place so the assertion is readable without the RTL open:
//
//   LM1 the header is PLAYBACK-class: its own slot is the record's pixel 0, an
//       implicit opaque cmp_color0, so it waits for H_ACTIVE like a RUN and is
//       never eager in blanking.  The header carries NO colour.
//   LM2 the record is SIXTEEN slots — pixel 0 implicit, 1..7 the header's seven
//       dibits, 8..15 the data word's eight — and it is MODAL: a dibit
//       overrides the source for its own pixel only and the span in force
//       resumes, untouched, at the end.
//   LM3 the mask owns staged_word and the fetch parks exactly one word behind
//       it, except on two edges: the pixel-8 edge takes the data word out of
//       the park and reads d8 straight off Q, and the pixel-15 edge captures
//       the NEXT RECORD one slot early.
//   LM4 no data word at pixel 8 -> HOLD, pixel 7's source stretches.
//   LM5 the data word is captured with have_staged LOW, so no bit pattern of it
//       can reach the SET/RUN decode or the PIXEL bus.
//
// Two seam numbers below are DERIVED HERE rather than measured by the tb, and
// both are written out cycle by cycle at their trace.

VidcmdMask trace_mask(const std::array<uint32_t, MASK_SLOTS - 1> &d1_15)
{
    std::array<uint32_t, MASK_SLOTS> d{};
    d[0] = MASK_PIXEL0_DIBIT;   // no bit exists for it; this is documentation
    for (uint32_t i = 0; i < MASK_SLOTS - 1; i++)
    {
        d[i + 1] = d1_15[i];
    }
    return vidcmd_mask(d);
}

void push_mask(std::vector<uint16_t> &w, const VidcmdMask &m)
{
    w.push_back(m.header);
    w.push_back(m.data);
}

void push_mask_into(CompositorUnit &cmp, const VidcmdMask &m)
{
    cmp.push_word(m.header);
    cmp.push_word(m.data);
}

// The lowest slot at or after `from` whose colour is not `bg`.
uint32_t first_ne_from(const std::vector<Rgb444> &px, uint32_t from, Rgb444 bg)
{
    for (uint32_t i = from; i < px.size(); i++)
    {
        if (px[i] != bg)
        {
            return i;
        }
    }
    return static_cast<uint32_t>(px.size());
}

void check_mask_traces()
{
    printf("\n=== compositor-mask — the model against compositor_tb.v's MASKB_* traces ===\n");

    constexpr uint32_t T = MASK_DIBIT_PASSTHROUGH;
    constexpr uint32_t R = MASK_DIBIT_RESERVED;
    constexpr uint32_t B = MASK_DIBIT_COLOR0;
    constexpr uint32_t F = MASK_DIBIT_COLOR1;

    constexpr Rgb444 C0 = 0x00F;
    constexpr Rgb444 C1 = 0xF0F;
    const Rgb444     PT = TRACE_PASSTHROUGH;

    uint32_t mask_run_gap      = 0;
    uint32_t mask_chain_gap    = 0;
    uint32_t mask_set_gap      = 0;
    uint32_t mask_set_set_gap  = 0;
    uint32_t run_set_set_gap   = 0;
    uint32_t starve_reload_slot = 0;

    // MASKB_SPRITE.  SET(color0), SET(color1), RUN(pt,8), header, data, and
    // NOTHING behind it, so the tail proves the modal restore by HOLD rather
    // than by a following RUN.  L2 puts the RUN on slots 0..7; the header is
    // playback-class so it waits (LM1) and executes on slot 8, which is the
    // record's PIXEL 0 — hence a RUN-to-mask gap of ZERO.  The data word here
    // is 0xC6F8: bit 15 set with target 4, so it would decode as a PIXEL-target
    // SET if it were ever decoded (LM5).
    {
        std::vector<uint16_t> w = {vidcmd_set(SET_CMP_COLOR0, C0),
                                   vidcmd_set(SET_CMP_COLOR1, C1),
                                   vidcmd_run(RUN_SRC_PASSTHROUGH, 8)};
        const VidcmdMask m = trace_mask({T, R, B, F, T, R, B, F, T, R, B, F, F, B, T});
        push_mask(w, m);
        const std::vector<Rgb444> px = trace_slots(w, H_ACTIVE);

        static const Rgb444 want[MASK_SLOTS] = {C0, PT, PT, C0, C1, PT, PT, C0,
                                                C1, PT, PT, C0, C1, C1, C0, PT};
        uint32_t bad = 0;
        for (uint32_t i = 0; i < MASK_SLOTS; i++)
        {
            if (px[8 + i] != want[i])
            {
                bad++;
            }
        }
        uint32_t tail = 0;
        for (uint32_t i = 8 + MASK_SLOTS; i < H_ACTIVE; i++)
        {
            if (px[i] != PT)
            {
                tail++;
            }
        }
        mask_run_gap = first_ne_from(px, 0, PT) - 7 - 1;

        printf("  MASKB_SPRITE        px0..px15 %03X %03X %03X %03X %03X %03X %03X %03X "
               "%03X %03X %03X %03X %03X %03X %03X %03X\n",
               px[8], px[9], px[10], px[11], px[12], px[13], px[14], px[15],
               px[16], px[17], px[18], px[19], px[20], px[21], px[22], px[23]);
        CHECK(px[8] == C0, "mask sprite: pixel 0 must be the implicit opaque color0 0x%03X, "
              "got 0x%03X", C0, px[8]);
        CHECK(bad == 0, "mask sprite: %u of the sixteen pixels do not match their dibits", bad);
        CHECK(tail == 0, "mask sprite: %u tail pixels are not the passthrough the record "
              "borrowed from — the modal restore failed", tail);
        CHECK(mask_run_gap == MASK_GAP_AFTER_RUN,
              "mask sprite: RUN-to-mask gap is %u slot(s), the tb measures %u",
              mask_run_gap, MASK_GAP_AFTER_RUN);
        CHECK(m.data == 0xC6F8,
              "mask sprite: the data word encodes to 0x%04X, the tb pushes 0xC6F8 — the two "
              "encoders have drifted", m.data);
    }

    // LM5, with a data word chosen to be VISIBLE if it were ever decoded.
    // 0x9ABC is SET(target 1 = cmp_color0, 0xABC): if the shifter's word reached
    // the decode, mask B's implicit pixel 0 would come out 0xABC.  It must stay
    // the color0 the SET in blanking wrote.
    {
        const VidcmdMask a = trace_mask({F, F, F, F, F, F, F, B, R, B, B, B, F, F, T});
        std::vector<uint16_t> w = {vidcmd_set(SET_CMP_COLOR0, C0),
                                   vidcmd_set(SET_CMP_COLOR1, C1),
                                   vidcmd_run(RUN_SRC_PASSTHROUGH, 4)};
        push_mask(w, a);
        push_mask(w, trace_mask({F, F, F, F, F, F, F, F, F, F, F, F, F, F, F}));
        const std::vector<Rgb444> px = trace_slots(w, H_ACTIVE);
        printf("  MASK data not decoded  data word 0x%04X would be SET(color0,0x%03X); "
               "mask B pixel 0 is 0x%03X\n", a.data, a.data & 0xFFFu, px[20]);
        CHECK(a.data == 0x9ABC,
              "mask LM5: the probe data word encodes to 0x%04X, expected 0x9ABC", a.data);
        CHECK(px[20] == C0,
              "mask LM5: the data word reached the SET decode — mask B pixel 0 is 0x%03X, "
              "expected the unchanged color0 0x%03X", px[20], C0);
    }

    // MASKB_BLANK.  SET, SET, header, data, RUN(color1,600).  The SETs are
    // setup and are eager; the header PAINTS, so it waits for H_ACTIVE (LM1),
    // which is what puts pixel 0 on slot 0.  The RUN behind it is captured on
    // the pixel-15 edge and executes on slot 16 — gap 0 — and its load must
    // BEAT the modal restore on that same edge, so d15 is passthrough on
    // purpose and slot 16 must be color1, not passthrough.
    {
        std::vector<uint16_t> w = {vidcmd_set(SET_CMP_COLOR0, C0),
                                   vidcmd_set(SET_CMP_COLOR1, C1)};
        push_mask(w, trace_mask({F, F, B, B, T, T, F, B, T, F, B, F, B, F, T}));
        w.push_back(vidcmd_run(RUN_SRC_COLOR1, 600));
        const std::vector<Rgb444> px = trace_slots(w, H_ACTIVE);

        static const Rgb444 want[MASK_SLOTS] = {C0, C1, C1, C0, C0, PT, PT, C1,
                                                C0, PT, C1, C0, C1, C0, C1, PT};
        uint32_t bad = 0;
        for (uint32_t i = 0; i < MASK_SLOTS; i++)
        {
            if (px[i] != want[i])
            {
                bad++;
            }
        }
        uint32_t tail = 0;
        for (uint32_t i = MASK_SLOTS; i < H_ACTIVE; i++)
        {
            if (px[i] != C1)
            {
                tail++;
            }
        }
        printf("  MASKB_BLANK         a banked header plays pixel 0 in slot 0 (%03X), the "
               "record behind it lands at slot %u\n", px[0], MASK_SLOTS);
        CHECK(bad == 0, "mask blank: %u of the sixteen pixels are wrong — a header banked in "
              "blanking must not play early", bad);
        CHECK(tail == 0, "mask blank: %u pixels after the record are not the following RUN's "
              "colour — the load did not beat the modal restore", tail);
    }

    // MASKB_CHAIN32 — THE HEADLINE PROPERTY.  SET, SET, RUN(pt,4), hdrA, dataA,
    // hdrB, dataB, and nothing else.  By LM3, hdrB is captured on the very edge
    // that paints A's pixel 15, so it is staged with a terminal count during A's
    // last slot and the ordinary consume rule fires it on the NEXT edge: B's
    // pixel 0 is the slot immediately after A's pixel 15.  GAP = 0.
    //
    // Nothing follows the pair on purpose: the tail discriminates sav_src.  Mask
    // B must NOT have re-saved cur_src at its own load edge (which was mask A's
    // d15, cmp_color1), or the tail would be color1 instead of the passthrough
    // the FIRST mask borrowed.
    {
        std::vector<uint16_t> w = {vidcmd_set(SET_CMP_COLOR0, C0),
                                   vidcmd_set(SET_CMP_COLOR1, C1),
                                   vidcmd_run(RUN_SRC_PASSTHROUGH, 4)};
        push_mask(w, trace_mask({F, F, F, F, F, F, F, F, F, F, F, F, F, F, F}));
        push_mask(w, trace_mask({B, F, B, F, B, F, B, F, B, F, B, F, B, F, B}));
        const std::vector<Rgb444> px = trace_slots(w, H_ACTIVE);

        uint32_t bad = 0;
        for (uint32_t i = 5; i <= 19; i++)
        {
            if (px[i] != C1)
            {
                bad++;
            }
        }
        // Mask B is px0 (implicit color0) followed by d1 = color0, so the pair
        // shows TWO color0 pixels at the seam and alternates from px2 on.  That
        // is what makes the 19/20 boundary checkable in both directions.
        static const Rgb444 want_b[MASK_SLOTS] = {C0, C0, C1, C0, C1, C0, C1, C0,
                                                  C1, C0, C1, C0, C1, C0, C1, C0};
        for (uint32_t i = 0; i < MASK_SLOTS; i++)
        {
            if (px[20 + i] != want_b[i])
            {
                bad++;
            }
        }
        uint32_t tail = 0;
        for (uint32_t i = 36; i < H_ACTIVE; i++)
        {
            if (px[i] != PT)
            {
                tail++;
            }
        }
        mask_chain_gap = first_ne_from(px, 20, PT) - 19 - 1;

        printf("  MASKB_CHAIN32       32 contiguous sprite pixels over slots 4..35, "
               "%u pattern exceptions, chain gap %u\n", bad, mask_chain_gap);
        CHECK(px[4] == C0, "mask chain: mask A pixel 0 must be the implicit color0");
        CHECK(px[20] == C0, "mask chain: mask B pixel 0 must be the implicit color0 in the "
              "very next slot, got 0x%03X", px[20]);
        CHECK(bad == 0, "mask chain: %u pixels of the 32-pixel pair are wrong", bad);
        CHECK(mask_chain_gap == MASK_GAP_AFTER_MASK,
              "mask chain: chained masks left a %u-slot seam, the tb measures %u",
              mask_chain_gap, MASK_GAP_AFTER_MASK);
        CHECK(tail == 0, "mask chain: %u tail pixels are not the source the FIRST mask "
              "borrowed — a chained mask re-saved sav_src", tail);
    }

    // MASKB_RECOLOR — mask, SET(color1), mask.  DERIVED: the pixel-15 edge
    // captures the SET (LM3), so the SET is staged with a terminal count during
    // mask A's last slot and executes on the slot after it, showing the restored
    // underlying source.  /RE only falls on that edge, so header B is on Q
    // during the NEXT slot and is captured at its end — one HOLD slot — and is
    // consumed one edge later.  GAP = 2, the ordinary L4 cadence.
    {
        std::vector<uint16_t> w = {vidcmd_set(SET_CMP_COLOR0, C0),
                                   vidcmd_set(SET_CMP_COLOR1, C1),
                                   vidcmd_run(RUN_SRC_PASSTHROUGH, 4)};
        push_mask(w, trace_mask({F, F, F, F, F, F, F, F, F, F, F, F, F, F, F}));
        w.push_back(vidcmd_set(SET_CMP_COLOR1, 0x0F0));
        push_mask(w, trace_mask({F, F, F, F, F, F, F, F, F, F, F, F, F, F, F}));
        const std::vector<Rgb444> px = trace_slots(w, H_ACTIVE);

        mask_set_gap = first_ne_from(px, 20, PT) - 19 - 1;
        uint32_t bad = 0;
        for (uint32_t i = 23; i <= 37; i++)
        {
            if (px[i] != 0x0F0)
            {
                bad++;
            }
        }
        printf("  MASKB_RECOLOR       slots 19..22 %03X %03X %03X %03X, seam %u slot(s)\n",
               px[19], px[20], px[21], px[22], mask_set_gap);
        CHECK(px[20] == PT && px[21] == PT,
              "mask recolor: the SET's own slot and its HOLD slot must show the restored "
              "source, got 0x%03X 0x%03X", px[20], px[21]);
        CHECK(px[22] == C0, "mask recolor: mask B pixel 0 is still the implicit color0, "
              "got 0x%03X", px[22]);
        CHECK(bad == 0, "mask recolor: %u pixels of mask B are not the NEW color1", bad);
        CHECK(mask_set_gap == MASK_GAP_AFTER_MASK_SET,
              "mask recolor: a SET between two masks cost %u slot(s), the tb measures %u",
              mask_set_gap, MASK_GAP_AFTER_MASK_SET);
    }

    // MASK -> SET, SET -> MASK.  DERIVED HERE, NOT MEASURED BY THE TB, and the
    // derivation is what says it is 4 and not 3.  Let mask A's pixel 15 be slot
    // P.  While A plays, mask_holds keeps the buffer shut, so exactly ONE word
    // is parked on Q — the two SETs cannot be a banked pair.
    //
    //   E(P)    pos 14: SET 1 is captured, and /RE rises because nothing is
    //           left parked.
    //   E(P+1)  SET 1 executes (slot P+1, showing the restored source), and /RE
    //           falls here for the first time since, so SET 2 is only on Q
    //           during slot P+2.
    //   E(P+2)  SET 2 is captured.  Slot P+2 is a HOLD.
    //   E(P+3)  SET 2 executes (slot P+3); /RE falls; header B on Q at P+4.
    //   E(P+4)  header B captured.  Slot P+4 is a HOLD.
    //   E(P+5)  header B executes: mask B pixel 0 is slot P+5.
    //
    // GAP = (P+5) - P - 1 = 4 — two records at the ordinary 2-slot cadence,
    // exactly MASK_GAP_AFTER_MASK_SET twice.
    {
        std::vector<uint16_t> w = {vidcmd_set(SET_CMP_COLOR0, C0),
                                   vidcmd_set(SET_CMP_COLOR1, C1),
                                   vidcmd_run(RUN_SRC_PASSTHROUGH, 4)};
        push_mask(w, trace_mask({F, F, F, F, F, F, F, F, F, F, F, F, F, F, F}));
        w.push_back(vidcmd_set(SET_CMP_COLOR1, 0x0F0));
        w.push_back(vidcmd_set(SET_CMP_COLOR0, 0x0FF));
        push_mask(w, trace_mask({F, F, F, F, F, F, F, F, F, F, F, F, F, F, F}));
        const std::vector<Rgb444> px = trace_slots(w, H_ACTIVE);

        mask_set_set_gap = first_ne_from(px, 20, PT) - 19 - 1;
        printf("  MASK,SET,SET,MASK   slots 19..25 %03X %03X %03X %03X %03X %03X %03X, "
               "seam %u slot(s)\n",
               px[19], px[20], px[21], px[22], px[23], px[24], px[25], mask_set_set_gap);
        CHECK(mask_set_set_gap == MASK_GAP_AFTER_MASK_SET_SET,
              "mask SET pair seam: derived %u slot(s) from the laws, the model plays %u — "
              "one of the two is wrong and it is not the derivation's job to move",
              MASK_GAP_AFTER_MASK_SET_SET, mask_set_set_gap);
        CHECK(mask_set_set_gap == 2 * MASK_GAP_AFTER_MASK_SET,
              "mask SET pair seam: a second SET must cost exactly what the first did");
        CHECK(px[19 + 1 + mask_set_set_gap] == 0x0FF,
              "mask SET pair seam: mask B's implicit pixel 0 must be the NEW color0 0x0FF, "
              "got 0x%03X", px[19 + 1 + mask_set_set_gap]);
    }

    // RUN -> SET, SET -> MASK, the arrangement the kiosk uses wherever it can.
    // A RUN does NOT hold the buffer, so the fetch banks both SETs while it
    // counts and the pair law puts them on CONSECUTIVE slots.  The header does
    // NOT follow immediately, though, and the reason is the park: /RE was held
    // low to bank SET 2, so it can only RISE on the edge that consumes SET 1
    // and can only FALL on the edge after.  Let the RUN's last slot be X-1:
    //
    //   E(X)    SET 1 executes; the park moves in, so SET 2 is staged; /RE
    //           rises (nothing is parked any more).
    //   E(X+1)  SET 2 executes; /RE falls for the first time, so the header is
    //           only on Q during slot X+2.
    //   E(X+2)  the header is captured.  Slot X+2 is a HOLD.
    //   E(X+3)  the header executes: mask pixel 0 is slot X+3.
    //
    // GAP = 3.  compositor_tb's PAIR_LOCAL pins the same shape with a RUN in
    // place of the mask (SETs on 4 and 5, HOLD on 6, next record on 7), so this
    // is a re-derivation of a measured trace rather than a new claim.
    {
        std::vector<uint16_t> w = {vidcmd_set(SET_CMP_COLOR0, C0),
                                   vidcmd_set(SET_CMP_COLOR1, C1),
                                   vidcmd_run_color(RUN_COLOR_GREEN, 20),
                                   vidcmd_set(SET_CMP_COLOR1, 0x0F0),
                                   vidcmd_set(SET_CMP_COLOR0, 0x0FF)};
        push_mask(w, trace_mask({F, F, F, F, F, F, F, F, F, F, F, F, F, F, F}));
        const std::vector<Rgb444> px = trace_slots(w, H_ACTIVE);

        run_set_set_gap = first_ne_from(px, 20, 0x0F0) - 19 - 1;
        printf("  RUN,SET,SET,MASK    slots 19..23 %03X %03X %03X %03X %03X, seam %u slot(s)\n",
               px[19], px[20], px[21], px[22], px[23], run_set_set_gap);
        CHECK(run_set_set_gap == MASK_GAP_AFTER_RUN_SET_SET,
              "run SET pair seam: derived %u slot(s), the model plays %u",
              MASK_GAP_AFTER_RUN_SET_SET, run_set_set_gap);
        CHECK(px[20 + run_set_set_gap] == 0x0FF,
              "run SET pair seam: the mask's implicit pixel 0 must be the NEW color0, "
              "got 0x%03X", px[20 + run_set_set_gap]);
    }

    // MASKB_STARVE (LM4).  RUN_COLOR(GREEN,4) then a header whose DATA WORD is
    // withheld.  The span under the mask is a RUN_COLOR on purpose: giving it
    // back means giving back both the source select and cur_colour.  With
    // nothing on Q at the pixel-8 edge the record freezes whole — no count, no
    // shift, no source change — so pixel 7's colour stretches until the word
    // arrives, and the reload edge itself is the one that paints pixel 8.
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
        const VidcmdMask m = trace_mask({T, B, F, T, B, T, F, T, B, F, F, B, T, F, B});
        cmp.push_word(vidcmd_set(SET_CMP_COLOR0, 0x0C0));
        cmp.push_word(vidcmd_set(SET_CMP_COLOR1, C1));
        cmp.push_word(vidcmd_run_color(RUN_COLOR_GREEN, 4));
        cmp.push_word(m.header);
        for (uint32_t i = 0; i < H_BLANK; i++)
        {
            cmp.blank_clock(pix);
        }
        pix.begin_line();

        std::vector<Rgb444> px;
        px.reserve(H_ACTIVE);
        for (uint32_t i = 0; i < H_ACTIVE; i++)
        {
            if (i == 200)
            {
                cmp.push_word(m.data);
            }
            px.push_back(cmp.active_slot(pix));
        }

        // d8 is passthrough and the stretch is color1, so the first slot at or
        // after the stall that is not color1 IS the slot d8 landed in.
        starve_reload_slot = first_ne_from(px, 12, C1);

        static const Rgb444 want_head[8] = {0x0C0, PT, 0x0C0, C1, PT, 0x0C0, PT, C1};
        static const Rgb444 want_data[8] = {PT, 0x0C0, C1, C1, 0x0C0, PT, C1, 0x0C0};
        uint32_t bad = 0;
        for (uint32_t i = 0; i < 8; i++)
        {
            if (px[4 + i] != want_head[i])
            {
                bad++;
            }
            if (px[starve_reload_slot + i] != want_data[i])
            {
                bad++;
            }
        }
        printf("  MASKB_STARVE        px7 held for %u slots, the withheld data half took "
               "effect at slot %u\n", starve_reload_slot - 12, starve_reload_slot);
        CHECK(bad == 0, "mask starve: %u pixels of the interrupted record are wrong", bad);
        CHECK(px[12] == C1 && px[100] == C1 && px[199] == C1,
              "mask starve: the stall must HOLD pixel 7's colour for its whole length");
        CHECK(starve_reload_slot > 200,
              "mask starve: the record resumed at slot %u, before the word was deposited",
              starve_reload_slot);
        // Modal, even across a stall, and even for a RUN_COLOR: the span must
        // come back whole, colour latch included.
        uint32_t tail = 0;
        for (uint32_t i = starve_reload_slot + MASK_DATA_DIBITS; i < H_ACTIVE; i++)
        {
            if (px[i] != 0x0F0)
            {
                tail++;
            }
        }
        CHECK(tail == 0, "mask starve: %u tail pixels are not the RUN_COLOR green the record "
              "interrupted — the modal restore lost cur_colour", tail);
    }

    // A mask that straddles the H_ACTIVE fall freezes rather than being
    // abandoned, and its parked data word is NOT taken during blanking: the two
    // borrow-back edges are gated by H_ACTIVE.  The pixels that are left play at
    // the start of the next line — wrong x, self-healing, exactly like every
    // other overrun here.
    {
        PixelUnit      pix;
        CompositorUnit cmp;
        pix.reset();
        cmp.reset();
        pix.set_register(SET_PIX_PAL_FG, TRACE_PASSTHROUGH);
        for (uint32_t i = 0; i < 2 * (PIXELS_WORDS_1BPP + 1); i++)
        {
            pix.push_word(0xFFFF);
        }
        cmp.push_word(vidcmd_set(SET_CMP_COLOR0, C0));
        cmp.push_word(vidcmd_set(SET_CMP_COLOR1, C1));
        cmp.push_word(vidcmd_run(RUN_SRC_PASSTHROUGH, 4));
        push_mask_into(cmp, trace_mask({F, B, F, B, F, B, F, B, F, B, F, T, B, F, F}));
        for (uint32_t i = 0; i < H_BLANK; i++)
        {
            cmp.blank_clock(pix);
        }

        // A 12-slot line: slots 4..11 are the record's pixels 0..7, and then
        // H_ACTIVE falls with the whole data half unplayed.
        pix.begin_line();
        std::vector<Rgb444> a;
        for (uint32_t i = 0; i < 12; i++)
        {
            a.push_back(cmp.active_slot(pix));
        }
        cmp.end_of_line();
        for (uint32_t i = 0; i < H_BLANK; i++)
        {
            cmp.blank_clock(pix);
        }
        pix.begin_line();
        std::vector<Rgb444> b;
        for (uint32_t i = 0; i < 16; i++)
        {
            b.push_back(cmp.active_slot(pix));
        }

        // d1..d15 alternate C1/C0 through pixel 11, then d12 is passthrough.
        static const Rgb444 want_a[8] = {C0, C1, C0, C1, C0, C1, C0, C1};
        static const Rgb444 want_b[8] = {C0, C1, C0, C1, PT, C0, C1, C1};
        uint32_t bad = 0;
        for (uint32_t i = 0; i < 8; i++)
        {
            if (a[4 + i] != want_a[i])
            {
                bad++;
            }
            if (b[i] != want_b[i])
            {
                bad++;
            }
        }
        for (uint32_t i = 8; i < 16; i++)
        {
            if (b[i] != PT)
            {
                bad++;
            }
        }
        printf("  MASKB_RESUME        pixels 8..15 replay at slots 0..7 of the next line, "
               "%u exceptions\n", bad);
        CHECK(bad == 0, "mask resume: %u pixels of the straddling record are wrong — the "
              "record must FREEZE at the line boundary, not be abandoned", bad);
    }

    // THE THIRD COLOUR — MODEL BEHAVIOUR, NOT YET HARDWARE.  With the PIXELS
    // FIFO never written, PIXEL re-delivers its reset word forever; that word is
    // all zeroes, and a zero bit in 1bpp direct mode selects pix_pal_bg.  So
    // RGB_IN is pal_bg, a dibit-00 pixel resolves to pal_bg, and pal_bg is a
    // register a VIDCMD SET can write — dibit 00 becomes a third settable colour
    // per record with no SET between the groups that use it.
    //
    // The probe: two identical masks whose dibits are all 00, with a
    // SET(pix_pal_bg) between them.  If passthrough were anything fixed the two
    // groups would come out the same colour.
    {
        PixelUnit      pix;
        CompositorUnit cmp;
        pix.reset();
        cmp.reset();
        // Deliberately NO pix.push_word() anywhere: the FIFO is dry, which is
        // exactly the pure-VIDCMD screen's condition.
        cmp.push_word(vidcmd_set(SET_CMP_COLOR0, C0));
        cmp.push_word(vidcmd_set(SET_PIX_MODE, PIXEL_MODE_DIRECT_1BPP));
        cmp.push_word(vidcmd_set(SET_PIX_PAL_BG, 0x123));
        push_mask_into(cmp, trace_mask({T, T, T, T, T, T, T, T, T, T, T, T, T, T, T}));
        cmp.push_word(vidcmd_set(SET_PIX_PAL_BG, 0x456));
        push_mask_into(cmp, trace_mask({T, T, T, T, T, T, T, T, T, T, T, T, T, T, T}));
        for (uint32_t i = 0; i < H_BLANK; i++)
        {
            cmp.blank_clock(pix);
        }
        pix.begin_line();
        std::vector<Rgb444> px;
        for (uint32_t i = 0; i < 64; i++)
        {
            px.push_back(cmp.active_slot(pix));
        }

        // Mask A owns slots 0..15, the SET pays MASK_GAP_AFTER_MASK_SET, mask B
        // starts at 16 + that.
        const uint32_t b0 = MASK_SLOTS + MASK_GAP_AFTER_MASK_SET;
        uint32_t bad = 0;
        for (uint32_t i = 1; i < MASK_SLOTS; i++)
        {
            if (px[i] != 0x123)
            {
                bad++;
            }
            if (px[b0 + i] != 0x456)
            {
                bad++;
            }
        }
        printf("  THIRD COLOUR probe  dibit 00 over a dry PIXELS FIFO: group A 0x%03X, "
               "group B 0x%03X after SET(pal_bg)\n", px[1], px[b0 + 1]);
        printf("                      *** MODEL BEHAVIOUR, NOT YET VERIFIED IN HARDWARE ***\n");
        printf("                      render.cpp follows pixel.v's documented tiling (no\n"
               "                      empty flag, a 7200 holds Q while empty), but what a\n"
               "                      7200 pair presents after /RS with no write at all, and\n"
               "                      what pixel.v's shifter holds before its first fetch,\n"
               "                      are unverified.  Treat dibit 00 as a third colour only\n"
               "                      after checking both against the RTL and the part.\n");
        CHECK(px[0] == C0,
              "third colour: the record's implicit pixel 0 is still opaque color0, got 0x%03X",
              px[0]);
        CHECK(bad == 0,
              "third colour: %u dibit-00 pixels do not follow pix_pal_bg — the model's "
              "passthrough chain has changed", bad);
        CHECK(px[1] != px[b0 + 1],
              "third colour: both groups came out 0x%03X, so passthrough is not settable in "
              "the model after all", px[1]);
    }

    printf("  ------------------------------------------------------------------\n");
    printf("  MEASURED CONSTANTS FOR THE SUITE (MASK)\n");
    printf("    MASK record                          : %u slots per %u-word record\n",
           MASK_SLOTS, MASK_RECORD_WORDS);
    printf("                                           (px0 implicit opaque color0, "
           "%u + %u dibits)\n", MASK_HEADER_DIBITS, MASK_DATA_DIBITS);
    printf("    mid-record data reload at pixel      : %u (HOLD stretch on starvation)\n",
           MASK_RELOAD_PIXEL);
    printf("    RUN -> mask gap                      : %u slot(s)   [tb MASKB_SPRITE]\n",
           mask_run_gap);
    printf("    mask -> mask gap                     : %u slot(s)   [tb MASKB_CHAIN32, "
           "GAPLESS]\n", mask_chain_gap);
    printf("    mask -> SET -> mask gap              : %u slot(s)   [tb MASKB_RECOLOR]\n",
           mask_set_gap);
    printf("    mask -> SET,SET -> mask gap          : %u slot(s)   [DERIVED here, not 3]\n",
           mask_set_set_gap);
    printf("    RUN  -> SET,SET -> mask gap          : %u slot(s)   [DERIVED, banked pair]\n",
           run_set_set_gap);
    printf("    header is playback-class, NOT blank-eager\n");
    printf("  ------------------------------------------------------------------\n");
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
    write_test_pattern_indexed2(ram, FB_IDX2_BASE, FB_IDX2_STRIDE, FB_LINES);
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
    printf("  PIXEL mode register is BITWISE: [0] micro-HAM  [1] 2bpp indexed  [2] half rate "
           "([1:0]==11 reserved, HAM wins)\n");
    printf("  PIXELS words/line   %u 1bpp, %u micro-HAM, %u indexed; at half rate %u / %u "
           "(half rate is masked off in micro-HAM)\n",
           PIXELS_WORDS_1BPP, PIXELS_WORDS_MICROHAM, PIXELS_WORDS_INDEXED2,
           PIXELS_WORDS_HALF_1BPP, PIXELS_WORDS_HALF_INDEXED2);

    // The laws first, directly against the two units, before any list builder
    // gets to interpret them.
    check_pixel_modes();
    check_cadence_traces();
    check_mask_traces();

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

    // --- M8: 2bpp indexed, four colours per line -------------------------------
    uint32_t m8_vidcmd_words = 0;
    {
        CaseSpec s;
        s.name  = "m8-indexed2";
        s.title = "2bpp indexed: 80 words/line, four colours, ham_held as the third entry";
        s.base  = common;
        s.base.mode             = PixelMode::INDEXED_2BPP;
        s.base.fb_base          = FB_IDX2_BASE;
        s.base.fb_stride_bytes  = FB_IDX2_STRIDE;
        s.base.per_line_palette = true;
        s.base.per_line_mode    = true;
        // ONE SET(pix_ham_held) for the whole frame, emitted immediately after
        // the SET(pix_mode) that selects indexed.  Both halves of that matter:
        // the ORDER is the chip's one order dependence, and ONCE PER FRAME is
        // the strongest available assertion that indexed really does lock both
        // writers out of ham_held — in any other mode blanking would have
        // overwritten it with pal_fg long before line 1.
        s.base.frame_ham_held   = true;
        s.base.ham_held_color   = rgb444(15, 12, 0);
        // A mid-line recolour of two of the four indices, placed by the same
        // slot arithmetic m2-split uses.  Codes 10 and 11 must NOT move.
        s.base.mid_line_split   = true;
        s.base.split_pixel      = 320;
        s.base.split_fg         = rgb444(15, 0, 0);
        s.base.split_bg         = rgb444(0, 15, 0);
        CaseResult r = run_case(s, ram, REARM_LATENCY_CYCLES);
        write_artifacts(s, r);
        print_report(s, r);
        check_case(s, r);

        const uint32_t words = pixel_words_for(s.base.mode, 0);
        printf("  stream rate         %u words/line, the same as micro-HAM (%u): both spend "
               "two stream bits per pixel clock\n", words, PIXELS_WORDS_MICROHAM);
        CHECK(words == PIXELS_WORDS_INDEXED2 && words == PIXELS_WORDS_MICROHAM,
              "m8-indexed2: %u words/line, expected %u", words, PIXELS_WORDS_INDEXED2);

        const FrameImage ref = indexed2_reference_frame(s.base);
        const uint32_t differing = compare_frames("indexed2", r, ref);
        CHECK(differing == 0,
              "m8-indexed2: %u pixels differ from the host-rasterized reference — the dibit "
              "index, the third colour or the mid-line SET seam is wrong", differing);

        // All four codes, counted off the rendered frame.
        const FrameImage &f1 = r.rendered.frames[0];
        uint32_t n_black = 0;
        uint32_t n_held  = 0;
        uint32_t n_split_fg = 0;
        uint32_t n_split_bg = 0;
        for (Rgb444 c : f1.pixels)
        {
            n_black    += (c == RGB444_BLACK) ? 1u : 0u;
            n_held     += (c == s.base.ham_held_color) ? 1u : 0u;
            n_split_fg += (c == s.base.split_fg) ? 1u : 0u;
            n_split_bg += (c == s.base.split_bg) ? 1u : 0u;
        }
        // Each of the sixteen 40-pixel blocks carries one code and the phase
        // walks, so every code takes exactly a quarter of the frame.
        const uint32_t want_quarter = (H_ACTIVE / 4u) * V_ACTIVE;
        printf("  four codes          00/01 recoloured mid-line, 10 ham_held %u px, "
               "11 black %u px (want %u each)\n", n_held, n_black, want_quarter);
        CHECK(n_black == want_quarter,
              "m8-indexed2: code 11 painted %u pixels, expected %u — black is a constant, "
              "not a register", n_black, want_quarter);
        CHECK(n_held == want_quarter,
              "m8-indexed2: code 10 painted %u pixels of 0x%03X, expected %u",
              n_held, s.base.ham_held_color, want_quarter);
        CHECK(n_split_fg > 0 && n_split_bg > 0,
              "m8-indexed2: the mid-line recolour painted %u fg / %u bg pixels — the SET "
              "pair did not land", n_split_fg, n_split_bg);

        // The lockout, in one number: the ham_held SET happened on line 0 and
        // its colour still paints line 479.
        uint32_t held_last_line = 0;
        for (uint32_t x = 0; x < H_ACTIVE; x++)
        {
            if (at(f1, x, V_ACTIVE - 1) == s.base.ham_held_color)
            {
                held_last_line++;
            }
        }
        printf("  ham_held lockout    SET once on line 0; line %u still paints it on %u "
               "pixels (blanking reload suppressed by the mode bit)\n",
               V_ACTIVE - 1, held_last_line);
        CHECK(held_last_line > 0,
              "m8-indexed2: line %u shows no ham_held pixels, so the per-frame SET did not "
              "survive %u blanking intervals", V_ACTIVE - 1, V_ACTIVE - 1);

        // The mid-line seam, pixel by pixel on a line whose codes make it
        // legible: pick the first line whose block at x=320 is code 00.
        {
            uint32_t probe = 0;
            while (probe < V_ACTIVE && idx2_pattern_code(320, probe) != IDX2_CODE_PAL_BG)
            {
                probe++;
            }
            printf("  split seam          line %u: x=319 0x%03X x=320 0x%03X (want split bg "
                   "0x%03X), x=321.. follows fg 0x%03X\n",
                   probe, at(f1, 319, probe), at(f1, 320, probe), s.base.split_bg,
                   s.base.split_fg);
            CHECK(at(f1, 320, probe) == s.base.split_bg,
                  "m8-indexed2: pixel 320 of line %u is 0x%03X, expected the split bg "
                  "0x%03X — SET(pix_pal_bg) did not land on its own slot",
                  probe, at(f1, 320, probe), s.base.split_bg);
        }

        // Per-line delivery budget: the pixel stream AND the line's records, in
        // words, against the same 66%-of-the-bus cap the screen cases use.
        m8_vidcmd_words = r.vidcmd_words_max;
        const uint32_t line_total = words + r.vidcmd_words_max;
        printf("  line word budget    %u PIXELS + %u VIDCMD = %u words/line against the "
               "%u-word cap (%u%% of the %llu-SYSCLK line)\n",
               words, r.vidcmd_words_max, line_total, VIDCMD_WORDS_PER_LINE_CAP,
               VIDCMD_BUS_BUDGET_PERCENT, static_cast<unsigned long long>(LINE_SYSCLK));
        CHECK(line_total <= VIDCMD_WORDS_PER_LINE_CAP,
              "m8-indexed2: %u words/line (%u pixel + %u VIDCMD) is over the %u-word cap",
              line_total, words, r.vidcmd_words_max, VIDCMD_WORDS_PER_LINE_CAP);
    }

    // --- M9: half rate, with MASK sprites at full pixel resolution -------------
    {
        CaseSpec s;
        s.name  = "m9-halfrate";
        s.title = "half rate: a 320-wide playfield under 640-wide MASK sprites";
        s.base  = common;
        s.base.mode             = PixelMode::HALF_1BPP;
        s.base.per_line_palette = true;
        s.base.per_line_mode    = true;
        s.base.sprites          = SpriteStyle::MASK_SPRITES;
        s.base.held_fg          = rgb444(15, 15, 0);
        s.base.held_bg          = rgb444(2, 0, 8);
        CaseResult r = run_case(s, ram, REARM_LATENCY_CYCLES);
        write_artifacts(s, r);
        print_report(s, r);
        check_case(s, r);

        const uint32_t m9_half_1bpp_words = pixel_words_for(s.base.mode, 0);
        printf("  stream cost         %u words/line, half of 1bpp's %u — 320 groups across "
               "the same 640-clock window\n", m9_half_1bpp_words, PIXELS_WORDS_1BPP);
        CHECK(m9_half_1bpp_words == PIXELS_WORDS_HALF_1BPP,
              "m9-halfrate: half-rate 1bpp is %u words/line, expected %u",
              m9_half_1bpp_words, PIXELS_WORDS_HALF_1BPP);

        const FrameImage ref = halfrate_reference_frame(s.base);
        const uint32_t differing = compare_frames("halfrate", r, ref);
        CHECK(differing == 0,
              "m9-halfrate: %u pixels differ from the host-rasterized reference — the group "
              "pairing or a mask dibit is landing on the wrong pixel", differing);

        // THE GROUP LAW, measured off the frame rather than asserted from the
        // authoring: outside the sprite cells every 2k / 2k+1 pair must be one
        // colour, because one consumed group is held for exactly two pixel
        // clocks.  A group held for one clock, three, or with the pair phase
        // inverted breaks this immediately.
        const FrameImage &f1 = r.rendered.frames[0];
        uint32_t pairs_checked = 0;
        uint32_t pairs_split   = 0;
        uint32_t mask_splits   = 0;
        for (uint32_t y = 0; y < V_ACTIVE; y++)
        {
            for (uint32_t k = 0; k < H_ACTIVE / 2; k++)
            {
                const uint32_t x = 2 * k;
                const bool differs = at(f1, x, y) != at(f1, x + 1, y);
                if (in_mask_sprite(x) || in_mask_sprite(x + 1))
                {
                    mask_splits += differs ? 1u : 0u;
                    continue;
                }
                pairs_checked++;
                pairs_split += differs ? 1u : 0u;
            }
        }
        printf("  group law           %u playfield groups checked, %u span less than 2 "
               "slots\n", pairs_checked, pairs_split);
        CHECK(pairs_split == 0,
              "m9-halfrate: %u of %u playfield groups do not span exactly 2 pixel clocks",
              pairs_split, pairs_checked);

        // AND THE INTEROP STATEMENT, as its own number: inside the sprite cells
        // adjacent pixels DO differ, because a MASK steps one dibit per
        // pixel-clock slot and COMPOSITOR never learns what rate PIXEL is
        // running at.  Half rate is a stream-side gate inside PIXEL alone.
        printf("  MASK interop        %u adjacent pairs inside the sprite cells differ: a "
               "mask keeps full %u-pixel resolution over a %u-wide playfield\n",
               mask_splits, H_ACTIVE, H_ACTIVE / 2);
        CHECK(mask_splits > 0,
              "m9-halfrate: no adjacent pair inside a sprite cell differs, so the MASK "
              "records are being quantized to the playfield's groups");
        CHECK(r.rendered.stats.vidcmd_mask_records / RENDER_FRAMES ==
                  MASK_SPRITE_COUNT * V_ACTIVE,
              "m9-halfrate: %u MASK records per frame, expected %u",
              r.rendered.stats.vidcmd_mask_records / RENDER_FRAMES,
              MASK_SPRITE_COUNT * V_ACTIVE);

        // The same case at two bits per clock.  Same 320 groups, same sprites,
        // twice the stream — and the group law has to hold identically, which
        // is what says half rate is one gate and not a per-mode special case.
        CaseSpec t = s;
        t.name  = "m9-halfrate-idx2";
        t.title = "half rate at 2bpp indexed: 40 words/line, four colours, same sprites";
        t.base.mode           = PixelMode::HALF_INDEXED_2BPP;
        t.base.fb_base        = FB_IDX2_BASE;
        t.base.fb_stride_bytes = FB_IDX2_STRIDE;
        t.base.frame_ham_held = true;
        t.base.ham_held_color = rgb444(0, 15, 12);
        CaseResult tr = run_case(t, ram, REARM_LATENCY_CYCLES);
        write_artifacts(t, tr);
        print_report(t, tr);
        check_case(t, tr);

        const uint32_t m9_half_idx2_words = pixel_words_for(t.base.mode, 0);
        const FrameImage &g1 = tr.rendered.frames[0];
        uint32_t idx_pairs_split = 0;
        uint32_t idx_colours     = 0;
        for (uint32_t y = 0; y < V_ACTIVE; y++)
        {
            for (uint32_t k = 0; k < H_ACTIVE / 2; k++)
            {
                const uint32_t x = 2 * k;
                if (in_mask_sprite(x) || in_mask_sprite(x + 1))
                {
                    continue;
                }
                if (at(g1, x, y) != at(g1, x + 1, y))
                {
                    idx_pairs_split++;
                }
                // The playfield dibit for group k is the FULL-RATE pattern's
                // dibit k, because a half-rate line consumes the first 320 of
                // the buffer's 640.
                if (at(g1, x, y) == t.base.ham_held_color ||
                    at(g1, x, y) == RGB444_BLACK)
                {
                    idx_colours++;
                }
            }
        }
        printf("  half+indexed        %u words/line (half of %u), %u split groups, "
               "%u groups painted by codes 10/11\n",
               m9_half_idx2_words, PIXELS_WORDS_INDEXED2, idx_pairs_split, idx_colours);
        CHECK(m9_half_idx2_words == PIXELS_WORDS_HALF_INDEXED2,
              "m9-halfrate-idx2: %u words/line, expected %u",
              m9_half_idx2_words, PIXELS_WORDS_HALF_INDEXED2);
        CHECK(idx_pairs_split == 0,
              "m9-halfrate-idx2: %u playfield groups do not span exactly 2 pixel clocks",
              idx_pairs_split);
        CHECK(idx_colours > 0,
              "m9-halfrate-idx2: codes 10 and 11 painted nothing, so the indexed decode is "
              "not running at half rate");

        // Half rate is masked off in micro-HAM, in the helper as in the chip:
        // a HAM code can span two consumption clocks and ham_second is not
        // phase-gated, so a half-rate HAM line would mis-pair every 4-bit code.
        // One literal on half_rate turns that into "the flag does nothing".
        const uint32_t ham_half =
            pixels_words_per_line(PIXEL_MODE_MICRO_HAM | PIXEL_MODE_HALF_RATE, 0);
        printf("  HAM masking         mode HAM|HALF is %u words/line, unchanged from HAM's "
               "%u — the flag does nothing there\n", ham_half, PIXELS_WORDS_MICROHAM);
        CHECK(ham_half == PIXELS_WORDS_MICROHAM,
              "m9-halfrate: half rate shortened a micro-HAM line to %u words; it is masked "
              "off in HAM by construction", ham_half);
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

    // --- M6: the console screen, entirely out of MASK records ------------------
    {
        CaseSpec s;
        s.name  = "m6-console";
        s.title = "80x60 text screen as MASK groups: pure VIDCMD, no PIXELS descriptors";
        s.base  = common;
        s.base.screen      = ScreenStyle::CONSOLE;
        s.base.pure_vidcmd = true;
        CaseResult r = run_case(s, ram, REARM_LATENCY_CYCLES);
        write_artifacts(s, r);
        print_report(s, r);
        check_case(s, r);

        const uint32_t px0_bad = console_pixel0_violations();
        printf("  layout invariant    %u of %u groups want ink on a record's implicit pixel 0 "
               "(the 8-px cell's blank column 0 is what keeps it 0)\n",
               px0_bad, (H_ACTIVE / MASK_SLOTS) * V_ACTIVE);
        CHECK(px0_bad == 0,
              "m6-console: %u mask groups want ink on pixel 0, which the record cannot "
              "express — the cell layout is wrong", px0_bad);

        const FrameImage ref = console_reference_frame();
        const uint32_t differing = compare_frames("console", r, ref);
        CHECK(differing == 0,
              "m6-console: %u pixels differ from the host-rasterized reference — the MASK "
              "records do not reproduce the text screen", differing);

        // Per-line delivery budget.  A full-width line is every group a record
        // plus the two per-line SETs; the cap is the 66% bus figure the rest of
        // the suite sits at (descriptor.h).
        uint32_t worst   = 0;
        uint32_t typical = 0;
        uint32_t blank_lines = 0;
        std::array<uint32_t, 256> hist{};
        for (uint8_t n : r.line_words)
        {
            hist[n]++;
            if (n > worst)
            {
                worst = n;
            }
            if (n <= 3)
            {
                blank_lines++;
            }
        }
        for (uint32_t n = 0; n < hist.size(); n++)
        {
            if (hist[n] > hist[typical])
            {
                typical = n;
            }
        }
        printf("  console budget      worst line %u words, most common %u words (%u lines), "
               "%u all-background lines\n", worst, typical, hist[typical], blank_lines);
        printf("                      full-width ceiling %u words (%u groups x %u + 2 SETs) "
               "vs the %u-word cap\n",
               CONSOLE_FULL_LINE_WORDS, H_ACTIVE / MASK_SLOTS, MASK_RECORD_WORDS,
               VIDCMD_WORDS_PER_LINE_CAP);
        printf("                      %u SYSCLK/line worst against the %llu-SYSCLK line "
               "(%u%% budget = %u SYSCLK)\n",
               r.budget.max_busy, static_cast<unsigned long long>(LINE_SYSCLK),
               VIDCMD_BUS_BUDGET_PERCENT, VIDCMD_LINE_SYSCLK_BUDGET);
        CHECK(worst <= VIDCMD_WORDS_PER_LINE_CAP,
              "m6-console: worst line is %u VIDCMD words, over the %u-word delivery cap",
              worst, VIDCMD_WORDS_PER_LINE_CAP);
        CHECK(CONSOLE_FULL_LINE_WORDS <= VIDCMD_WORDS_PER_LINE_CAP,
              "m6-console: even a full-width console line (%u words) would not fit the "
              "%u-word cap, so the screen mode is not deliverable",
              CONSOLE_FULL_LINE_WORDS, VIDCMD_WORDS_PER_LINE_CAP);
        CHECK(blank_lines > 0,
              "m6-console: no line collapsed its blank groups into RUNs, so the RUN-gap half "
              "of the encoding is not being exercised");

        // The third colour, in situ: the title row, the status row and every
        // row's "NN>" prefix are dibit 00.
        uint32_t alt_px = 0;
        for (Rgb444 c : r.rendered.frames[0].pixels)
        {
            if (c == CONSOLE_ALT_COLOR)
            {
                alt_px++;
            }
        }
        printf("  third colour in use %u pixels are dibit-00 passthrough at 0x%03X "
               "(pix_pal_bg), painted with no SET between the groups that carry them\n",
               alt_px, CONSOLE_ALT_COLOR);
        printf("                      *** MODEL BEHAVIOUR pending verification against "
               "pixel.v — see the THIRD COLOUR probe above ***\n");
        CHECK(alt_px > 0,
              "m6-console: no passthrough pixels rendered, so the third colour is not being "
              "exercised");

        // The group is bigger than HBLANK and that is fine: an 82-word deposit
        // is 164 SYSCLK against an 89-SYSCLK HBLANK, so most lines' VIDCMD words
        // land during the previous line's ACTIVE video.  Under CUSHION that is
        // legal by construction — what matters is that the words are in the FIFO
        // before the line they belong to starts, and the compositor eats a
        // mask's two words over sixteen slots (one word per ~4.4 SYSCLK) against
        // an engine that delivers one per 2.  Both dry counters are what say so.
        printf("  delivery            %u words spill past HBLANK into the previous line's "
               "active video; FIFO high %u, dry 0, cadence holds %u\n",
               r.rendered.stats.vidcmd_late_words / RENDER_FRAMES,
               r.rendered.stats.vidcmd_fifo_high, r.rendered.stats.vidcmd_cadence_slots);
        CHECK(r.rendered.stats.vidcmd_hold_slots == 0,
              "m6-console: the compositor ran dry for %u slots — the spill is not covered by "
              "the cushion", r.rendered.stats.vidcmd_hold_slots);

        printf("  headline            %u MASK records per frame paint an 80x60 text screen "
               "with ZERO PIXELS words;\n"
               "                      the display list IS the framebuffer, and a text cell "
               "costs one word.\n",
               r.rendered.stats.vidcmd_mask_records / RENDER_FRAMES);
    }

    // --- M7: the kiosk menu ----------------------------------------------------
    {
        CaseSpec s;
        s.name  = "m7-kiosk";
        s.title = "large-text menu: one MASK record per big glyph, SET pairs, RUN_COLOR bar";
        s.base  = common;
        s.base.screen      = ScreenStyle::KIOSK;
        s.base.pure_vidcmd = true;
        CaseResult r = run_case(s, ram, REARM_LATENCY_CYCLES);
        write_artifacts(s, r);
        print_report(s, r);
        check_case(s, r);

        const uint32_t px0_bad = kiosk_pixel0_violations();
        printf("  layout invariant    %u mask groups want ink on their implicit pixel 0 "
               "(the x%u font is 15 px in a 16-px cell, so column 0 is always blank)\n",
               px0_bad, KIOSK_SCALE_X);
        CHECK(px0_bad == 0,
              "m7-kiosk: %u mask groups want ink on pixel 0 — a big font wider than %u px "
              "would need two records per glyph and force its middle column to color0",
              px0_bad, MASK_SLOTS - 1);

        const FrameImage ref = kiosk_reference_frame();
        const uint32_t differing = compare_frames("kiosk", r, ref);
        CHECK(differing == 0,
              "m7-kiosk: %u pixels differ from the host-rasterized reference — the mid-item "
              "SET seam or a mask group is landing on the wrong pixel", differing);

        // THE [SET,SET,MASK] SEAM, measured off the rendered frame rather than
        // asserted from the authoring.  Item 0 is "PLAY" then a recolour then
        // "DEMO"; the recolour sits BETWEEN two mask groups, so by the derived
        // law it costs MASK_GAP_AFTER_MASK_SET_SET slots and "DEMO" starts that
        // many pixels later.  "PLAY"'s last record ends at 192; "DEMO"'s first
        // record is a 16-pixel cell whose column 0 is blank, so the first ink
        // pixel after the seam is (seam start + gap + 1).
        {
            const KioskLinePlan plan = kiosk_plan_line(145);
            const uint32_t after_play = plan.seg[0].x0 + plan.seg[0].records * KIOSK_CELL_W;
            const Rgb444   bg = run_colour_to_rgb444(plan.background_color);
            uint32_t first_ink = after_play;
            while (first_ink < H_ACTIVE &&
                   at(r.rendered.frames[0], first_ink, 145) == bg)
            {
                first_ink++;
            }
            const uint32_t seam = (first_ink - 1) - after_play;
            printf("  SET,SET seam        \"PLAY\" ends at x=%u, \"DEMO\"'s cell starts at "
                   "x=%u: seam %u slot(s)\n", after_play, first_ink - 1, seam);
            printf("                      derived: the mask holds staged_word, so only ONE "
                   "word parks on Q and the two\n"
                   "                      SETs cannot pair — each costs its own slot plus a "
                   "cadence HOLD, %u + %u = %u.\n",
                   MASK_GAP_AFTER_MASK_SET, MASK_GAP_AFTER_MASK_SET,
                   MASK_GAP_AFTER_MASK_SET_SET);
            CHECK(seam == MASK_GAP_AFTER_MASK_SET_SET,
                  "m7-kiosk: the [SET,SET,MASK] group seam renders as %u slot(s), the laws "
                  "give %u", seam, MASK_GAP_AFTER_MASK_SET_SET);
        }

        uint32_t worst = 0;
        for (uint8_t n : r.line_words)
        {
            if (n > worst)
            {
                worst = n;
            }
        }
        // The reported stretch is the SET seams, not starvation: the authored
        // slot SUM does not include the cadence's HOLD slots, so a line with a
        // recolour reads as stretched even though it frames exactly 640 and the
        // FIFO never runs dry.  That is what the two counters below separate.
        printf("  kiosk cadence       stretch max %u slot(s) on %u lines = the SET seams, "
               "not starvation (dry slots %u, occupancy %u..%u)\n",
               r.vidcmd_stretch_max, r.vidcmd_stretch_lines / RENDER_FRAMES,
               r.rendered.stats.vidcmd_hold_slots, r.vidcmd_slot_min, r.vidcmd_slot_max);
        CHECK(r.vidcmd_stretch_max <= MASK_GAP_AFTER_MASK_SET_SET - 1,
              "m7-kiosk: a line stretched %u slots, more than the %u the mid-item SET seam "
              "accounts for", r.vidcmd_stretch_max, MASK_GAP_AFTER_MASK_SET_SET - 1);
        printf("  kiosk budget        worst line %u words; sparse recolouring (one SET pair "
               "per menu item, not per group) FITS the %u-word cap\n",
               worst, VIDCMD_WORDS_PER_LINE_CAP);
        printf("                      full-density recolouring — a SET pair in front of "
               "EVERY 16-pixel group — costs\n"
               "                      %u groups x (2 SETs + %u mask words) = %u words/line, "
               "which BLOWS the %u-word cap;\n"
               "                      and it does not even fit the LINE: %u x (%u + %u) = %u "
               "slots against %u available.\n",
               H_ACTIVE / MASK_SLOTS, MASK_RECORD_WORDS, KIOSK_FULL_DENSITY_WORDS,
               VIDCMD_WORDS_PER_LINE_CAP, H_ACTIVE / MASK_SLOTS, MASK_SLOTS,
               MASK_GAP_AFTER_MASK_SET_SET, KIOSK_FULL_DENSITY_SLOTS, H_ACTIVE);
        CHECK(worst <= VIDCMD_WORDS_PER_LINE_CAP,
              "m7-kiosk: worst line is %u VIDCMD words, over the %u-word delivery cap",
              worst, VIDCMD_WORDS_PER_LINE_CAP);
        CHECK(KIOSK_FULL_DENSITY_WORDS > VIDCMD_WORDS_PER_LINE_CAP,
              "m7-kiosk: full-density recolouring (%u words) now fits the %u-word cap, so "
              "the budget arithmetic this case prints needs re-deriving",
              KIOSK_FULL_DENSITY_WORDS, VIDCMD_WORDS_PER_LINE_CAP);
        CHECK(KIOSK_FULL_DENSITY_SLOTS > H_ACTIVE,
              "m7-kiosk: full-density recolouring now fits in %u slots", H_ACTIVE);

        // The RUN_COLOR highlight bar, and the mask backgrounds that have to
        // match it: inside a mask, dibit 00 is PASSTHROUGH, not "the span
        // underneath", so the bar's colour has to be re-stated as cmp_color0.
        const Rgb444 bar = run_colour_to_rgb444(RUN_COLOR_CYAN);
        uint32_t bar_px = 0;
        for (Rgb444 c : r.rendered.frames[0].pixels)
        {
            if (c == bar)
            {
                bar_px++;
            }
        }
        printf("  highlight bar       %u pixels at 0x%03X: a RUN_COLOR across the line plus "
               "the selected item's cmp_color0,\n"
               "                      which must be stated separately because a mask's "
               "dibit 00 is passthrough, not the span under it.\n", bar_px, bar);
        CHECK(bar_px > H_ACTIVE, "m7-kiosk: the highlight bar painted only %u pixels", bar_px);

        uint32_t alt_px = 0;
        for (Rgb444 c : r.rendered.frames[0].pixels)
        {
            if (c == KIOSK_ALT_COLOR)
            {
                alt_px++;
            }
        }
        printf("  third colour in use %u hotkey pixels at 0x%03X are dibit-00 passthrough, "
               "a third colour inside a group with no SET\n", alt_px, KIOSK_ALT_COLOR);
        printf("                      *** MODEL BEHAVIOUR pending verification against "
               "pixel.v — see the THIRD COLOUR probe above ***\n");
        CHECK(alt_px > 0, "m7-kiosk: no passthrough hotkey pixels rendered");
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
           "     actually for.  ANSWERED 2026-08-24 by MASK — see finding 20.\n",
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

    printf(" 20. NEW 2026-08-24, AND IT IS THE ANSWER TO FINDING 16. The `01` prefix carries\n"
           "     MASK: two words, SIXTEEN pixels, no inline colour, no new shifters (it runs\n"
           "     through staged_word and the shared playback counter).  That is a flat %u\n"
           "     pixels per word for art of ANY hole density, against RUN spans' one word per\n"
           "     contiguous segment — so the rule of thumb inverts above about two holes per\n"
           "     16 pixels, and the cursor row that regressed to five records is one MASK.\n"
           "     What it costs instead is RECOLOURING.  The header spends all fourteen\n"
           "     payload bits on dibits, so changing colour is an ordinary SET, and the seams\n"
           "     are %u slots between two mask groups (the mask holds staged_word, so the two\n"
           "     SETs cannot be a banked pair) against %u behind a RUN.  Chaining itself is\n"
           "     FREE: mask-to-mask and RUN-to-mask are both ZERO slots, because the\n"
           "     pixel-15 edge captures the next record one slot early.\n",
           MASK_SLOTS / MASK_RECORD_WORDS, MASK_GAP_AFTER_MASK_SET_SET,
           MASK_GAP_AFTER_RUN_SET_SET);
    printf(" 21. NEW 2026-08-24, AND IT IS A NEW SCREEN MODE. A display list can author NO\n"
           "     PIXELS DESCRIPTORS AT ALL and still paint a full screen: the display list IS\n"
           "     the framebuffer.  m6-console draws an 80x60 text screen at %u words on its\n"
           "     widest line (%u groups x %u + 2 per-line SETs) against the %u-word delivery\n"
           "     cap, pixel-exact against a host rasterization of the same font; m7-kiosk\n"
           "     draws large text one glyph per record.  The two geometries both come out of\n"
           "     the implicit pixel 0: whatever lands on a record boundary is cmp_color0, so\n"
           "     an 8-px cell with a blank column 0 fits TWO glyphs in a record and a x3 font\n"
           "     in a 16-px cell fits ONE.  What runs out is recolouring, not records: forty\n"
           "     chained masks are exactly %u slots, but a SET pair in front of every group is\n"
           "     %u words/line (over the cap) and %u slots (over the LINE) — impossible rather\n"
           "     than merely late.\n",
           CONSOLE_FULL_LINE_WORDS, H_ACTIVE / MASK_SLOTS, MASK_RECORD_WORDS,
           VIDCMD_WORDS_PER_LINE_CAP, H_ACTIVE, KIOSK_FULL_DENSITY_WORDS,
           KIOSK_FULL_DENSITY_SLOTS);
    printf(" 22. NEW 2026-08-24, AND EXPLICITLY UNVERIFIED. With the PIXELS FIFO never\n"
           "     written, the modelled PIXEL re-shifts its reset word — all zeroes, which in\n"
           "     1bpp direct mode selects pix_pal_bg — so a passthrough dibit resolves to\n"
           "     pal_bg, a register a VIDCMD SET can write.  Dibit 00 is then a THIRD\n"
           "     SETTABLE COLOUR inside a record, with no SET between the groups that use it,\n"
           "     and both screen cases spend it (console prefixes and status line, kiosk\n"
           "     hotkey letters).  THIS IS WHAT THE MODEL DOES, NOT WHAT THE CHIP IS KNOWN TO\n"
           "     DO.  render.cpp follows pixel.v's documented tiling (no empty flag, a 7200\n"
           "     holds Q while empty), but what a 7200 pair presents on Q after /RS with no\n"
           "     write at all, and what pixel.v's shifter holds before its first fetch, are\n"
           "     both open.  Settle them against the RTL and the datasheet before any art\n"
           "     depends on the third colour.\n");

    printf(" 23. NEW 2026-08-26, AND THE MODE REGISTER IS NOT AN ENUMERATION. pixel.v decodes\n"
           "     SET(pix_mode) BITWISE — [0] micro-HAM, [1] 2bpp indexed, [2] half rate — with\n"
           "     [1:0] == 11 reserved and micro-HAM winning it BY CONSTRUCTION rather than by\n"
           "     decode accident.  Everything downstream of that is derived, not tabulated:\n"
           "     bits per clock is \"either two-bit mode\", words per line is groups x bits, and\n"
           "     the odd-skip clamp belongs to BOTH two-bit modes.  2bpp indexed costs the same\n"
           "     %u words/line as micro-HAM and no new SET targets at all — 00 pal_bg, 01\n"
           "     pal_fg, 10 ham_held, 11 a constant black — so a display list recolours four\n"
           "     indices per line with SETs it already emits.  m8-indexed2 renders it\n"
           "     pixel-exact against a host rasterization at %u PIXELS + %u VIDCMD words a\n"
           "     line, against the %u-word cap.\n",
           PIXELS_WORDS_INDEXED2, PIXELS_WORDS_INDEXED2, m8_vidcmd_words,
           VIDCMD_WORDS_PER_LINE_CAP);
    printf(" 24. NEW 2026-08-26, AND IT IS THE ONE ORDER DEPENDENCE IN THE CHIP. Indexed mode\n"
           "     has to show what SET *put* in ham_held, which cannot survive a decoder that\n"
           "     overwrites it — so pixel.v locks the stream out of ham_held there AND drops\n"
           "     the blanking reload (held_init = ~pix_consume & ~mode_idx2).  Consequence: a\n"
           "     list that SETs ham_held and THEN SETs mode loses the colour to pal_fg on the\n"
           "     blank clocks in between.  SET MODE FIRST.  render.cpp models the reload clock\n"
           "     by clock (PixelUnit::blank_clock) so this is a failing check rather than a\n"
           "     comment: the IDX2_SET_ORDER trace proves the wrong order loses the colour,\n"
           "     and m8-indexed2 SETs ham_held ONCE PER FRAME and still paints it on line %u.\n",
           V_ACTIVE - 1);
    printf(" 25. NEW 2026-08-26, AND HALF RATE COSTS NOTHING OUTSIDE PIXEL. mode[2] holds every\n"
           "     consumed group for two pixel clocks: %u groups across the same 640-clock\n"
           "     window, so a line costs half the stream — %u words 1bpp, %u indexed — at half\n"
           "     the horizontal resolution.  It is a STREAM-SIDE GATE and nothing else in the\n"
           "     pipeline learns about it: RGB_OUT's window, PIXEL_OUT_LEAD, the SET path and\n"
           "     COMPOSITOR are untouched.  The interop that matters for a game mode is that\n"
           "     MASK DIBITS STEP ONCE PER PIXEL-CLOCK SLOT REGARDLESS, so sprites keep full\n"
           "     %u-pixel horizontal resolution over a %u-wide playfield — m9-halfrate measures\n"
           "     both halves at once: zero playfield groups spanning other than 2 slots, and\n"
           "     adjacent pixels inside the sprite cells that DO differ.  Half rate is masked\n"
           "     off in micro-HAM in hardware, not merely declared undefined, because a HAM\n"
           "     code can span two consumption clocks and ham_second is not phase-gated: one\n"
           "     literal turns a whole garbage line into \"the flag does nothing\".\n",
           H_ACTIVE / 2, PIXELS_WORDS_HALF_1BPP, PIXELS_WORDS_HALF_INDEXED2,
           H_ACTIVE, H_ACTIVE / 2);

    printf("\n=== summary ===\n");
    if (g_failures == 0)
    {
        printf("  all cases PASS\n");
        return 0;
    }
    printf("  %d check(s) FAILED\n", g_failures);
    return 1;
}
