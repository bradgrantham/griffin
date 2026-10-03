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

#include "../apps/lib/griffin_cursor.h"

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

// The two entries a plain 1bpp playfield selects at pix_base 0.  Distinct from
// every held colour the sprite cases use, so a visibility count can tell
// composited pixels from playfield ones.
inline constexpr Rgb444 PLAYFIELD_BG = rgb444(0, 0, 5);
inline constexpr Rgb444 PLAYFIELD_FG = rgb444(10, 10, 2);

// The sixteen-entry palette the deeper modes load, once per frame.  Distinct,
// non-black and nowhere near the compositor's held colours, so a pixel's colour
// names its entry and a missing load (the RAM powers up random) is obvious.
inline constexpr std::array<Rgb444, PALETTE_ENTRIES> TEST_PALETTE = {
    rgb444(1, 1, 1),    rgb444(15, 0, 0),   rgb444(0, 15, 0),   rgb444(0, 0, 15),
    rgb444(15, 15, 0),  rgb444(15, 0, 15),  rgb444(0, 15, 15),  rgb444(9, 9, 9),
    rgb444(7, 3, 0),    rgb444(3, 7, 0),    rgb444(0, 3, 7),    rgb444(7, 0, 3),
    rgb444(12, 6, 3),   rgb444(3, 12, 6),   rgb444(6, 3, 12),   rgb444(14, 14, 10),
};

constexpr uint32_t FB_SOLID_BASE      = 0x010000;   // every bit set, for the slot regression
constexpr uint32_t FB_WEB_BASE        = 0x040000;   // the Tempest wireframe, drawn once

// The three deeper planes, all 80 words a line and all given the same 88-word
// stride with scroll headroom.  ONE buffer serves both rates in each: a
// HALF-RATE line consumes the first half of the words, so the half-rate
// expectation is the full-rate one indexed by k/2.
constexpr uint32_t FB_IDX2_BASE       = 0x080000;   // 2bpp, 80 words
constexpr uint32_t FB_IDX2_STRIDE     = 176;
constexpr uint32_t FB_IDX4_BASE       = 0x020000;   // 4bpp half rate, 80 words
constexpr uint32_t FB_IDX4_STRIDE     = 176;
constexpr uint32_t FB_ATTR_BASE       = 0x0A0000;   // attribute cells, 80 / 40 words
constexpr uint32_t FB_ATTR_STRIDE     = 176;

// The VIDCMD records are per-frame data — they carry this frame's palette
// ramp, its pixel_skip and its tile masks — so they need double buffering
// exactly as much as the descriptor table does.  Only the table has to live in
// the top 64K; the VIDCMD regions are ordinary RAM, which makes it easy to
// forget that the CPU is authoring frame N+1's records while the engine is
// still reading frame N's.
// m11-static-pieces: the read-only fragments a sprite line is assembled from,
// and the scratch area the monolithic form copies them into.  Both are written
// once, before any frame, which is the whole claim of the pieces form.
constexpr uint32_t SP_DATA_BASE         = 0x200000;
constexpr uint32_t SP_PROGRAM_BASE      = 0x210000;

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

// The audio FIFOs start empty, so the very first list has to prime them as
// well as carry vertical blanking's share.  A real driver does this once at
// startup (audio/scheduler.h keeps a margin instead); here it is one bigger
// preamble on list A.  Without it the steady-state low water is 1 sample.
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
    printf("  palette             writes %u  bad fillers %u  disturbed-on-DAC %u  "
           "unloaded reads %u\n",
           res.rendered.stats.palette_writes, res.rendered.stats.palette_bad_filler,
           res.rendered.stats.palette_disturbed_visible,
           res.rendered.stats.palette_unwritten_reads);
    printf("  VIDCMD FIFO         high %3u  low %3u  popped %u  overflows %u\n",
           rr.stats.vidcmd_fifo_high, rr.stats.vidcmd_fifo_low, rr.stats.vidcmd_words_popped,
           rr.stats.vidcmd_overflows);
    printf("  VIDCMD holds        cadence %u  starved %u   (cadence = the 2-clock fetch, "
           "starved = a dry FIFO)\n",
           rr.stats.vidcmd_cadence_slots, rr.stats.vidcmd_hold_slots);
    printf("  VIDCMD framing      overruns %u  late words %u  LITERAL %u  MASK %u "
           "(mask stalls %u)  stream leftovers %u\n",
           rr.stats.vidcmd_overruns, rr.stats.vidcmd_late_words, rr.stats.vidcmd_literals,
           rr.stats.vidcmd_mask_records, rr.stats.vidcmd_mask_stalls,
           rr.stats.vidcmd_stream_leftovers);
    if (spec.base.audio)
    {
        printf("  AUDIO FIFO (ch 0)   high %4u low %4u underruns %u  overflows %u  (depth %u)\n",
               rr.audio_fifo_high, rr.audio_fifo_low, rr.audio_underruns,
               rr.audio_overflows, AUDIO_FIFO_SAMPLES);
        printf("  AUDIO samples/ch    %u deposited inside the %u rendered frames, %u consumed\n",
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

    // THE PALETTE, and all three of these are list-builder bugs rather than
    // model assertions: nothing in hardware clears the RAM, the filler is not
    // optional, and a write steals four pixel slots (vidcmd-spec.md §3.2).
    CHECK(rr.stats.palette_unwritten_reads == 0,
          "%s: %u pixels were painted out of palette entries the list never loaded — the "
          "RAM powers up UNDEFINED", spec.name.c_str(), rr.stats.palette_unwritten_reads);
    CHECK(rr.stats.palette_bad_filler == 0,
          "%s: %u palette writes stored the wrong colour — a three-word entry's filler is "
          "missing or carries a different value", spec.name.c_str(),
          rr.stats.palette_bad_filler);
    CHECK(rr.stats.palette_disturbed_visible == 0,
          "%s: %u palette-write slots reached the DAC through passthrough — the four slots "
          "after a clut_data SET must be in blanking or covered", spec.name.c_str(),
          rr.stats.palette_disturbed_visible);
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
// Drive one palette entry into PIXEL the way the three-word VIDCMD entry does,
// but directly — no COMPOSITOR here, so the bus is held by hand.  The clock
// count is the measured sequence: the data SET's commit, then the three-clock
// address window with the pulse in the middle, then the recovery guard.
void palette_load_direct(PixelUnit &pix, uint32_t index, Rgb444 colour)
{
    pix.set_register(SET_PIX_CLUT_INDEX, index);
    pix.set_value_bus(colour);
    pix.blank_clock();

    pix.set_register(SET_PIX_CLUT_DATA, colour);   // clock 0: the commit
    pix.set_value_bus(colour);
    pix.blank_clock();                            // clock 1: setup
    pix.set_value_bus(colour);                    // the filler holds the bus
    pix.blank_clock();                            // clock 2: the pulse
    pix.blank_clock();                            // clock 3: hold / recovery
    pix.blank_clock();                            // clock 4: the guard
}

void palette_load_identity(PixelUnit &pix)
{
    for (uint32_t i = 0; i < PALETTE_ENTRIES; i++)
    {
        palette_load_direct(pix, i, static_cast<Rgb444>(i));
    }
}

constexpr Rgb444 TRACE_PASSTHROUGH = rgb444(9, 10, 3);

// A palette write needs the four slots after its data SET covered by a
// non-passthrough source.  The SOURCE is what covers them, not the record: a SET
// does not touch cur_src, so one opaque RUN in front of an entry holds the cover
// over the entry's own slots and the guard, and the first record that may resume
// passthrough is the one after the filler's successor (vidcmd-spec.md §3.2).
constexpr uint32_t PALETTE_COVER_SLOTS = 1;

std::vector<Rgb444> trace_slots(const std::vector<uint16_t> &words, uint32_t slots)
{
    PixelUnit      pix;
    CompositorUnit cmp;
    pix.power_on_reset();
    cmp.reset();          // the traces are a line INSIDE a frame, i.e. after /RS

    // The playfield is all ones, so 1bpp at base 0 selects PALETTE ENTRY 1: load
    // it and passthrough has a known colour.  Nothing else in the palette is
    // touched, so a trace that accidentally read another entry would show the
    // RAM's random power-up contents.
    palette_load_direct(pix, 1, TRACE_PASSTHROUGH);
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
        // PIX_PRELOAD, where the raster generator puts it: without it the line's
        // first word is never fetched and its first sixteen pixels come out of
        // the shifter's reset value.
        if (i == H_BLANK - PIXEL_FETCH_LEAD)
        {
            pix.arm_preload();
        }
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

    // ONE_PX_SPANS.  Three one-slot RUNs, c1 (RED), c0 (GREEN), c1 again, the
    // held colours loaded eagerly in HBLANK (RUN_COLOR, which this trace used
    // to use, is retired; the record class and so the timing are the same).
    // RED is staged and GREEN parked at the line
    // start, so RED takes slot 0 and GREEN slot 1 (L5); slot 2 HOLDs GREEN; the
    // third record is captured on the edge ending slot 2 and executes on slot 3
    // (L4) with slot 4 its HOLD; the passthrough RUN then executes on slot 5.
    {
        const std::vector<uint16_t> w = {vidcmd_set(SET_CMP_COLOR1, 0xF00),
                                         vidcmd_set(SET_CMP_COLOR0, 0x0F0),
                                         vidcmd_run(RUN_SRC_COLOR1, 1),
                                         vidcmd_run(RUN_SRC_COLOR0, 1),
                                         vidcmd_run(RUN_SRC_COLOR1, 1),
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
        CHECK(px[3] == 0xF00 && px[4] == 0xF00,
              "trace 1px: the third span is one cadence later, slots 3 and 4");
        CHECK(bad == 0, "trace 1px: passthrough must resume at slot 5 (%u exceptions)", bad);
    }

    // SUSTAINED_2SLOT.  A line of one-slot RUNs alternating c1 (RED) and c0
    // (GREEN), loaded eagerly in HBLANK: record 1 paints slot 0 as
    // the staged half of the opening pair, and every record after that paints
    // its own slot plus the HOLD slot behind it, so record k >= 2 covers slots
    // 2k-3 and 2k-2 and N records reach only slot 2N-2.  A full 256-word FIFO
    // therefore covers 511 of the 640 slots — the cadence, not the depth, is
    // what a dense line runs out of.
    {
        constexpr uint32_t RECORDS = VIDCMD_FIFO_WORDS;
        constexpr uint32_t COVERED = 2 * RECORDS - 1;
        std::vector<uint16_t> w = {vidcmd_set(SET_CMP_COLOR1, 0xF00),
                                   vidcmd_set(SET_CMP_COLOR0, 0x0F0)};
        for (uint32_t i = 0; i < RECORDS - 2; i++)
        {
            w.push_back(vidcmd_run((i % 2) == 0 ? RUN_SRC_COLOR1 : RUN_SRC_COLOR0, 1));
        }
        // The two eager SETs take two of the FIFO's words, so the line has two
        // records fewer than the FIFO's depth: 254 records reach slot 506.
        const std::vector<Rgb444> px = trace_slots(w, COVERED - 4);
        uint32_t off = 0;
        for (uint32_t i = 0; i < COVERED - 4; i++)
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
            const Rgb444 magenta = rgb444(15, 0, 15);
            std::vector<uint16_t> w = {vidcmd_set(SET_CMP_COLOR1, magenta)};
            uint32_t x = 0;
            for (uint32_t i = 0; i < SPANS; i++)
            {
                const uint32_t sx = FIRST_X + i * pitch;
                if (sx > x)
                {
                    w.push_back(vidcmd_run(RUN_SRC_PASSTHROUGH, sx - x));
                }
                w.push_back(vidcmd_run(RUN_SRC_COLOR1, 1));
                x = sx + 1;
            }
            w.push_back(vidcmd_run(RUN_SRC_PASSTHROUGH, H_ACTIVE - x));

            const std::vector<Rgb444> px = trace_slots(w, H_ACTIVE);
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
// These drive PixelUnit ALONE, the way pixel_tb.v drives the DUT: SET the
// registers during blanking, load the palette, fill the FIFO with a line's
// stream, run the raster's blanking with PIX_PRELOAD in it, then take 640 pixel
// clocks.  Every case names the pixel_tb.v case it mirrors, and every
// expectation is derived from the mode definition here — not read out of the
// model — so a disagreement is a real disagreement.
//
// WHAT IS ASSERTED PER CASE, and the second one is the reason this section was
// rewritten: (1) the 640 per-pixel indices, and (2) THE WORDS THE LINE READ.
// The old model had no fetch cadence at all — it pulled bits out of a buffer on
// demand — so it could not see the read count, which is how it missed two
// shipped RTL bugs.  This model reads the FIFO on the clocks the chip does, so
// `words_consumed` is pixel_tb.v's per-line /RE count, and the whole
// mode-by-skip matrix below is that bench's table at the real 640 width.
//
// THE PALETTE IS PART OF EVERY CASE.  PIXEL emits an index; the colour comes
// from a RAM that nothing resets.  The traces load an IDENTITY palette (entry i
// holds colour i), so an index error and a colour error are the same error and
// the expectation is the index itself.  PALETTE_UNLOADED at the end is the
// negative control on that: a case that forgets an entry reads the RAM's random
// power-up contents and the model says so.

struct PixelLine
{
    std::vector<uint32_t> idx;              // the palette index of each pixel
    uint32_t              words_consumed = 0;
    uint32_t              tiled_words    = 0;
    uint32_t              unwritten      = 0;
};

// One visible line out of a freshly powered-up PixelUnit, with the raster's own
// blanking in front of it so PIX_PRELOAD lands where timing.v puts it:
// PIXEL_FETCH_LEAD clocks before the first consumption clock.
PixelLine pixel_line(uint32_t mode, const std::vector<uint16_t> &words,
                     uint32_t base, uint32_t skip, bool load_palette = true)
{
    RenderStats stats;
    PixelUnit   pix;
    pix.power_on_reset();
    pix.attach_stats(&stats);
    pix.set_counting(true);

    if (load_palette)
    {
        palette_load_identity(pix);
    }

    pix.set_register(SET_PIX_BASE, base);
    pix.blank_clock();
    pix.set_register(SET_PIX_MODE, mode);
    pix.blank_clock();
    pix.set_register(SET_PIX_PIXEL_SKIP, skip);
    pix.blank_clock();

    for (uint16_t w : words)
    {
        pix.push_word(w);
    }

    const uint32_t before = pix.fifo_count();
    for (uint32_t h = 0; h < H_BLANK; h++)
    {
        if (h == H_BLANK - PIXEL_FETCH_LEAD)
        {
            pix.arm_preload();
        }
        pix.blank_clock();
    }
    pix.begin_line();

    PixelLine out;
    out.idx.reserve(H_ACTIVE);
    for (uint32_t i = 0; i < H_ACTIVE; i++)
    {
        // With the identity palette the colour IS the index.
        out.idx.push_back(pix.next_pixel() & 0xFu);
    }
    out.words_consumed = before - pix.fifo_count();
    out.tiled_words    = stats.pixels_tiled_words;
    out.unwritten      = stats.palette_unwritten_reads;
    return out;
}

// A line's stream, long enough for any mode: a fixed pseudo-random word list, so
// every expectation below is a function of the mode and not of the data.
std::vector<uint16_t> stream_words(uint32_t n)
{
    std::vector<uint16_t> w;
    w.reserve(n);
    uint32_t x = 0xA53C1234u;
    for (uint32_t i = 0; i < n; i++)
    {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        w.push_back(static_cast<uint16_t>(x & 0xFFFFu));
    }
    return w;
}

// THE EXPECTATION, derived from the mode definition and nothing else: group g of
// the line is `w` bits of the words the line read, concatenated, starting at
// stream bit `skip_fine + g*w`, MSB first, and zero once those words run out.
// This is pixel_tb.v's model_group(), and with production clamps its
// displacement and lead-deficit terms are both zero.
uint32_t stream_group(const std::vector<uint16_t> &words, uint32_t reads,
                      uint32_t start_bit, uint32_t g, uint32_t w)
{
    uint32_t v = 0;
    for (uint32_t i = 0; i < w; i++)
    {
        const uint32_t bitn = start_bit + g * w + i;
        const uint32_t wi   = bitn / 16u;
        const uint32_t bit  = (wi < reads) ? ((words[wi] >> (15u - (bitn % 16u))) & 1u) : 0u;
        v = (v << 1) | bit;
    }
    return v;
}

// The index a mode makes of a group, straight out of the table in descriptor.h.
uint32_t mode_index(uint32_t mode, uint32_t base, uint32_t group, uint32_t attr_colour)
{
    switch (pixel_mode_depth(mode))
    {
        case PIXEL_DEPTH_ATTR:
            return (group != 0) ? ((attr_colour >> 4) & 0xFu) : (attr_colour & 0xFu);
        case PIXEL_DEPTH_4BPP:
            return group & 0xFu;
        case PIXEL_DEPTH_2BPP:
            return (base & 0xCu) | (group & 3u);
        default:
            return (base & 0xEu) | (group & 1u);
    }
}

// The whole line's expectation for a non-attribute mode.
std::vector<uint32_t> expect_line(uint32_t mode, uint32_t base, uint32_t skip,
                                  const std::vector<uint16_t> &words, uint32_t reads)
{
    const uint32_t w    = pixels_bits_per_group(mode);
    const bool     half = pixel_mode_half_rate(mode);
    const uint32_t s    = pixels_skip_effective(mode, skip);

    std::vector<uint32_t> want(H_ACTIVE);
    for (uint32_t p = 0; p < H_ACTIVE; p++)
    {
        const uint32_t g = half ? (p / 2u) : p;
        want[p] = mode_index(mode, base, stream_group(words, reads, s, g, w), 0);
    }
    return want;
}

// Attribute mode's expectation: one word per eight-group cell, PIXELS_Q[15:8]
// the colour byte and PIXELS_Q[7:0] the pattern, pattern MSB first.
std::vector<uint32_t> expect_attr_line(uint32_t mode, const std::vector<uint16_t> &words)
{
    const bool half = pixel_mode_half_rate(mode);

    std::vector<uint32_t> want(H_ACTIVE);
    for (uint32_t p = 0; p < H_ACTIVE; p++)
    {
        const uint32_t g      = half ? (p / 2u) : p;
        const uint32_t cell   = g / 8u;
        const uint32_t colour = (words[cell] >> 8) & 0xFFu;
        const uint32_t bit    = (words[cell] >> (7u - (g % 8u))) & 1u;
        want[p] = bit != 0 ? ((colour >> 4) & 0xFu) : (colour & 0xFu);
    }
    return want;
}

uint32_t index_mismatches(const PixelLine &line, const std::vector<uint32_t> &want,
                          uint32_t first, uint32_t last)
{
    uint32_t bad = 0;
    for (uint32_t k = first; k <= last; k++)
    {
        if (line.idx[k] != want[k])
        {
            bad++;
        }
    }
    return bad;
}

void report_pixel_case(const char *name, const char *tb_case, const PixelLine &line,
                       uint32_t bad, uint32_t checked, uint32_t want_words)
{
    printf("  %-16s %-10s %4u pixels checked, %u wrong, %2u words read (want %u)\n",
           name, tb_case, checked, bad, line.words_consumed, want_words);
    CHECK(bad == 0, "pixel trace %s: %u of %u checked pixels are wrong", name, bad, checked);
    CHECK(line.words_consumed == want_words,
          "pixel trace %s: the line read %u PIXELS words, pixel_tb.v's /RE count law says "
          "%u", name, line.words_consumed, want_words);
    CHECK(line.unwritten == 0,
          "pixel trace %s: %u pixels came out of palette entries nobody loaded", name,
          line.unwritten);
}

void check_pixel_modes()
{
    printf("\n=== pixel-modes — the model against cpld/pixel/pixel_tb.v ===\n");

    const std::vector<uint16_t> w = stream_words(128);

    // --- the four depths at full rate, and both rates where defined ---------
    //
    // pixel_tb.v cases A (1bpp full), B (2bpp full), C (4bpp half), D (attr
    // full), E (attr half), F (1bpp half), each re-run at the real 640 width by
    // its case W.  pix_base picks the PAIR in 1bpp and the QUAD in 2bpp.
    struct ModeCase
    {
        const char *name;
        const char *tb;
        uint32_t    mode;
        uint32_t    base;
        uint32_t    words;
    };
    const ModeCase MODES[] = {
        {"1BPP",      "A / W", pixel_mode(PIXEL_DEPTH_1BPP, false), 0xA, PIXELS_WORDS_1BPP},
        {"1BPP_HALF", "F / W", pixel_mode(PIXEL_DEPTH_1BPP, true),  0xA, PIXELS_WORDS_1BPP_HALF},
        {"2BPP",      "B / W", pixel_mode(PIXEL_DEPTH_2BPP, false), 0xC, PIXELS_WORDS_2BPP},
        {"2BPP_HALF", "W",     pixel_mode(PIXEL_DEPTH_2BPP, true),  0xC, PIXELS_WORDS_2BPP_HALF},
        {"4BPP_HALF", "C / W", pixel_mode(PIXEL_DEPTH_4BPP, true),  0x0, PIXELS_WORDS_4BPP_HALF},
    };
    for (const ModeCase &m : MODES)
    {
        const PixelLine line = pixel_line(m.mode, w, m.base, 0);
        const std::vector<uint32_t> want = expect_line(m.mode, m.base, 0, w, m.words);
        report_pixel_case(m.name, m.tb, line,
                          index_mismatches(line, want, 0, H_ACTIVE - 1), H_ACTIVE, m.words);
    }

    // --- the attribute word, both rates (pixel_tb.v D / E / D2 / E2) --------
    {
        const uint32_t mode = pixel_mode(PIXEL_DEPTH_ATTR, false);
        const PixelLine line = pixel_line(mode, w, 0, 0);
        report_pixel_case("ATTR", "D / D2", line,
                          index_mismatches(line, expect_attr_line(mode, w), 0, H_ACTIVE - 1),
                          H_ACTIVE, PIXELS_WORDS_ATTR);
    }
    {
        const uint32_t mode = pixel_mode(PIXEL_DEPTH_ATTR, true);
        const PixelLine line = pixel_line(mode, w, 0, 0);
        report_pixel_case("ATTR_HALF", "E / E2", line,
                          index_mismatches(line, expect_attr_line(mode, w), 0, H_ACTIVE - 1),
                          H_ACTIVE, PIXELS_WORDS_ATTR_HALF);
    }

    // --- pix_base: 1bpp selects a PAIR, 2bpp a QUAD -------------------------
    //
    // The same stream through two different bases must paint the same PATTERN
    // out of different entries, which is what makes a per-line base change a
    // free recolour and a mid-line one pixel-exact (m0/m2).
    {
        const uint32_t mode = pixel_mode(PIXEL_DEPTH_1BPP, false);
        const PixelLine a = pixel_line(mode, w, 0x0, 0);
        const PixelLine b = pixel_line(mode, w, 0x6, 0);
        uint32_t bad = 0;
        for (uint32_t k = 0; k < H_ACTIVE; k++)
        {
            if ((a.idx[k] & 1u) != (b.idx[k] & 1u) || (b.idx[k] & 0xEu) != 0x6u)
            {
                bad++;
            }
        }
        printf("  %-16s %-10s pair 0/1 vs 6/7: %u pixels disagree\n",
               "BASE_PAIR", "A", bad);
        CHECK(bad == 0, "pixel trace BASE_PAIR: %u pixels do not follow {base[3:1], bit}",
              bad);
    }
    {
        const uint32_t mode = pixel_mode(PIXEL_DEPTH_2BPP, false);
        const PixelLine a = pixel_line(mode, w, 0x0, 0);
        const PixelLine b = pixel_line(mode, w, 0x8, 0);
        uint32_t bad = 0;
        for (uint32_t k = 0; k < H_ACTIVE; k++)
        {
            if ((a.idx[k] & 3u) != (b.idx[k] & 3u) || (b.idx[k] & 0xCu) != 0x8u)
            {
                bad++;
            }
        }
        printf("  %-16s %-10s quad 0..3 vs 8..B: %u pixels disagree\n",
               "BASE_QUAD", "B", bad);
        CHECK(bad == 0, "pixel trace BASE_QUAD: %u pixels do not follow {base[3:2], dibit}",
              bad);
    }

    // --- pixel_skip: the whole matrix, words AND leading pixels -------------
    //
    // pixel_tb.v's SKIP_TABLE at the real 640 width.  The word count is
    //
    //     record + (effective skip != 0)
    //
    // and the effective skip is the register truncated to four bits, then the
    // depth clamp, then the cap — 14 (1bpp either rate), 12 (2bpp full), 14
    // (2bpp half), 12 (4bpp half).  Attribute mode refuses skip entirely.  The
    // leading pixels are checked against the same start-bit law, which is what
    // proves the clamp CLAMPS rather than being ignored.
    {
        const struct { const char *name; uint32_t mode; uint32_t base; uint32_t record; } SK[] = {
            {"1bpp",      pixel_mode(PIXEL_DEPTH_1BPP, false), 0xA, PIXELS_WORDS_1BPP},
            {"1bpp half", pixel_mode(PIXEL_DEPTH_1BPP, true),  0xA, PIXELS_WORDS_1BPP_HALF},
            {"2bpp",      pixel_mode(PIXEL_DEPTH_2BPP, false), 0xC, PIXELS_WORDS_2BPP},
            {"2bpp half", pixel_mode(PIXEL_DEPTH_2BPP, true),  0xC, PIXELS_WORDS_2BPP_HALF},
            {"4bpp half", pixel_mode(PIXEL_DEPTH_4BPP, true),  0x0, PIXELS_WORDS_4BPP_HALF},
            {"attr",      pixel_mode(PIXEL_DEPTH_ATTR, false), 0x0, PIXELS_WORDS_ATTR},
        };
        uint32_t cells = 0;
        uint32_t bad_words = 0;
        uint32_t bad_px = 0;
        for (const auto &m : SK)
        {
            for (uint32_t skip = 0; skip <= 17; skip++)
            {
                const uint32_t eff  = pixels_skip_effective(m.mode, skip);
                const uint32_t want = m.record + (eff != 0 ? PIXELS_SKIP_EXTRA_WORDS : 0u);
                const PixelLine line = pixel_line(m.mode, w, m.base, skip);
                cells++;
                if (line.words_consumed != want)
                {
                    bad_words++;
                    printf("    skip %2u %-9s read %u words, want %u\n", skip, m.name,
                           line.words_consumed, want);
                }
                if (pixel_mode_depth(m.mode) != PIXEL_DEPTH_ATTR)
                {
                    const std::vector<uint32_t> exp =
                        expect_line(m.mode, m.base, skip, w, want);
                    // The leading 64 pixels: enough to cover the first four
                    // words in every mode, and independent of the tail.
                    const uint32_t b = index_mismatches(line, exp, 0, 63);
                    if (b != 0)
                    {
                        bad_px += b;
                        printf("    skip %2u %-9s %u of the leading 64 pixels wrong\n",
                               skip, m.name, b);
                    }
                }
            }
        }
        printf("  %-16s %-10s %u mode/skip cells: %u word counts wrong, %u leading "
               "pixels wrong\n", "SKIP_MATRIX", "W / skip", cells, bad_words, bad_px);
        CHECK(bad_words == 0, "SKIP_MATRIX: %u of %u cells read the wrong number of words",
              bad_words, cells);
        CHECK(bad_px == 0, "SKIP_MATRIX: %u leading pixels are wrong under skip", bad_px);
    }

    // --- THE SKIPPED LINE'S TAIL SHOWS REAL DATA ----------------------------
    //
    // The one visible semantic change from the retired byte engine: with the
    // extra word read, the last pixels of a scrolled line are stream data, not
    // the zeros the old decoder shifted in.  pixel_tb.v's mut5 is the negative
    // control on the allowance itself.
    {
        const uint32_t mode = pixel_mode(PIXEL_DEPTH_1BPP, false);
        const PixelLine line = pixel_line(mode, w, 0x0, 3);
        const std::vector<uint32_t> want =
            expect_line(mode, 0x0, 3, w, PIXELS_WORDS_1BPP + 1);
        const uint32_t bad = index_mismatches(line, want, H_ACTIVE - 16, H_ACTIVE - 1);
        uint32_t nonzero = 0;
        for (uint32_t k = H_ACTIVE - 16; k < H_ACTIVE; k++)
        {
            nonzero += (line.idx[k] & 1u);
        }
        printf("  %-16s %-10s skip 3: %u words, last 16 pixels %u wrong, %u of them set\n",
               "SKIP_TAIL", "skip", line.words_consumed, bad, nonzero);
        CHECK(bad == 0, "SKIP_TAIL: %u of the last 16 pixels are wrong", bad);
        CHECK(line.words_consumed == PIXELS_WORDS_1BPP + 1,
              "SKIP_TAIL: a skipped line must read record+1 = %u words, not %u",
              PIXELS_WORDS_1BPP + 1, line.words_consumed);
    }

    // --- RESET: an untouched chip is 1bpp full rate out of entries 0 and 1 --
    {
        RenderStats stats;
        PixelUnit   pix;
        pix.power_on_reset();
        pix.attach_stats(&stats);
        pix.set_counting(true);
        palette_load_identity(pix);
        pix.reset();                 // /RS, which does NOT clear the RAM
        for (uint16_t v : w)
        {
            pix.push_word(v);
        }
        for (uint32_t h = 0; h < H_BLANK; h++)
        {
            if (h == H_BLANK - PIXEL_FETCH_LEAD)
            {
                pix.arm_preload();
            }
            pix.blank_clock();
        }
        pix.begin_line();
        uint32_t bad = 0;
        for (uint32_t k = 0; k < H_ACTIVE; k++)
        {
            const uint32_t got  = pix.next_pixel() & 0xFu;
            const uint32_t want = (w[k / 16] >> (15u - (k % 16u))) & 1u;
            bad += (got == want) ? 0u : 1u;
        }
        printf("  %-16s %-10s /RS leaves mode 0, base 0, skip 0 and the RAM LOADED: "
               "%u wrong, %u unloaded reads\n", "RESET_1BPP", "R", bad,
               stats.palette_unwritten_reads);
        CHECK(bad == 0, "RESET_1BPP: an untouched chip must render 1bpp out of entries 0 "
              "and 1 — %u pixels wrong", bad);
        CHECK(stats.palette_unwritten_reads == 0,
              "RESET_1BPP: /RS must NOT clear the palette RAM, but %u reads found entries "
              "unloaded", stats.palette_unwritten_reads);
    }

    // --- THE NEGATIVE CONTROL ON THE RANDOM POWER-UP ------------------------
    //
    // A case that forgets to load the entries it displays must FAIL, and this is
    // the proof that it does: the same line with no palette load at all comes
    // out of the RAM's reproducible random contents, so it does not match the
    // identity expectation and the model counts every unloaded read.
    {
        const uint32_t mode = pixel_mode(PIXEL_DEPTH_1BPP, false);
        const PixelLine line = pixel_line(mode, w, 0xA, 0, false);
        const std::vector<uint32_t> want = expect_line(mode, 0xA, 0, w, PIXELS_WORDS_1BPP);
        const uint32_t bad = index_mismatches(line, want, 0, H_ACTIVE - 1);
        printf("  %-16s %-10s no palette load: %u of %u pixels wrong, %u unloaded reads\n",
               "PALETTE_UNLOADED", "S0", bad, H_ACTIVE, line.unwritten);
        CHECK(bad > 0,
              "PALETTE_UNLOADED: the RAM must power up UNDEFINED — with no load at all the "
              "line matched the loaded expectation, so something is clearing it to a "
              "convenient value");
        CHECK(line.unwritten == H_ACTIVE,
              "PALETTE_UNLOADED: %u of %u pixels should have been flagged as reads of "
              "entries nobody loaded", line.unwritten, H_ACTIVE);
    }

    printf("  MEASURED         words/line 40/20 1bpp, 80/40 2bpp, -/80 4bpp, 80/40 attr; "
           "skip caps 14/12/14/12; +1 word on any non-zero skip\n");
}
// ---------------------------------------------------------------------------
// PALETTE traces — the model against cpld/pixel/clut_pair_tb.v
// ---------------------------------------------------------------------------
//
// clut_pair_tb.v instantiates COMPOSITOR, PIXEL and three behavioural RAMs and
// is GROUND TRUTH for everything here.  Its cases and the checks below:
//
//   S1a/S1b  the two illegal third words — another PIXEL-target SET, a
//            COMPOSITOR colour SET — both of which STORE THEIR OWN PAYLOAD in
//            the entry.  DETECTED, and the model stores the wrong colour too.
//   S1c      the contract: SET clut_index, SET clut_data, same-value SET 7.
//   S3       eight entries reloaded in ONE HBLANK, then the line that uses them.
//   S6       a write behind a cmp_color RUN mid-line: undisturbed.
//   S6-neg   the same write under passthrough: three clocks of undefined RGB
//            reach RGB_OUT.  DETECTED.
//   S8       a sixteen-entry load in vertical blank, then one line per mode.
//
// These drive the two units directly, like the cadence and mask traces, because
// the claim is about the WRITE WINDOW and not about a list builder.

struct PaletteTrace
{
    std::vector<Rgb444> px;
    RenderStats         stats;
    Rgb444              entry_after[PALETTE_ENTRIES] = {};
};

// Push `words` into VIDCMD, run one HBLANK (with the raster's preload in it),
// then take `slots` active pixel clocks over an all-ones 1bpp playfield.
PaletteTrace palette_trace(const std::vector<uint16_t> &words, uint32_t slots,
                           uint32_t hblank_words = 0)
{
    PaletteTrace out;
    PixelUnit      pix;
    CompositorUnit cmp;
    pix.power_on_reset();
    cmp.reset();
    pix.attach_stats(&out.stats);
    cmp.attach_stats(&out.stats);
    pix.set_counting(true);
    cmp.set_counting(true);

    for (uint32_t i = 0; i < PIXELS_WORDS_1BPP + 1; i++)
    {
        pix.push_word(0xFFFF);          // every group is 1: index = pix_base | 1
    }

    // Words the list delivers before the line: the HBLANK packet.
    for (uint32_t i = 0; i < hblank_words && i < words.size(); i++)
    {
        cmp.push_word(words[i]);
    }
    for (uint32_t i = 0; i < H_BLANK; i++)
    {
        if (i == H_BLANK - PIXEL_FETCH_LEAD)
        {
            pix.arm_preload();
        }
        cmp.blank_clock(pix);
    }
    // The rest arrive for the active line.
    for (uint32_t i = hblank_words; i < words.size(); i++)
    {
        cmp.push_word(words[i]);
    }

    pix.begin_line();
    out.px.reserve(slots);
    for (uint32_t i = 0; i < slots; i++)
    {
        out.px.push_back(cmp.active_slot(pix));
    }
    for (uint32_t i = 0; i < PALETTE_ENTRIES; i++)
    {
        out.entry_after[i] = pix.palette().entry(i);
    }
    return out;
}

void check_palette_traces()
{
    printf("\n=== palette — the model against cpld/pixel/clut_pair_tb.v ===\n");

    constexpr Rgb444 V   = 0x0ABC;   // the colour an entry is meant to get
    constexpr Rgb444 OTH = 0x0123;   // the colour an illegal third word carries
    constexpr Rgb444 COV = 0x0F0F;   // the cover colour

    // --- S1c: the contract, in HBLANK where a write is always legal ----------
    {
        std::vector<uint16_t> w;
        for (uint16_t v : vidcmd_palette_entry(5, V))
        {
            w.push_back(v);
        }
        w.push_back(vidcmd_set(SET_PIX_BASE, 4));       // index = 4 | 1 = 5
        w.push_back(vidcmd_run(RUN_SRC_PASSTHROUGH, H_ACTIVE));
        const PaletteTrace t = palette_trace(w, 32, static_cast<uint32_t>(w.size()));
        uint32_t bad = 0;
        for (Rgb444 c : t.px)
        {
            bad += (c == V) ? 0u : 1u;
        }
        printf("  S1c CONTRACT        entry 5 <- 0x%03X in HBLANK: stored 0x%03X, %u of %u "
               "pixels wrong, %u bad fillers\n", V, t.entry_after[5], bad,
               static_cast<uint32_t>(t.px.size()), t.stats.palette_bad_filler);
        CHECK(t.entry_after[5] == V, "S1c: the entry holds 0x%03X, not the committed 0x%03X",
              t.entry_after[5], V);
        CHECK(bad == 0, "S1c: %u pixels do not show the entry the HBLANK packet wrote", bad);
        CHECK(t.stats.palette_bad_filler == 0, "S1c: the contract must not flag a filler");
        CHECK(t.stats.palette_disturbed_visible == 0,
              "S1c: a write in HBLANK cannot disturb a pixel, yet %u were flagged",
              t.stats.palette_disturbed_visible);
    }

    // --- S1a: a PIXEL-target SET as the third word --------------------------
    //
    // The bus is COMPOSITOR's, re-registered from every staged word, so the RAM
    // latches whatever the word behind the data SET carries.  A SET(pix_base)
    // with a different payload therefore writes the ENTRY with its own value,
    // and the model stores that wrong colour exactly as the bench measured it.
    {
        std::vector<uint16_t> w = {
            vidcmd_set(SET_PIX_CLUT_INDEX, 5),
            vidcmd_set(SET_PIX_CLUT_DATA, V),
            vidcmd_set(SET_PIX_BASE, OTH),              // NOT the filler
            vidcmd_run(RUN_SRC_PASSTHROUGH, H_ACTIVE),
        };
        const PaletteTrace t = palette_trace(w, 32, static_cast<uint32_t>(w.size()));
        printf("  S1a MISSING FILLER  third word SET(pix_base,0x%03X): entry 5 holds 0x%03X "
               "(wanted 0x%03X), %u flagged\n", OTH, t.entry_after[5], V,
               t.stats.palette_bad_filler);
        CHECK(t.entry_after[5] == OTH,
              "S1a: the entry holds 0x%03X; the hardware stores the THIRD WORD's payload "
              "0x%03X, and the model must reproduce that rather than fixing it up",
              t.entry_after[5], OTH);
        CHECK(t.stats.palette_bad_filler == 1,
              "S1a: %u writes flagged, expected exactly 1 — a missing filler must be "
              "DETECTED", t.stats.palette_bad_filler);
    }

    // --- S1b: a COMPOSITOR colour SET as the third word ---------------------
    {
        std::vector<uint16_t> w = {
            vidcmd_set(SET_PIX_CLUT_INDEX, 5),
            vidcmd_set(SET_PIX_CLUT_DATA, V),
            vidcmd_set(SET_CMP_COLOR1, OTH),            // COMPOSITOR's own, still wrong
            vidcmd_run(RUN_SRC_PASSTHROUGH, H_ACTIVE),
        };
        const PaletteTrace t = palette_trace(w, 32, static_cast<uint32_t>(w.size()));
        printf("  S1b MISSING FILLER  third word SET(cmp_color1,0x%03X): entry 5 holds "
               "0x%03X, %u flagged\n", OTH, t.entry_after[5], t.stats.palette_bad_filler);
        CHECK(t.entry_after[5] == OTH,
              "S1b: a COMPOSITOR-target third word protects nothing — the entry should hold "
              "0x%03X, got 0x%03X", OTH, t.entry_after[5]);
        CHECK(t.stats.palette_bad_filler == 1,
              "S1b: %u writes flagged, expected exactly 1", t.stats.palette_bad_filler);
    }

    // --- S3: eight entries in ONE HBLANK, then the line that uses them ------
    //
    // Three words an entry, so eight entries is 24 words and a 26-word HBLANK
    // packet still has room for the mode / base / skip SETs.  The line then
    // shows one of them, which is the assertion that all eight landed.
    {
        std::vector<uint16_t> w;
        for (uint32_t i = 0; i < 8; i++)
        {
            for (uint16_t v : vidcmd_palette_entry(i, static_cast<Rgb444>(0x100u * (i + 1u))))
            {
                w.push_back(v);
            }
        }
        w.push_back(vidcmd_set(SET_PIX_MODE, pixel_mode(PIXEL_DEPTH_1BPP, false)));
        w.push_back(vidcmd_set(SET_PIX_BASE, 6));       // index 6 | 1 = 7
        w.push_back(vidcmd_run(RUN_SRC_PASSTHROUGH, H_ACTIVE));
        const uint32_t packet = static_cast<uint32_t>(w.size());
        const PaletteTrace t = palette_trace(w, 32, packet);
        uint32_t bad = 0;
        for (uint32_t i = 0; i < 8; i++)
        {
            bad += (t.entry_after[i] == static_cast<Rgb444>(0x100u * (i + 1u))) ? 0u : 1u;
        }
        printf("  S3 HBLANK RELOAD    %u words = 8 entries + mode/base/RUN; %u entries "
               "wrong, the line paints entry 7 as 0x%03X\n", packet, bad, t.px[0]);
        CHECK(packet <= 26 + 1,
              "S3: the packet is %u words; eight entries plus the SETs must fit the "
              "26-word HBLANK budget the spec claims", packet);
        CHECK(bad == 0, "S3: %u of the eight entries did not land", bad);
        CHECK(t.px[0] == 0x800, "S3: the line paints 0x%03X, expected entry 7's 0x800",
              t.px[0]);
        CHECK(t.stats.palette_disturbed_visible == 0,
              "S3: %u disturbed pixels reached the DAC from an HBLANK packet",
              t.stats.palette_disturbed_visible);
    }

    // --- S6 / S6-neg: THE WHERE RULE ----------------------------------------
    //
    // The same mid-line entry reload, twice: once with the source held on a
    // cmp_color RUN (legal — the write's four slots are covered) and once under
    // passthrough (illegal — undefined RGB reaches the DAC).  Nothing about the
    // words changes; only what is painting.
    {
        std::vector<uint16_t> legal = {
            vidcmd_set(SET_CMP_COLOR1, COV),
            vidcmd_set(SET_PIX_BASE, 4),
            vidcmd_run(RUN_SRC_HELD_FG, 32),            // the cover, in force
        };
        for (uint16_t v : vidcmd_palette_entry(5, V))
        {
            legal.push_back(v);
        }
        legal.push_back(vidcmd_run(RUN_SRC_HELD_FG, 8));        // still covered
        legal.push_back(vidcmd_run(RUN_SRC_PASSTHROUGH, 128));  // passthrough resumes here
        const PaletteTrace t = palette_trace(legal, 200, 3);

        printf("  S6 COVERED          entry 5 rewritten mid-line behind a cmp_color1 RUN: "
               "%u disturbed pixels on the DAC, entry holds 0x%03X\n",
               t.stats.palette_disturbed_visible, t.entry_after[5]);
        CHECK(t.entry_after[5] == V, "S6: the covered write stored 0x%03X, not 0x%03X",
              t.entry_after[5], V);
        CHECK(t.stats.palette_disturbed_visible == 0,
              "S6: %u disturbed pixels reached the DAC even though the source was "
              "cmp_color1 throughout the window", t.stats.palette_disturbed_visible);
        CHECK(t.stats.palette_bad_filler == 0, "S6: the filler was flagged");
    }
    {
        std::vector<uint16_t> illegal = {
            vidcmd_set(SET_PIX_BASE, 4),
            vidcmd_run(RUN_SRC_PASSTHROUGH, 32),        // NO cover at all
        };
        for (uint16_t v : vidcmd_palette_entry(5, V))
        {
            illegal.push_back(v);
        }
        illegal.push_back(vidcmd_run(RUN_SRC_PASSTHROUGH, 128));
        const PaletteTrace t = palette_trace(illegal, 200, 2);

        uint32_t undef = 0;
        for (Rgb444 c : t.px)
        {
            undef += (c == RGB444_UNDEFINED) ? 1u : 0u;
        }
        printf("  S6-neg UNCOVERED    the same write under passthrough: %u disturbed pixels "
               "on the DAC, %u of them UNDEFINED\n",
               t.stats.palette_disturbed_visible, undef);
        CHECK(t.stats.palette_disturbed_visible >= PALETTE_WRITE_MEASURED_SLOTS,
              "S6-neg: only %u disturbed pixels were flagged; a write under passthrough "
              "must put at least the %u measured slots on the DAC",
              t.stats.palette_disturbed_visible, PALETTE_WRITE_MEASURED_SLOTS);
        CHECK(undef == 2,
              "S6-neg: %u pixels came out UNDEFINED, expected the two clocks the RAM's "
              "outputs are high-Z and recovering", undef);
        CHECK(t.entry_after[5] == V,
              "S6-neg: the write itself still works — the entry should hold 0x%03X, got "
              "0x%03X", V, t.entry_after[5]);
    }

    // --- S8: sixteen entries in vertical blank, then a line of each depth ---
    //
    // 48 words is more than an HBLANK delivers, which is why a full table load
    // belongs in vertical blanking; the entries then survive to the end of the
    // frame because nothing clears the RAM.
    {
        std::vector<uint16_t> w;
        for (uint32_t i = 0; i < PALETTE_ENTRIES; i++)
        {
            for (uint16_t v : vidcmd_palette_entry(i, TEST_PALETTE[i]))
            {
                w.push_back(v);
            }
        }
        w.push_back(vidcmd_set(SET_PIX_BASE, 0xE));     // 1bpp pair 14/15
        w.push_back(vidcmd_run(RUN_SRC_PASSTHROUGH, H_ACTIVE));
        printf("  S8 VBLANK LOAD      %u words for %u entries (%u each)\n",
               static_cast<uint32_t>(w.size()) - 2, PALETTE_ENTRIES,
               PALETTE_WORDS_PER_ENTRY);
        CHECK(w.size() - 2 == PALETTE_ENTRIES * PALETTE_WORDS_PER_ENTRY,
              "S8: a sixteen-entry load is %u words, expected %u",
              static_cast<uint32_t>(w.size()) - 2,
              PALETTE_ENTRIES * PALETTE_WORDS_PER_ENTRY);

        const PaletteTrace t = palette_trace(w, 64, static_cast<uint32_t>(w.size()));
        uint32_t bad = 0;
        for (uint32_t i = 0; i < PALETTE_ENTRIES; i++)
        {
            bad += (t.entry_after[i] == TEST_PALETTE[i]) ? 0u : 1u;
        }
        uint32_t px_bad = 0;
        for (Rgb444 c : t.px)
        {
            px_bad += (c == TEST_PALETTE[15]) ? 0u : 1u;
        }
        printf("                      %u entries wrong, the line paints entry 15 on %u of "
               "%u slots, %u unloaded reads\n", bad,
               static_cast<uint32_t>(t.px.size()) - px_bad,
               static_cast<uint32_t>(t.px.size()), t.stats.palette_unwritten_reads);
        CHECK(bad == 0, "S8: %u of the sixteen entries did not land", bad);
        CHECK(px_bad == 0, "S8: %u pixels do not show entry 15", px_bad);
        CHECK(t.stats.palette_unwritten_reads == 0,
              "S8: %u pixels read an entry nobody loaded", t.stats.palette_unwritten_reads);
    }

    printf("  MEASURED            an entry is %u words; the write disturbs the %u slots "
           "after the data SET (%u measured + 1 recovery guard)\n",
           PALETTE_WORDS_PER_ENTRY, PALETTE_WRITE_DISTURBED_SLOTS,
           PALETTE_WRITE_MEASURED_SLOTS);
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
        const Rgb444        bg   = plan.background_color;
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

// 2bpp, with the mid-line SET(pix_base) pair placed by slot arithmetic.  The two
// SETs are the leading RUN's banked pair, so the first commits on slot
// split_pixel and the second on slot split_pixel + 1 — exactly what m2-split
// pins in 1bpp, restated here with QUADS instead of pairs.  A base change
// recolours all four indices at once and costs one word, where a palette reload
// would cost three per entry AND four covered pixel slots.
FrameImage indexed2_reference_frame(const FrameParams &p)
{
    FrameImage img;
    img.pixels.assign(static_cast<size_t>(H_ACTIVE) * V_ACTIVE, 0);

    for (uint32_t y = 0; y < V_ACTIVE; y++)
    {
        for (uint32_t x = 0; x < H_ACTIVE; x++)
        {
            uint32_t base = p.pix_base;
            if (p.mid_line_split && x == p.split_pixel)
            {
                base = SPLIT_BASE_MID;
            }
            else if (p.mid_line_split && x > p.split_pixel)
            {
                base = SPLIT_BASE_HI;
            }
            const uint32_t index = palette_index_2bpp(base, idx2_pattern_code(x, y));
            img.pixels[y * H_ACTIVE + x] = p.palette[index];
        }
    }
    return img;
}

// 4bpp at HALF rate: one nibble per group, each group two pixels wide, straight
// into the palette with pix_base unused.  All sixteen entries on every line.
FrameImage indexed4_half_reference_frame(const FrameParams &p)
{
    FrameImage img;
    img.pixels.assign(static_cast<size_t>(H_ACTIVE) * V_ACTIVE, 0);

    for (uint32_t y = 0; y < V_ACTIVE; y++)
    {
        for (uint32_t x = 0; x < H_ACTIVE; x++)
        {
            img.pixels[y * H_ACTIVE + x] = p.palette[idx4_pattern_code(x / 2u, y)];
        }
    }
    return img;
}

// ATTRIBUTE mode, either rate: one {fg, bg, pattern} word per eight-group cell,
// pattern MSB first, both nibbles of the colour byte palette indices.  At full
// rate a cell is 8 pixels (an 80-column coloured text mode); at half rate 16 (a
// 320-wide tile mode).
FrameImage attr_reference_frame(const FrameParams &p)
{
    const bool     half      = pixel_mode_half_rate(pixel_mode_bits(p.mode));
    const uint32_t cell_w    = half ? 16u : 8u;

    FrameImage img;
    img.pixels.assign(static_cast<size_t>(H_ACTIVE) * V_ACTIVE, 0);

    for (uint32_t y = 0; y < V_ACTIVE; y++)
    {
        for (uint32_t x = 0; x < H_ACTIVE; x++)
        {
            const uint32_t cell  = x / cell_w;
            const uint32_t group = half ? ((x % cell_w) / 2u) : (x % cell_w);
            const uint32_t pat   = attr_cell_pattern(cell, y);
            const uint32_t bit   = (pat >> (7u - group)) & 1u;
            const uint32_t index = bit != 0 ? attr_cell_fg(cell, y) : attr_cell_bg(cell, y);
            img.pixels[y * H_ACTIVE + x] = p.palette[index];
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
                                   vidcmd_run(RUN_SRC_COLOR1, 20),
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

    // MASKB_STARVE (LM4).  LITERAL(GREEN) + HOLD(2) (slots 0..3: the literal,
    // its KEEP slot, the HOLD captured one edge late by LL) then a header whose
    // DATA WORD is withheld.  The span under the mask is a KEEP span on
    // purpose: the modal restore gives back the SOURCE, and over KEEP that
    // repeats the mask's LAST pixel, not the green underneath.  With
    // nothing on Q at the pixel-8 edge the record freezes whole — no count, no
    // shift, no source change — so pixel 7's colour stretches until the word
    // arrives, and the reload edge itself is the one that paints pixel 8.
    {
        PixelUnit      pix;
        CompositorUnit cmp;
        pix.power_on_reset();
        cmp.reset();      // a line inside a frame: the /RS values are in force
        palette_load_direct(pix, 1, TRACE_PASSTHROUGH);
        for (uint32_t i = 0; i < PIXELS_WORDS_1BPP + 1; i++)
        {
            pix.push_word(0xFFFF);
        }
        const VidcmdMask m = trace_mask({T, B, F, T, B, T, F, T, B, F, F, B, T, F, B});
        cmp.push_word(vidcmd_set(SET_CMP_COLOR0, 0x0C0));
        cmp.push_word(vidcmd_set(SET_CMP_COLOR1, C1));
        cmp.push_word(vidcmd_literal(0x0F0));
        cmp.push_word(vidcmd_hold(2));
        cmp.push_word(m.header);
        for (uint32_t i = 0; i < H_BLANK; i++)
        {
            if (i == H_BLANK - PIXEL_FETCH_LEAD)
            {
                pix.arm_preload();
            }
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
        // Modal, even across a stall: the SOURCE comes back, and over a KEEP
        // span that is KEEP, which repeats the mask's last pixel (want_data[7],
        // cmp_color0 0x0C0) — re-issue the literal after a mask laid over one.
        uint32_t tail = 0;
        for (uint32_t i = starve_reload_slot + MASK_DATA_DIBITS; i < H_ACTIVE; i++)
        {
            if (px[i] != 0x0C0)
            {
                tail++;
            }
        }
        CHECK(px[0] == 0x0F0 && px[1] == 0x0F0 && px[2] == 0x0F0 && px[3] == 0x0F0,
              "mask starve: the literal span under the mask must be slots 0..3");
        CHECK(tail == 0, "mask starve: %u tail pixels are not KEEP of the mask's last pixel "
              "(0x0C0) — the modal restore did not give back KEEP", tail);
    }

    // A mask that straddles the H_ACTIVE fall freezes rather than being
    // abandoned, and its parked data word is NOT taken during blanking: the two
    // borrow-back edges are gated by H_ACTIVE.  The pixels that are left play at
    // the start of the next line — wrong x, self-healing, exactly like every
    // other overrun here.
    {
        PixelUnit      pix;
        CompositorUnit cmp;
        pix.power_on_reset();
        cmp.reset();      // a line inside a frame: the /RS values are in force
        palette_load_direct(pix, 1, TRACE_PASSTHROUGH);
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
            if (i == H_BLANK - PIXEL_FETCH_LEAD)
            {
                pix.arm_preload();
            }
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
            if (i == H_BLANK - PIXEL_FETCH_LEAD)
            {
                pix.arm_preload();
            }
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
    // all zeroes, and a zero group in 1bpp at pix_base 0 selects PALETTE ENTRY
    // 0.  So RGB_IN is entry 0, a dibit-00 pixel resolves to it, and a
    // three-word table entry can write it — dibit 00 is a third settable colour
    // per record with no SET between the groups that use it.
    //
    // The probe: two identical masks whose dibits are all 00, with a RELOAD of
    // entry 0 between them.  If passthrough were anything fixed the two groups
    // would come out the same colour.
    //
    // WHAT CHANGED WITH THE PALETTE RAM: the reload is a WRITE, and a write
    // steals four pixel slots (vidcmd-spec.md §3.2).  Between two
    // all-passthrough masks those slots are passthrough, so the reload has to be
    // covered — here by an opaque cmp_color0 RUN long enough to cover the
    // window, which is also what makes the seam arithmetic below explicit.
    {
        RenderStats    stats;
        PixelUnit      pix;
        CompositorUnit cmp;
        pix.power_on_reset();
        cmp.reset();      // a line inside a frame: the /RS values are in force
        pix.attach_stats(&stats);
        cmp.attach_stats(&stats);
        pix.set_counting(true);
        cmp.set_counting(true);
        // Deliberately NO pix.push_word() anywhere: the FIFO is dry, which is
        // exactly the pure-VIDCMD screen's condition.
        cmp.push_word(vidcmd_set(SET_CMP_COLOR0, C0));
        cmp.push_word(vidcmd_set(SET_PIX_MODE, pixel_mode_bits(PixelMode::DIRECT_1BPP)));
        cmp.push_word(vidcmd_set(SET_PIX_BASE, 0));
        for (uint16_t v : vidcmd_palette_entry(0, 0x123))
        {
            cmp.push_word(v);
        }
        push_mask_into(cmp, trace_mask({T, T, T, T, T, T, T, T, T, T, T, T, T, T, T}));
        // The cover: an opaque run over the write's four slots, then the reload.
        cmp.push_word(vidcmd_run(RUN_SRC_HELD_BG, PALETTE_COVER_SLOTS));
        for (uint16_t v : vidcmd_palette_entry(0, 0x456))
        {
            cmp.push_word(v);
        }
        push_mask_into(cmp, trace_mask({T, T, T, T, T, T, T, T, T, T, T, T, T, T, T}));
        for (uint32_t i = 0; i < H_BLANK; i++)
        {
            if (i == H_BLANK - PIXEL_FETCH_LEAD)
            {
                pix.arm_preload();
            }
            cmp.blank_clock(pix);
        }
        pix.begin_line();
        std::vector<Rgb444> px;
        for (uint32_t i = 0; i < 64; i++)
        {
            px.push_back(cmp.active_slot(pix));
        }

        // Mask A owns slots 0..15.  Mask B's start is no longer a closed form —
        // the cover RUN and the three entry words sit between them — so it is
        // FOUND: the first slot after mask A that paints the reloaded colour.
        const uint32_t b0 = first_ne_from(px, MASK_SLOTS, C0) + 1u;
        uint32_t bad = 0;
        for (uint32_t i = 1; i < MASK_SLOTS; i++)
        {
            if (px[i] != 0x123)
            {
                bad++;
            }
        }
        for (uint32_t i = 0; i < MASK_SLOTS - 2 && b0 + i < px.size(); i++)
        {
            if (px[b0 + i] != 0x456)
            {
                bad++;
            }
        }
        printf("  THIRD COLOUR probe  dibit 00 over a dry PIXELS FIFO: group A 0x%03X, "
               "group B 0x%03X after a COVERED palette reload at slot %u\n",
               px[1], px[b0], b0);
        printf("                      the reload's four disturbed slots are covered by the "
               "cmp_color0 source: %u disturbed pixels reached the DAC\n",
               stats.palette_disturbed_visible);
        CHECK(stats.palette_disturbed_visible == 0,
              "third colour: %u palette-write slots reached the DAC through passthrough — "
              "the cover is not doing its job", stats.palette_disturbed_visible);
        CHECK(stats.palette_bad_filler == 0,
              "third colour: %u palette writes stored the wrong colour",
              stats.palette_bad_filler);
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
              "third colour: %u dibit-00 pixels do not follow palette entry 0 — the model's "
              "passthrough chain has changed", bad);
        CHECK(px[1] != px[b0],
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

// ---------------------------------------------------------------------------
// m11-static-pieces — can ENGINE assemble a sprite line from read-only pieces?
// ---------------------------------------------------------------------------
//
// Derivations, ALL of them done by hand before anything was simulated, and all
// written up in super-engine/STATIC-PIECES.md:
//
//   D1 FRAMING.  A sprite line's program is
//        SET(pix_base) | HOLD(64) | RUN(pt,g) | 8 x 2-slot LITERAL or RUN(pt,2)
//                      | RUN(pt,t) | RUN(cmp_color0,64)
//      with g + t = SP_MIDDLE_PIXELS, so the authored sum is
//      64 + g + 16 + t + 64 = 640.  Every playback record is at least
//      SP_POSE_SPAN = 2 slots, the two-slot fetch cadence: a pose LITERAL is
//      exactly two by LL and a hole RUN(pt,2) two, and each such record is
//      captured on the edge before the one it executes on, so no record can be
//      late and OCCUPANCY == AUTHORED SUM.  (The borders and pose words were
//      RUN_COLOR until 2026-10-03; the word counts, and so D2..D5, are
//      unchanged.)  The leading SET is
//      the FIRST word of the program and the line before it framed at exactly
//      640, so it is consumed in this line's blanking at zero slots.
//
//   D2 HBLANK COST, from descriptor.h's cost model and edma3's split of a
//      wait_hblank descriptor (the 4-word fetch is charged to the PREVIOUS
//      group; only arbitration + assert + payload + release happen at the
//      edge).  Counting SYSCLK from the synchronized HBLANK edge:
//        MONOLITHIC, VIDCMD first  first PIXELS word at 2+5+42+1+13+2 = 65
//        PIECES,     VIDCMD first  first PIXELS word at
//                                  2+5+4+1 +16+2+1 +16+16+1 +16+2+1 +16+2+1
//                                  +13+2 = 105
//        PIECES,     PIXELS first  first PIXELS word at 2+5+2 = 9
//      PIX_PRELOAD is H_BLANK - PIXEL_FETCH_LEAD = 144 pixel clocks after the
//      group start, i.e. floor(144*560/1007) = 80 SYSCLK.  So MONOLITHIC has
//      15 SYSCLK of margin, PIECES-with-lookahead has 71, and PIECES at k == 0
//      MISSES BY 25 — which is the whole reason lookahead is needed.
//
//   D3 PER-LINE BUS COST.  One sprite line's group, arbitration 4:
//        MONOLITHIC  VIDCMD 1 desc x 21 words + PIXELS 2 x 20 = 62 + 108 = 170
//        PIECES      PIXELS 2 x 20 + 5 piece descs = 114 + 96   = 210
//      A plain line is 130 either way.  Both are under the 294-SYSCLK /
//      147-word per-line delivery budget and under the 445-SYSCLK line.
//
//   D4 FIFO.  With lookahead k the FIFO carries the programs of lines
//      y .. y+k, so the peak is about (k+1) x SP_SEMANTIC_WORDS words when the
//      whole window is sprite lines: 117 words at k = 8, against a 256-word
//      FIFO.  Lookahead is bounded by the FIFO long before it is bounded by
//      anything else.
//
//   D5 NEGATIVE CONTROLS.  k == 0 must fail, and it must fail differently
//      depending on the order: VIDCMD-first starves PIXEL (D2: preload missed
//      by 25 SYSCLK), PIXELS-first delivers the line's own VIDCMD words after
//      its pixel 0 and starves COMPOSITOR.

inline constexpr std::array<Rgb444, SP_PALETTE_ENTRIES> SP_PALETTE = {
    rgb444(0, 0, 5),  rgb444(10, 10, 2),    // pair selected by pix_base 0
    rgb444(0, 6, 1),  rgb444(13, 4, 4),     // pair selected by pix_base 2
};

// /RS empties both video FIFOs at line V_SYNC_START, so the list's palette and
// its lookahead bootstrap must be deposited AFTER it.  Pacer j is released at
// the HBLANK edge of line 479+j, so eleven pacers land the preamble in line
// 490's blanking — after that line's h=0 /RS pulse.
constexpr uint32_t SP_PACERS_BEFORE_PREAMBLE = V_SYNC_START - FIRST_ARM_LINE + 1;   // 11
static_assert(SP_PACERS_BEFORE_PREAMBLE < VBLANK_PACING_LINES);

struct SpVariant
{
    std::string      name;
    std::string      title;
    StaticPiecesForm form         = StaticPiecesForm::PIECES;
    uint32_t         lookahead    = 0;
    bool             pixels_first = false;
    bool             expect_clean = true;

    // False puts the palette and the lookahead bootstrap in the vblank BEFORE
    // /RS, which is the third negative control: the pulse empties both video
    // FIFOs, so those words never reach the compositor.
    bool preamble_after_rs = true;
};

struct SpResult
{
    AuthorResult    author[RENDER_FRAMES];
    InterpretResult interp;
    RenderResult    rendered;
    GroupBudget     budget;

    uint32_t vidcmd_words_per_frame = 0;
    uint32_t pixels_words_per_frame = 0;

    // Per-line delivery, measured clean-room off the event stream rather than
    // read out of RenderStats: vidcmd_late_words counts every word that arrives
    // in active video, which under lookahead is BY DESIGN and says nothing about
    // whether a word made its own line's deadline.
    int64_t  vidcmd_min_margin = 0;   // line pixel 0 - last word of that line's program
    uint32_t vidcmd_late_lines = 0;
    int64_t  pixels_min_margin = 0;   // PIX_PRELOAD - first PIXELS word of that line
    uint32_t pixels_late_lines = 0;
    uint32_t stream_mismatch   = 0;   // deposits did not match the authored stream length

    uint32_t sprite_group_sysclk = 0;   // a group that carries a sprite program
    uint32_t plain_group_sysclk  = 0;   // a group that carries none
};

// Walk the deposit stream and price every line's two deadlines.  VIDCMD words
// arrive in stream order, so the prefix sums of the per-line word counts turn a
// stream index back into a line without the checker having to know anything
// about the descriptor layout.
void measure_sp_delivery(const SpVariant &v, SpResult &out)
{
    std::vector<uint32_t> prefix(V_ACTIVE + 1, 0);
    prefix[0] = SP_PALETTE_WORDS;
    for (uint32_t y = 0; y < V_ACTIVE; y++)
    {
        prefix[y + 1] = prefix[y] + sp_line_stream_words(y, v.form);
    }

    out.vidcmd_min_margin = INT64_MAX;
    out.pixels_min_margin = INT64_MAX;

    for (uint32_t f = 0; f < RENDER_FRAMES; f++)
    {
        // One list instance's deposits: it is armed inside line FIRST_ARM_LINE of
        // the frame before the one it feeds and stops just after that frame's
        // line 479, so this window holds exactly one list however its preamble
        // is scheduled.  Deliberately NOT keyed off /RS: a list that deposits
        // before the pulse still puts every word on the bus, and the loss is on
        // the FIFO side — which is why a deposit-time margin cannot see a PN5
        // violation and the k4-pre-rs control has to be caught by hold slots.
        const uint64_t shown  = RENDER_FIRST_FRAME + f;   // raster frame this list feeds
        const uint64_t win_lo =
            sysclk_of_pixel(((shown - 1) * V_TOTAL + FIRST_ARM_LINE) * H_TOTAL);
        const uint64_t win_hi =
            sysclk_of_pixel((shown * V_TOTAL + FIRST_ARM_LINE) * H_TOTAL);

        std::vector<uint64_t> vc;
        std::vector<uint64_t> px;
        for (const DepositEvent &e : out.interp.events)
        {
            if (e.cycle < win_lo || e.cycle >= win_hi)
            {
                continue;
            }
            if (e.destination == DEST_VIDCMD_FIFO_W)
            {
                vc.push_back(e.cycle);
            }
            else if (e.destination == DEST_PIXELS_FIFO_W)
            {
                px.push_back(e.cycle);
            }
        }

        if (vc.size() != prefix[V_ACTIVE] ||
            px.size() != static_cast<size_t>(V_ACTIVE) * PIXELS_WORDS_1BPP)
        {
            out.stream_mismatch++;
            continue;
        }
        out.vidcmd_words_per_frame = static_cast<uint32_t>(vc.size());
        out.pixels_words_per_frame = static_cast<uint32_t>(px.size());

        for (uint32_t y = 0; y < V_ACTIVE; y++)
        {
            const uint64_t pixel0  = sysclk_of_pixel((shown * V_TOTAL + y) * H_TOTAL);
            const uint64_t preload =
                sysclk_of_pixel((shown * V_TOTAL + y) * H_TOTAL - PIXEL_FETCH_LEAD);

            const int64_t vm = static_cast<int64_t>(pixel0) -
                               static_cast<int64_t>(vc[prefix[y + 1] - 1]);
            if (vm < out.vidcmd_min_margin)
            {
                out.vidcmd_min_margin = vm;
            }
            if (vm < 0)
            {
                out.vidcmd_late_lines++;
            }

            const int64_t pm = static_cast<int64_t>(preload) -
                               static_cast<int64_t>(px[y * PIXELS_WORDS_1BPP]);
            if (pm < out.pixels_min_margin)
            {
                out.pixels_min_margin = pm;
            }
            if (pm < 0)
            {
                out.pixels_late_lines++;
            }
        }
    }

    // Which group carries what depends on the lookahead, so read the cost of a
    // group that DOES carry a sprite program and one that does not.
    const uint32_t sprite_group = SP_FIRST_LINE + SP_SPRITE_LINES / 2 - v.lookahead;
    const uint32_t plain_group  = SP_FIRST_LINE / 2;
    const size_t   base         = static_cast<size_t>(RENDER_FIRST_FRAME) * V_TOTAL;
    if (base + sprite_group < out.interp.groups.size())
    {
        out.sprite_group_sysclk = out.interp.groups[base + sprite_group].busy_cycles;
    }
    if (base + plain_group < out.interp.groups.size())
    {
        out.plain_group_sysclk = out.interp.groups[base + plain_group].busy_cycles;
    }
}

SpResult run_static_pieces(const SpVariant &v, Memory ram)
{
    StaticPiecesParams base;
    base.fb_base                = FB_BASE;
    base.fb_stride_bytes        = FB_STRIDE_BYTES;
    base.data_base              = SP_DATA_BASE;
    base.program_base           = SP_PROGRAM_BASE;
    base.form                   = v.form;
    base.lookahead              = v.lookahead;
    base.pixels_first           = v.pixels_first;
    base.vblank_pacing_lines    = VBLANK_PACING_LINES;
    base.pacers_before_preamble = v.preamble_after_rs ? SP_PACERS_BEFORE_PREAMBLE : 0u;

    write_static_pieces_data(base, ram, SP_PALETTE);

    SpResult out;
    const uint32_t  tables[RENDER_FRAMES] = {TABLE_A, TABLE_B};
    InterpretParams ip;
    ip.arbitration_cycles   = base.arbitration_cycles;
    ip.start_cycle          = first_arm_cycle();
    ip.end_cycle            = run_end_cycle();
    ip.rearm_latency_cycles = REARM_LATENCY_CYCLES;

    for (uint32_t f = 0; f < RENDER_FRAMES; f++)
    {
        StaticPiecesParams fp = base;
        fp.table_base  = tables[f];
        fp.table_bytes = TABLE_HALF_BYTES;
        out.author[f]  = author_static_pieces(fp, ram);
        ip.arm_addresses.push_back(tables[f]);
    }

    out.interp = interpret(ram, ip);

    RenderParams rp;
    rp.first_frame = RENDER_FIRST_FRAME;
    rp.frame_count = RENDER_FRAMES;
    out.rendered   = render(out.interp.events, rp);
    out.budget     = group_budget(out.interp);

    measure_sp_delivery(v, out);
    return out;
}

void print_sp_report(const SpVariant &v, const SpResult &r)
{
    const RenderStats &s = r.rendered.stats;
    printf("\n  -- %-10s %s\n", v.name.c_str(), v.title.c_str());
    printf("     descriptors/frame %5u   table bytes %5u/%u   VIDCMD words/frame %u\n",
           r.author[0].descriptor_count, r.author[0].table_bytes, TABLE_HALF_BYTES,
           r.vidcmd_words_per_frame);
    printf("     group SYSCLK      sprite-carrying %3u   plain %3u   worst %3u of %llu "
           "(budget %u)\n",
           r.sprite_group_sysclk, r.plain_group_sysclk, r.budget.max_busy,
           static_cast<unsigned long long>(LINE_SYSCLK), VIDCMD_LINE_SYSCLK_BUDGET);
    printf("     delivery margin   VIDCMD %+6lld SYSCLK (late lines %u)   "
           "PIXELS/PIX_PRELOAD %+6lld SYSCLK (late lines %u)\n",
           static_cast<long long>(r.vidcmd_min_margin), r.vidcmd_late_lines,
           static_cast<long long>(r.pixels_min_margin), r.pixels_late_lines);
    printf("     VIDCMD FIFO       high %3u  low %3u  overflows %u  (depth %u)\n",
           s.vidcmd_fifo_high, s.vidcmd_fifo_low, s.vidcmd_overflows, VIDCMD_FIFO_WORDS);
    printf("     holds/framing     starved %u  cadence %u  overruns %u  "
           "raw late-word counter %u\n",
           s.vidcmd_hold_slots, s.vidcmd_cadence_slots, s.vidcmd_overruns,
           s.vidcmd_late_words);
    printf("     PIXELS            tiled %u  overflows %u  fifo high %3u low %3u\n",
           s.pixels_tiled_words, s.pixels_overflows, s.pixels_fifo_high, s.pixels_fifo_low);
    printf("     palette           writes %u  bad filler %u  disturbed %u  unloaded reads %u\n",
           s.palette_writes, s.palette_bad_filler, s.palette_disturbed_visible,
           s.palette_unwritten_reads);
    for (const std::string &x : r.interp.violations)
    {
        printf("     VIOLATION  %s\n", x.c_str());
    }
}

// Every pixel of every sprite line, against the geometry and nothing else.  The
// borders, the pose spans and the passthrough regions are all placed by hand
// from D1, and the pix_base pair a passthrough pixel must come out of is the
// probe that the line's eager SET executed in ITS OWN blanking.
uint32_t check_sp_geometry(const SpVariant &v, const SpResult &r, uint32_t frame)
{
    const FrameImage &img  = r.rendered.frames[frame];
    uint32_t          bad  = 0;
    const Rgb444      black = RGB444_BLACK;

    for (uint32_t i = 0; i < SP_SPRITE_LINES; i++)
    {
        const uint32_t y   = SP_FIRST_LINE + i;
        const uint32_t g   = sp_gap_for(i);
        const uint32_t x0  = sp_sprite_x(i);
        const uint32_t sel = sp_pix_base_sel(i);
        const Rgb444   p0  = SP_PALETTE[sel * 2 + 0];
        const Rgb444   p1  = SP_PALETTE[sel * 2 + 1];

        for (uint32_t x = 0; x < H_ACTIVE; x++)
        {
            const Rgb444 c = at(img, x, y);
            if (x < SP_BORDER_PIXELS || x >= H_ACTIVE - SP_BORDER_PIXELS)
            {
                bad += (c != black) ? 1u : 0u;
                continue;
            }
            if (x >= x0 && x < x0 + SP_SPRITE_PIXELS)
            {
                const uint32_t j = (x - x0) / SP_POSE_SPAN;
                if (!sp_pose_hole(i, j))
                {
                    bad += (c != sp_pose_color(i, j)) ? 1u : 0u;
                    continue;
                }
            }
            // Gap, tail or a hole in the pose: the playfield shows through, out
            // of the PAIR this line's pix_base SET selected.
            bad += (c != p0 && c != p1) ? 1u : 0u;
        }
        (void)g;
    }

    printf("     geometry          %u of %u sprite-line pixels do not match the hand-placed "
           "borders / pose spans / pix_base pair\n",
           bad, SP_SPRITE_LINES * H_ACTIVE);
    if (v.expect_clean)
    {
        CHECK(bad == 0, "m11-static-pieces/%s: %u sprite-line pixels are wrong",
              v.name.c_str(), bad);
    }
    return bad;
}

uint32_t sp_frame_diff(const FrameImage &a, const FrameImage &b)
{
    uint32_t n = 0;
    for (size_t i = 0; i < a.pixels.size(); i++)
    {
        if (a.pixels[i] != b.pixels[i])
        {
            n++;
        }
    }
    return n;
}

// ---------------------------------------------------------------------------
// m12-cursor-chain — a whole frame's cursor as ONE record chain, over a static
// 320x240 16-colour playfield
// ---------------------------------------------------------------------------
//
// THE QUESTION.  apps/lib/griffin_cursor.h draws a mouse cursor without the
// per-line display list knowing where it is: the chain carries passthrough
// RUNs totalling y*640 + x slots, then one MASK per cursor row separated by
// passthrough RUNs, then a one-slot RUN that holds, then filler SETs.  Since
// RUN's count became ten bits (2026-10-03) the chain is up to 350 words, more
// than the 256-word FIFO, so it is delivered in two FIXED parts: part A (192
// words) in vblank behind the palette and SETs, part B in 32-word chunks
// appended to the line groups of lines 184..188 — griffin_cursor.h derives
// (and static_asserts) that no non-filler part-B word can play before line
// 226 and that the FIFO peaks at <= 233 words.  The list stays static.
// That only works if duration arithmetic really does close across hundreds of
// line boundaries — playback frozen in every HBLANK, RUNs resuming mid-count,
// a MASK landing on its exact slot 300,000 slots after the chain began.
//
// THE EXPECTATION, derived from the laws and NOT from this model's output:
//
//   D1  Slots are active pixel clocks only (compositor.v "SLOTS"), 640 per
//       line, so raster (x, y) is slot y*640 + x of the frame.
//   D2  RUN -> MASK and MASK -> next record are zero-slot seams
//       (MASK_GAP_AFTER_RUN, MASK_GAP_AFTER_MASK), and every RUN in the chain
//       is long enough to keep the two-deep bank full, so no record is late.
//   D3  A mask caught by the H_ACTIVE fall freezes and resumes at the start of
//       the next line (compositor.v "HOLD"); the chain's arithmetic already
//       counts those slots, and griffin_cursor.h forces their dibits to
//       passthrough, so a cursor clipped by the right edge shows nothing on
//       the left edge of the line below.
//
//   Therefore: pixel (X, Y) is the art's colour wherever the art is opaque at
//   (X - x, Y - y), and the playfield's palette colour everywhere else.  Every
//   one of the 307,200 pixels is compared.
//
// The playfield is the mode the Mandelbrot app uses: 4bpp half rate, three
// PIXELS descriptors per line (32 + 32 + 16 words), each framebuffer row shown
// on two scanlines by pointing both lines' descriptors at the same words.
constexpr uint32_t CC_FB_BASE       = 0x300000;
constexpr uint32_t CC_FB_STRIDE     = PIXELS_WORDS_4BPP_HALF * 2;         // 160 bytes
constexpr uint32_t CC_FB_ROWS       = V_ACTIVE / 2;                       // 240
constexpr uint32_t CC_PREAMBLE_BASE = 0x310000;
constexpr uint32_t CC_PREAMBLE_STEP = 0x000400;                           // per frame
constexpr uint32_t CC_FIXED_WORDS   = PALETTE_ENTRIES * PALETTE_WORDS_PER_ENTRY + 4;   // 52
constexpr uint32_t CC_PREAMBLE_WORDS = CC_FIXED_WORDS + GriffinCursor::CHAIN_WORDS;    // 402
constexpr uint32_t CC_VBLANK_WORDS   = CC_FIXED_WORDS + GriffinCursor::PART_A_WORDS;   // 244
constexpr Rgb444   CC_OUTLINE       = rgb444(0x0, 0x0, 0x0);
constexpr Rgb444   CC_FILL          = rgb444(0xF, 0xF, 0xF);

// The suite arms inside line 480; /RS is at the top of line 490.  The preamble
// has to follow /RS, so it sits behind the pacers that walk 480..490.
constexpr uint32_t CC_PACERS_BEFORE_PREAMBLE = V_SYNC_START - FIRST_ARM_LINE + 1;      // 11
constexpr uint32_t CC_PACERS_AFTER_PREAMBLE  = VBLANK_PACING_LINES - CC_PACERS_BEFORE_PREAMBLE;

uint32_t cc_fb_nibble(uint32_t fx, uint32_t fy)
{
    // Changes on every pixel in both directions, so a one-slot or one-line
    // slip of the playfield cannot hide.
    return (fx * 7u + fy * 11u + (fx >> 4) + (fy >> 3)) & 0xFu;
}

Rgb444 cc_palette(uint32_t i)
{
    // Sixteen distinct colours, none of them the cursor's black or white.
    return rgb444(1u + (i * 13u) % 14u, 1u + (i * 5u) % 14u, 1u + (i * 11u) % 14u);
}

void cc_write_framebuffer(Memory ram)
{
    for (uint32_t fy = 0; fy < CC_FB_ROWS; fy++)
    {
        for (uint32_t w = 0; w < PIXELS_WORDS_4BPP_HALF; w++)
        {
            uint16_t word = 0;
            for (uint32_t n = 0; n < 4; n++)
            {
                word = static_cast<uint16_t>((word << 4) | cc_fb_nibble(w * 4 + n, fy));
            }
            ram[CC_FB_BASE + fy * CC_FB_STRIDE + w * 2] = word;
        }
    }
}

struct CcEmitter
{
    Memory   ram;
    uint32_t at    = 0;
    uint32_t count = 0;

    void emit(const Descriptor &d)
    {
        const DescriptorWords w = encode_descriptor(d);
        for (uint32_t i = 0; i < DESC_WORDS; i++)
        {
            ram[at + i * 2] = w.w[i];
        }
        at += DESC_BYTES;
        count++;
    }

    void pacer(bool stop_after)
    {
        Descriptor d;
        d.src         = CC_FB_BASE;
        d.count       = 1;
        d.destination = DEST_NONE;
        d.wait_hblank = true;
        d.stop_after  = stop_after;
        emit(d);
    }

    void stream(uint32_t src, uint32_t words, uint8_t destination, bool wait_first)
    {
        uint32_t done = 0;
        while (done < words)
        {
            const uint32_t take = (words - done > DESC_MAX_COUNT) ? DESC_MAX_COUNT : (words - done);
            Descriptor d;
            d.src         = src + done * 2;
            d.count       = static_cast<uint16_t>(take);
            d.destination = destination;
            d.wait_hblank = wait_first && done == 0;
            emit(d);
            done += take;
        }
    }
};

// One frame's list and preamble.  Returns the descriptor count.  part_b_line
// moves the chain's part B, for the negative control only.
uint32_t cc_author(Memory ram, uint32_t table, uint32_t preamble, uint32_t x, uint32_t y,
                   uint32_t part_b_line = GriffinCursor::PART_B_FIRST_LINE)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < PALETTE_ENTRIES; i++)
    {
        for (uint16_t w : vidcmd_palette_entry(i, cc_palette(i)))
        {
            ram[preamble + (n++) * 2] = w;
        }
    }
    const uint16_t mode = vidcmd_set(SET_PIX_MODE, pixel_mode(PIXEL_DEPTH_4BPP, true));
    ram[preamble + (n++) * 2] = vidcmd_set(SET_PIX_BASE, 0);
    ram[preamble + (n++) * 2] = mode;
    ram[preamble + (n++) * 2] = vidcmd_set(SET_CMP_COLOR0, CC_OUTLINE);
    ram[preamble + (n++) * 2] = vidcmd_set(SET_CMP_COLOR1, CC_FILL);

    uint16_t chain[GriffinCursor::CHAIN_WORDS];
    GriffinCursor::build_chain(chain, GriffinCursor::ARROW, x, y);
    for (uint16_t w : chain)
    {
        ram[preamble + (n++) * 2] = w;
    }

    CcEmitter e{ram, table, 0};
    for (uint32_t i = 0; i < CC_PACERS_BEFORE_PREAMBLE; i++)
    {
        e.pacer(false);
    }
    e.stream(preamble, CC_VBLANK_WORDS, DEST_VIDCMD_FIFO_W, false);
    for (uint32_t i = 0; i < CC_PACERS_AFTER_PREAMBLE; i++)
    {
        e.pacer(false);
    }
    for (uint32_t line = 0; line < V_ACTIVE; line++)
    {
        e.stream(CC_FB_BASE + (line / 2) * CC_FB_STRIDE, PIXELS_WORDS_4BPP_HALF,
                 DEST_PIXELS_FIFO_W, true);
        if (line >= part_b_line && line - part_b_line < GriffinCursor::PART_B_LINES)
        {
            const uint32_t j = line - part_b_line;
            e.stream(preamble + (CC_VBLANK_WORDS + j * GriffinCursor::PART_B_CHUNK) * 2,
                     GriffinCursor::part_b_chunk_words(j), DEST_VIDCMD_FIFO_W, false);
        }
    }
    e.pacer(true);
    return e.count;
}

Rgb444 cc_expected(uint32_t px, uint32_t py, uint32_t x, uint32_t y)
{
    if (px >= x && py >= y)
    {
        const unsigned a = GriffinCursor::art_pixel(GriffinCursor::ARROW_ART, px - x, py - y);
        if (a == 1)
        {
            return CC_OUTLINE;
        }
        if (a == 2)
        {
            return CC_FILL;
        }
    }
    return cc_palette(cc_fb_nibble(px / 2, py / 2));
}

struct CcPosition
{
    uint32_t    x;
    uint32_t    y;
    const char *why;
};

// Frame 1 shows positions[0], frame 2 positions[1] — the two list halves carry
// different chains, which is also what a moving cursor is.
RenderResult cc_run(Memory ram, const CcPosition &a, const CcPosition &b, uint32_t &descriptors,
                    InterpretResult &interp, bool line_at_a_time = false,
                    uint32_t part_b_line = GriffinCursor::PART_B_FIRST_LINE)
{
    const uint32_t  tables[RENDER_FRAMES] = {TABLE_A, TABLE_B};
    const CcPosition *pos[RENDER_FRAMES]  = {&a, &b};
    InterpretParams ip;
    ip.start_cycle          = first_arm_cycle();
    ip.end_cycle            = run_end_cycle();
    ip.rearm_latency_cycles = REARM_LATENCY_CYCLES;
    for (uint32_t f = 0; f < RENDER_FRAMES; f++)
    {
        descriptors = cc_author(ram, tables[f], CC_PREAMBLE_BASE + f * CC_PREAMBLE_STEP,
                                pos[f]->x, pos[f]->y, part_b_line);
        ip.arm_addresses.push_back(tables[f]);
    }
    interp = interpret(ram, ip);

    RenderParams rp;
    rp.first_frame = RENDER_FIRST_FRAME;
    rp.frame_count = RENDER_FRAMES;
    return line_at_a_time ? render_line_at_a_time(interp.events, rp) : render(interp.events, rp);
}

uint32_t cc_mismatches(const FrameImage &img, uint32_t x, uint32_t y)
{
    uint32_t bad = 0;
    for (uint32_t py = 0; py < V_ACTIVE; py++)
    {
        for (uint32_t px = 0; px < H_ACTIVE; px++)
        {
            bad += (at(img, px, py) != cc_expected(px, py, x, y)) ? 1u : 0u;
        }
    }
    return bad;
}

void run_cursor_chain_case(Memory ram)
{
    static_assert(CC_PREAMBLE_WORDS * 2 <= CC_PREAMBLE_STEP);
    static_assert(CC_VBLANK_WORDS < VIDCMD_FIFO_WORDS);

    printf("\n=== m12-cursor-chain — a 640x480 MASK cursor as one record chain (part A in "
           "vblank, part B from line %u) over a static 4bpp half-rate playfield ===\n",
           GriffinCursor::PART_B_FIRST_LINE);
    printf("  preamble %u words (palette %u, SETs 4, chain %u padded at the end); %u in "
           "vblank after %u pacers and /RS, then %u pacers; part B %u words on lines "
           "%u..%u (FIFO bound %u, earliest part-B use line %u)\n",
           CC_PREAMBLE_WORDS, PALETTE_ENTRIES * PALETTE_WORDS_PER_ENTRY,
           GriffinCursor::CHAIN_WORDS, CC_VBLANK_WORDS, CC_PACERS_BEFORE_PREAMBLE,
           CC_PACERS_AFTER_PREAMBLE, GriffinCursor::PART_B_WORDS,
           GriffinCursor::PART_B_FIRST_LINE,
           GriffinCursor::PART_B_FIRST_LINE + GriffinCursor::PART_B_LINES - 1,
           GriffinCursor::PART_B_FIFO_PEAK,
           GriffinCursor::earliest_line_of_word(GriffinCursor::PART_A_WORDS));

    cc_write_framebuffer(ram);

    // Chosen for what each one stresses, not for coverage by volume.
    const CcPosition positions[] = {
        {0, 0, "no leading RUN at all: the first MASK is the first record of the frame"},
        {1, 0, "a ONE-slot leading RUN in front of a MASK"},
        {317, 243, "mid-screen, odd x and odd y: 640-res placement over the 320 playfield"},
        {1023 % 640, 1023 / 640, "leading passthrough is exactly one maximum (10-bit) RUN"},
        {639, 100, "right edge: one visible column, fifteen dibits run onto the next line"},
        {630, 470, "right edge and bottom edge together: rows past 479 not emitted"},
        {5, 479, "last line: a single one-pixel row"},
        {633, 465, "row 15's offset record starts at column 638"},
        {490, 239, "150 full lead RUNs: rows 14, 15 and the closing RUN come in part B"},
        {576, 306, "192 full lead RUNs: row 0's MASK header is part B's first word"},
    };
    const uint32_t count = sizeof(positions) / sizeof(positions[0]);
    static_assert(sizeof(positions) / sizeof(positions[0]) % 2 == 0);

    uint32_t fifo_high       = 0;
    uint32_t line_driver_bad = 0;
    for (uint32_t i = 0; i < count; i += 2)
    {
        uint32_t        descriptors = 0;
        InterpretResult interp;
        const RenderResult r = cc_run(ram, positions[i], positions[i + 1], descriptors, interp);
        const RenderStats &s = r.stats;

        for (uint32_t f = 0; f < RENDER_FRAMES; f++)
        {
            const CcPosition &p   = positions[i + f];
            const uint32_t    bad = cc_mismatches(r.frames[f], p.x, p.y);
            printf("  (%3u,%3u)  %6u of %u pixels wrong   %s\n", p.x, p.y, bad,
                   H_ACTIVE * V_ACTIVE, p.why);
            CHECK(bad == 0, "m12-cursor-chain: cursor at (%u,%u): %u pixels differ from the "
                  "hand-derived frame", p.x, p.y, bad);
        }
        if (s.vidcmd_fifo_high > fifo_high)
        {
            fifo_high = s.vidcmd_fifo_high;
        }

        // The emulator's driver: a line's deposits lumped into its HBLANK, then
        // the line rendered whole.  A chain whose records span hundreds of
        // lines is exactly what could come apart there, and the emulator is
        // where the application actually runs.
        {
            uint32_t        d2 = 0;
            InterpretResult i2;
            const RenderResult e = cc_run(ram, positions[i], positions[i + 1], d2, i2, true);
            for (uint32_t f = 0; f < RENDER_FRAMES; f++)
            {
                const CcPosition &p   = positions[i + f];
                const uint32_t    bad = cc_mismatches(e.frames[f], p.x, p.y);
                CHECK(bad == 0, "m12-cursor-chain: emulator driver, cursor at (%u,%u): %u pixels "
                      "differ from the hand-derived frame", p.x, p.y, bad);
                line_driver_bad += bad;
            }
        }

        CHECK(interp.violations.empty(), "m12-cursor-chain: interpreter reported %zu structural "
              "violations", interp.violations.size());
        CHECK(descriptors * DESC_BYTES <= TABLE_HALF_BYTES,
              "m12-cursor-chain: %u descriptors overflow the %u-byte table half", descriptors,
              TABLE_HALF_BYTES);
        CHECK(s.vidcmd_overflows == 0, "m12-cursor-chain: VIDCMD FIFO overflowed %u times",
              s.vidcmd_overflows);
        CHECK(s.pixels_tiled_words == 0, "m12-cursor-chain: PIXEL tiled its last word %u times",
              s.pixels_tiled_words);
        CHECK(s.pixels_overflows == 0, "m12-cursor-chain: PIXELS FIFO overflowed %u times",
              s.pixels_overflows);
        CHECK(s.palette_unwritten_reads == 0, "m12-cursor-chain: %u pixels came out of palette "
              "entries the list never loaded", s.palette_unwritten_reads);
        CHECK(s.palette_bad_filler == 0, "m12-cursor-chain: %u palette writes stored the wrong "
              "colour", s.palette_bad_filler);
        CHECK(s.palette_disturbed_visible == 0, "m12-cursor-chain: %u palette-write slots "
              "reached the DAC", s.palette_disturbed_visible);
        if (i == 0)
        {
            printf("  list              %u descriptors, %u bytes of a %u-byte half\n",
                   descriptors, descriptors * DESC_BYTES, TABLE_HALF_BYTES);
            const std::string path = "m12-cursor-chain.ppm";
            write_ppm(path, r.frames[1]);
            printf("  artifact          %s\n", path.c_str());
        }
    }
    printf("  emulator driver   %u pixels wrong across all %u positions "
           "(render_line_at_a_time)\n", line_driver_bad, count);
    printf("  VIDCMD FIFO       high water %u of %u words\n", fifo_high, VIDCMD_FIFO_WORDS);
    CHECK(fifo_high < VIDCMD_FIFO_WORDS, "m12-cursor-chain: the vblank chain filled the FIFO");

    // NEGATIVE CONTROLS.  The comparison has to be able to fail, and it has to
    // fail for a slip of ONE slot and of ONE line: author the chain a pixel
    // right, then a line down, and hold each against the unmoved expectation.
    {
        const CcPosition want{317, 243, ""};
        const CcPosition right{318, 243, ""};
        const CcPosition down{317, 244, ""};
        uint32_t        descriptors = 0;
        InterpretResult interp;
        const RenderResult r = cc_run(ram, right, down, descriptors, interp);
        const uint32_t bad_x = cc_mismatches(r.frames[0], want.x, want.y);
        const uint32_t bad_y = cc_mismatches(r.frames[1], want.x, want.y);
        printf("  NEGATIVE CONTROL  chain one slot late: %u pixels wrong   one line late: %u "
               "pixels wrong\n", bad_x, bad_y);
        CHECK(bad_x != 0, "m12-cursor-chain: a one-slot slip went undetected");
        CHECK(bad_y != 0, "m12-cursor-chain: a one-line slip went undetected");
    }

    // The part-B deadline can fail: deliver part B from line 320 instead of
    // 184.  Both cursors' part-B rows (lines 253.. and 306..) are then played
    // before their words exist, so both frames must differ.
    {
        const CcPosition straddle{490, 239, ""};
        const CcPosition first_b{576, 306, ""};
        uint32_t        descriptors = 0;
        InterpretResult interp;
        const RenderResult r = cc_run(ram, straddle, first_b, descriptors, interp, false, 320);
        const uint32_t bad_a = cc_mismatches(r.frames[0], straddle.x, straddle.y);
        const uint32_t bad_b = cc_mismatches(r.frames[1], first_b.x, first_b.y);
        printf("  NEGATIVE CONTROL  part B from line 320: (490,239) %u pixels wrong   "
               "(576,306) %u pixels wrong\n", bad_a, bad_b);
        CHECK(bad_a != 0 && bad_b != 0,
              "m12-cursor-chain: part B delivered past its deadline went undetected");
    }
}

// ---------------------------------------------------------------------------
// m13-set-stream — a literal-pixel stream out of SET(cmp_color1) words, on
// TODAY's COMPOSITOR, with no CPLD change (write-up: SET-STREAM.md)
// ---------------------------------------------------------------------------
//
// WHAT THE 2026-10-03 CHIP CHANGES FOR THIS CASE: nothing it asserts.  Every
// stream here plays under an ORDINARY RUN, whose SETs still wait for the
// terminal and cost their slot plus the cadence slot, so every slot map below
// is unchanged (and all its RUN words, counts <= 640, are bit-for-bit the new
// ten-bit RUN).  What is new is the alternative: under a STREAM RUN the SETs
// cost NO slots — the run's count fixes the geometry and each SET recolours at
// the ordinary cadence inside it (compositor_tb STREAM_FEW; m14 (d)) — and a
// LITERAL row (m14) paints any colour per pixel with no lead-in, exact at every
// x, at one word per pixel.
//
// THE QUESTION.  Played while the source is cmp_color1, does a run of
// SET(cmp_color1) words paint one arbitrary 12-bit colour per word, two slots
// wide?  Every expectation below was derived from the fetch laws (vidcmd-spec.md
// §4.1, compositor.v's header) BEFORE the model was run, and is written as slot
// ranges; the model is held to it pixel for pixel.
//
// The three facts everything follows from:
//   F1  Sustained, a word costs two slots: a one-slot record paints its own slot
//       and one HOLD slot that keeps the source, so a SET's colour shows twice.
//   F2  A record of n slots consumed with NO word parked behind it ("cadence
//       entry": it was itself captured on the previous edge) is followed at +2
//       when n <= 2, and builds a BANKED PAIR when n >= 3: the next record lands
//       on its terminal slot and the one after that a single slot later.
//       Consumed WITH a word parked ("banked entry": a line's first record, or
//       the first half of a pair) it builds the pair already at n >= 2.
//   F3  Therefore exact 2-slot geometry is ONE WORD PER PIXEL, plus one extra
//       word on the first slot of the pair behind every record of >= 3 slots,
//       and that word must be INVISIBLE: a SET of the register not on screen, a
//       SET while passthrough is showing, or the target-7 filler.

constexpr Rgb444 ss_c(uint32_t i)
{
    // Sixteen distinct colours (the red nibble is a bijection on 0..15), none of
    // them the trace's passthrough colour, white or black.
    return rgb444((i * 3u + 1u) & 15u, (i * 7u + 2u) & 15u, (i * 11u + 5u) & 15u);
}

constexpr uint16_t ss_set1(Rgb444 c)
{
    return vidcmd_set(SET_CMP_COLOR1, c);
}

constexpr uint16_t ss_set0(Rgb444 c)
{
    return vidcmd_set(SET_CMP_COLOR0, c);
}

constexpr uint16_t ss_pass(uint32_t n)
{
    return vidcmd_run(RUN_SRC_PASSTHROUGH, n);
}

constexpr uint16_t ss_run1(uint32_t n)
{
    return vidcmd_run(RUN_SRC_COLOR1, n);
}

constexpr uint16_t ss_run0(uint32_t n)
{
    return vidcmd_run(RUN_SRC_COLOR0, n);
}

struct SsPush
{
    uint32_t slot;
    uint16_t word;
};

struct SsTrace
{
    std::vector<Rgb444> px;
    RenderStats         stats;
};

// trace_slots() with the stats attached, plus words deposited DURING the line:
// each SsPush enters the FIFO at the start of its slot, before that slot's
// clock — exactly where render() puts a deposit whose cycle has passed.  After
// the 640th slot H_ACTIVE falls, so a line that over-ran is counted.
SsTrace ss_trace(const std::vector<uint16_t> &words, const std::vector<SsPush> &late = {})
{
    SsTrace        t;
    PixelUnit      pix;
    CompositorUnit cmp;
    pix.power_on_reset();
    cmp.reset();
    palette_load_direct(pix, 1, TRACE_PASSTHROUGH);
    cmp.attach_stats(&t.stats);
    cmp.set_counting(true);
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
        if (i == H_BLANK - PIXEL_FETCH_LEAD)
        {
            pix.arm_preload();
        }
        cmp.blank_clock(pix);
    }
    pix.begin_line();

    size_t next = 0;
    t.px.reserve(H_ACTIVE);
    for (uint32_t s = 0; s < H_ACTIVE; s++)
    {
        while (next < late.size() && late[next].slot <= s)
        {
            cmp.push_word(late[next].word);
            next++;
        }
        t.px.push_back(cmp.active_slot(pix));
    }
    cmp.end_of_line();
    return t;
}

// The hand-derived picture: passthrough everywhere unless painted.
struct SsExpect
{
    std::vector<Rgb444> px = std::vector<Rgb444>(H_ACTIVE, TRACE_PASSTHROUGH);

    SsExpect &paint(uint32_t first, uint32_t last, Rgb444 c)
    {
        for (uint32_t s = first; s <= last; s++)
        {
            px[s] = c;
        }
        return *this;
    }
};

uint32_t ss_diff(const std::vector<Rgb444> &got, const std::vector<Rgb444> &want,
                 uint32_t &first_bad)
{
    uint32_t bad = 0;
    first_bad    = H_ACTIVE;
    for (uint32_t s = 0; s < H_ACTIVE; s++)
    {
        if (got[s] != want[s])
        {
            if (bad == 0)
            {
                first_bad = s;
            }
            bad++;
        }
    }
    return bad;
}

void ss_check(const char *name, const SsTrace &t, const SsExpect &want, uint32_t cadence,
              uint32_t starved, uint32_t overruns)
{
    uint32_t       first = 0;
    const uint32_t bad   = ss_diff(t.px, want.px, first);
    printf("  %-34s %3u slots off   cadence %2u  starvation %2u  overrun %u\n", name, bad,
           t.stats.vidcmd_cadence_slots, t.stats.vidcmd_hold_slots, t.stats.vidcmd_overruns);
    CHECK(bad == 0, "m13 trace %s: %u slots differ from the derivation; first at slot %u "
          "(got 0x%03X, derived 0x%03X)", name, bad, first,
          first < H_ACTIVE ? t.px[first] : 0u, first < H_ACTIVE ? want.px[first] : 0u);
    CHECK(t.stats.vidcmd_cadence_slots == cadence,
          "m13 trace %s: %u cadence holds, derived %u", name, t.stats.vidcmd_cadence_slots,
          cadence);
    CHECK(t.stats.vidcmd_hold_slots == starved,
          "m13 trace %s: %u starvation holds, derived %u", name, t.stats.vidcmd_hold_slots,
          starved);
    CHECK(t.stats.vidcmd_overruns == overruns, "m13 trace %s: %u overruns, derived %u", name,
          t.stats.vidcmd_overruns, overruns);
}

// The trickle schedule of (g): ENGINE delivering one word per 2 SYSCLK from the
// slot the sprite's lead-in SET executes on.  Word n (n = 1, 2, ...) of the
// trickle is deposited 2n SYSCLK later, i.e. ceil(2n * 1007 / 560) pixel clocks.
uint32_t ss_trickle_offset(uint32_t n)
{
    const uint64_t num = 2ull * n * PIXEL_SYSCLK_DEN;
    return static_cast<uint32_t>((num + PIXEL_SYSCLK_NUM - 1) / PIXEL_SYSCLK_NUM);
}

void check_set_stream_traces()
{
    printf("\n=== m13-set-stream traces — SET(cmp_color1) streams against the hand-derived "
           "slot maps ===\n");
    const Rgb444 FG0 = VIDCMD_RESET_COLOR1;   // what cmp_color1 holds after /RS

    // (a) T1 — the naive stream behind a RUN(cmp_color1, 4).  RUN(pass,100) is
    // banked at slot 0, so RUN(c1,4) and SET c0 are its pair: RUN(c1,4) at 100
    // (showing the stale 0xFFF for 4 slots) with SET c0 moved into staging and
    // SET c1 parked by edge 102.  RUN(c1,4) is a banked entry of n >= 2, so its
    // successors are a PAIR: c0 on slot 104 ALONE, c1 on 105.  Then F1: c2 107,
    // c3 109, holds 106/108/110, passthrough 111.  K SETs occupy 2K-1 slots.
    {
        const SsTrace t = ss_trace({ss_pass(100), ss_run1(4), ss_set1(ss_c(0)), ss_set1(ss_c(1)),
                                    ss_set1(ss_c(2)), ss_set1(ss_c(3)), ss_pass(529)});
        SsExpect e;
        e.paint(100, 103, FG0).paint(104, 104, ss_c(0)).paint(105, 106, ss_c(1));
        e.paint(107, 108, ss_c(2)).paint(109, 110, ss_c(3));
        ss_check("a  naive RUN(c1,4) + 4 SETs", t, e, 3, 0, 0);
    }

    // (a) T2 — RUN(c1,1) instead.  Banked entry with n = 1 builds no pair: SET c0
    // lands on 101 with SET c1 only popped on that edge, so 2K slots of stream —
    // but slot 100 shows whatever cmp_color1 held (a STALE slot).
    {
        const SsTrace t = ss_trace({ss_pass(100), ss_run1(1), ss_set1(ss_c(0)), ss_set1(ss_c(1)),
                                    ss_set1(ss_c(2)), ss_set1(ss_c(3)), ss_pass(531)});
        SsExpect e;
        e.paint(100, 100, FG0).paint(101, 102, ss_c(0)).paint(103, 104, ss_c(1));
        e.paint(105, 106, ss_c(2)).paint(107, 108, ss_c(3));
        ss_check("a  naive RUN(c1,1) + 4 SETs", t, e, 4, 0, 0);
    }

    // (a) T3 — THE LEAD-IN: spend the pair on [SET c1=first colour, RUN(c1,2)].
    // SET at 100 is the pair's first half while passthrough still shows, so it is
    // invisible; RUN(c1,2) is the second half (cadence entry, n = 2: no pair
    // behind it), painting c0 on 101-102; c1 103, c2 105, c3 107, passthrough
    // 109 = x0 + 2K.  Every pixel two slots, no stale slot either side.
    {
        const SsTrace t = ss_trace({ss_pass(100), ss_set1(ss_c(0)), ss_run1(2), ss_set1(ss_c(1)),
                                    ss_set1(ss_c(2)), ss_set1(ss_c(3)), ss_pass(531)});
        SsExpect e;
        e.paint(101, 102, ss_c(0)).paint(103, 104, ss_c(1));
        e.paint(105, 106, ss_c(2)).paint(107, 108, ss_c(3));
        ss_check("a  lead-in SET + RUN(c1,2)", t, e, 3, 0, 0);
    }

    // (a) T3b — the same with RUN(c1,1): identical picture, one more cadence hold
    // (slot 102, while SET c1 is still in flight).  RUN(c1,3) would be WRONG: a
    // cadence-entry RUN of 3 builds a pair and the second pixel goes one slot.
    {
        const SsTrace t = ss_trace({ss_pass(100), ss_set1(ss_c(0)), ss_run1(1), ss_set1(ss_c(1)),
                                    ss_set1(ss_c(2)), ss_set1(ss_c(3)), ss_pass(531)});
        SsExpect e;
        e.paint(101, 102, ss_c(0)).paint(103, 104, ss_c(1));
        e.paint(105, 106, ss_c(2)).paint(107, 108, ss_c(3));
        ss_check("a  lead-in SET + RUN(c1,1)", t, e, 4, 0, 0);
    }

    // (g) T4 — the LEFT EDGE.  A line's first record is always banked with the
    // second parked, so the landing slots from pixel 0 are {0, 1, 3, 5, ...} for
    // a 1-slot first record and {0, n, n+1, n+3, ...} for an n-slot one.
    //   x=0, RUN(c1,1): pixel 0 is ONE slot, pixels 1.. at 1, 3, 5.
    {
        const SsTrace t = ss_trace({ss_set1(ss_c(0)), ss_run1(1), ss_set1(ss_c(1)),
                                    ss_set1(ss_c(2)), ss_set1(ss_c(3)), ss_pass(633)});
        SsExpect e;
        e.paint(0, 0, ss_c(0)).paint(1, 2, ss_c(1)).paint(3, 4, ss_c(2)).paint(5, 6, ss_c(3));
        ss_check("g  x=0, eager SET + RUN(c1,1)", t, e, 3, 0, 0);
    }
    //   x=0, RUN(c1,2): pixel 0 two slots, but it builds the pair, so pixel 1 is
    //   ONE slot (2) and pixel 2 starts on 3.  Hence x = 0 cannot have both.
    {
        const SsTrace t = ss_trace({ss_set1(ss_c(0)), ss_run1(2), ss_set1(ss_c(1)),
                                    ss_set1(ss_c(2)), ss_set1(ss_c(3)), ss_pass(633)});
        SsExpect e;
        e.paint(0, 1, ss_c(0)).paint(2, 2, ss_c(1)).paint(3, 4, ss_c(2)).paint(5, 6, ss_c(3));
        ss_check("g  x=0, eager SET + RUN(c1,2)", t, e, 2, 0, 0);
    }
    //   x=1: RUN(pass,1) banked at 0, RUN(c1,2) on 1 (no pair: n = 1), then F1.
    {
        const SsTrace t = ss_trace({ss_set1(ss_c(0)), ss_pass(1), ss_run1(2), ss_set1(ss_c(1)),
                                    ss_set1(ss_c(2)), ss_set1(ss_c(3)), ss_pass(631)});
        SsExpect e;
        e.paint(1, 2, ss_c(0)).paint(3, 4, ss_c(1)).paint(5, 6, ss_c(2)).paint(7, 8, ss_c(3));
        ss_check("g  x=1, eager SET + pass1 + RUN(c1,2)", t, e, 3, 0, 0);
    }
    //   x=3: RUN(pass,2) banked builds the pair, spent on the lead-in (SET on 2).
    {
        const SsTrace t = ss_trace({ss_pass(2), ss_set1(ss_c(0)), ss_run1(2), ss_set1(ss_c(1)),
                                    ss_set1(ss_c(2)), ss_set1(ss_c(3)), ss_pass(629)});
        SsExpect e;
        e.paint(3, 4, ss_c(0)).paint(5, 6, ss_c(1)).paint(7, 8, ss_c(2)).paint(9, 10, ss_c(3));
        ss_check("g  x=3, pass2 + lead-in", t, e, 3, 0, 0);
    }

    // (c) T5a — C C T C (A B . D), cmp_color0 = D preloaded eagerly in HBLANK.
    // RUN(pass,2) on 105 and RUN(c0,2) on 107 are both cadence entries with
    // n = 2, so each is one two-slot pixel and one word; the stream carries on
    // in cmp_color0 (SET target 1: E on 109).  4 words for 4 pixels.
    {
        const Rgb444  A = ss_c(4);
        const Rgb444  B = ss_c(5);
        const Rgb444  D = ss_c(6);
        const Rgb444  E = ss_c(7);
        const SsTrace t = ss_trace({ss_set0(D), ss_pass(100), ss_set1(A), ss_run1(2), ss_set1(B),
                                    ss_pass(2), ss_run0(2), ss_set0(E), ss_pass(529)});
        SsExpect e;
        e.paint(101, 102, A).paint(103, 104, B).paint(107, 108, D).paint(109, 110, E);
        ss_check("c  C C T C, c0 preloaded", t, e, 2, 0, 0);
    }

    // (c) T5b — C C T C where the colour after the hole equals the one before it:
    // re-enter cmp_color1 with RUN(c1,2), no preload needed.
    {
        const Rgb444  A = ss_c(4);
        const Rgb444  B = ss_c(5);
        const Rgb444  E = ss_c(7);
        const SsTrace t = ss_trace({ss_pass(100), ss_set1(A), ss_run1(2), ss_set1(B), ss_pass(2),
                                    ss_run1(2), ss_set1(E), ss_pass(529)});
        SsExpect e;
        e.paint(101, 102, A).paint(103, 104, B).paint(107, 108, B).paint(109, 110, E);
        ss_check("c  C C T C, D == B", t, e, 2, 0, 0);
    }

    // (c) T5c — C T T C (A . . D): RUN(pass,3) on 103 is a cadence entry of 3,
    // so it builds a pair: SET c1=D on 106 is invisible under passthrough and
    // RUN(c1,2) paints D on 107.  3 words for the 3 pixels T T D, no preload.
    {
        const Rgb444  A = ss_c(4);
        const Rgb444  D = ss_c(6);
        const Rgb444  E = ss_c(7);
        const SsTrace t = ss_trace({ss_pass(100), ss_set1(A), ss_run1(2), ss_pass(3), ss_set1(D),
                                    ss_run1(2), ss_set1(E), ss_pass(529)});
        SsExpect e;
        e.paint(101, 102, A).paint(107, 108, D).paint(109, 110, E);
        ss_check("c  C T T C", t, e, 1, 0, 0);
    }

    // (c) T5d — NEGATIVE: C T C with nothing preloaded, loading D inside the
    // hole.  RUN(pass,2) has no pair behind it, so SET D takes 105-106 and the
    // hole is TWO pixels wide, D one pixel late.  This is the derived WRONG
    // picture; the intended one must differ from it.
    {
        const Rgb444  A = ss_c(4);
        const Rgb444  D = ss_c(6);
        const SsTrace t = ss_trace({ss_pass(100), ss_set1(A), ss_run1(2), ss_pass(2), ss_set1(D),
                                    ss_run1(2), ss_pass(531)});
        SsExpect e;
        e.paint(101, 102, A).paint(107, 108, D);
        ss_check("c  C T C, D loaded in the hole (BAD)", t, e, 1, 0, 0);
        SsExpect intended;
        intended.paint(101, 102, A).paint(105, 106, D);
        uint32_t first = 0;
        CHECK(ss_diff(t.px, intended.px, first) != 0,
              "m13 trace: a single hole re-entered with a SET inside it must NOT land as intended");
    }

    // (c) T5e — a same-colour run with a FREE word: A A A B . X.  RUN(c1,5) on
    // 101 (cadence entry, n >= 3) builds a pair on 106/107, so SET c0=X is the
    // invisible first half (cmp_color1 is on screen) and B lands on 107.  The
    // preloaded X then re-enters after the single hole.
    {
        const Rgb444  A = ss_c(8);
        const Rgb444  B = ss_c(9);
        const Rgb444  X = ss_c(10);
        const SsTrace t = ss_trace({ss_pass(100), ss_set1(A), ss_run1(5), ss_set0(X), ss_set1(B),
                                    ss_pass(2), ss_run0(2), ss_pass(527)});
        SsExpect e;
        e.paint(101, 106, A).paint(107, 108, B).paint(111, 112, X);
        ss_check("c  A A A B T X, free preload", t, e, 1, 0, 0);
    }

    // (f) T6 — the other register and the clobber.  cmp_color0 = Q is preset;
    // after a 3-pixel stream RUN(c0,10) still shows Q (the stream never writes
    // target 1), but RUN(c1,10) shows the stream's LAST colour C, not anything a
    // single-colour sprite would have expected.
    {
        const Rgb444  Q = ss_c(11);
        const Rgb444  A = ss_c(12);
        const Rgb444  B = ss_c(13);
        const Rgb444  C = ss_c(14);
        const SsTrace t = ss_trace({ss_set0(Q), ss_pass(100), ss_set1(A), ss_run1(2), ss_set1(B),
                                    ss_set1(C), ss_pass(20), ss_run0(10), ss_pass(20), ss_run1(10),
                                    ss_pass(473)});
        SsExpect e;
        e.paint(101, 102, A).paint(103, 104, B).paint(105, 106, C);
        e.paint(127, 136, Q).paint(157, 166, C);
        ss_check("f  c0 untouched, c1 clobbered", t, e, 2, 0, 0);
    }

    // (f) T6b — the repair: re-SET cmp_color1 as the invisible first half of the
    // pair in front of the RUN, paid for by shortening the passthrough RUN by one.
    {
        const Rgb444  Q = ss_c(11);
        const Rgb444  A = ss_c(12);
        const Rgb444  B = ss_c(13);
        const Rgb444  C = ss_c(14);
        const Rgb444  S = ss_c(15);
        const SsTrace t = ss_trace({ss_set0(Q), ss_pass(100), ss_set1(A), ss_run1(2), ss_set1(B),
                                    ss_set1(C), ss_pass(20), ss_run0(10), ss_pass(19), ss_set1(S),
                                    ss_run1(10), ss_pass(473)});
        SsExpect e;
        e.paint(101, 102, A).paint(103, 104, B).paint(105, 106, C);
        e.paint(127, 136, Q).paint(157, 166, S);
        ss_check("f  c1 re-SET in the pair slot", t, e, 2, 0, 0);
    }

    // (e) T7 — ONE LATE WORD.  The line is in the FIFO up to SET C; SET D, SET E
    // and the exit arrive at the start of slot 140.  C executes on 105 with an
    // empty FIFO, so 106..139 are STARVATION holds keeping cmp_color1 = C.  The
    // push meets a cold /EF synchronizer: no fall on edges 140 and 141, fall on
    // 142, capture 143, D executes on 144 = P + 4.  Behind it the stream is
    // sustained again: E 146, exit 148.  Geometry is NOT kept: every record from
    // D on is 37 slots late, and the line over-runs.
    {
        const SsTrace t = ss_trace({ss_pass(100), ss_set1(ss_c(0)), ss_run1(2), ss_set1(ss_c(1)),
                                    ss_set1(ss_c(2))},
                                   {{140, ss_set1(ss_c(3))}, {140, ss_set1(ss_c(4))},
                                    {140, ss_pass(529)}});
        SsExpect e;
        e.paint(101, 102, ss_c(0)).paint(103, 104, ss_c(1)).paint(105, 143, ss_c(2));
        e.paint(144, 145, ss_c(3)).paint(146, 147, ss_c(4));
        ss_check("e  one late SET (pushed at 140)", t, e, 7, 34, 1);
    }

    // (g) T8 — how much must be BUFFERED when a 16-pixel sprite starts.  Sprite
    // words j = 0..17 (lead-in SET, RUN(c1,2), SET c1..c15, exit) at x0 = 201;
    // word j >= 1 executes on x0 + 2(j-1).  In a sustained stream word j falls
    // on edge x0+2(j-1)-2 only if it was in the FIFO by slot x0+2(j-1)-3 (the
    // /EF synchronizer's meta stage).  With B words buffered and the rest
    // trickled at ENGINE's 2 SYSCLK each from the lead-in's slot, word B-1+n
    // arrives at x0-1+ceil(3.596n):  B = 10 meets every deadline (the last word
    // with one slot to spare); B = 9 misses word 16 (SET c15, x0+28 > x0+27),
    // which then takes the cold path (lands x0+32) and drags the exit to x0+36.
    {
        constexpr uint32_t X0 = 201;
        std::vector<uint16_t> sprite = {ss_set1(ss_c(0)), ss_run1(2)};
        for (uint32_t i = 1; i < 16; i++)
        {
            sprite.push_back(ss_set1(ss_c(i)));
        }
        sprite.push_back(ss_pass(H_ACTIVE - (X0 + 32)));

        for (uint32_t buffered : {10u, 9u})
        {
            std::vector<uint16_t> words = {ss_pass(X0 - 1)};
            std::vector<SsPush>   late;
            for (uint32_t j = 0; j < sprite.size(); j++)
            {
                if (j < buffered)
                {
                    words.push_back(sprite[j]);
                }
                else
                {
                    late.push_back({X0 - 1 + ss_trickle_offset(j - buffered + 1), sprite[j]});
                }
            }
            const SsTrace t = ss_trace(words, late);
            SsExpect e;
            if (buffered == 10)
            {
                for (uint32_t i = 0; i < 16; i++)
                {
                    e.paint(X0 + 2 * i, X0 + 2 * i + 1, ss_c(i));
                }
                ss_check("g  16-px sprite, 10 of 18 buffered", t, e, 15, 0, 0);
            }
            else
            {
                for (uint32_t i = 0; i < 14; i++)
                {
                    e.paint(X0 + 2 * i, X0 + 2 * i + 1, ss_c(i));
                }
                e.paint(X0 + 28, X0 + 31, ss_c(14)).paint(X0 + 32, X0 + 35, ss_c(15));
                ss_check("g  16-px sprite, 9 of 18 buffered", t, e, 19, 0, 1);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// LITERAL / HOLD / KEEP / STREAM RUN — the model against compositor_tb.v
// ---------------------------------------------------------------------------
//
// Every case below is a compositor_tb.v case of the same name (2026-10-03),
// and every expectation is the bench's own hand derivation copied from its
// comment block — NOT read out of the model.  The bench drives RGB_IN as a
// constant 0x9A3, which is TRACE_PASSTHROUGH here, so its spans compare 1:1.
//
// Two places where the model cannot be the bench verbatim, and what is done:
//
//   * A PIXEL-target SET acts on the model's REAL playfield (the bench's
//     RGB_IN is a constant register), so values are chosen that leave
//     passthrough unchanged: pix_base 0, pix_mode 0, pixel_skip 0.  A
//     clut_data SET has no such value, so for it only the KEEP slots are
//     asserted.  STREAM_MIXED likewise writes pix_base 0 / pix_mode 0 where the
//     bench writes 0x123 / 0x001; the timing is the record's, not the value's.
//   * The model's VIDCMD FIFO is the real 256 words (the bench's stub is 4096),
//     so SUSTAINED_LIT's 320 literals go in as 256 before the line plus 64
//     pushed at slot 128, when the FIFO has room; the bench pushes all 640 and
//     consumes 320.
//
// /RS in these traces resets COMPOSITOR only (its FIFO included, as the bench
// zeroes its stub's pointers); PIXEL is left alone so passthrough stays known.

constexpr Rgb444   LT_PF0          = rgb444(3, 12, 5);   // palette entry 0
constexpr uint32_t LT_BEFORE_BLANK = 0xFFFFFFFFu;        // an LtPush slot

struct LtPush
{
    uint32_t line;
    uint32_t slot;   // pushed at the start of this slot, or LT_BEFORE_BLANK
    uint16_t word;
};

struct LtSnap
{
    bool     staged    = false;
    uint32_t src       = 0;
    bool     streaming = false;
    bool     lit       = false;
    Rgb444   c1        = 0;
    Rgb444   c0        = 0;
    Rgb444   clut_b    = 0;   // palette entry 0xB, for STREAM_MIXED
};

struct LtSpec
{
    std::vector<uint16_t> words;           // in the FIFO before line 0's blanking
    std::vector<LtPush>   late;            // in nondecreasing (line, slot) order
    uint32_t              lines     = 1;
    uint16_t              playfield = 0xFFFF;
    int32_t               rs_before = -1;  // /RS just before this line's blanking
    std::vector<uint16_t> after_rs;        // pushed right after that /RS

    // A deeper playfield for the m14 rows: PIXEL's mode, the sixteen-entry
    // TEST_PALETTE instead of entries 0/1, and the PIXELS words of one line.
    uint32_t              pix_mode     = 0;
    bool                  full_palette = false;
    std::vector<uint16_t> pixel_words;
};

struct LtTrace
{
    std::vector<std::vector<Rgb444>> px;
    std::vector<LtSnap>              pre;    // after each line's blanking
    std::vector<LtSnap>              post;   // after each line's H_ACTIVE fall
    LtSnap                           at_rs;  // right after the /RS
    RenderStats                      stats;
};

LtSnap lt_snap(const CompositorUnit &cmp, PixelUnit &pix)
{
    LtSnap s;
    s.staged    = cmp.has_staged();
    s.src       = cmp.cur_src();
    s.streaming = cmp.streaming();
    s.lit       = cmp.literal_active();
    s.c1        = cmp.held_fg();
    s.c0        = cmp.held_bg();
    s.clut_b    = pix.palette().entry(0xB);
    return s;
}

LtTrace lt_trace(const LtSpec &spec)
{
    LtTrace        t;
    PixelUnit      pix;
    CompositorUnit cmp;
    pix.power_on_reset();
    cmp.reset();
    if (spec.full_palette)
    {
        for (uint32_t i = 0; i < PALETTE_ENTRIES; i++)
        {
            palette_load_direct(pix, i, TEST_PALETTE[i]);
        }
    }
    else
    {
        palette_load_direct(pix, 0, LT_PF0);
        palette_load_direct(pix, 1, TRACE_PASSTHROUGH);
    }
    pix.set_register(SET_PIX_MODE, spec.pix_mode);
    cmp.attach_stats(&t.stats);
    pix.attach_stats(&t.stats);
    cmp.set_counting(true);
    pix.set_counting(true);
    for (uint16_t w : spec.words)
    {
        cmp.push_word(w);
    }

    size_t next = 0;
    for (uint32_t line = 0; line < spec.lines; line++)
    {
        if (static_cast<int32_t>(line) == spec.rs_before)
        {
            cmp.reset();
            t.at_rs = lt_snap(cmp, pix);
            for (uint16_t w : spec.after_rs)
            {
                cmp.push_word(w);
            }
        }
        while (next < spec.late.size() && spec.late[next].line == line &&
               spec.late[next].slot == LT_BEFORE_BLANK)
        {
            cmp.push_word(spec.late[next].word);
            next++;
        }
        if (spec.pixel_words.empty())
        {
            for (uint32_t i = 0; i < PIXELS_WORDS_1BPP + 1; i++)
            {
                pix.push_word(spec.playfield);
            }
        }
        else
        {
            for (uint16_t w : spec.pixel_words)
            {
                pix.push_word(w);
            }
        }
        for (uint32_t i = 0; i < H_BLANK; i++)
        {
            if (i == H_BLANK - PIXEL_FETCH_LEAD)
            {
                pix.arm_preload();
            }
            cmp.blank_clock(pix);
        }
        t.pre.push_back(lt_snap(cmp, pix));
        pix.begin_line();
        std::vector<Rgb444> px;
        px.reserve(H_ACTIVE);
        for (uint32_t s = 0; s < H_ACTIVE; s++)
        {
            while (next < spec.late.size() && spec.late[next].line == line &&
                   spec.late[next].slot <= s)
            {
                cmp.push_word(spec.late[next].word);
                next++;
            }
            px.push_back(cmp.active_slot(pix));
        }
        cmp.end_of_line();
        t.post.push_back(lt_snap(cmp, pix));
        t.px.push_back(std::move(px));
    }
    return t;
}

LtTrace lt_trace(const std::vector<uint16_t> &words)
{
    LtSpec spec;
    spec.words = words;
    return lt_trace(spec);
}

// All of [lo, hi] on `line` equal c?  Reports the first offender.
bool lt_span(const LtTrace &t, uint32_t line, uint32_t lo, uint32_t hi, Rgb444 c,
             const char *name)
{
    for (uint32_t s = lo; s <= hi; s++)
    {
        if (t.px[line][s] != c)
        {
            fprintf(stderr, "  %s: line %u slot %u is 0x%03X, derived 0x%03X (span %u..%u)\n",
                    name, line, s, t.px[line][s], c, lo, hi);
            return false;
        }
    }
    return true;
}

uint32_t lt_first_is(const std::vector<Rgb444> &px, Rgb444 c)
{
    for (uint32_t s = 0; s < px.size(); s++)
    {
        if (px[s] == c)
        {
            return s;
        }
    }
    return H_ACTIVE;
}

uint32_t g_lt_cases = 0;

void lt_check(const char *name, bool ok)
{
    g_lt_cases++;
    printf("  %-58s %s\n", name, ok ? "as derived" : "DIFFERS");
    CHECK(ok, "literal trace %s: the model does not play the bench's derivation", name);
}

constexpr uint16_t lt_set1(Rgb444 c) { return vidcmd_set(SET_CMP_COLOR1, c); }
constexpr uint16_t lt_set0(Rgb444 c) { return vidcmd_set(SET_CMP_COLOR0, c); }
constexpr uint16_t lt_pt(uint32_t n) { return vidcmd_run(RUN_SRC_PASSTHROUGH, n); }

// The bench's mask: d1..d14 all one class, d15 another.
VidcmdMask lt_mask(uint32_t body, uint32_t last)
{
    std::array<uint32_t, MASK_SLOTS> d{};
    d[0] = MASK_PIXEL0_DIBIT;
    for (uint32_t i = 1; i < MASK_SLOTS - 1; i++)
    {
        d[i] = body;
    }
    d[MASK_SLOTS - 1] = last;
    return vidcmd_mask(d);
}

void check_literal_traces()
{
    printf("\n=== compositor-literal — LITERAL / HOLD / KEEP / STREAM RUN against "
           "compositor_tb.v's derivations ===\n");
    const Rgb444 PT  = TRACE_PASSTHROUGH;    // the bench's RGB_IN 0x9A3
    const Rgb444 FFF = VIDCMD_RESET_COLOR1;
    const Rgb444 X   = 0x5E7;
    static_assert(TRACE_PASSTHROUGH == 0x9A3);

    // LIT_SPAN: RUN(pt,100) LIT HOLD(98) RUN(pt,440) -> pt 0-99, F00 100-199
    // (N + 2), pt 200-639.
    {
        const LtTrace t = lt_trace({lt_pt(100), vidcmd_literal(0xF00), vidcmd_hold(98), lt_pt(440)});
        lt_check("LIT_SPAN literal + HOLD(98) = slots 100..199",
                 lt_span(t, 0, 0, 99, PT, "LIT_SPAN") && lt_span(t, 0, 100, 199, 0xF00, "LIT_SPAN") &&
                 lt_span(t, 0, 200, H_ACTIVE - 1, PT, "LIT_SPAN"));
    }

    // LIT_SPANS8: eight LITERAL + HOLD(78), every span exactly 80 slots.
    {
        std::vector<uint16_t> w;
        for (uint32_t i = 0; i < 8; i++)
        {
            w.push_back(vidcmd_literal(static_cast<Rgb444>((0x123 + i * 0x1F1) & 0xFFF)));
            w.push_back(vidcmd_hold(78));
        }
        const LtTrace t = lt_trace(w);
        bool ok = true;
        for (uint32_t i = 0; i < 8; i++)
        {
            ok = ok && lt_span(t, 0, i * 80, i * 80 + 79,
                               static_cast<Rgb444>((0x123 + i * 0x1F1) & 0xFFF), "LIT_SPANS8");
        }
        lt_check("LIT_SPANS8 eight 80-slot spans in any colour", ok);
    }

    // LIT_ABC: three literals banked in HBLANK -> A 0-1, B 2-3, C 4-5, pt 6.
    // The banked pair at the head does NOT make A and B adjacent (LL).
    {
        const LtTrace t = lt_trace({vidcmd_literal(0xA11), vidcmd_literal(0xB22),
                                    vidcmd_literal(0xC33), lt_pt(634)});
        lt_check("LIT_ABC A 0-1, B 2-3, C 4-5, pt from 6",
                 lt_span(t, 0, 0, 1, 0xA11, "LIT_ABC") && lt_span(t, 0, 2, 3, 0xB22, "LIT_ABC") &&
                 lt_span(t, 0, 4, 5, 0xC33, "LIT_ABC") &&
                 lt_span(t, 0, 6, H_ACTIVE - 1, PT, "LIT_ABC") &&
                 t.post[0].c1 == FFF && t.post[0].c0 == VIDCMD_RESET_COLOR0);
    }

    // SUSTAINED_LIT: alternating literals for a whole line; record k paints
    // 2k and 2k+1, 320 records, no irregularity at the head.
    {
        LtSpec spec;
        for (uint32_t i = 0; i < VIDCMD_FIFO_WORDS; i++)
        {
            spec.words.push_back(vidcmd_literal((i % 2) == 0 ? 0xF00 : 0x0F0));
        }
        for (uint32_t i = VIDCMD_FIFO_WORDS; i < H_ACTIVE / 2; i++)
        {
            spec.late.push_back({0, 128, vidcmd_literal((i % 2) == 0 ? 0xF00 : 0x0F0)});
        }
        const LtTrace t = lt_trace(spec);
        uint32_t off = 0;
        for (uint32_t s = 0; s < H_ACTIVE; s++)
        {
            off += (t.px[0][s] != (((s / 2) % 2) == 0 ? 0xF00 : 0x0F0)) ? 1u : 0u;
        }
        lt_check("SUSTAINED_LIT literal pitch 2 for all 640 slots", off == 0);
    }

    // LIT_PAIR_TAIL: RUN(pt,4) SET(c0) LIT RUN(pt) -> pt 0-4, X 5-6, pt 7.
    {
        const LtTrace t = lt_trace({lt_pt(4), lt_set0(0x00C), vidcmd_literal(X), lt_pt(600)});
        lt_check("LIT_PAIR_TAIL a literal as a pair's 2nd: pt 0-4, X 5-6, pt 7",
                 lt_span(t, 0, 0, 4, PT, "LIT_PAIR_TAIL") && lt_span(t, 0, 5, 6, X, "LIT_PAIR_TAIL") &&
                 lt_span(t, 0, 7, H_ACTIVE - 1, PT, "LIT_PAIR_TAIL") && t.post[0].c0 == 0x00C);
    }

    // LIT_BEHIND_RUN: RUN(pt,4) A B C RUN(pt) -> A 4-5, B 6-7, C 8-9, pt 10.
    {
        const LtTrace t = lt_trace({lt_pt(4), vidcmd_literal(0xA11), vidcmd_literal(0xB22),
                                    vidcmd_literal(0xC33), lt_pt(600)});
        const uint32_t pitch = lt_first_is(t.px[0], 0xB22) - lt_first_is(t.px[0], 0xA11);
        lt_check("LIT_BEHIND_RUN pt 0-3, A 4-5, B 6-7, C 8-9, pt 10 (pair pitch 2)",
                 lt_span(t, 0, 0, 3, PT, "LIT_BEHIND_RUN") &&
                 lt_span(t, 0, 4, 5, 0xA11, "LIT_BEHIND_RUN") &&
                 lt_span(t, 0, 6, 7, 0xB22, "LIT_BEHIND_RUN") &&
                 lt_span(t, 0, 8, 9, 0xC33, "LIT_BEHIND_RUN") &&
                 lt_span(t, 0, 10, H_ACTIVE - 1, PT, "LIT_BEHIND_RUN") &&
                 pitch == VIDCMD_LITERAL_PITCH);
    }

    // LIT_NEXT_*: RUN(pt,4) X R — R executes at slot 6 whatever it is (LL); a
    // SET there shows KEEP in its own slot and the cadence slot, so the record
    // behind it is at 8.
    for (uint32_t k = 0; k < 15; k++)
    {
        std::vector<uint16_t> w = {lt_set1(0x0C1), lt_set0(0x0C0), lt_pt(4), vidcmd_literal(X)};
        const VidcmdMask m = lt_mask(MASK_DIBIT_COLOR1, MASK_DIBIT_PASSTHROUGH);
        switch (k)
        {
            case 0:  w.push_back(lt_pt(600)); break;
            case 1:  w.push_back(vidcmd_run(RUN_SRC_COLOR1, 600)); break;
            case 2:  w.push_back(vidcmd_run(RUN_SRC_COLOR0, 600)); break;
            case 3:  w.push_back(vidcmd_stream_run(RUN_SRC_COLOR1, 600)); break;
            case 4:  w.push_back(vidcmd_hold(10)); w.push_back(lt_pt(600)); break;
            case 5:  w.push_back(m.header); w.push_back(m.data); break;
            case 6:  w.push_back(lt_set1(0x777)); w.push_back(vidcmd_run(RUN_SRC_COLOR1, 600)); break;
            case 7:  w.push_back(lt_set0(0x777)); w.push_back(vidcmd_run(RUN_SRC_COLOR0, 600)); break;
            case 8:  w.push_back(vidcmd_set(SET_PIX_CLUT_INDEX, 0x001)); w.push_back(lt_pt(600)); break;
            case 9:  w.push_back(vidcmd_set(SET_PIX_CLUT_DATA, 0x777)); w.push_back(lt_pt(600)); break;
            case 10: w.push_back(vidcmd_set(SET_PIX_BASE, 0)); w.push_back(lt_pt(600)); break;
            case 11: w.push_back(vidcmd_set(SET_PIX_MODE, 0)); w.push_back(lt_pt(600)); break;
            case 12: w.push_back(vidcmd_set(SET_PIX_PIXEL_SKIP, 0)); w.push_back(lt_pt(600)); break;
            case 13: w.push_back(vidcmd_set(SET_FILLER, 0x777)); w.push_back(lt_pt(600)); break;
            default: w.push_back(vidcmd_literal(0x6F8)); w.push_back(lt_pt(600)); break;
        }
        const LtTrace t = lt_trace(w);
        const char   *n = "LIT_NEXT";
        bool          ok = lt_span(t, 0, 0, 3, PT, n);
        char          name[80];
        switch (k)
        {
            case 0:
                ok = ok && lt_span(t, 0, 4, 5, X, n) && lt_span(t, 0, 6, H_ACTIVE - 1, PT, n);
                snprintf(name, sizeof(name), "LIT_NEXT RUN(pt): X 4-5, pt from 6");
                break;
            case 1:
                ok = ok && lt_span(t, 0, 4, 5, X, n) && lt_span(t, 0, 6, H_ACTIVE - 1, 0x0C1, n);
                snprintf(name, sizeof(name), "LIT_NEXT RUN(c1): X 4-5, c1 from 6");
                break;
            case 2:
                ok = ok && lt_span(t, 0, 4, 5, X, n) && lt_span(t, 0, 6, H_ACTIVE - 1, 0x0C0, n);
                snprintf(name, sizeof(name), "LIT_NEXT RUN(c0): X 4-5, c0 from 6");
                break;
            case 3:
                ok = ok && lt_span(t, 0, 4, 5, X, n) && lt_span(t, 0, 6, H_ACTIVE - 1, 0x0C1, n);
                snprintf(name, sizeof(name), "LIT_NEXT STREAM(c1): X 4-5, c1 from 6");
                break;
            case 4:
                ok = ok && lt_span(t, 0, 4, 15, X, n) && lt_span(t, 0, 16, H_ACTIVE - 1, PT, n);
                snprintf(name, sizeof(name), "LIT_NEXT HOLD(10): X 4-15, pt from 16");
                break;
            case 5:
                ok = ok && lt_span(t, 0, 4, 5, X, n) && lt_span(t, 0, 6, 6, 0x0C0, n) &&
                     lt_span(t, 0, 7, 20, 0x0C1, n) && lt_span(t, 0, 21, H_ACTIVE - 1, PT, n);
                snprintf(name, sizeof(name), "LIT_NEXT MASK: X 4-5, c0 6, c1 7-20, pt/KEEP from 21");
                break;
            case 6:
            case 7:
                ok = ok && lt_span(t, 0, 4, 7, X, n) && lt_span(t, 0, 8, H_ACTIVE - 1, 0x777, n);
                snprintf(name, sizeof(name), "LIT_NEXT SET %s: X 4-7, new colour from 8",
                         k == 6 ? "c1" : "c0");
                break;
            case 9:
                // No value of clut_data leaves the playfield alone: KEEP only.
                ok = ok && lt_span(t, 0, 4, 7, X, n);
                snprintf(name, sizeof(name), "LIT_NEXT SET clut_data: X 4-7 (SET slot + cadence KEEP)");
                break;
            case 8:
            case 10:
            case 11:
            case 12:
            case 13:
                ok = ok && lt_span(t, 0, 4, 7, X, n) && lt_span(t, 0, 8, H_ACTIVE - 1, PT, n);
                snprintf(name, sizeof(name), "LIT_NEXT SET target %u: X 4-7, pt from 8", k - 6);
                break;
            default:
                ok = ok && lt_span(t, 0, 4, 5, X, n) && lt_span(t, 0, 6, 7, 0x6F8, n) &&
                     lt_span(t, 0, 8, H_ACTIVE - 1, PT, n) &&
                     lt_first_is(t.px[0], 0x6F8) - lt_first_is(t.px[0], X) == VIDCMD_LITERAL_NEXT_GAP;
                snprintf(name, sizeof(name), "LIT_NEXT LIT: X 4-5, Y 6-7, pt from 8");
                break;
        }
        lt_check(name, ok);
    }

    // LIT_HOLD_n: RUN(pt,4) X HOLD(n) RUN(pt) — width max(n + 2, 4).
    for (uint32_t n : {1u, 2u, 100u})
    {
        const LtTrace t = lt_trace({lt_pt(4), vidcmd_literal(X), vidcmd_hold(n), lt_pt(600)});
        const uint32_t width = (n + 2 > VIDCMD_LIT_HOLD_MIN_SLOTS) ? n + 2 : VIDCMD_LIT_HOLD_MIN_SLOTS;
        char name[80];
        snprintf(name, sizeof(name), "LIT_HOLD_%u X 4-%u, pt from %u", n, 4 + width - 1, 4 + width);
        lt_check(name, lt_span(t, 0, 0, 3, PT, "LIT_HOLD") &&
                       lt_span(t, 0, 4, 4 + width - 1, X, "LIT_HOLD") &&
                       lt_span(t, 0, 4 + width, H_ACTIVE - 1, PT, "LIT_HOLD"));
    }

    // HOLD_AFTER_PT: KEEP repeats the last passthrough PIXEL.  The playfield
    // alternates entries 0/1 (0x5555, pixel 0 = bit 15 = 0), so slots 0..3
    // differ pairwise and 4..8 must all equal slot 3; RUN(c1) from 9.
    {
        LtSpec spec;
        spec.words     = {lt_pt(4), vidcmd_hold(5), vidcmd_run(RUN_SRC_COLOR1, 600)};
        spec.playfield = 0x5555;
        const LtTrace t = lt_trace(spec);
        lt_check("HOLD_AFTER_PT KEEP repeats slot 3 on 4-8, c1 from 9",
                 t.px[0][0] == LT_PF0 && t.px[0][1] == PT && t.px[0][2] == LT_PF0 &&
                 t.px[0][3] == PT && lt_span(t, 0, 4, 8, t.px[0][3], "HOLD_AFTER_PT") &&
                 lt_span(t, 0, 9, H_ACTIVE - 1, FFF, "HOLD_AFTER_PT"));
    }

    // HOLD_AFTER_MASK: KEEP repeats the mask's LAST pixel: mask 4-19 (d15 c0),
    // KEEP 0C0 on 20-23, pt from 24.
    {
        const VidcmdMask m = lt_mask(MASK_DIBIT_COLOR1, MASK_DIBIT_COLOR0);
        const LtTrace t = lt_trace({lt_set1(0x0C1), lt_set0(0x0C0), lt_pt(4), m.header, m.data,
                                    vidcmd_hold(4), lt_pt(600)});
        lt_check("HOLD_AFTER_MASK mask 4-19, KEEP of px15 on 20-23, pt from 24",
                 lt_span(t, 0, 4, 4, 0x0C0, "HOLD_AFTER_MASK") &&
                 lt_span(t, 0, 5, 18, 0x0C1, "HOLD_AFTER_MASK") &&
                 lt_span(t, 0, 19, 23, 0x0C0, "HOLD_AFTER_MASK") &&
                 lt_span(t, 0, 24, H_ACTIVE - 1, PT, "HOLD_AFTER_MASK"));
    }

    // HOLD_LINE_START / KEEP_ACROSS_BLANK: KEEP at a line's start is black; a
    // trailing literal holds to the line end; the unfilled line after it is
    // black, and the source is still KEEP.
    {
        LtSpec spec;
        spec.words = {vidcmd_hold(10), lt_pt(600)};
        spec.lines = 3;
        spec.late  = {{1, LT_BEFORE_BLANK, lt_pt(4)}, {1, LT_BEFORE_BLANK, vidcmd_literal(X)}};
        const LtTrace t = lt_trace(spec);
        lt_check("HOLD_LINE_START KEEP at slot 0 is black for 10 slots, pt from 10",
                 lt_span(t, 0, 0, 9, RGB444_BLACK, "HOLD_LINE_START") &&
                 lt_span(t, 0, 10, H_ACTIVE - 1, PT, "HOLD_LINE_START"));
        lt_check("KEEP_ACROSS_BLANK trailing literal to line end; next line black, src KEEP",
                 lt_span(t, 1, 0, 3, PT, "KEEP_ACROSS_BLANK") &&
                 lt_span(t, 1, 4, H_ACTIVE - 1, X, "KEEP_ACROSS_BLANK") &&
                 lt_span(t, 2, 0, H_ACTIVE - 1, RGB444_BLACK, "KEEP_ACROSS_BLANK") &&
                 t.post[2].src == RUN_SRC_KEEP);
    }

    // STREAM_FEW: three SETs of cmp_color1 inside STREAM(c1,20) -> FFF 0,
    // A 1-2, B 3-4, C 5-19, 00F from 20 exactly.
    {
        const LtTrace t = lt_trace({lt_set0(0x00F), vidcmd_stream_run(RUN_SRC_COLOR1, 20),
                                    lt_set1(0xA11), lt_set1(0xB22), lt_set1(0xC33),
                                    vidcmd_run(RUN_SRC_COLOR0, 600)});
        const uint32_t first = lt_first_is(t.px[0], 0xA11);
        const uint32_t pitch = lt_first_is(t.px[0], 0xB22) - first;
        lt_check("STREAM_FEW FFF 0, A 1-2, B 3-4, C 5-19, c0 from 20 (first +1, pitch 2)",
                 lt_span(t, 0, 0, 0, FFF, "STREAM_FEW") && lt_span(t, 0, 1, 2, 0xA11, "STREAM_FEW") &&
                 lt_span(t, 0, 3, 4, 0xB22, "STREAM_FEW") && lt_span(t, 0, 5, 19, 0xC33, "STREAM_FEW") &&
                 lt_span(t, 0, 20, H_ACTIVE - 1, 0x00F, "STREAM_FEW") &&
                 first == VIDCMD_STREAM_FIRST_SET && pitch == VIDCMD_STREAM_SET_PITCH &&
                 t.stats.vidcmd_stream_leftovers == 0);
    }

    // STREAM_ORDINARY: the same under an ordinary RUN: A 20, B 21-22, C 23-24,
    // c0 25.
    {
        const LtTrace t = lt_trace({lt_set0(0x00F), vidcmd_run(RUN_SRC_COLOR1, 20), lt_set1(0xA11),
                                    lt_set1(0xB22), lt_set1(0xC33), vidcmd_run(RUN_SRC_COLOR0, 600)});
        lt_check("STREAM_ORDINARY FFF 0-19, A 20, B 21-22, C 23-24, c0 from 25",
                 lt_span(t, 0, 0, 19, FFF, "STREAM_ORDINARY") &&
                 lt_span(t, 0, 20, 20, 0xA11, "STREAM_ORDINARY") &&
                 lt_span(t, 0, 21, 22, 0xB22, "STREAM_ORDINARY") &&
                 lt_span(t, 0, 23, 24, 0xC33, "STREAM_ORDINARY") &&
                 lt_span(t, 0, 25, H_ACTIVE - 1, 0x00F, "STREAM_ORDINARY"));
    }

    // STREAM_MORE: five SETs in STREAM(c1,6): A 1-2, B 3-4, C 5-6, leftovers
    // D 7-8, E 9-10, c0 from 11 — the line is five slots long.
    {
        const LtTrace t = lt_trace({lt_set0(0x00F), vidcmd_stream_run(RUN_SRC_COLOR1, 6),
                                    lt_set1(0xA11), lt_set1(0xB22), lt_set1(0xC33), lt_set1(0xD44),
                                    lt_set1(0xE55), vidcmd_run(RUN_SRC_COLOR0, 629)});
        lt_check("STREAM_MORE A 1-2 B 3-4 C 5-6, leftovers D 7-8 E 9-10, c0 from 11",
                 lt_span(t, 0, 0, 0, FFF, "STREAM_MORE") && lt_span(t, 0, 1, 2, 0xA11, "STREAM_MORE") &&
                 lt_span(t, 0, 3, 4, 0xB22, "STREAM_MORE") && lt_span(t, 0, 5, 6, 0xC33, "STREAM_MORE") &&
                 lt_span(t, 0, 7, 8, 0xD44, "STREAM_MORE") && lt_span(t, 0, 9, 10, 0xE55, "STREAM_MORE") &&
                 lt_span(t, 0, 11, H_ACTIVE - 1, 0x00F, "STREAM_MORE"));
        lt_check("STREAM_MORE the model counts the two LS5 leftovers as a fault",
                 t.stats.vidcmd_stream_leftovers == 2);
    }

    // STREAM_MORE2: the leftovers ahead of a second stream run whose own SETs
    // stream: E 9-10, 00F 11-12, F 13-14, G 15-20, pt from 21.
    {
        const LtTrace t = lt_trace({lt_set0(0x00F), vidcmd_stream_run(RUN_SRC_COLOR1, 6),
                                    lt_set1(0xA11), lt_set1(0xB22), lt_set1(0xC33), lt_set1(0xD44),
                                    lt_set1(0xE55), vidcmd_stream_run(RUN_SRC_COLOR0, 10),
                                    lt_set0(0xF66), lt_set0(0x677), lt_pt(600)});
        lt_check("STREAM_MORE2 E 9-10, c0 11-12, F 13-14, G 15-20, pt from 21",
                 lt_span(t, 0, 9, 10, 0xE55, "STREAM_MORE2") &&
                 lt_span(t, 0, 11, 12, 0x00F, "STREAM_MORE2") &&
                 lt_span(t, 0, 13, 14, 0xF66, "STREAM_MORE2") &&
                 lt_span(t, 0, 15, 20, 0x677, "STREAM_MORE2") &&
                 lt_span(t, 0, 21, H_ACTIVE - 1, PT, "STREAM_MORE2"));
    }

    // STREAM_THEN_ORDINARY: the flag does not outlive its run: FFF 0, A 1-5,
    // 00F 6-15, V 16, pt from 17.
    {
        const LtTrace t = lt_trace({lt_set0(0x00F), vidcmd_stream_run(RUN_SRC_COLOR1, 6),
                                    lt_set1(0xA11), vidcmd_run(RUN_SRC_COLOR0, 10), lt_set0(0x0D0),
                                    lt_pt(600)});
        lt_check("STREAM_THEN_ORDINARY A 1-5, c0 6-15, V at 16 only, pt from 17",
                 lt_span(t, 0, 0, 0, FFF, "STREAM_THEN_ORDINARY") &&
                 lt_span(t, 0, 1, 5, 0xA11, "STREAM_THEN_ORDINARY") &&
                 lt_span(t, 0, 6, 15, 0x00F, "STREAM_THEN_ORDINARY") &&
                 lt_span(t, 0, 16, 16, 0x0D0, "STREAM_THEN_ORDINARY") &&
                 lt_span(t, 0, 17, H_ACTIVE - 1, PT, "STREAM_THEN_ORDINARY"));
    }

    // STREAM_MIXED: cmp_color0, pix_base, pix_mode and a palette entry inside
    // STREAM(c1,40): colour1 for all 40 slots, pt from 40; the entry is stored
    // with the right value and its disturbed window is covered by colour1.
    {
        const PaletteEntryWords e = vidcmd_palette_entry(0xB, 0x5A5);
        const LtTrace t = lt_trace({vidcmd_stream_run(RUN_SRC_COLOR1, 40), lt_set0(0x0AB),
                                    vidcmd_set(SET_PIX_BASE, 0), vidcmd_set(SET_PIX_MODE, 0),
                                    e.w[0], e.w[1], e.w[2], lt_pt(600)});
        lt_check("STREAM_MIXED c1 0-39, pt from 40, c0 and a palette entry written, covered",
                 lt_span(t, 0, 0, 39, FFF, "STREAM_MIXED") &&
                 lt_span(t, 0, 40, H_ACTIVE - 1, PT, "STREAM_MIXED") && t.post[0].c0 == 0x0AB &&
                 t.post[0].clut_b == 0x5A5 && t.stats.palette_bad_filler == 0 &&
                 t.stats.palette_disturbed_visible == 0);
    }

    // LATE_IN_STREAM vs LATE_LITERAL: a word pushed 10 slots into the line
    // executes at slot 14.  In the stream run the colour is late and the
    // geometry is not (c0 from 40); after a late literal everything shifts
    // (c0 from 16 instead of 6).
    {
        LtSpec s1;
        s1.words = {lt_set0(0x00F), vidcmd_stream_run(RUN_SRC_COLOR1, 40), lt_set1(0xA11)};
        s1.late  = {{0, 10, lt_set1(0xB22)}, {0, 10, vidcmd_run(RUN_SRC_COLOR0, 600)}};
        const LtTrace t1 = lt_trace(s1);
        lt_check("LATE_IN_STREAM A 1-13, late B 14-39, geometry intact: c0 from 40",
                 lt_span(t1, 0, 0, 0, FFF, "LATE_IN_STREAM") &&
                 lt_span(t1, 0, 1, 13, 0xA11, "LATE_IN_STREAM") &&
                 lt_span(t1, 0, 14, 39, 0xB22, "LATE_IN_STREAM") &&
                 lt_span(t1, 0, 40, H_ACTIVE - 1, 0x00F, "LATE_IN_STREAM"));
        LtSpec s2;
        s2.words = {lt_set0(0x00F), lt_pt(4), vidcmd_literal(0xA11)};
        s2.late  = {{0, 10, vidcmd_literal(0xB22)}, {0, 10, vidcmd_run(RUN_SRC_COLOR0, 600)}};
        const LtTrace t2 = lt_trace(s2);
        lt_check("LATE_LITERAL A 4-13 (KEEP while starved), B 14-15, c0 from 16",
                 lt_span(t2, 0, 0, 3, PT, "LATE_LITERAL") &&
                 lt_span(t2, 0, 4, 13, 0xA11, "LATE_LITERAL") &&
                 lt_span(t2, 0, 14, 15, 0xB22, "LATE_LITERAL") &&
                 lt_span(t2, 0, 16, H_ACTIVE - 1, 0x00F, "LATE_LITERAL"));
    }

    // BLANK_LIT / BLANK_STREAM: playback-class records block eagerness.
    {
        const LtTrace t1 = lt_trace({vidcmd_literal(X), lt_set1(0x0D1), vidcmd_run(RUN_SRC_COLOR1, 600)});
        lt_check("BLANK_LIT SET behind a staged literal not eager; X 0-3, V from 4",
                 t1.pre[0].c1 == FFF && lt_span(t1, 0, 0, 3, X, "BLANK_LIT") &&
                 lt_span(t1, 0, 4, H_ACTIVE - 1, 0x0D1, "BLANK_LIT"));
        const LtTrace t2 = lt_trace({vidcmd_stream_run(RUN_SRC_COLOR1, 10), lt_set1(0x0D1)});
        lt_check("BLANK_STREAM SET behind a staged stream run not eager; FFF 0, V from 1",
                 t2.pre[0].c1 == FFF && lt_span(t2, 0, 0, 0, FFF, "BLANK_STREAM") &&
                 lt_span(t2, 0, 1, H_ACTIVE - 1, 0x0D1, "BLANK_STREAM"));
    }

    // EXACT640_NEW: every new record in one exact-640 line, then a cushioned
    // next line staged at the H_ACTIVE fall.
    {
        LtSpec spec;
        spec.words = {lt_pt(100), vidcmd_literal(X), vidcmd_hold(98),
                      vidcmd_stream_run(RUN_SRC_COLOR1, 300), lt_set1(0xA11), lt_set1(0xB22),
                      vidcmd_run(RUN_SRC_COLOR0, 140), vidcmd_run(RUN_SRC_COLOR1, 640)};
        spec.lines = 2;
        const LtTrace t = lt_trace(spec);
        lt_check("EXACT640_NEW pt 0-99, X 100-199, stream 200-499 (V1 201, V2 203), c0 500-639",
                 lt_span(t, 0, 0, 99, PT, "EXACT640_NEW") && lt_span(t, 0, 100, 199, X, "EXACT640_NEW") &&
                 lt_span(t, 0, 200, 200, FFF, "EXACT640_NEW") &&
                 lt_span(t, 0, 201, 202, 0xA11, "EXACT640_NEW") &&
                 lt_span(t, 0, 203, 499, 0xB22, "EXACT640_NEW") &&
                 lt_span(t, 0, 500, H_ACTIVE - 1, RGB444_BLACK, "EXACT640_NEW"));
        lt_check("EXACT640_NEW the next line's RUN is staged at the fall and starts at slot 0",
                 t.post[0].staged && lt_span(t, 1, 0, H_ACTIVE - 1, 0xB22, "EXACT640_NEW"));
    }

    // NRS_KEEP / NRS_STREAM: /RS clears KEEP, the literal's bank hold and the
    // streaming flag.
    {
        LtSpec spec;
        spec.words     = {lt_pt(4), vidcmd_literal(X)};
        spec.lines     = 2;
        spec.rs_before = 1;
        const LtTrace t = lt_trace(spec);
        lt_check("NRS_KEEP src passthrough, no bank hold, no stream; unfilled frame passthrough",
                 t.at_rs.src == RUN_SRC_PASSTHROUGH && !t.at_rs.lit && !t.at_rs.streaming &&
                 lt_span(t, 1, 0, H_ACTIVE - 1, PT, "NRS_KEEP"));
        LtSpec s2;
        s2.words     = {vidcmd_stream_run(RUN_SRC_COLOR1, 600)};
        s2.lines     = 2;
        s2.rs_before = 1;
        s2.after_rs  = {vidcmd_run(RUN_SRC_COLOR1, 10), lt_set1(0x0D1), vidcmd_run(RUN_SRC_COLOR1, 600)};
        const LtTrace t2 = lt_trace(s2);
        lt_check("NRS_STREAM flag set before /RS, gone after: V lands at slot 10",
                 t2.post[0].streaming && lt_span(t2, 1, 0, 9, FFF, "NRS_STREAM") &&
                 lt_span(t2, 1, 10, H_ACTIVE - 1, 0x0D1, "NRS_STREAM"));
    }

    // COUNT_ZERO: RUN(c1, 0) is 1024 slots: line 1 all c1, line 2 c1 to 383.
    {
        LtSpec spec;
        spec.words = {vidcmd_run(RUN_SRC_COLOR1, 0), vidcmd_run(RUN_SRC_COLOR0, 600)};
        spec.lines = 2;
        const LtTrace t = lt_trace(spec);
        lt_check("COUNT_ZERO RUN(0) = 1024 slots: line 2 c1 to 383, c0 from 384",
                 lt_span(t, 0, 0, H_ACTIVE - 1, FFF, "COUNT_ZERO") &&
                 lt_span(t, 1, 0, 383, FFF, "COUNT_ZERO") &&
                 lt_span(t, 1, 384, H_ACTIVE - 1, RGB444_BLACK, "COUNT_ZERO"));
    }

    // LIT_AFTER_MASK: a literal captured on the pixel-15 edge: gap 0.
    {
        const VidcmdMask m = lt_mask(MASK_DIBIT_COLOR1, MASK_DIBIT_COLOR1);
        const LtTrace t = lt_trace({lt_set1(0x0C1), lt_set0(0x0C0), lt_pt(4), m.header, m.data,
                                    vidcmd_literal(X), lt_pt(600)});
        lt_check("LIT_AFTER_MASK mask 4-19, X 20-21 (gap 0), pt from 22",
                 lt_span(t, 0, 4, 4, 0x0C0, "LIT_AFTER_MASK") &&
                 lt_span(t, 0, 5, 19, 0x0C1, "LIT_AFTER_MASK") &&
                 lt_span(t, 0, 20, 21, X, "LIT_AFTER_MASK") &&
                 lt_span(t, 0, 22, H_ACTIVE - 1, PT, "LIT_AFTER_MASK") &&
                 lt_first_is(t.px[0], X) - 19 - 1 == VIDCMD_MASK_LITERAL_GAP);
    }

    // THE PLANNER against the same derivations: vidcmd_plan_line() must put the
    // last record where the bench does (occupied slots), and count literals,
    // streamed SETs and LS5 leftovers.  Hand-derived rows.
    {
        struct PlanRow
        {
            const char           *name;
            std::vector<uint16_t> words;
            uint32_t              slots;
            uint32_t              literals;
            uint32_t              stream_sets;
            uint32_t              leftovers;
            uint32_t              stretch;
        };
        const std::vector<PlanRow> rows = {
            {"LIT_ABC", {vidcmd_literal(1), vidcmd_literal(2), vidcmd_literal(3), lt_pt(634)},
             640, 3, 0, 0, 0},
            {"LIT_BEHIND_RUN", {lt_pt(4), vidcmd_literal(1), vidcmd_literal(2), vidcmd_literal(3),
                                lt_pt(600)}, 610, 3, 0, 0, 0},
            {"LIT_PAIR_TAIL", {lt_pt(4), lt_set0(1), vidcmd_literal(X), lt_pt(600)}, 607, 1, 0, 0, 0},
            {"LIT_HOLD_1", {lt_pt(4), vidcmd_literal(X), vidcmd_hold(1), lt_pt(600)}, 608, 1, 0, 0, 1},
            {"LIT_HOLD_2", {lt_pt(4), vidcmd_literal(X), vidcmd_hold(2), lt_pt(600)}, 608, 1, 0, 0, 0},
            {"LIT_HOLD_100", {lt_pt(4), vidcmd_literal(X), vidcmd_hold(100), lt_pt(600)}, 706, 1, 0, 0, 0},
            {"LIT_SPAN", {lt_pt(100), vidcmd_literal(X), vidcmd_hold(98), lt_pt(440)}, 640, 1, 0, 0, 0},
            {"STREAM_FEW", {lt_set0(1), vidcmd_stream_run(RUN_SRC_COLOR1, 20), lt_set1(1), lt_set1(2),
                            lt_set1(3), vidcmd_run(RUN_SRC_COLOR0, 600)}, 620, 0, 3, 0, 0},
            {"STREAM_ORDINARY", {lt_set0(1), vidcmd_run(RUN_SRC_COLOR1, 20), lt_set1(1), lt_set1(2),
                                 lt_set1(3), vidcmd_run(RUN_SRC_COLOR0, 600)}, 625, 0, 0, 0, 2},
            {"STREAM_MORE", {lt_set0(1), vidcmd_stream_run(RUN_SRC_COLOR1, 6), lt_set1(1), lt_set1(2),
                             lt_set1(3), lt_set1(4), lt_set1(5), vidcmd_run(RUN_SRC_COLOR0, 629)},
             640, 0, 3, 2, 5},
            {"EXACT640_NEW", {lt_pt(100), vidcmd_literal(X), vidcmd_hold(98),
                              vidcmd_stream_run(RUN_SRC_COLOR1, 300), lt_set1(1), lt_set1(2),
                              vidcmd_run(RUN_SRC_COLOR0, 140)}, 640, 1, 2, 0, 0},
        };
        for (const PlanRow &r : rows)
        {
            const VidcmdSlotPlan p = vidcmd_plan_line(r.words);
            char name[96];
            snprintf(name, sizeof(name), "PLAN %s: %u slots, %u lit, %u streamed, %u leftover, "
                     "stretch %u", r.name, r.slots, r.literals, r.stream_sets, r.leftovers, r.stretch);
            const bool ok = p.slots == r.slots && p.literals == r.literals &&
                            p.stream_sets == r.stream_sets && p.stream_leftovers == r.leftovers &&
                            p.stretch == r.stretch;
            if (!ok)
            {
                fprintf(stderr, "  PLAN %s: got %u slots, %u lit, %u streamed, %u leftover, "
                        "stretch %u\n", r.name, p.slots, p.literals, p.stream_sets,
                        p.stream_leftovers, p.stretch);
            }
            lt_check(name, ok);
        }
    }

    printf("  ------------------------------------------------------------------\n");
    printf("  %u bench-derived literal/stream traces checked\n", g_lt_cases);
    printf("  MEASURED CONSTANTS FOR THE SUITE (LITERAL / HOLD / STREAM RUN)\n");
    printf("    LITERAL pitch, sustained and out of a pair : %u slots   [tb SUSTAINED_LIT, "
           "LIT_BEHIND_RUN]\n", VIDCMD_LITERAL_PITCH);
    printf("    literal -> next record, any kind           : %u slots   [tb LIT_NEXT_*]\n",
           VIDCMD_LITERAL_NEXT_GAP);
    printf("    mask -> literal gap                        : %u slots   [tb LIT_AFTER_MASK]\n",
           VIDCMD_MASK_LITERAL_GAP);
    printf("    LITERAL + HOLD(n) behind a run             : max(n + 2, %u) [tb LIT_HOLD_n]\n",
           VIDCMD_LIT_HOLD_MIN_SLOTS);
    printf("    stream run -> first SET, then every        : %u, %u slots [tb STREAM_FEW]\n",
           VIDCMD_STREAM_FIRST_SET, VIDCMD_STREAM_SET_PITCH);
    printf("    run counter                                : 10 bits, RUN(0) = %u slots "
           "[tb COUNT_ZERO]\n", VIDCMD_RUN_COUNT_ZERO_SLOTS);
    printf("  ------------------------------------------------------------------\n");
}

// --- The whole-frame case -----------------------------------------------------
//
// A 4bpp half-rate playfield (80 PIXELS words a line, the m12 / Mandelbrot
// mode) with a 32-line band of SET-stream sprites:
//   S1  at the LEFT EDGE, 16 opaque colours that change every line: eager
//       SET c1 in HBLANK + RUN(c1,1) + 15 SETs.  Pixel 0 is one slot (slot 0);
//       pixel i >= 1 is slots 2i-1, 2i (T4: x = 0 cannot have both pixels 0 and
//       1 two slots wide).  31 slots, exit RUN on slot 31.
//   S2  at x = 200, the transparency row A B . D E E . . G H . J K L M N, its
//       word list derived slot by slot below.  32 slots, exit on 232.
//
//   S2 word            slot   why
//   SET c1=A           199    pair 1st behind S1's exit RUN(168): invisible
//   RUN(c1,2)          200    A 200-201; cadence entry n=2, next at +2
//   SET c1=B           202    B, hold 203
//   RUN(pass,2)        204    hole 204-205
//   RUN(c0,2)          206    D, preloaded eagerly in HBLANK
//   SET c0=E           208    E, hold 209 (the stream now runs in cmp_color0)
//   SET c1=G           210    the repeated E pixel: an invisible PRELOAD of G
//   RUN(pass,3)        212    hole 212-214 and a pair behind it (n = 3)
//   SET c0=J           215    pair 1st, invisible under passthrough: preload
//   RUN(c1,2)          216    G (pair 2nd)
//   SET c1=H           218    H, hold 219
//   RUN(pass,2)        220    hole 220-221
//   RUN(c0,2)          222    J
//   SET c0=K,L,M,N     224..  2 slots each, holds 225..231
//   RUN(pass,408)      232    to slot 639: the line occupies exactly 640
//
// Cadence holds per band line: S1 15 (one per SET) + S2 8 = 23; starvation 0.

enum class SsVariant : uint8_t
{
    K1_TWO,            // both sprites, programs deposited one line ahead
    K0_VIDCMD_FIRST,   // S1 only, the line's program first in its own HBLANK group
    K0_PIXELS_FIRST,   // S1 only, the program behind the line's 80 PIXELS words
    K0_LATE,           // as K0_VIDCMD_FIRST, but line 200's last two words go last
};

constexpr uint32_t SS_PREAMBLE_BASE = 0x320000;
constexpr uint32_t SS_PREAMBLE_STEP = 0x000400;
constexpr uint32_t SS_PROGRAM_BASE  = 0x330000;
constexpr uint32_t SS_PROGRAM_STEP  = 0x010000;
constexpr uint32_t SS_PROGRAM_BYTES = 0x80;
constexpr uint32_t SS_BAND_FIRST    = 200;
constexpr uint32_t SS_BAND_LINES    = 32;
constexpr uint32_t SS_LATE_LINE     = SS_BAND_FIRST;
constexpr uint32_t SS_LATE_SPLIT    = 16;   // eager SET, RUN(c1,1), SET A1..A14
constexpr uint32_t SS_ROW           = 16;
constexpr uint32_t SS_S1_SLOTS      = 2 * SS_ROW - 1;
constexpr uint32_t SS_S2_X          = 200;
constexpr uint32_t SS_HOLE          = 0xFFFF;

constexpr Rgb444 SS_A = 0xF80;
constexpr Rgb444 SS_B = 0x08F;
constexpr Rgb444 SS_D = 0x8F0;
constexpr Rgb444 SS_E = 0xF08;
constexpr Rgb444 SS_G = 0x0F8;
constexpr Rgb444 SS_H = 0x80F;
constexpr Rgb444 SS_J = 0xFF8;
constexpr Rgb444 SS_K = 0x8FF;
constexpr Rgb444 SS_L = 0xF8F;
constexpr Rgb444 SS_M = 0x444;
constexpr Rgb444 SS_N = 0xCCC;

constexpr std::array<uint32_t, SS_ROW> SS_S2_ART = {
    SS_A, SS_B, SS_HOLE, SS_D, SS_E, SS_E, SS_HOLE, SS_HOLE,
    SS_G, SS_H, SS_HOLE, SS_J, SS_K, SS_L, SS_M,   SS_N,
};

constexpr std::array<uint16_t, 17> SS_S2_WORDS = {
    ss_set1(SS_A), ss_run1(2), ss_set1(SS_B), ss_pass(2), ss_run0(2), ss_set0(SS_E),
    ss_set1(SS_G), ss_pass(3), ss_set0(SS_J), ss_run1(2), ss_set1(SS_H), ss_pass(2),
    ss_run0(2), ss_set0(SS_K), ss_set0(SS_L), ss_set0(SS_M), ss_set0(SS_N),
};

constexpr uint32_t SS_CADENCE_S1 = SS_ROW - 1;
constexpr uint32_t SS_CADENCE_S2 = 8;

bool ss_band(uint32_t y)
{
    return y >= SS_BAND_FIRST && y < SS_BAND_FIRST + SS_BAND_LINES;
}

Rgb444 ss_s1_colour(uint32_t i, uint32_t y)
{
    return ss_c((i + y) % 16u);
}

// One line's VIDCMD program into `out`; returns its word count.
uint32_t ss_line_program(SsVariant v, uint32_t y, uint16_t *out)
{
    uint32_t n = 0;
    if (!ss_band(y))
    {
        out[n++] = ss_pass(H_ACTIVE);
        return n;
    }
    const bool two = (v == SsVariant::K1_TWO);
    out[n++] = ss_set1(ss_s1_colour(0, y));   // eager, in this line's HBLANK
    if (two)
    {
        out[n++] = ss_set0(SS_D);              // eager: the hole re-entry colour
    }
    out[n++] = ss_run1(1);
    for (uint32_t i = 1; i < SS_ROW; i++)
    {
        out[n++] = ss_set1(ss_s1_colour(i, y));
    }
    if (two)
    {
        out[n++] = ss_pass(SS_S2_X - 1 - SS_S1_SLOTS);
        for (uint16_t w : SS_S2_WORDS)
        {
            out[n++] = w;
        }
        out[n++] = ss_pass(H_ACTIVE - (SS_S2_X + 2 * SS_ROW));
    }
    else
    {
        out[n++] = ss_pass(H_ACTIVE - SS_S1_SLOTS);
    }
    return n;
}

uint32_t ss_author(Memory ram, SsVariant v, uint32_t table, uint32_t frame)
{
    const uint32_t preamble = SS_PREAMBLE_BASE + frame * SS_PREAMBLE_STEP;
    const uint32_t programs = SS_PROGRAM_BASE + frame * SS_PROGRAM_STEP;

    std::vector<uint32_t> len(V_ACTIVE, 0);
    uint16_t              buf[SS_PROGRAM_BYTES / 2];
    for (uint32_t y = 0; y < V_ACTIVE; y++)
    {
        len[y] = ss_line_program(v, y, buf);
        for (uint32_t i = 0; i < len[y]; i++)
        {
            ram[programs + y * SS_PROGRAM_BYTES + i * 2] = buf[i];
        }
    }

    uint32_t n = 0;
    for (uint32_t i = 0; i < PALETTE_ENTRIES; i++)
    {
        for (uint16_t w : vidcmd_palette_entry(i, cc_palette(i)))
        {
            ram[preamble + (n++) * 2] = w;
        }
    }
    ram[preamble + (n++) * 2] = vidcmd_set(SET_PIX_BASE, 0);
    ram[preamble + (n++) * 2] = vidcmd_set(SET_PIX_MODE, pixel_mode(PIXEL_DEPTH_4BPP, true));
    if (v == SsVariant::K1_TWO)
    {
        // One line of lookahead: line 0's program has no group to ride in.
        for (uint32_t i = 0; i < len[0]; i++)
        {
            ram[preamble + (n++) * 2] = ram[programs + i * 2];
        }
    }

    CcEmitter e{ram, table, 0};
    for (uint32_t i = 0; i < CC_PACERS_BEFORE_PREAMBLE; i++)
    {
        e.pacer(false);
    }
    e.stream(preamble, n, DEST_VIDCMD_FIFO_W, false);
    for (uint32_t i = 0; i < CC_PACERS_AFTER_PREAMBLE; i++)
    {
        e.pacer(false);
    }
    for (uint32_t y = 0; y < V_ACTIVE; y++)
    {
        const uint32_t px   = CC_FB_BASE + (y / 2) * CC_FB_STRIDE;
        const uint32_t prog = programs + y * SS_PROGRAM_BYTES;
        if (v == SsVariant::K1_TWO)
        {
            e.stream(px, PIXELS_WORDS_4BPP_HALF, DEST_PIXELS_FIFO_W, true);
            if (y + 1 < V_ACTIVE)
            {
                e.stream(prog + SS_PROGRAM_BYTES, len[y + 1], DEST_VIDCMD_FIFO_W, false);
            }
        }
        else if (v == SsVariant::K0_PIXELS_FIRST)
        {
            e.stream(px, PIXELS_WORDS_4BPP_HALF, DEST_PIXELS_FIFO_W, true);
            e.stream(prog, len[y], DEST_VIDCMD_FIFO_W, false);
        }
        else if (v == SsVariant::K0_LATE && y == SS_LATE_LINE)
        {
            e.stream(prog, SS_LATE_SPLIT, DEST_VIDCMD_FIFO_W, true);
            e.stream(px, PIXELS_WORDS_4BPP_HALF, DEST_PIXELS_FIFO_W, false);
            e.stream(prog + SS_LATE_SPLIT * 2, len[y] - SS_LATE_SPLIT, DEST_VIDCMD_FIFO_W, false);
        }
        else
        {
            e.stream(prog, len[y], DEST_VIDCMD_FIFO_W, true);
            e.stream(px, PIXELS_WORDS_4BPP_HALF, DEST_PIXELS_FIFO_W, false);
        }
    }
    e.pacer(true);
    return e.count;
}

struct SsRun
{
    RenderResult    r;
    InterpretResult interp;
    uint32_t        descriptors = 0;
};

SsRun ss_run(Memory ram, SsVariant v, bool line_at_a_time)
{
    SsRun          out;
    const uint32_t tables[RENDER_FRAMES] = {TABLE_A, TABLE_B};
    InterpretParams ip;
    ip.start_cycle          = first_arm_cycle();
    ip.end_cycle            = run_end_cycle();
    ip.rearm_latency_cycles = REARM_LATENCY_CYCLES;
    for (uint32_t f = 0; f < RENDER_FRAMES; f++)
    {
        out.descriptors = ss_author(ram, v, tables[f], f);
        ip.arm_addresses.push_back(tables[f]);
    }
    out.interp = interpret(ram, ip);

    RenderParams rp;
    rp.first_frame = RENDER_FIRST_FRAME;
    rp.frame_count = RENDER_FRAMES;
    out.r = line_at_a_time ? render_line_at_a_time(out.interp.events, rp)
                           : render(out.interp.events, rp);
    return out;
}

// Where line 200's two late words land, derived from their DEPOSIT cycles (the
// ENGINE model's output, an input to the compositor law, not a compositor
// result).  P0/P1 are the slots at whose start each word is in the FIFO.
struct SsLate
{
    uint32_t p0   = 0;
    uint32_t p1   = 0;
    uint32_t exit = 0;   // the exit RUN's slot
    uint32_t words = 0;
};

SsLate ss_late_positions(const InterpretResult &ir, uint32_t frame_out)
{
    SsLate         l;
    const uint64_t abs_line = static_cast<uint64_t>(RENDER_FIRST_FRAME + frame_out) * V_TOTAL +
                              SS_LATE_LINE;
    const uint64_t pixel0   = abs_line * H_TOTAL;
    const uint64_t on_time  = sysclk_of_pixel(pixel0);
    const uint64_t last     = sysclk_of_pixel(pixel0 + H_ACTIVE - 1);
    uint32_t       slots[2] = {0, 0};
    for (const DepositEvent &e : ir.events)
    {
        if (e.destination != DEST_VIDCMD_FIFO_W || e.cycle <= on_time || e.cycle > last)
        {
            continue;
        }
        uint32_t h = 0;
        while (sysclk_of_pixel(pixel0 + h) < e.cycle)
        {
            h++;
        }
        if (l.words < 2)
        {
            slots[l.words] = h;
        }
        l.words++;
    }
    l.p0 = slots[0];
    l.p1 = slots[1];
    // The derivation: SET A15 meets a cold synchronizer and executes on P0+4;
    // the exit RUN falls on P0+4 if its push was in by P0+3 (its meta stage was
    // set on edge P0+3), else it takes the cold path from P1 = P0+4.
    l.exit = (l.p1 <= l.p0 + 3) ? (l.p0 + 6) : (l.p0 + 8);
    return l;
}

Rgb444 ss_expected(SsVariant v, uint32_t x, uint32_t y, const SsLate *late, int32_t s2_slip = 0)
{
    const Rgb444 bd = cc_palette(cc_fb_nibble(x / 2, y / 2));
    if (!ss_band(y))
    {
        return bd;
    }
    uint32_t shift = 0;
    if (v == SsVariant::K0_LATE && late != nullptr)
    {
        if (y == SS_LATE_LINE)
        {
            if (x <= 26)
            {
                return ss_s1_colour((x + 1) / 2, y);
            }
            if (x < late->p0 + 4)
            {
                return ss_s1_colour(14, y);
            }
            if (x < late->exit)
            {
                return ss_s1_colour(15, y);
            }
            return bd;
        }
        if (y > SS_LATE_LINE)
        {
            shift = late->exit - SS_S1_SLOTS;
        }
    }
    if (x >= shift && x < shift + SS_S1_SLOTS)
    {
        return ss_s1_colour((x - shift + 1) / 2, y);
    }
    const int32_t s2x = static_cast<int32_t>(SS_S2_X) + s2_slip;
    const int32_t xi  = static_cast<int32_t>(x);
    if (v == SsVariant::K1_TWO && xi >= s2x && xi < s2x + static_cast<int32_t>(2 * SS_ROW))
    {
        const uint32_t a = SS_S2_ART[static_cast<uint32_t>(xi - s2x) / 2];
        return (a == SS_HOLE) ? bd : static_cast<Rgb444>(a);
    }
    return bd;
}

uint32_t ss_mismatches(const FrameImage &img, SsVariant v, const SsLate *late, int32_t slip = 0)
{
    uint32_t bad = 0;
    for (uint32_t y = 0; y < V_ACTIVE; y++)
    {
        for (uint32_t x = 0; x < H_ACTIVE; x++)
        {
            bad += (at(img, x, y) != ss_expected(v, x, y, late, slip)) ? 1u : 0u;
        }
    }
    return bad;
}

void ss_common_checks(const char *name, const SsRun &run)
{
    const RenderStats &s = run.r.stats;
    CHECK(run.interp.violations.empty(), "m13-set-stream/%s: %zu interpreter violations", name,
          run.interp.violations.size());
    CHECK(run.descriptors * DESC_BYTES <= TABLE_HALF_BYTES,
          "m13-set-stream/%s: %u descriptors overflow the table half", name, run.descriptors);
    CHECK(s.vidcmd_overflows == 0, "m13-set-stream/%s: VIDCMD FIFO overflowed", name);
    CHECK(s.pixels_overflows == 0, "m13-set-stream/%s: PIXELS FIFO overflowed", name);
    CHECK(s.palette_unwritten_reads == 0 && s.palette_bad_filler == 0 &&
          s.palette_disturbed_visible == 0, "m13-set-stream/%s: palette rule broken", name);
    CHECK(s.vidcmd_mask_records == 0, "m13-set-stream/%s: stray MASK records", name);
}

void run_set_stream_case(Memory ram)
{
    printf("\n=== m13-set-stream — 16-pixel SET(cmp_color1) sprites over a 4bpp half-rate "
           "playfield, band lines %u..%u ===\n", SS_BAND_FIRST, SS_BAND_FIRST + SS_BAND_LINES - 1);
    cc_write_framebuffer(ram);

    // The planner (cushion assumption) has to agree that both band programs
    // close at exactly 640 slots with no record behind its authored slot.
    {
        uint16_t buf[SS_PROGRAM_BYTES / 2];
        for (SsVariant v : {SsVariant::K1_TWO, SsVariant::K0_VIDCMD_FIRST})
        {
            const uint32_t n = ss_line_program(v, SS_BAND_FIRST, buf);
            const VidcmdSlotPlan plan = vidcmd_plan_line(std::span<const uint16_t>(buf, n));
            printf("  band program        %2u words: planner %u slots, %u eager SETs, "
                   "stretch %u\n", n, plan.slots, plan.blank_sets, plan.stretch);
            CHECK(plan.slots == H_ACTIVE, "m13-set-stream: band program plans %u slots, not %u",
                  plan.slots, H_ACTIVE);
        }
    }

    const struct
    {
        SsVariant   v;
        const char *name;
        const char *why;
    } variants[] = {
        {SsVariant::K1_TWO, "k1-two", "two sprites, programs one line ahead, PIXELS first"},
        {SsVariant::K0_VIDCMD_FIRST, "k0-vc", "left-edge sprite, program first in its HBLANK"},
        {SsVariant::K0_PIXELS_FIRST, "k0-px", "left-edge sprite, program behind 80 PIXELS words"},
        {SsVariant::K0_LATE, "late", "line 200's last two words deposited after its PIXELS"},
    };

    for (const auto &var : variants)
    {
        const SsRun        run = ss_run(ram, var.v, false);
        const RenderStats &s   = run.r.stats;
        const GroupBudget  b   = group_budget(run.interp);

        SsLate late[RENDER_FRAMES];
        const SsLate *lp[RENDER_FRAMES] = {nullptr, nullptr};
        if (var.v == SsVariant::K0_LATE)
        {
            for (uint32_t f = 0; f < RENDER_FRAMES; f++)
            {
                late[f] = ss_late_positions(run.interp, f);
                lp[f]   = &late[f];
            }
        }
        uint32_t bad[RENDER_FRAMES];
        for (uint32_t f = 0; f < RENDER_FRAMES; f++)
        {
            bad[f] = ss_mismatches(run.r.frames[f], var.v, lp[f]);
        }

        printf("  %-7s %-52s\n", var.name, var.why);
        printf("          wrong pixels %u + %u of %u   cadence %u  starvation %u  overruns %u  "
               "late words %u\n", bad[0], bad[1], H_ACTIVE * V_ACTIVE, s.vidcmd_cadence_slots,
               s.vidcmd_hold_slots, s.vidcmd_overruns, s.vidcmd_late_words);
        printf("          worst group %u SYSCLK of %llu (66%% budget %u), %u descriptors, "
               "PIXELS tiled %u, VIDCMD high water %u\n", b.max_busy,
               static_cast<unsigned long long>(LINE_SYSCLK), VIDCMD_LINE_SYSCLK_BUDGET,
               run.descriptors, s.pixels_tiled_words, s.vidcmd_fifo_high);
        ss_common_checks(var.name, run);

        if (var.v == SsVariant::K0_PIXELS_FIRST)
        {
            // NEGATIVE: the program lands after pixel 0 on every line.  What it
            // does there is the starvation law of T7 applied line after line;
            // what matters here is that the case can fail and does.
            CHECK(bad[0] != 0 && bad[1] != 0,
                  "m13-set-stream/k0-px: a program delivered after pixel 0 drew the sprite "
                  "correctly — the comparison cannot fail");
            CHECK(s.vidcmd_hold_slots != 0 && s.vidcmd_overruns != 0,
                  "m13-set-stream/k0-px: expected starvation holds and overruns");
            continue;
        }

        for (uint32_t f = 0; f < RENDER_FRAMES; f++)
        {
            CHECK(bad[f] == 0, "m13-set-stream/%s: frame %u has %u pixels off the derived image",
                  var.name, f + 1, bad[f]);
        }
        CHECK(s.pixels_tiled_words == 0, "m13-set-stream/%s: PIXEL tiled %u words", var.name,
              s.pixels_tiled_words);

        if (var.v == SsVariant::K0_LATE)
        {
            uint32_t starve = 0;
            uint32_t cad    = 0;
            for (uint32_t f = 0; f < RENDER_FRAMES; f++)
            {
                const SsLate &l = late[f];
                printf("          frame %u: late words in at slots %u and %u -> A15 on %u, exit "
                       "on %u (nominal 29 / 31), shift %u for the rest of the frame\n", f + 1,
                       l.p0, l.p1, l.p0 + 4, l.exit, l.exit - SS_S1_SLOTS);
                CHECK(l.words == 2, "m13-set-stream/late: %u words arrived during line %u, "
                      "expected 2", l.words, SS_LATE_LINE);
                CHECK(l.p1 >= l.p0 + 3 && l.p1 <= l.p0 + 4 && l.p0 > 40,
                      "m13-set-stream/late: deposit slots %u/%u outside the derivation's range",
                      l.p0, l.p1);
                starve += l.p0 - 28;
                cad += (SS_BAND_LINES - 1) * SS_CADENCE_S1 + 13 + 4 + (l.exit - l.p0 - 5);
            }
            CHECK(s.vidcmd_hold_slots == starve, "m13-set-stream/late: %u starvation holds, "
                  "derived %u", s.vidcmd_hold_slots, starve);
            CHECK(s.vidcmd_cadence_slots == cad, "m13-set-stream/late: %u cadence holds, "
                  "derived %u", s.vidcmd_cadence_slots, cad);
            CHECK(s.vidcmd_overruns == RENDER_FRAMES * (V_ACTIVE - SS_LATE_LINE),
                  "m13-set-stream/late: %u overruns, derived %u", s.vidcmd_overruns,
                  RENDER_FRAMES * (V_ACTIVE - SS_LATE_LINE));
            CHECK(s.vidcmd_late_words == 2 * RENDER_FRAMES,
                  "m13-set-stream/late: %u late words, expected %u", s.vidcmd_late_words,
                  2 * RENDER_FRAMES);
            continue;
        }

        // Clean variants: exact 640 framing, no starvation, the cadence holds
        // are the derived mechanism count, and the emulator's driver agrees.
        const uint32_t per_line = SS_CADENCE_S1 +
                                  ((var.v == SsVariant::K1_TWO) ? SS_CADENCE_S2 : 0u);
        CHECK(s.vidcmd_hold_slots == 0, "m13-set-stream/%s: %u starvation holds", var.name,
              s.vidcmd_hold_slots);
        CHECK(s.vidcmd_overruns == 0, "m13-set-stream/%s: %u overruns", var.name,
              s.vidcmd_overruns);
        CHECK(s.vidcmd_cadence_slots == RENDER_FRAMES * SS_BAND_LINES * per_line,
              "m13-set-stream/%s: %u cadence holds, derived %u", var.name,
              s.vidcmd_cadence_slots, RENDER_FRAMES * SS_BAND_LINES * per_line);
        CHECK(b.max_busy < LINE_SYSCLK, "m13-set-stream/%s: a group costs %u SYSCLK", var.name,
              b.max_busy);

        const SsRun emu = ss_run(ram, var.v, true);
        uint32_t    emu_bad = 0;
        for (uint32_t f = 0; f < RENDER_FRAMES; f++)
        {
            emu_bad += ss_mismatches(emu.r.frames[f], var.v, nullptr);
        }
        printf("          emulator driver: %u pixels off\n", emu_bad);
        CHECK(emu_bad == 0, "m13-set-stream/%s: emulator driver has %u pixels off", var.name,
              emu_bad);

        if (var.v == SsVariant::K1_TWO)
        {
            const std::string path = "m13-set-stream.ppm";
            write_ppm(path, run.r.frames[1]);
            printf("          artifact %s\n", path.c_str());

            // NEGATIVE CONTROL: the comparison must see a one-slot slip of S2.
            const uint32_t slip = ss_mismatches(run.r.frames[0], var.v, nullptr, 1);
            printf("          NEGATIVE CONTROL  S2 expected one slot right: %u pixels wrong\n",
                   slip);
            CHECK(slip != 0, "m13-set-stream: a one-slot slip of S2 went undetected");
        }
    }
}

// ---------------------------------------------------------------------------
// m14-literal-sprites — sprite rows as LITERALs (package 2, 2026-10-03)
// ---------------------------------------------------------------------------
//
// THE RECORD.  A sprite row of 16 SOURCE pixels is one word per pixel: a
// LITERAL for an opaque pixel and RUN(passthrough, 2) for a transparent one.
// Every element is exactly two slots — a literal by law LL, the hole by its
// count — so a row is 32 slots at 320 density, which is the 4bpp half-rate
// playfield's own density.
//
// DERIVATION, by hand from L4/L5/LL before anything was simulated:
//
//   R1  ANY CONCATENATION OF TWO-SLOT ELEMENTS IS EXACT.  An element E
//       executing on edge s was captured no later than edge s-1.  If E is a
//       literal it holds the bank, so its successor (parked or fetched) is
//       captured on s+1 and executes on s+2.  If E is RUN(pt,2) its successor
//       is captured by s+1 (/RE rose on E's capture edge s-1 at the latest, fell
//       on s, captured on s+1) and executes at E's terminal s+2.  Either way
//       the next element lands two slots later and no cadence slot appears.
//   R2  THE ENTRY.  A row behind a lead RUN(pt, x) of any x >= 1: element 0 is
//       the parked half of the pair and executes on x; element 1 is captured
//       on x+1 (behind a literal) or by x+2 (behind a hole) and executes on
//       x+2.  At x = 0 element 0 is banked in HBLANK with element 1 parked and
//       the same holds from slot 0.  So EVERY x is exact, 0, 1 and 2 included
//       — the SET-stream form (m13) could not do x = 0 (pixel 0 one slot or
//       pixel 1 one slot) nor x = 2 without a free word.
//   R3  THE RIGHT EDGE.  A row at x paints x .. x+31; at x = 608 it ends on
//       slot 639 with no exit RUN.  Past that a row is CLIPPED to the elements
//       that fit (an element at 639 would put its second slot on the next line).
//   R4  PAINTER'S-ORDER SLICING NEEDS NO FIX-UP WORD.  A seam between two
//       slices is a seam between two two-slot elements, which R1 already
//       covers, so back[0..a) + front[all] + back[b..16) — three sub-ranges of
//       static rows — plays as one row.  The price is the notch: where the
//       front is transparent the BACKDROP shows, not the back sprite, because
//       the back's covered elements were simply not sent.
//   R5  DELIVERY.  A literal row consumes one word per two slots; ENGINE
//       deposits one per 2 SYSCLK = 3.6 pixel clocks.  So a row cannot be
//       trickled: word j of a row at x must be in the FIFO by slot x + 2j - 4
//       (the cold /EF path, LATE_LITERAL: pushed at P, executes at P + 4).  A
//       17-word left-edge program deposited FIRST in its own HBLANK has its last
//       word in 2 + 5 + 32 + 1 + 13 + 2 = 55 SYSCLK after the edge (about 99 of
//       the 160 blank clocks), and the line's first PIXELS word lands at
//       55 + 1 + 13 + 2 = 71 SYSCLK, inside PIX_PRELOAD's 80: both deadlines met.
//       With one line of lookahead (k = 1) the deadline is a whole line away.

constexpr uint32_t LS_HOLE   = 0xFFFF;
constexpr uint32_t LS_ROW    = 16;
constexpr uint32_t LS_FRONT  = 8;

constexpr std::array<uint32_t, LS_ROW> LS_BACK_ART = {
    0xF80, 0x08F, LS_HOLE, 0x8F0, 0xF08, 0xF08, LS_HOLE, LS_HOLE,
    0x0F8, 0x80F, LS_HOLE, 0xFF8, 0x8FF, 0xF8F, 0x444,   0xCCC,
};

constexpr std::array<uint32_t, LS_FRONT> LS_FRONT_ART = {
    0xE11, LS_HOLE, 0x1E1, 0x11E, LS_HOLE, LS_HOLE, 0xEE1, 0x1EE,
};

constexpr uint16_t ls_element(uint32_t art)
{
    return (art == LS_HOLE) ? vidcmd_run(RUN_SRC_PASSTHROUGH, VIDCMD_LITERAL_SLOTS)
                            : vidcmd_literal(static_cast<Rgb444>(art));
}

// A row at x: lead RUN, the elements that fit, and the exit RUN.
std::vector<uint16_t> ls_row_line(uint32_t x, uint32_t elements = LS_ROW)
{
    std::vector<uint16_t> w;
    if (x > 0)
    {
        w.push_back(vidcmd_run(RUN_SRC_PASSTHROUGH, x));
    }
    uint32_t e = 0;
    for (; e < elements && x + 2 * e + 1 < H_ACTIVE; e++)
    {
        w.push_back(ls_element(LS_BACK_ART[e]));
    }
    if (x + 2 * e < H_ACTIVE)
    {
        w.push_back(vidcmd_run(RUN_SRC_PASSTHROUGH, H_ACTIVE - (x + 2 * e)));
    }
    return w;
}

// The backdrop the passthrough shows, from the same line rendered with no
// sprite at all: that is what the transparent pixels must show.
std::vector<Rgb444> ls_backdrop(const LtSpec &like)
{
    LtSpec spec    = like;
    spec.words     = {vidcmd_run(RUN_SRC_PASSTHROUGH, H_ACTIVE)};
    spec.late      = {};
    return lt_trace(spec).px[0];
}

void ls_line_check(const char *name, const std::vector<Rgb444> &got,
                   const std::vector<Rgb444> &want, const RenderStats &stats)
{
    uint32_t bad   = 0;
    uint32_t first = H_ACTIVE;
    for (uint32_t s = 0; s < H_ACTIVE; s++)
    {
        if (got[s] != want[s])
        {
            first = (bad == 0) ? s : first;
            bad++;
        }
    }
    printf("  %-50s %3u slots off  cadence %u  starved %u\n", name, bad,
           stats.vidcmd_cadence_slots, stats.vidcmd_hold_slots);
    CHECK(bad == 0, "m14 %s: %u slots differ from the derivation, first at %u (got 0x%03X, "
          "derived 0x%03X)", name, bad, first, first < H_ACTIVE ? got[first] : 0u,
          first < H_ACTIVE ? want[first] : 0u);
    CHECK(stats.vidcmd_cadence_slots == 0 && stats.vidcmd_hold_slots == 0,
          "m14 %s: %u cadence / %u starvation holds, derived 0 / 0 (R1)", name,
          stats.vidcmd_cadence_slots, stats.vidcmd_hold_slots);
}

void check_literal_sprite_traces()
{
    printf("\n=== m14 literal-sprite traces — rows, flat spans, stream runs, palette cover ===\n");

    // (a) LITERAL ROWS at x = 0, 1, 2, mid, the right edge and clipped, over
    // the 4bpp half-rate and the attribute playfields (TEST_PALETTE).
    struct Backdrop
    {
        const char *name;
        uint32_t    mode;
        uint32_t    words;
    };
    const Backdrop backdrops[] = {
        {"4bpp-half", pixel_mode(PIXEL_DEPTH_4BPP, true), PIXELS_WORDS_4BPP_HALF},
        {"attribute", pixel_mode(PIXEL_DEPTH_ATTR, false), PIXELS_WORDS_ATTR},
    };
    for (const Backdrop &b : backdrops)
    {
        LtSpec base;
        base.pix_mode     = b.mode;
        base.full_palette = true;
        for (uint32_t i = 0; i < b.words + 1; i++)
        {
            base.pixel_words.push_back(static_cast<uint16_t>(0x9E37u * (i + 1) ^ (i << 7)));
        }
        const std::vector<Rgb444> bd = ls_backdrop(base);
        uint32_t distinct = 0;
        for (uint32_t s = 1; s < H_ACTIVE; s++)
        {
            distinct += (bd[s] != bd[s - 1]) ? 1u : 0u;
        }
        CHECK(distinct > 100, "m14 (a): the %s backdrop is too flat (%u changes) to show a slip",
              b.name, distinct);
        for (uint32_t x : {0u, 1u, 2u, 301u, 608u, 624u})
        {
            LtSpec spec = base;
            spec.words  = ls_row_line(x);
            const LtTrace t = lt_trace(spec);
            std::vector<Rgb444> want = bd;
            for (uint32_t e = 0; e < LS_ROW && x + 2 * e + 1 < H_ACTIVE; e++)
            {
                if (LS_BACK_ART[e] != LS_HOLE)
                {
                    want[x + 2 * e]     = static_cast<Rgb444>(LS_BACK_ART[e]);
                    want[x + 2 * e + 1] = static_cast<Rgb444>(LS_BACK_ART[e]);
                }
            }
            char name[80];
            snprintf(name, sizeof(name), "a  row at x=%u over %s%s", x, b.name,
                     x == 624 ? " (clipped to 8)" : "");
            ls_line_check(name, t.px[0], want, t.stats);
            CHECK(t.stats.vidcmd_overruns == 0, "m14 %s: the row overran the line", name);
        }
    }

    // (c) LITERAL + HOLD(n) flat spans behind RUN(pt,100): max(n + 2, 4), and
    // a chain of LITERAL + HOLD(6) spans, each exactly 8.
    {
        const Rgb444 pt = TRACE_PASSTHROUGH;
        for (uint32_t n : {1u, 2u, 3u, 4u, 62u})
        {
            const uint32_t width = (n + 2 > VIDCMD_LIT_HOLD_MIN_SLOTS) ? n + 2 : VIDCMD_LIT_HOLD_MIN_SLOTS;
            const LtTrace t = lt_trace({vidcmd_run(RUN_SRC_PASSTHROUGH, 100), vidcmd_literal(0x5E7),
                                        vidcmd_hold(n), vidcmd_run(RUN_SRC_PASSTHROUGH, 400)});
            std::vector<Rgb444> want(H_ACTIVE, pt);
            for (uint32_t s = 100; s < 100 + width; s++)
            {
                want[s] = 0x5E7;
            }
            char name[80];
            snprintf(name, sizeof(name), "c  LITERAL + HOLD(%u) = %u slots", n, width);
            uint32_t bad = 0;
            for (uint32_t s = 0; s < H_ACTIVE; s++)
            {
                bad += (t.px[0][s] != want[s]) ? 1u : 0u;
            }
            printf("  %-50s %3u slots off\n", name, bad);
            CHECK(bad == 0, "m14 %s: %u slots off", name, bad);
        }
        std::vector<uint16_t> w = {vidcmd_run(RUN_SRC_PASSTHROUGH, 64)};
        for (uint32_t i = 0; i < 8; i++)
        {
            w.push_back(vidcmd_literal(ss_c(i)));
            w.push_back(vidcmd_hold(6));
        }
        w.push_back(vidcmd_run(RUN_SRC_PASSTHROUGH, H_ACTIVE - 128));
        const LtTrace t = lt_trace(w);
        std::vector<Rgb444> want(H_ACTIVE, pt);
        for (uint32_t s = 64; s < 128; s++)
        {
            want[s] = ss_c((s - 64) / 8);
        }
        ls_line_check("c  eight LITERAL + HOLD(6) spans, 8 slots each", t.px[0], want, t.stats);
    }

    // (d) A STREAM RUN recoloured by SET(cmp_color1), one word LATE, against
    // the same content as literals.  x = 100: RUN(pt,99), SET c1 = C0 (the
    // invisible first half of the pair), STREAM(c1,32) on 100, SETs C1..C11
    // streamed on 102, 104, ... 122, then the run's flat tail to 131; exit
    // RUN(pt,508) on 132.  The literal form is LIT C0..C10, LIT C11 + HOLD(8):
    // the same 32 slots.  Words up to C5 are in the FIFO; C6 onward (and the
    // exits) arrive at slot 114, two slots after C6 was due (112).  Cold path:
    // C6 executes on 118 (P + 4), C7..C11 every two after it (120..128).
    //   stream:  C5 on 110..117, C6 118, ..., C11 128..131; exit on 132 EXACTLY,
    //            line 1 (a LITERAL + HOLD(638) program) unshifted.
    //   literal: C5 KEEP 110..117, C6 118, ..., C11 128 + HOLD(8) 130..137; exit
    //            on 138 — six slots late — overrunning six slots into line 1,
    //            whose program starts on its slot 6.
    {
        const Rgb444 pt   = TRACE_PASSTHROUGH;
        const Rgb444 next = 0x0D0;
        for (int form = 0; form < 2; form++)
        {
            LtSpec spec;
            spec.lines = 2;
            std::vector<uint16_t> all;
            uint32_t              split = 0;
            if (form == 0)
            {
                all = {vidcmd_run(RUN_SRC_PASSTHROUGH, 99), lt_set1(ss_c(0)),
                       vidcmd_stream_run(RUN_SRC_COLOR1, 32)};
                for (uint32_t j = 1; j <= 11; j++)
                {
                    if (j == 6)
                    {
                        split = static_cast<uint32_t>(all.size());
                    }
                    all.push_back(lt_set1(ss_c(j)));
                }
            }
            else
            {
                all = {vidcmd_run(RUN_SRC_PASSTHROUGH, 100)};
                for (uint32_t j = 0; j <= 11; j++)
                {
                    if (j == 6)
                    {
                        split = static_cast<uint32_t>(all.size());
                    }
                    all.push_back(vidcmd_literal(ss_c(j)));
                }
                all.push_back(vidcmd_hold(8));
            }
            all.push_back(vidcmd_run(RUN_SRC_PASSTHROUGH, 508));
            for (uint32_t i = 0; i < all.size(); i++)
            {
                if (i < split)
                {
                    spec.words.push_back(all[i]);
                }
                else
                {
                    spec.late.push_back({0, 114, all[i]});
                }
            }
            spec.late.push_back({1, LT_BEFORE_BLANK, vidcmd_literal(next)});
            spec.late.push_back({1, LT_BEFORE_BLANK, vidcmd_hold(638)});
            const LtTrace t = lt_trace(spec);

            std::vector<Rgb444> want0(H_ACTIVE, pt);
            std::vector<Rgb444> want1(H_ACTIVE, next);
            for (uint32_t j = 0; j <= 5; j++)
            {
                want0[100 + 2 * j]     = ss_c(j);
                want0[100 + 2 * j + 1] = ss_c(j);
            }
            for (uint32_t s = 110; s < 118; s++)
            {
                want0[s] = ss_c(5);
            }
            for (uint32_t j = 6; j <= 11; j++)
            {
                want0[118 + 2 * (j - 6)]     = ss_c(j);
                want0[118 + 2 * (j - 6) + 1] = ss_c(j);
            }
            const uint32_t tail_end = (form == 0) ? 131 : 137;
            for (uint32_t s = 128; s <= tail_end; s++)
            {
                want0[s] = ss_c(11);
            }
            if (form == 1)
            {
                for (uint32_t s = 0; s < 6; s++)
                {
                    want1[s] = pt;   // the overrunning exit RUN's last six slots
                }
            }
            ls_line_check(form == 0 ? "d  stream run, C6.. late: geometry kept (line 0)"
                                    : "d  literals, C6.. late: everything after shifts (line 0)",
                          t.px[0], want0, RenderStats{});
            uint32_t bad1 = 0;
            for (uint32_t s = 0; s < H_ACTIVE; s++)
            {
                bad1 += (t.px[1][s] != want1[s]) ? 1u : 0u;
            }
            printf("  %-50s %3u slots off  overruns %u  leftovers %u\n",
                   form == 0 ? "d  stream run: line 1 unshifted" : "d  literals: line 1 starts 6 late",
                   bad1, t.stats.vidcmd_overruns, t.stats.vidcmd_stream_leftovers);
            CHECK(bad1 == 0, "m14 (d) form %d: line 1 has %u slots off its derivation", form, bad1);
            CHECK(t.stats.vidcmd_overruns == static_cast<uint32_t>(form == 0 ? 0 : 2),
                  "m14 (d) form %d: %u overruns, derived %d", form, t.stats.vidcmd_overruns,
                  form == 0 ? 0 : 2);
            CHECK(t.stats.vidcmd_stream_leftovers == 0, "m14 (d): stream leftovers");
        }
    }

    // (e) A palette entry inside a STREAM RUN: legal over a colour register,
    // flagged over passthrough (the four disturbed slots after the data SET
    // reach the DAC).  The entry is stored either way.
    for (uint32_t src : {RUN_SRC_COLOR1, RUN_SRC_PASSTHROUGH})
    {
        const PaletteEntryWords e = vidcmd_palette_entry(0xB, 0x5A5);
        const LtTrace t = lt_trace({vidcmd_stream_run(src, 40), e.w[0], e.w[1], e.w[2],
                                    vidcmd_run(RUN_SRC_PASSTHROUGH, 600)});
        const bool legal = (src != RUN_SRC_PASSTHROUGH);
        printf("  e  palette entry in STREAM(%s,40)%*s stored 0x%03X  disturbed visible %u  %s\n",
               legal ? "c1" : "pt", 13, "", t.post[0].clut_b, t.stats.palette_disturbed_visible,
               legal ? "legal" : "FLAGGED, as derived");
        CHECK(t.post[0].clut_b == 0x5A5 && t.stats.palette_bad_filler == 0,
              "m14 (e): the streamed palette entry was not stored");
        CHECK(legal ? (t.stats.palette_disturbed_visible == 0)
                    : (t.stats.palette_disturbed_visible > 0),
              "m14 (e): a streamed palette entry over %s shows %u disturbed pixels",
              legal ? "colour1" : "passthrough", t.stats.palette_disturbed_visible);
    }
}

// --- the frame case: (b) painter's-order slicing and (f) delivery ------------
//
// A 4bpp half-rate playfield (the m12/m13 one) and a band of 32 sprite lines,
// 200..231.  Three variants:
//
//   K0_VIDCMD_FIRST  the back row at x = 0 (the left edge), its program two
//                    descriptors pointing at static data — the row and a tail
//                    word out of a RUN(pt, n) table — FIRST in the line's own
//                    HBLANK group, then the line's 80 PIXELS words.  R5: met.
//   K1_SLICED        a back row at xb(i) and a FRONT row of 8 sliced into it at
//                    element a(i): lead word, back[0..a), front[0..8),
//                    back[a+8..16), tail word — up to five descriptors at
//                    sub-ranges of static rows, deposited one line ahead, behind
//                    the PIXELS words.  R4: no fix-up word.
//   K0_PIXELS_FIRST  NEGATIVE CONTROL: K0's program behind the 80 PIXELS words.
//                    Its first VIDCMD word lands about 211 SYSCLK after the
//                    edge, 219 pixel clocks into the line (R5): the row is late.

enum class LsVariant : uint8_t
{
    K0_VIDCMD_FIRST,
    K1_SLICED,
    K0_PIXELS_FIRST,
};

constexpr uint32_t LS_DATA_BASE      = 0x360000;   // read-only, written once
constexpr uint32_t LS_OFF_GAP_TABLE  = 0x000;      // RUN(pt, 1..640)
constexpr uint32_t LS_OFF_BACK       = 0x600;
constexpr uint32_t LS_OFF_FRONT      = 0x640;
constexpr uint32_t LS_PREAMBLE_BASE  = 0x380000;
constexpr uint32_t LS_PREAMBLE_STEP  = 0x000400;
constexpr uint32_t LS_BAND_FIRST     = 200;
constexpr uint32_t LS_BAND_LINES     = 32;

bool ls_band(uint32_t y)
{
    return y >= LS_BAND_FIRST && y < LS_BAND_FIRST + LS_BAND_LINES;
}

uint32_t ls_xb(uint32_t i)
{
    return 160 + 3 * i;   // both parities
}

uint32_t ls_a(uint32_t i)
{
    return i % (LS_FRONT + 1);   // 0..8: the front's element offset into the back row
}

uint32_t ls_gap_addr(uint32_t n)
{
    return LS_DATA_BASE + LS_OFF_GAP_TABLE + (n - 1u) * 2u;
}

void ls_write_data(Memory ram)
{
    for (uint32_t n = 1; n <= H_ACTIVE; n++)
    {
        ram[ls_gap_addr(n)] = vidcmd_run(RUN_SRC_PASSTHROUGH, n);
    }
    for (uint32_t e = 0; e < LS_ROW; e++)
    {
        ram[LS_DATA_BASE + LS_OFF_BACK + e * 2] = ls_element(LS_BACK_ART[e]);
    }
    for (uint32_t e = 0; e < LS_FRONT; e++)
    {
        ram[LS_DATA_BASE + LS_OFF_FRONT + e * 2] = ls_element(LS_FRONT_ART[e]);
    }
}

struct LsPiece
{
    uint32_t addr;
    uint32_t words;
};

// Line y's program as pieces of static data, in stream order.
std::vector<LsPiece> ls_pieces(LsVariant v, uint32_t y)
{
    if (!ls_band(y))
    {
        return {{ls_gap_addr(H_ACTIVE), 1}};
    }
    const uint32_t back  = LS_DATA_BASE + LS_OFF_BACK;
    const uint32_t front = LS_DATA_BASE + LS_OFF_FRONT;
    if (v != LsVariant::K1_SLICED)
    {
        return {{back, LS_ROW}, {ls_gap_addr(H_ACTIVE - 2 * LS_ROW), 1}};
    }
    const uint32_t i  = y - LS_BAND_FIRST;
    const uint32_t xb = ls_xb(i);
    const uint32_t a  = ls_a(i);
    std::vector<LsPiece> p = {{ls_gap_addr(xb), 1}};
    if (a > 0)
    {
        p.push_back({back, a});
    }
    p.push_back({front, LS_FRONT});
    if (a + LS_FRONT < LS_ROW)
    {
        p.push_back({back + (a + LS_FRONT) * 2, LS_ROW - (a + LS_FRONT)});
    }
    p.push_back({ls_gap_addr(H_ACTIVE - xb - 2 * LS_ROW), 1});
    return p;
}

uint32_t ls_line_words(LsVariant v, uint32_t y)
{
    uint32_t n = 0;
    for (const LsPiece &p : ls_pieces(v, y))
    {
        n += p.words;
    }
    return n;
}

constexpr uint32_t LS_FIXED_PREAMBLE = PALETTE_ENTRIES * PALETTE_WORDS_PER_ENTRY + 2;

uint32_t ls_author(Memory ram, LsVariant v, uint32_t table, uint32_t frame)
{
    const uint32_t preamble = LS_PREAMBLE_BASE + frame * LS_PREAMBLE_STEP;
    uint32_t n = 0;
    for (uint32_t i = 0; i < PALETTE_ENTRIES; i++)
    {
        for (uint16_t w : vidcmd_palette_entry(i, cc_palette(i)))
        {
            ram[preamble + (n++) * 2] = w;
        }
    }
    ram[preamble + (n++) * 2] = vidcmd_set(SET_PIX_BASE, 0);
    ram[preamble + (n++) * 2] = vidcmd_set(SET_PIX_MODE, pixel_mode(PIXEL_DEPTH_4BPP, true));
    if (v == LsVariant::K1_SLICED)
    {
        // One line of lookahead: line 0's program has no group to ride in.
        for (const LsPiece &p : ls_pieces(v, 0))
        {
            for (uint32_t k = 0; k < p.words; k++)
            {
                ram[preamble + (n++) * 2] = ram[p.addr + k * 2];
            }
        }
    }

    CcEmitter e{ram, table, 0};
    for (uint32_t i = 0; i < CC_PACERS_BEFORE_PREAMBLE; i++)
    {
        e.pacer(false);
    }
    e.stream(preamble, n, DEST_VIDCMD_FIFO_W, false);
    for (uint32_t i = 0; i < CC_PACERS_AFTER_PREAMBLE; i++)
    {
        e.pacer(false);
    }
    for (uint32_t y = 0; y < V_ACTIVE; y++)
    {
        const uint32_t px = CC_FB_BASE + (y / 2) * CC_FB_STRIDE;
        auto program = [&](uint32_t line, bool wait_first)
        {
            bool first = wait_first;
            for (const LsPiece &p : ls_pieces(v, line))
            {
                e.stream(p.addr, p.words, DEST_VIDCMD_FIFO_W, first);
                first = false;
            }
        };
        if (v == LsVariant::K0_VIDCMD_FIRST)
        {
            program(y, true);
            e.stream(px, PIXELS_WORDS_4BPP_HALF, DEST_PIXELS_FIFO_W, false);
        }
        else
        {
            e.stream(px, PIXELS_WORDS_4BPP_HALF, DEST_PIXELS_FIFO_W, true);
            if (v == LsVariant::K0_PIXELS_FIRST)
            {
                program(y, false);
            }
            else if (y + 1 < V_ACTIVE)
            {
                program(y + 1, false);
            }
        }
    }
    e.pacer(true);
    return e.count;
}

Rgb444 ls_expected(LsVariant v, uint32_t x, uint32_t y)
{
    const Rgb444 bd = cc_palette(cc_fb_nibble(x / 2, y / 2));
    if (!ls_band(y))
    {
        return bd;
    }
    if (v != LsVariant::K1_SLICED)
    {
        if (x < 2 * LS_ROW)
        {
            const uint32_t a = LS_BACK_ART[x / 2];
            return (a == LS_HOLE) ? bd : static_cast<Rgb444>(a);
        }
        return bd;
    }
    const uint32_t i  = y - LS_BAND_FIRST;
    const uint32_t xb = ls_xb(i);
    if (x < xb || x >= xb + 2 * LS_ROW)
    {
        return bd;
    }
    const uint32_t e = (x - xb) / 2;
    const uint32_t a = ls_a(i);
    // Painter's order by slicing: inside the front's extent the FRONT decides,
    // and its transparent pixels show the backdrop (the notch), not the back.
    const uint32_t art = (e >= a && e < a + LS_FRONT) ? LS_FRONT_ART[e - a] : LS_BACK_ART[e];
    return (art == LS_HOLE) ? bd : static_cast<Rgb444>(art);
}

struct LsRun
{
    RenderResult    r;
    InterpretResult interp;
    uint32_t        descriptors = 0;
    int64_t         min_margin  = INT64_MAX;   // SYSCLK: band line pixel 0 - its last VIDCMD word
};

LsRun ls_run(Memory ram, LsVariant v, bool line_at_a_time)
{
    LsRun          out;
    const uint32_t tables[RENDER_FRAMES] = {TABLE_A, TABLE_B};
    InterpretParams ip;
    ip.start_cycle          = first_arm_cycle();
    ip.end_cycle            = run_end_cycle();
    ip.rearm_latency_cycles = REARM_LATENCY_CYCLES;
    for (uint32_t f = 0; f < RENDER_FRAMES; f++)
    {
        out.descriptors = ls_author(ram, v, tables[f], f);
        ip.arm_addresses.push_back(tables[f]);
    }
    out.interp = interpret(ram, ip);

    RenderParams rp;
    rp.first_frame = RENDER_FIRST_FRAME;
    rp.frame_count = RENDER_FRAMES;
    out.r = line_at_a_time ? render_line_at_a_time(out.interp.events, rp)
                           : render(out.interp.events, rp);

    // R5, measured clean-room off the deposit stream.  Each list's VIDCMD
    // stream is its fixed preamble then the line programs in line order (K1's
    // line 0 rides in the preamble), so prefix sums turn a word index into the
    // line it belongs to.
    std::vector<uint64_t> cycles;
    for (const DepositEvent &e : out.interp.events)
    {
        if (e.destination == DEST_VIDCMD_FIFO_W)
        {
            cycles.push_back(e.cycle);
        }
    }
    size_t idx = 0;
    for (uint32_t f = 0; f < RENDER_FRAMES && idx < cycles.size(); f++)
    {
        idx += LS_FIXED_PREAMBLE;
        for (uint32_t y = 0; y < V_ACTIVE; y++)
        {
            idx += ls_line_words(v, y);
            if (ls_band(y) && idx - 1 < cycles.size())
            {
                const uint64_t line0 = static_cast<uint64_t>(RENDER_FIRST_FRAME + f) * V_TOTAL + y;
                const int64_t  m = static_cast<int64_t>(sysclk_of_pixel(line0 * H_TOTAL)) -
                                   static_cast<int64_t>(cycles[idx - 1]);
                out.min_margin = (m < out.min_margin) ? m : out.min_margin;
            }
        }
    }
    return out;
}

void run_literal_sprite_case(Memory ram)
{
    printf("\n=== m14-literal-sprites — LITERAL rows over a 4bpp half-rate playfield, band "
           "lines %u..%u ===\n", LS_BAND_FIRST, LS_BAND_FIRST + LS_BAND_LINES - 1);
    cc_write_framebuffer(ram);
    ls_write_data(ram);

    // The planner (cushion assumption) agrees every band program closes at
    // exactly 640 slots with nothing late (R1, R2).
    for (LsVariant v : {LsVariant::K0_VIDCMD_FIRST, LsVariant::K1_SLICED})
    {
        uint32_t bad = 0;
        for (uint32_t y = LS_BAND_FIRST; y < LS_BAND_FIRST + LS_BAND_LINES; y++)
        {
            std::vector<uint16_t> w;
            for (const LsPiece &p : ls_pieces(v, y))
            {
                for (uint32_t k = 0; k < p.words; k++)
                {
                    w.push_back(ram[p.addr + k * 2]);
                }
            }
            const VidcmdSlotPlan plan = vidcmd_plan_line(w);
            bad += (plan.slots != H_ACTIVE || plan.stretch != 0) ? 1u : 0u;
        }
        CHECK(bad == 0, "m14: %u band programs do not plan to exactly 640 slots", bad);
    }

    const uint32_t opaque_back  = 12;   // LS_BACK_ART: 16 minus 4 holes (elements 2, 6, 7, 10)
    struct Row
    {
        const char *name;
        LsVariant   v;
        bool        clean;
    };
    const Row rows[] = {
        {"k0-vidcmd-first (left edge, x = 0)", LsVariant::K0_VIDCMD_FIRST, true},
        {"k1-sliced (painter's order by slices)", LsVariant::K1_SLICED, true},
        {"k0-pixels-first (NEGATIVE CONTROL)", LsVariant::K0_PIXELS_FIRST, false},
    };
    for (const Row &row : rows)
    {
        for (bool lat : {false, true})
        {
            if (lat && !row.clean)
            {
                continue;
            }
            const LsRun run = ls_run(ram, row.v, lat);
            const RenderStats &s = run.r.stats;
            uint32_t bad = 0;
            for (uint32_t f = 0; f < RENDER_FRAMES; f++)
            {
                for (uint32_t y = 0; y < V_ACTIVE; y++)
                {
                    for (uint32_t x = 0; x < H_ACTIVE; x++)
                    {
                        bad += (at(run.r.frames[f], x, y) != ls_expected(row.v, x, y)) ? 1u : 0u;
                    }
                }
            }
            printf("  %-40s %s  %7u px off  margin %+lld SYSCLK  cadence %u  starved %u  "
                   "literals %u  desc %u\n", row.name, lat ? "emu " : "clk ", bad,
                   static_cast<long long>(run.min_margin), s.vidcmd_cadence_slots,
                   s.vidcmd_hold_slots, s.vidcmd_literals, run.descriptors);
            CHECK(run.interp.violations.empty(), "m14/%s: %zu interpreter violations", row.name,
                  run.interp.violations.size());
            CHECK(s.vidcmd_overflows == 0 && s.pixels_overflows == 0,
                  "m14/%s: a FIFO overflowed", row.name);
            CHECK(s.palette_unwritten_reads == 0 && s.palette_bad_filler == 0 &&
                  s.palette_disturbed_visible == 0, "m14/%s: palette rule broken", row.name);
            CHECK(run.descriptors * DESC_BYTES <= TABLE_HALF_BYTES,
                  "m14/%s: %u descriptors overflow the table half", row.name, run.descriptors);
            if (row.clean)
            {
                CHECK(bad == 0, "m14/%s: %u pixels differ from the hand-derived frame", row.name, bad);
                CHECK(s.vidcmd_cadence_slots == 0 && s.vidcmd_hold_slots == 0,
                      "m14/%s: %u cadence / %u starvation holds, derived 0 / 0 (R1)", row.name,
                      s.vidcmd_cadence_slots, s.vidcmd_hold_slots);
                CHECK(s.vidcmd_overruns == 0, "m14/%s: %u overruns", row.name, s.vidcmd_overruns);
                CHECK(run.min_margin > 0, "m14/%s: a band line's last VIDCMD word arrived %lld "
                      "SYSCLK after its pixel 0", row.name, static_cast<long long>(-run.min_margin));
                if (row.v == LsVariant::K0_VIDCMD_FIRST)
                {
                    // R5: the last word 55 SYSCLK after the edge, the edge 160
                    // pixel clocks = 89 SYSCLK before pixel 0: a margin near 34.
                    CHECK(run.min_margin >= 25 && run.min_margin <= 40,
                          "m14/%s: margin %lld SYSCLK, derived about 34 (R5)", row.name,
                          static_cast<long long>(run.min_margin));
                    CHECK(s.vidcmd_literals == opaque_back * LS_BAND_LINES * RENDER_FRAMES,
                          "m14/%s: %u literals played, derived %u", row.name, s.vidcmd_literals,
                          opaque_back * LS_BAND_LINES * RENDER_FRAMES);
                }
            }
            else
            {
                CHECK(bad != 0, "m14/%s: the late program went undetected", row.name);
                CHECK(run.min_margin < 0, "m14/%s: the negative control met its deadline", row.name);
            }
        }
    }

    // NEGATIVE CONTROL on the reference itself: a one-slot slip of the front
    // slice must be visible.
    {
        const LsRun run = ls_run(ram, LsVariant::K1_SLICED, false);
        uint32_t slip = 0;
        for (uint32_t y = LS_BAND_FIRST; y < LS_BAND_FIRST + LS_BAND_LINES; y++)
        {
            for (uint32_t x = 1; x < H_ACTIVE; x++)
            {
                slip += (at(run.r.frames[0], x, y) != ls_expected(LsVariant::K1_SLICED, x - 1, y))
                            ? 1u : 0u;
            }
        }
        printf("  NEGATIVE CONTROL  reference one slot right: %u pixels wrong\n", slip);
        CHECK(slip != 0, "m14: a one-slot slip of the sliced rows went undetected");
        const std::string path = "m14-literal-sprites.ppm";
        write_ppm(path, run.r.frames[0]);
        printf("  artifact %s\n", path.c_str());
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
    write_test_pattern_indexed2(ram, FB_IDX2_BASE, FB_IDX2_STRIDE, FB_LINES);
    write_test_pattern_indexed4_half(ram, FB_IDX4_BASE, FB_IDX4_STRIDE, FB_LINES);
    write_test_pattern_attr(ram, FB_ATTR_BASE, FB_ATTR_STRIDE, PIXELS_WORDS_ATTR, FB_LINES);
    write_audio_source(ram, AUDIO_BASE, AUDIO_SOURCE_PAIRS);
    draw_tempest_web(ram, FB_WEB_BASE, FB_STRIDE_BYTES);

    FrameParams common;
    common.fb_base         = FB_BASE;
    common.fb_stride_bytes = FB_STRIDE_BYTES;
    common.vidcmd_base     = VIDCMD_BASE;
    common.audio_base      = AUDIO_BASE;
    common.audio_source_pairs = AUDIO_SOURCE_PAIRS;

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
    printf("  PIXEL is INDEX-ONLY: mode[1:0] depth (1bpp / 2bpp / 4bpp / attribute), "
           "mode[2] half rate, mode[3] reserved\n");
    printf("  palette 16 x 12 in external RAM, NEVER RESET, random at power-up; "
           "%u VIDCMD words per entry\n", PALETTE_WORDS_PER_ENTRY);
    printf("  PIXELS words/line   full rate %u 1bpp, %u 2bpp, %u attr (4bpp UNDEFINED); "
           "half rate %u / %u / %u / %u 4bpp\n",
           PIXELS_WORDS_1BPP, PIXELS_WORDS_2BPP, PIXELS_WORDS_ATTR,
           PIXELS_WORDS_1BPP_HALF, PIXELS_WORDS_2BPP_HALF, PIXELS_WORDS_ATTR_HALF,
           PIXELS_WORDS_4BPP_HALF);

    // The laws first, directly against the two units, before any list builder
    // gets to interpret them.
    check_pixel_modes();
    check_palette_traces();
    check_cadence_traces();
    check_mask_traces();
    check_literal_traces();

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
        // The two PAIRS the mid-line SET(pix_base) switches between, loaded once
        // per frame in vertical blanking: base 0 is {c1, c1} and base
        // REGRESSION_BASE_HI is {c2, c2}, so the pixel a SET lands on is named by
        // its colour and not by the bitmap.
        s.base.palette[0] = s.base.regression_c1;
        s.base.palette[1] = s.base.regression_c1;
        s.base.palette[REGRESSION_BASE_HI | 0] = s.base.regression_c2;
        s.base.palette[REGRESSION_BASE_HI | 1] = s.base.regression_c2;
        s.base.palette_entries = PALETTE_ENTRIES;
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
        s.title = "1bpp vertical scroll with a 9-word-per-FRAME VIDCMD preamble";
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

        // This is the firmware console's list shape: NINE VIDCMD words buy the
        // whole 480-line frame, because compositor.v holds the last source to
        // the end of a line and a line that gets no fill keeps holding.  Nine
        // and not three since the palette became a RAM: two three-word table
        // entries for the console's two colours, then SET(pix_base),
        // SET(pix_mode) and the one-slot passthrough RUN.
        printf("  JIT framing         %u VIDCMD words for the whole frame (%u..%u per line); "
               "hold covered %u slots\n",
               r.vidcmd_words_total, r.vidcmd_words_min, r.vidcmd_words_max,
               r.rendered.stats.vidcmd_hold_slots);
        CHECK(r.vidcmd_words_total == 2 * PALETTE_WORDS_PER_ENTRY + 3,
              "m1-vscroll-jit: expected exactly %u VIDCMD words per frame (two palette "
              "entries, base, mode and the RUN), got %u",
              2 * PALETTE_WORDS_PER_ENTRY + 3, r.vidcmd_words_total);
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
        s.title = "mid-line recolour by SET(pix_base), placed by VIDCMD slot arithmetic";
        s.base  = common;
        s.base.per_line_palette = true;
        s.base.per_line_mode    = true;
        s.base.mid_line_split   = true;
        s.base.split_pixel      = 320;
        s.base.split_fg         = rgb444(15, 0, 0);
        s.base.split_bg         = rgb444(0, 15, 0);
        // The two PAIRS the banked SET(pix_base) pair switches to, loaded once
        // per frame in vertical blanking.  Both entries of each pair carry ONE
        // colour, so the pixel a SET landed on is named by its colour whatever
        // the bitmap says there.  Entries 0 and 1 are the per-line ramp and are
        // rewritten in every HBLANK.
        s.base.palette[SPLIT_BASE_MID | 0] = s.base.split_bg;
        s.base.palette[SPLIT_BASE_MID | 1] = s.base.split_bg;
        s.base.palette[SPLIT_BASE_HI  | 0] = s.base.split_fg;
        s.base.palette[SPLIT_BASE_HI  | 1] = s.base.split_fg;
        s.base.palette_entries  = PALETTE_ENTRIES;
        CaseResult r = run_case(s, ram, REARM_LATENCY_CYCLES);
        write_artifacts(s, r);
        print_report(s, r);
        check_case(s, r);

        // Hand-computed expectation on line 100 of frame 1 (h_scroll 0):
        //   entries 0/1 are the line's own ramp: fg rgb444(3,12,15), bg (0,0,5)
        //   pattern bit(x,y) = ((x+y)/16)&1, and line 100 is not a marker row
        //   x=319: (419/16)=26 even -> bit 0 -> entry 0, the line's own bg
        //   x=320: the FIRST SET(pix_base) owns this slot -> the MID pair, which
        //          carries split_bg in BOTH entries
        //   x=336: the SECOND SET owned slot 321 -> the HI pair, split_fg
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
              "m2-split: pixel 320 is 0x%03X, expected the MID pair's 0x%03X — the first "
              "SET(pix_base) did not land on its own slot", at(f1, 320, 100), s.base.split_bg);
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

    // --- ATTRIBUTE MODE, full rate: 80-column coloured text cells --------------
    //
    // One {fg[3:0], bg[3:0], pattern[7:0]} word per eight-pixel cell, so a
    // coloured 80-column text screen costs the same 80 words a line a 2bpp
    // picture does and needs no per-cell VIDCMD at all: the colours are IN the
    // stream.  With a TMS9918 palette loaded this is its Graphics II word.
    {
        CaseSpec s;
        s.name  = "m2-attr";
        s.title = "attribute mode: 80 coloured text cells a line, colours in the stream";
        s.base  = common;
        s.base.mode             = PixelMode::ATTR;
        s.base.fb_base          = FB_ATTR_BASE;
        s.base.fb_stride_bytes  = FB_ATTR_STRIDE;
        s.base.per_line_mode    = true;
        s.base.palette          = TEST_PALETTE;
        s.base.palette_entries  = PALETTE_ENTRIES;
        CaseResult r = run_case(s, ram, REARM_LATENCY_CYCLES);
        write_artifacts(s, r);
        print_report(s, r);
        check_case(s, r);

        const uint32_t words = pixel_words_for(s.base.mode, 0);
        printf("  stream cost         %u words/line, one per eight-pixel cell\n", words);
        CHECK(words == PIXELS_WORDS_ATTR,
              "m2-attr: %u words/line, expected %u", words, PIXELS_WORDS_ATTR);

        const FrameImage ref = attr_reference_frame(s.base);
        const uint32_t differing = compare_frames("attr", r, ref);
        CHECK(differing == 0,
              "m2-attr: %u pixels differ from the host-rasterized reference — the colour "
              "byte, the pattern byte or the cell boundary is wrong", differing);

        // The colour byte must belong to ITS OWN cell.  attr_colour changes one
        // clock after the load, which is the only edge that keeps the previous
        // cell's last pixel out of the new cell's colours (pixel.v; pixel_tb.v's
        // mut4 and mut6 are the negative controls).  Measured here as: the two
        // pixels either side of a cell boundary follow their own cells.
        const FrameImage &f1 = r.rendered.frames[0];
        uint32_t seam_bad = 0;
        for (uint32_t y = 0; y < V_ACTIVE; y++)
        {
            for (uint32_t cell = 1; cell < PIXELS_WORDS_ATTR; cell++)
            {
                const uint32_t x = cell * 8u;
                if (at(f1, x, y) != ref.pixels[y * H_ACTIVE + x] ||
                    at(f1, x - 1, y) != ref.pixels[y * H_ACTIVE + x - 1])
                {
                    seam_bad++;
                }
            }
        }
        printf("  cell seams          %u boundaries checked, %u wrong: the colour byte "
               "changes on the cell's first pixel and not before\n",
               (PIXELS_WORDS_ATTR - 1) * V_ACTIVE, seam_bad);
        CHECK(seam_bad == 0, "m2-attr: %u cell boundaries take the wrong cell's colour",
              seam_bad);
    }

    // --- M8: 2bpp, four entries a line, and a mid-line QUAD switch -------------
    uint32_t m8_vidcmd_words = 0;
    {
        CaseSpec s;
        s.name  = "m8-idx2bpp";
        s.title = "2bpp: 80 words/line, a palette quad, and a mid-line SET(pix_base)";
        s.base  = common;
        s.base.mode             = PixelMode::TWO_BPP;
        s.base.fb_base          = FB_IDX2_BASE;
        s.base.fb_stride_bytes  = FB_IDX2_STRIDE;
        s.base.per_line_mode    = true;
        s.base.palette          = TEST_PALETTE;
        s.base.palette_entries  = PALETTE_ENTRIES;
        // THE WHOLE PALETTE ONCE PER FRAME, in the frame's own preamble: sixteen
        // three-word entries, 48 words, all of them in blanking where the write's
        // four disturbed pixel slots cannot be seen.  Nothing in hardware clears
        // the RAM, so this is also the assertion that the entries survive 479
        // lines and the /RS between frames.
        //
        // A mid-line recolour of all four indices at once, by switching the QUAD
        // rather than by writing the RAM: two SET(pix_base) records as the
        // leading RUN's banked pair, so they land on consecutive slots.  A
        // mid-line palette WRITE would need three words per entry and four
        // covered slots; a base switch is one word and is pixel-exact.
        s.base.mid_line_split   = true;
        s.base.split_pixel      = 320;
        CaseResult r = run_case(s, ram, REARM_LATENCY_CYCLES);
        write_artifacts(s, r);
        print_report(s, r);
        check_case(s, r);

        const uint32_t words = pixel_words_for(s.base.mode, 0);
        printf("  stream rate         %u words/line at two stream bits per pixel clock\n",
               words);
        CHECK(words == PIXELS_WORDS_2BPP,
              "m8-idx2bpp: %u words/line, expected %u", words, PIXELS_WORDS_2BPP);

        const FrameImage ref = indexed2_reference_frame(s.base);
        const uint32_t differing = compare_frames("idx2bpp", r, ref);
        CHECK(differing == 0,
              "m8-idx2bpp: %u pixels differ from the host-rasterized reference — the dibit "
              "index, the palette quad or the mid-line SET seam is wrong", differing);

        // The palette load survived the frame: the last line's colours come out
        // of entries loaded in line 0's blanking.
        CHECK(r.rendered.stats.palette_unwritten_reads == 0,
              "m8-idx2bpp: %u pixels read palette entries nobody loaded",
              r.rendered.stats.palette_unwritten_reads);
        CHECK(r.rendered.stats.palette_bad_filler == 0,
              "m8-idx2bpp: %u palette writes stored the wrong colour — a filler is missing "
              "or carries the wrong value", r.rendered.stats.palette_bad_filler);
        CHECK(r.rendered.stats.palette_disturbed_visible == 0,
              "m8-idx2bpp: %u palette-write slots reached the DAC through passthrough; the "
              "frame's entries are all loaded in blanking, so none should",
              r.rendered.stats.palette_disturbed_visible);

        // The mid-line seam, pixel by pixel: slot 320 takes the MID quad and
        // slot 321 onwards the HI quad, each SET visible in its own slot.
        const FrameImage &f1 = r.rendered.frames[0];
        {
            const uint32_t probe = 100;
            const uint32_t code_at_320 = idx2_pattern_code(320, probe);
            const uint32_t code_at_336 = idx2_pattern_code(336, probe);
            const Rgb444 want_320 =
                TEST_PALETTE[palette_index_2bpp(SPLIT_BASE_MID, code_at_320)];
            const Rgb444 want_336 =
                TEST_PALETTE[palette_index_2bpp(SPLIT_BASE_HI, code_at_336)];
            printf("  split seam          line %u: x=319 0x%03X, x=320 0x%03X (want the MID "
                   "quad's 0x%03X), x=336 0x%03X (want the HI quad's 0x%03X)\n",
                   probe, at(f1, 319, probe), at(f1, 320, probe), want_320,
                   at(f1, 336, probe), want_336);
            CHECK(at(f1, 320, probe) == want_320,
                  "m8-idx2bpp: pixel 320 of line %u is 0x%03X, expected 0x%03X — the first "
                  "SET(pix_base) did not land on its own slot",
                  probe, at(f1, 320, probe), want_320);
            CHECK(at(f1, 336, probe) == want_336,
                  "m8-idx2bpp: pixel 336 of line %u is 0x%03X, expected 0x%03X — the second "
                  "SET(pix_base) did not land one slot later",
                  probe, at(f1, 336, probe), want_336);
        }

        // All four codes are on the screen, in the quad the base names.
        uint32_t per_code[4] = {0, 0, 0, 0};
        for (uint32_t y = 0; y < V_ACTIVE; y++)
        {
            for (uint32_t x = 0; x < H_ACTIVE; x++)
            {
                per_code[idx2_pattern_code(x, y)]++;
            }
        }
        const uint32_t want_quarter = (H_ACTIVE / 4u) * V_ACTIVE;
        printf("  four codes          %u / %u / %u / %u pixels (want %u each)\n",
               per_code[0], per_code[1], per_code[2], per_code[3], want_quarter);
        CHECK(per_code[0] == want_quarter && per_code[3] == want_quarter,
              "m8-idx2bpp: the pattern does not spend a quarter of the frame on each code");

        // Per-line delivery budget: the pixel stream AND the line's records, in
        // words, against the same 66%-of-the-bus cap the screen cases use.
        m8_vidcmd_words = r.vidcmd_words_max;
        const uint32_t line_total = words + r.vidcmd_words_max;
        printf("  line word budget    %u PIXELS + %u VIDCMD = %u words/line against the "
               "%u-word cap (%u%% of the %llu-SYSCLK line)\n",
               words, r.vidcmd_words_max, line_total, VIDCMD_WORDS_PER_LINE_CAP,
               VIDCMD_BUS_BUDGET_PERCENT, static_cast<unsigned long long>(LINE_SYSCLK));
        CHECK(line_total <= VIDCMD_WORDS_PER_LINE_CAP,
              "m8-idx2bpp: %u words/line (%u pixel + %u VIDCMD) is over the %u-word cap",
              line_total, words, r.vidcmd_words_max, VIDCMD_WORDS_PER_LINE_CAP);
    }

    // --- M10: 4bpp at half rate, all sixteen entries on every line -------------
    //
    // The deepest defined mode: 320 groups of four bits, each group two pixels
    // wide, indexing the whole palette with no pix_base at all.  4bpp at FULL
    // rate is UNDEFINED by design (two fetch triggers inside the eight-clock
    // end-of-line window) and the suite never emits it.
    {
        CaseSpec s;
        s.name  = "m10-idx4-half";
        s.title = "4bpp half rate: 80 words/line, all sixteen palette entries";
        s.base  = common;
        s.base.mode             = PixelMode::HALF_FOUR_BPP;
        s.base.fb_base          = FB_IDX4_BASE;
        s.base.fb_stride_bytes  = FB_IDX4_STRIDE;
        s.base.per_line_mode    = true;
        s.base.palette          = TEST_PALETTE;
        s.base.palette_entries  = PALETTE_ENTRIES;
        CaseResult r = run_case(s, ram, REARM_LATENCY_CYCLES);
        write_artifacts(s, r);
        print_report(s, r);
        check_case(s, r);

        const uint32_t words = pixel_words_for(s.base.mode, 0);
        printf("  stream cost         %u words/line for 320 four-bit groups\n", words);
        CHECK(words == PIXELS_WORDS_4BPP_HALF,
              "m10-idx4-half: %u words/line, expected %u", words, PIXELS_WORDS_4BPP_HALF);
        CHECK(!pixels_mode_defined(pixel_mode(PIXEL_DEPTH_4BPP, false)),
              "m10-idx4-half: 4bpp at FULL rate must be refused by the model, not computed");

        const FrameImage ref = indexed4_half_reference_frame(s.base);
        const uint32_t differing = compare_frames("idx4half", r, ref);
        CHECK(differing == 0,
              "m10-idx4-half: %u pixels differ from the host-rasterized reference — the "
              "nibble index or the half-rate pairing is wrong", differing);

        // Every entry is used, and every group spans exactly two pixels.
        const FrameImage &f1 = r.rendered.frames[0];
        bool seen[PALETTE_ENTRIES] = {};
        uint32_t split = 0;
        for (uint32_t y = 0; y < V_ACTIVE; y++)
        {
            for (uint32_t k = 0; k < H_ACTIVE / 2; k++)
            {
                seen[idx4_pattern_code(k, y)] = true;
                if (at(f1, 2 * k, y) != at(f1, 2 * k + 1, y))
                {
                    split++;
                }
            }
        }
        uint32_t used = 0;
        for (bool b : seen)
        {
            used += b ? 1u : 0u;
        }
        printf("  palette coverage    %u of %u entries displayed, %u groups span other than "
               "two slots\n", used, PALETTE_ENTRIES, split);
        CHECK(used == PALETTE_ENTRIES,
              "m10-idx4-half: only %u of %u entries were displayed", used, PALETTE_ENTRIES);
        CHECK(split == 0,
              "m10-idx4-half: %u groups do not span exactly two pixel clocks", split);
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
        CHECK(m9_half_1bpp_words == PIXELS_WORDS_1BPP_HALF,
              "m9-halfrate: half-rate 1bpp is %u words/line, expected %u",
              m9_half_1bpp_words, PIXELS_WORDS_1BPP_HALF);

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

        // The same case in ATTRIBUTE mode at half rate: a 320-wide tile screen,
        // 40 words a line, under the same full-resolution sprites.  Same 320
        // groups, same group law — which is what says half rate is one gate and
        // not a per-mode special case.
        CaseSpec t = s;
        t.name  = "m9-halfrate-attr";
        t.title = "half rate in attribute mode: a 320-wide tile screen, 40 words/line";
        t.base.mode             = PixelMode::HALF_ATTR;
        t.base.fb_base          = FB_ATTR_BASE;
        t.base.fb_stride_bytes  = FB_ATTR_STRIDE;
        t.base.per_line_palette = false;
        t.base.palette          = TEST_PALETTE;
        t.base.palette_entries  = PALETTE_ENTRIES;
        CaseResult tr = run_case(t, ram, REARM_LATENCY_CYCLES);
        write_artifacts(t, tr);
        print_report(t, tr);
        check_case(t, tr);

        const uint32_t m9_half_attr_words = pixel_words_for(t.base.mode, 0);
        const FrameImage &g1 = tr.rendered.frames[0];
        uint32_t attr_pairs_split = 0;
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
                    attr_pairs_split++;
                }
            }
        }
        printf("  half+attribute      %u words/line (half of %u), %u split groups\n",
               m9_half_attr_words, PIXELS_WORDS_ATTR, attr_pairs_split);
        CHECK(m9_half_attr_words == PIXELS_WORDS_ATTR_HALF,
              "m9-halfrate-attr: %u words/line, expected %u",
              m9_half_attr_words, PIXELS_WORDS_ATTR_HALF);
        CHECK(attr_pairs_split == 0,
              "m9-halfrate-attr: %u playfield groups do not span exactly 2 pixel clocks",
              attr_pairs_split);
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
        // The PLAYFIELD's two entries.  These cases paint their sprites out of
        // COMPOSITOR's held colours, but the 1bpp bitmap underneath still comes
        // out of the palette RAM — which nothing clears, so the list has to load
        // it or the picture is the RAM's random power-up contents.
        s.base.palette[0] = PLAYFIELD_BG;
        s.base.palette[1] = PLAYFIELD_FG;
        s.base.palette_entries = 2;
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
        s.base.palette[0] = PLAYFIELD_BG;
        s.base.palette[1] = PLAYFIELD_FG;
        s.base.palette_entries = 2;
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
        s.title = "large-text menu: one MASK record per big glyph, SET pairs, LITERAL+HOLD bar";
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
            const Rgb444   bg = plan.background_color;
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

        // The LITERAL + HOLD highlight bar, and the mask backgrounds that have
        // to match it: inside a mask, dibit 00 is PASSTHROUGH, not "the span
        // underneath", so the bar's colour has to be re-stated as cmp_color0.
        const Rgb444 bar = KIOSK_HIGHLIGHT_COLOR;
        uint32_t bar_px = 0;
        for (Rgb444 c : r.rendered.frames[0].pixels)
        {
            if (c == bar)
            {
                bar_px++;
            }
        }
        printf("  highlight bar       %u pixels at 0x%03X: LITERAL + HOLD spans across the line plus "
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
        s.title = "M2 plus 16 samples per channel (L on 0, R on 2) every 32 lines, a 23-sample preamble";
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
        s.title = "static wireframe in the bitmap, moving objects as LITERAL spans";
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

        const Rgb444 claw    = TEMPEST_CLAW_COLOR;
        const Rgb444 flipper = TEMPEST_FLIPPER_COLOR;
        const Rgb444 shot    = TEMPEST_SHOT_COLOR;

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

    // --- M11: a sprite line assembled from static pieces, delivered ahead -----
    {
        printf("\n=== m11-static-pieces — a sprite line as %u read-only fragments, "
               "delivered k lines ahead ===\n", 5u);
        printf("  program %u semantic words (monolithic pads to %u), sprite lines %u..%u, "
               "x %u..%u step %u\n",
               SP_SEMANTIC_WORDS, SP_PROGRAM_WORDS, SP_FIRST_LINE,
               SP_FIRST_LINE + SP_SPRITE_LINES - 1, sp_sprite_x(0),
               sp_sprite_x(SP_SPRITE_LINES - 1), SP_GAP_STEP);

        // D1, checked before any list is built or any pixel rendered: the
        // authored sum and the fetch simulation must agree at exactly H_ACTIVE
        // on every one of the 480 lines.
        {
            uint32_t bad_slots   = 0;
            uint32_t bad_stretch = 0;
            uint32_t pad_slots   = 0;
            for (uint32_t y = 0; y < V_ACTIVE; y++)
            {
                std::array<uint16_t, SP_SEMANTIC_WORDS> prog{};
                uint32_t n = 1;
                if (sp_is_sprite_line(y))
                {
                    n = sp_program_words(sp_sprite_index(y), prog);
                }
                else
                {
                    prog[0] = vidcmd_run(RUN_SRC_PASSTHROUGH, H_ACTIVE);
                }
                const VidcmdSlotPlan plan =
                    vidcmd_plan_line(std::span<const uint16_t>(prog.data(), n), H_BLANK);
                bad_slots += (plan.slots != H_ACTIVE) ? 1u : 0u;
                bad_stretch += (plan.stretch != 0) ? 1u : 0u;
            }
            // And the monolithic shape, planned as one list, deliberately reads
            // LONG: vidcmd_plan_line() has no H_ACTIVE fall, so it plays the
            // pads as active records.  On the chip the line ends at slot 640 and
            // a staged SET in blanking is eager and free — which is why the pad
            // question is answered by the pixel-exact comparison below and not
            // by the planner.
            {
                std::array<uint16_t, SP_PROGRAM_WORDS> padded{};
                const uint32_t n = sp_program_words(0, padded);
                for (uint32_t k = n; k < SP_PROGRAM_WORDS; k++)
                {
                    padded[k] = vidcmd_set(SET_FILLER, 0);
                }
                pad_slots = vidcmd_plan_line(padded, H_BLANK).slots;
            }
            printf("  D1 framing          %u of %u lines miss %u slots, %u stretch; the padded "
                   "%u-word shape plans %u slots as one list (%u of pads, all in the NEXT "
                   "blanking)\n",
                   bad_slots, V_ACTIVE, H_ACTIVE, bad_stretch, SP_PROGRAM_WORDS, pad_slots,
                   pad_slots - H_ACTIVE);
            CHECK(bad_slots == 0,
                  "m11-static-pieces: %u lines do not occupy exactly %u slots", bad_slots,
                  H_ACTIVE);
            CHECK(bad_stretch == 0,
                  "m11-static-pieces: %u lines stretch under the 2-slot fetch cadence — every "
                  "record is at least %u slots and none should", bad_stretch, SP_POSE_SPAN);
        }

        const SpVariant variants[] = {
            {"ref", "MONOLITHIC 21-word program in its own hblank — what apps/smurf does today",
             StaticPiecesForm::MONOLITHIC, 0, false, true},
            {"k0-vc", "PIECES, k=0, VIDCMD first — NEGATIVE CONTROL, must miss PIX_PRELOAD",
             StaticPiecesForm::PIECES, 0, false, false},
            {"k0-px", "PIECES, k=0, PIXELS first — NEGATIVE CONTROL, must starve COMPOSITOR",
             StaticPiecesForm::PIECES, 0, true, false},
            {"k1", "PIECES, k=1, PIXELS first", StaticPiecesForm::PIECES, 1, true, true},
            {"k2", "PIECES, k=2, PIXELS first", StaticPiecesForm::PIECES, 2, true, true},
            {"k4", "PIECES, k=4, PIXELS first", StaticPiecesForm::PIECES, 4, true, true},
            {"k8", "PIECES, k=8, PIXELS first", StaticPiecesForm::PIECES, 8, true, true},
            {"k4-pre-rs",
             "PIECES, k=4, bootstrap BEFORE /RS — NEGATIVE CONTROL, the pulse eats it",
             StaticPiecesForm::PIECES, 4, true, false, false},
        };

        FrameImage reference;
        uint32_t   clean_k_min = 0;

        for (const SpVariant &v : variants)
        {
            const SpResult r = run_static_pieces(v, ram);
            print_sp_report(v, r);
            const uint32_t bad = check_sp_geometry(v, r, 0);

            CHECK(!r.author[0].table_overflow && !r.author[1].table_overflow,
                  "m11-static-pieces/%s: the list overflowed its %u-byte table half",
                  v.name.c_str(), TABLE_HALF_BYTES);
            CHECK(r.interp.violations.empty(),
                  "m11-static-pieces/%s: interpreter reported %zu structural violations",
                  v.name.c_str(), r.interp.violations.size());
            CHECK(r.interp.irq_count == RENDER_FRAMES,
                  "m11-static-pieces/%s: expected %u IRQs, got %u", v.name.c_str(),
                  RENDER_FRAMES, r.interp.irq_count);
            CHECK(r.stream_mismatch == 0,
                  "m11-static-pieces/%s: %u frames deposited a different number of words than "
                  "the authored stream", v.name.c_str(), r.stream_mismatch);
            CHECK(r.budget.max_busy < LINE_SYSCLK,
                  "m11-static-pieces/%s: worst group costs %u SYSCLK of a %llu-SYSCLK line",
                  v.name.c_str(), r.budget.max_busy,
                  static_cast<unsigned long long>(LINE_SYSCLK));
            CHECK(r.budget.max_busy <= VIDCMD_LINE_SYSCLK_BUDGET,
                  "m11-static-pieces/%s: worst group costs %u SYSCLK, over the %u-SYSCLK "
                  "(%u-word) per-line delivery budget", v.name.c_str(), r.budget.max_busy,
                  VIDCMD_LINE_SYSCLK_BUDGET, VIDCMD_WORDS_PER_LINE_CAP);
            CHECK(r.rendered.stats.vidcmd_fifo_high <= VIDCMD_FIFO_WORDS,
                  "m11-static-pieces/%s: VIDCMD FIFO reached %u of %u words", v.name.c_str(),
                  r.rendered.stats.vidcmd_fifo_high, VIDCMD_FIFO_WORDS);
            CHECK(r.rendered.stats.vidcmd_overflows == 0,
                  "m11-static-pieces/%s: VIDCMD FIFO overflowed %u times", v.name.c_str(),
                  r.rendered.stats.vidcmd_overflows);

            if (v.expect_clean)
            {
                CHECK(r.vidcmd_late_lines == 0,
                      "m11-static-pieces/%s: %u lines had VIDCMD words arrive after their own "
                      "pixel 0 (worst margin %+lld SYSCLK)", v.name.c_str(),
                      r.vidcmd_late_lines, static_cast<long long>(r.vidcmd_min_margin));
                CHECK(r.pixels_late_lines == 0,
                      "m11-static-pieces/%s: %u lines had their first PIXELS word arrive after "
                      "PIX_PRELOAD (worst margin %+lld SYSCLK)", v.name.c_str(),
                      r.pixels_late_lines, static_cast<long long>(r.pixels_min_margin));
                CHECK(r.rendered.stats.vidcmd_hold_slots == 0,
                      "m11-static-pieces/%s: COMPOSITOR held on a dry FIFO for %u slots",
                      v.name.c_str(), r.rendered.stats.vidcmd_hold_slots);
                CHECK(r.rendered.stats.vidcmd_cadence_slots == 0,
                      "m11-static-pieces/%s: %u cadence hold slots — every record is at least "
                      "%u slots and the fetch should never bind", v.name.c_str(),
                      r.rendered.stats.vidcmd_cadence_slots, SP_POSE_SPAN);
                CHECK(r.rendered.stats.vidcmd_overruns == 0,
                      "m11-static-pieces/%s: %u records were still running at the h=%u fall",
                      v.name.c_str(), r.rendered.stats.vidcmd_overruns, H_ACTIVE);
                CHECK(r.rendered.stats.pixels_tiled_words == 0,
                      "m11-static-pieces/%s: PIXEL tiled its last word %u times",
                      v.name.c_str(), r.rendered.stats.pixels_tiled_words);
                CHECK(r.rendered.stats.pixels_overflows == 0,
                      "m11-static-pieces/%s: PIXELS FIFO overflowed %u times", v.name.c_str(),
                      r.rendered.stats.pixels_overflows);
                CHECK(r.rendered.stats.palette_unwritten_reads == 0,
                      "m11-static-pieces/%s: %u pixels came out of palette entries the list "
                      "never loaded", v.name.c_str(), r.rendered.stats.palette_unwritten_reads);
                CHECK(r.rendered.stats.palette_bad_filler == 0,
                      "m11-static-pieces/%s: %u palette writes stored the wrong colour",
                      v.name.c_str(), r.rendered.stats.palette_bad_filler);
                CHECK(r.rendered.stats.palette_disturbed_visible == 0,
                      "m11-static-pieces/%s: %u palette-write slots reached the DAC",
                      v.name.c_str(), r.rendered.stats.palette_disturbed_visible);

                const uint32_t f1f2 = sp_frame_diff(r.rendered.frames[0], r.rendered.frames[1]);
                CHECK(f1f2 == 0,
                      "m11-static-pieces/%s: frames 1 and 2 differ in %u pixels from an "
                      "unchanging list", v.name.c_str(), f1f2);

                if (v.form == StaticPiecesForm::MONOLITHIC)
                {
                    reference = r.rendered.frames[0];
                    const std::string path = "m11-static-pieces-ref.ppm";
                    write_ppm(path, reference);
                    printf("     artifact          %s\n", path.c_str());
                }
                else
                {
                    const uint32_t diff = sp_frame_diff(reference, r.rendered.frames[0]);
                    printf("     pixel-exact       %u pixels differ from the monolithic "
                           "reference\n", diff);
                    CHECK(diff == 0,
                          "m11-static-pieces/%s: %u pixels differ from the monolithic "
                          "reference — delivering a line's words %u line(s) early changed "
                          "the picture", v.name.c_str(), diff, v.lookahead);
                    if (diff == 0 && (clean_k_min == 0 || v.lookahead < clean_k_min))
                    {
                        clean_k_min = v.lookahead;
                    }
                    if (v.lookahead == 4)
                    {
                        const std::string path = "m11-static-pieces-k4.ppm";
                        write_ppm(path, r.rendered.frames[0]);
                        printf("     artifact          %s\n", path.c_str());
                    }
                }
            }
            else
            {
                // D5: each control must fail, and fail in its own way.  A
                // control that came out clean would mean the deadline this case
                // exists to measure is not being measured.
                const bool missed_pixels = r.pixels_late_lines > 0 ||
                                           r.rendered.stats.pixels_tiled_words > 0;
                const bool missed_vidcmd = r.vidcmd_late_lines > 0 ||
                                           r.rendered.stats.vidcmd_hold_slots > 0;
                const uint32_t diff = sp_frame_diff(reference, r.rendered.frames[0]);
                printf("     NEGATIVE CONTROL  PIXELS deadline missed: %s   VIDCMD deadline "
                       "missed: %s   geometry violations %u   pixels differing from the "
                       "reference %u\n",
                       missed_pixels ? "yes" : "NO", missed_vidcmd ? "yes" : "NO", bad, diff);
                if (v.pixels_first)
                {
                    CHECK(missed_vidcmd,
                          "m11-static-pieces/%s: five piece descriptors behind the pixel "
                          "stream were expected to deliver a line's VIDCMD words late, and "
                          "did not — the control cannot fail, so it proves nothing",
                          v.name.c_str());
                }
                else
                {
                    CHECK(missed_pixels,
                          "m11-static-pieces/%s: five piece descriptors ahead of the pixel "
                          "stream were expected to push the first PIXELS word past "
                          "PIX_PRELOAD, and did not", v.name.c_str());
                }
                // The control has to show on screen, or the deadline it misses
                // is not one the picture depends on.  The geometry check alone
                // cannot see PIXELS corruption — a passthrough pixel is tested
                // against a PAIR of palette entries, and either member of the
                // pair passes whatever the bitmap says — so the reference diff
                // is what catches a starved pixel stream.
                CHECK(diff > 0,
                      "m11-static-pieces/%s: the control missed a delivery deadline but the "
                      "frame is pixel-identical to the reference, so the deadline is not one "
                      "this case can measure", v.name.c_str());
            }
        }

        // What bounds the descriptor count per line once the hblank no longer
        // does (question 2).  A 40-word 1bpp PIXELS run costs
        // engine_wait_descriptor_cycles(4,20) + engine_descriptor_cycles(4,20);
        // whatever is left of the per-line delivery budget pays for
        // 14 SYSCLK + 2 per word of piece descriptors.
        const uint32_t pixels_cost =
            engine_wait_descriptor_cycles(ENGINE_ARBITRATION_CYCLES, SP_PIXELS_CHUNK) +
            engine_descriptor_cycles(ENGINE_ARBITRATION_CYCLES, SP_PIXELS_CHUNK);
        const uint32_t piece_overhead = ENGINE_ARBITRATION_CYCLES + ENGINE_ASSERT_CYCLES +
                                        ENGINE_FETCH_CYCLES + ENGINE_RELEASE_CYCLES;   // 14
        const uint32_t budget_left = VIDCMD_LINE_SYSCLK_BUDGET - pixels_cost -
                                     ENGINE_CYCLES_PER_WORD * SP_SEMANTIC_WORDS;
        const uint32_t line_left = static_cast<uint32_t>(LINE_SYSCLK) - pixels_cost -
                                   ENGINE_CYCLES_PER_WORD * SP_SEMANTIC_WORDS;
        printf("\n  minimum clean lookahead: k = %u.  Per sprite line the pieces form spends\n"
               "  5 descriptors and %u payload words where the monolithic form spends 1 and\n"
               "  %u; the CPU writes no VIDCMD words at all, only the descriptors' source\n"
               "  pointers.\n", clean_k_min, SP_SEMANTIC_WORDS, SP_PROGRAM_WORDS);
        printf("  what bounds the descriptor count per line, with the hblank out of the way:\n"
               "    per-line delivery budget  %u SYSCLK - %u (PIXELS) - %u (13 words) = %u,\n"
               "                              at %u SYSCLK of turn-around each: %u descriptors\n"
               "    the whole line            %llu SYSCLK the same way: %u descriptors\n"
               "    the %u-word VIDCMD FIFO    %u lines of %u-word programs in flight\n",
               VIDCMD_LINE_SYSCLK_BUDGET, pixels_cost,
               ENGINE_CYCLES_PER_WORD * SP_SEMANTIC_WORDS, budget_left, piece_overhead,
               budget_left / piece_overhead,
               static_cast<unsigned long long>(LINE_SYSCLK), line_left / piece_overhead,
               VIDCMD_FIFO_WORDS, VIDCMD_FIFO_WORDS / SP_SEMANTIC_WORDS, SP_SEMANTIC_WORDS);
        CHECK(clean_k_min == 1,
              "m11-static-pieces: the minimum clean lookahead came out as %u, not 1",
              clean_k_min);
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
    printf("  3. STILL HOLDS. AUDIO pops through vertical blanking too, where no per-line\n"
           "     group exists to feed it, so a frame's list must carry vblank's 45/2 = 23\n"
           "     samples per channel in its preamble on top of the 15x16 the visible lines give.\n");
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
    printf(" 13. REVISED 2026-10-03.  RUN_COLOR was the atomic answer to finding 9 (two\n"
           "     held-colour SETs land one pixel apart) and it is GONE: its prefix is now\n"
           "     LITERAL, one pixel of ANY colour that never touches the held colours, so\n"
           "     independent objects on a line still cannot clobber each other.  The price\n"
           "     is law LL: a literal holds the on-chip buffer for its slot, so it always\n"
           "     paints two slots and the record after it lands two later — a literal is a\n"
           "     320-density pixel, and a flat span is LITERAL + HOLD(n), two words,\n"
           "     max(n + 2, %u) slots.  One-pixel 640-density marks stay a held-colour\n"
           "     RUN(c,1) (or MASK).  The run counter is ten bits (RUN up to %u slots).\n",
           VIDCMD_LIT_HOLD_MIN_SLOTS, VIDCMD_RUN_COUNT_MAX);
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

    printf(" 23. REPLACED 2026-09-20, AND THE MODE REGISTER IS AN ENUMERATION AGAIN. pixel.v is\n"
           "     INDEX-ONLY: mode[1:0] is a depth enumeration (1bpp / 2bpp / 4bpp /\n"
           "     attribute), mode[2] is half rate and mode[3] is reserved.  Every playfield\n"
           "     pixel in every mode is a four-bit index into an EXTERNAL 16 x 12 palette RAM;\n"
           "     micro-HAM, pal_fg/pal_bg/ham_held and the 2bpp fixed four colours are gone.\n"
           "     pix_base selects the PAIR a 1bpp line uses and the QUAD a 2bpp line uses, so\n"
           "     one word recolours a whole line or a whole half-line — m8-idx2bpp switches\n"
           "     quads mid-line with two banked SETs and renders pixel-exact at %u PIXELS +\n"
           "     %u VIDCMD words a line, against the %u-word cap.\n",
           PIXELS_WORDS_2BPP, m8_vidcmd_words, VIDCMD_WORDS_PER_LINE_CAP);
    printf(" 24. NEW 2026-09-20, AND IT IS THE RULE A LIST AUTHOR MUST NOT GET WRONG. A palette\n"
           "     entry is THREE VIDCMD words — SET clut_index, SET clut_data, and a SAME-VALUE\n"
           "     SET to target 7 — because COMPOSITOR re-registers set_pix_value from every\n"
           "     staged word and the RAM's data inputs sit on that bus: without the filler the\n"
           "     RAM stores the NEXT word's payload, which the model reproduces and counts.\n"
           "     And the write steals four pixel slots (the entry being written, two clocks of\n"
           "     high-Z and write recovery, plus a guard), so the four slots after the data\n"
           "     SET must be in blanking or covered by a non-passthrough source.  The\n"
           "     palette-rule traces assert both directions: a covered write is invisible, an\n"
           "     uncovered one puts undefined pixels on the DAC, and a missing filler stores\n"
           "     the wrong colour.  THE RAM IS NEVER RESET: it powers up undefined (random in\n"
           "     the model), so a list that forgets an entry paints noise.\n");
    printf(" 25. UNCHANGED, AND HALF RATE STILL COSTS NOTHING OUTSIDE PIXEL. mode[2] holds every\n"
           "     consumed group for two pixel clocks: %u groups across the same 640-clock\n"
           "     window, so a line costs half the stream — %u words 1bpp, %u 2bpp, %u attribute\n"
           "     — at half the horizontal resolution, and it is the only way to run 4bpp at all\n"
           "     (%u words; full rate is UNDEFINED).  It is a STREAM-SIDE GATE and nothing else\n"
           "     in the pipeline learns about it: RGB_OUT's window, PIXEL_OUT_LEAD, the SET\n"
           "     path and COMPOSITOR are untouched.  The interop that matters for a game mode\n"
           "     is that MASK DIBITS STEP ONCE PER PIXEL-CLOCK SLOT REGARDLESS, so sprites keep\n"
           "     full %u-pixel horizontal resolution over a %u-wide playfield — m9-halfrate\n"
           "     measures both halves at once: zero playfield groups spanning other than 2\n"
           "     slots, and adjacent pixels inside the sprite cells that DO differ.\n",
           H_ACTIVE / 2, PIXELS_WORDS_1BPP_HALF, PIXELS_WORDS_2BPP_HALF,
           PIXELS_WORDS_ATTR_HALF, PIXELS_WORDS_4BPP_HALF, H_ACTIVE, H_ACTIVE / 2);

    // --- M12: the whole-frame cursor chain (apps/lib/griffin_cursor.h) --------
    run_cursor_chain_case(ram);

    // --- M13: literal pixels out of SET(cmp_color1) streams (SET-STREAM.md) --
    check_set_stream_traces();
    run_set_stream_case(ram);
    check_literal_sprite_traces();
    run_literal_sprite_case(ram);

    printf("\n=== summary ===\n");
    if (g_failures == 0)
    {
        printf("  all cases PASS\n");
        return 0;
    }
    printf("  %d check(s) FAILED\n", g_failures);
    return 1;
}
