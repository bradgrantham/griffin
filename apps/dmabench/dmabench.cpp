// dmabench -- how much CPU throughput a Griffin screen type costs in ENGINE DMA.
//
// The question: a 68000 program sharing the bus with the display-list DMA engine
// loses a fraction of its cycles to it.  How big is that fraction, and in
// particular what does an 80-column text console cost when it is built purely
// from COMPOSITOR VIDCMD MASK records (no framebuffer at all) instead of from a
// 640x480 1bpp PIXEL bitmap?
//
// METHOD.  For each screen configuration in turn:
//
//   1. build the whole display list once (one static descriptor table, no
//      per-frame authoring -- the authoring cost is a separate question and is
//      deliberately kept out of the timed region);
//   2. arm it, let two fields go by so the pipeline is in steady state;
//   3. run a fixed CPU-bound kernel over and over, counting completed kernel units,
//      re-arming ENGINE once per observed vblank, until FIELDS vblanks have been
//      counted.
//
// The kernel is pure RAM traffic plus integer arithmetic over 1 KB of .bss: no
// syscalls, no ROM execution, no MMIO except the one-byte vsync-latch poll and
// the one-word ENGINE_DESC re-arm.  Its pass count is therefore a direct
// measure of CPU throughput over a fixed span of wall time (FIELDS / 59.94 s),
// and the ratio between two configurations' pass counts is the ratio of the CPU
// time the two screens leave behind.
//
// The vblank latch is the clock because it is the one time reference that does
// not itself depend on the CPU: it comes out of TIMING at 59.94 Hz whatever the
// bus is doing.  griffin_getticks() (the firmware's 100 Hz systick) is read
// either side of the timed region as a cross-check that the field count really
// did span FIELDS/59.94 seconds.
//
// WHAT IS MEASURED IS A COST MODEL, NOT SILICON.  The emulator charges the CPU
// clock the SYSCLK that super-engine/descriptor.h says each descriptor spends on
// the bus (arbitration + assert + 4 descriptor words + 2 per payload word +
// release, doubled arbitration for a wait_hblank descriptor), lumping a whole
// scanline's worth at that line's HBLANK edge.  The app prints that same model's
// prediction next to the measurement so the two can be compared; see RESULTS.md.
//
// USAGE
//
//   dmabench [FIELDS]        run every configuration, print the table
//   dmabench shot N          arm configuration N and hold it forever, so an
//                            emulator --screenshot captures that screen
//
// Configuration N is the index in CONFIGS below (0 = C0 ... 5 = C4).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../../griffin.generated.h"
#include "../../griffin.generated.refs.h"
#include "../../super-engine/descriptor.h"
#include "../lib/griffin_app.h"
#include "../lib/griffin_video.h"

namespace se = SuperEngine;
using namespace Griffin::reg;

namespace
{

// ---------------------------------------------------------------------------
// Geometry and budgets
// ---------------------------------------------------------------------------

constexpr unsigned LINES = se::V_ACTIVE;            // 480
constexpr unsigned VBLANK_WALK_LINES = 34;          // lines 491..524, as rom.cpp

// A pure-VIDCMD text line: forty 16-pixel MASK records (two words each) plus the
// two per-line colour SETs.  vidcmd-spec.md section 4.5's "82 words/line".
constexpr unsigned GROUPS_PER_LINE = se::H_ACTIVE / se::MASK_SLOTS;          // 40
constexpr unsigned TEXT_WORDS      = 2 + GROUPS_PER_LINE * se::MASK_RECORD_WORDS;  // 82
constexpr unsigned TEXT_WORDS_LINE0 = TEXT_WORDS + 2;   // + pix_base and pix_mode

// The widest VIDCMD line any configuration authors.
constexpr unsigned VIDCMD_MAX_WORDS = TEXT_WORDS_LINE0;   // 84

// The widest PIXELS line any configuration authors (2bpp, full rate).
constexpr unsigned PIXELS_MAX_WORDS = se::PIXELS_WORDS_2BPP;   // 80

// Every configuration that feeds PIXEL loads its palette entries ONCE PER
// FRAME, in a VIDCMD descriptor with no wait_hblank -- i.e. in vertical
// blanking, the only place a palette write is unconditionally legal, and the
// only place a sixteen-entry load would fit.  /RS clears pix_mode and pix_base
// at every vsync but never touches the palette RAM, so mode and base are
// re-sent from the same preamble while the entries only have to be written
// because nothing initialises the RAM at all.  A preamble costs one descriptor
// per FRAME and leaves every configuration's WORDS PER LINE alone, which is
// what keeps RESULTS.md's analytic column comparable across this change.
constexpr unsigned PREAMBLE_MAX_WORDS =
    se::PALETTE_ENTRIES * se::PALETTE_WORDS_PER_ENTRY + 3;

// ---------------------------------------------------------------------------
// Payload storage.  Only DESCRIPTOR TABLES have to live in the firmware's carve
// (griffin_abi.h); VIDCMD and PIXELS words are ordinary DMA sources, so they
// stay in the app's own .bss.
// ---------------------------------------------------------------------------

uint16_t g_vidcmd[LINES][VIDCMD_MAX_WORDS];
uint16_t g_fb[LINES][PIXELS_MAX_WORDS];
uint16_t g_preamble[PREAMBLE_MAX_WORDS];

// ---------------------------------------------------------------------------
// The timed kernel: a read-modify-write checksum sweep over 1 KB of RAM with
// shift/xor/add arithmetic.  No multiply (that would be a libgcc call), no
// branch on data, so every unit of work is exactly the same number of bus
// cycles and instructions.
//
// The UNIT is a 32-longword CHUNK, not the whole buffer, and the chunk cursor
// wraps around the 1 KB so the sweep is continuous.  The reason is granularity:
// the field loop can only notice a vblank and re-arm ENGINE between two units,
// so a unit has to be short against both a field (233,625 SYSCLK) and the 34
// scanlines of pacer slack at the head of every list.  A chunk measures about
// 4,000 SYSCLK, i.e. nine scanlines; a whole-buffer pass measured 34,000, which
// is 76 scanlines and would have armed the list well past the walk.
// ---------------------------------------------------------------------------

constexpr unsigned KERNEL_WORDS  = 256;     // 1 KB of state
constexpr unsigned KERNEL_CHUNK  = 32;      // longwords per timed unit
constexpr unsigned KERNEL_CHUNKS = KERNEL_WORDS / KERNEL_CHUNK;

uint32_t g_kernel[KERNEL_WORDS];
volatile uint32_t g_kernel_sink;

uint32_t kernel_chunk(uint32_t seed, unsigned chunk)
{
    uint32_t       sum = seed;
    uint32_t      *p   = &g_kernel[chunk * KERNEL_CHUNK];
    for (unsigned i = 0; i < KERNEL_CHUNK; i++)
    {
        uint32_t v = p[i] + sum;
        v ^= v >> 1;
        p[i] = v;
        sum ^= v;
        sum += i;
    }
    return sum;
}

// ---------------------------------------------------------------------------
// The 5x7 font, copied from super-engine/author.h so this app does not drag the
// host-side authoring library (std::span, std::vector) onto the target.
// ---------------------------------------------------------------------------

constexpr unsigned FONT_COLS = 5;
constexpr unsigned FONT_ROWS = 7;
constexpr char FONT_CHARSET[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,:-/>()!*+=";
constexpr unsigned FONT_GLYPHS = sizeof(FONT_CHARSET) - 1;

constexpr uint8_t FONT5X7[FONT_GLYPHS * FONT_ROWS] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   // ' '
    0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11,   // A
    0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E,   // B
    0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E,   // C
    0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E,   // D
    0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F,   // E
    0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10,   // F
    0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F,   // G
    0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11,   // H
    0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E,   // I
    0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C,   // J
    0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11,   // K
    0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F,   // L
    0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11,   // M
    0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11,   // N
    0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E,   // O
    0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10,   // P
    0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D,   // Q
    0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11,   // R
    0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E,   // S
    0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,   // T
    0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E,   // U
    0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04,   // V
    0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11,   // W
    0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11,   // X
    0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04,   // Y
    0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F,   // Z
    0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E,   // 0
    0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E,   // 1
    0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F,   // 2
    0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E,   // 3
    0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02,   // 4
    0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E,   // 5
    0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E,   // 6
    0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08,   // 7
    0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E,   // 8
    0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C,   // 9
    0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C,   // .
    0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08,   // ,
    0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00,   // :
    0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00,   // -
    0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10,   // /
    0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08,   // >
    0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02,   // (
    0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08,   // )
    0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04,   // !
    0x00, 0x0A, 0x04, 0x1F, 0x04, 0x0A, 0x00,   // *
    0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00,   // +
    0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00,   // =
};

unsigned font_glyph_index(char c)
{
    for (unsigned i = 0; i < FONT_GLYPHS; i++)
    {
        if (FONT_CHARSET[i] == c)
        {
            return i;
        }
    }
    return 0;
}

bool font_pixel(char c, unsigned col, unsigned row)
{
    if (col >= FONT_COLS || row >= FONT_ROWS)
    {
        return false;
    }
    const uint8_t bits = FONT5X7[font_glyph_index(c) * FONT_ROWS + row];
    return ((bits >> (FONT_COLS - 1 - col)) & 1u) != 0;
}

// ---------------------------------------------------------------------------
// The text screen's content: 80 columns by 60 rows of 8x8 cells, glyph in cell
// columns 1..5 so that every record's implicit pixel 0 (which the hardware
// forces to cmp_color0) lands on an inter-character gap.
// ---------------------------------------------------------------------------

constexpr unsigned CELL_W = 8;
constexpr unsigned CELL_H = 8;
constexpr unsigned COLS   = se::H_ACTIVE / CELL_W;   // 80
constexpr unsigned ROWS   = se::V_ACTIVE / CELL_H;   // 60

constexpr const char *TITLE =
    "GRIFFIN DMABENCH - AN 80 COLUMN CONSOLE MADE ONLY OF VIDCMD MASK RECORDS";
constexpr const char *STATUS =
    "STATUS: 40 MASK RECORDS PER LINE = 80 WORDS + 2 SETS - PIXELS WORDS: 0";

constexpr const char *PHRASES[12] = {
    "NO PIXELS DESCRIPTORS ARE AUTHORED AT ALL ON THIS SCREEN",
    "EVERY GLYPH PAIR IS ONE 16 PIXEL TWO WORD MASK RECORD",
    "MASK TO MASK CHAINING COSTS ZERO SLOTS SO 40 X 16 = 640",
    "PIXEL 0 OF A RECORD IS THE IMPLICIT OPAQUE COLOR0 GAP",
    "82 WORDS PER LINE AT 2 SYSCLK EACH PLUS 3 DESCRIPTORS",
    "THE DISPLAY LIST IS THE FRAMEBUFFER AND NOTHING ELSE IS",
    "A 1BPP BITMAP CONSOLE MOVES 40 WORDS PER LINE INSTEAD",
    "SO THIS SCREEN COSTS THE CPU ROUGHLY TWICE AS MUCH BUS",
    "MEASURED BY COUNTING KERNEL PASSES PER 300 VIDEO FIELDS",
    "RECOLOURING IS WHAT RUNS OUT NOT RECORDS (SECTION 4.5)",
    "0123456789 .,:-/>()!*+= ABCDEFGHIJKLMNOPQRSTUVWXYZ",
    "GRIFFIN 68000 - 14 MHZ - 445 SYSCLK PER SCANLINE",
};

char text_char_at(const char *s, unsigned index)
{
    for (unsigned k = 0; k < index; k++)
    {
        if (s[k] == '\0')
        {
            return ' ';
        }
    }
    const char c = s[index];
    return (c == '\0') ? ' ' : c;
}

char screen_char(unsigned row, unsigned col)
{
    if (col >= COLS || row >= ROWS)
    {
        return ' ';
    }
    if (row == 0)
    {
        return text_char_at(TITLE, col);
    }
    if (row == ROWS - 1)
    {
        return text_char_at(STATUS, col);
    }
    if (col == 0)
    {
        return static_cast<char>('0' + (row / 10u) % 10u);
    }
    if (col == 1)
    {
        return static_cast<char>('0' + row % 10u);
    }
    if (col == 2)
    {
        return '>';
    }
    if (col == 3)
    {
        return ' ';
    }
    return text_char_at(PHRASES[(row - 1u) % 12u], col - 4u);
}

// The ink of one scanline as forty 16-bit words, bit 15 of word g being pixel
// g*16 of the line.  A set bit is cmp_color1; a clear bit is cmp_color0.  Cell
// column 0 is never set, which is what keeps every record's pixel 0 on
// background.
void text_line_ink(unsigned line, uint16_t *ink)
{
    for (unsigned g = 0; g < GROUPS_PER_LINE; g++)
    {
        ink[g] = 0;
    }

    const unsigned row       = line / CELL_H;
    const unsigned glyph_row = line % CELL_H;

    for (unsigned col = 0; col < COLS; col++)
    {
        const char c = screen_char(row, col);
        if (c == ' ')
        {
            continue;
        }
        for (unsigned fc = 0; fc < FONT_COLS; fc++)
        {
            if (!font_pixel(c, fc, glyph_row))
            {
                continue;
            }
            const unsigned x = col * CELL_W + 1u + fc;
            ink[x / 16u] |= static_cast<uint16_t>(0x8000u >> (x % 16u));
        }
    }
}

constexpr se::Rgb444 TEXT_BG = se::rgb444(0x0, 0x1, 0x3);

se::Rgb444 text_row_color1(unsigned row)
{
    // A slow green ramp down the screen, so the screenshot shows that the
    // per-line SETs really are taking effect line by line.
    const unsigned g = 8u + (row * 7u) / (ROWS - 1u);      // 8..15
    return se::rgb444(g, 0xF, g);
}

// ---------------------------------------------------------------------------
// Display-list construction
// ---------------------------------------------------------------------------

// Accumulates descriptors into the app's carve and, as it goes, the SYSCLK the
// list costs the bus per frame under super-engine/descriptor.h's model -- the
// same model the emulator charges the CPU clock with.
struct Builder
{
    uint16_t *table      = nullptr;
    unsigned  count      = 0;
    unsigned  limit      = 0;
    unsigned  words      = 0;    // payload words per frame
    uint32_t  busy       = 0;    // SYSCLK per frame
    bool      overflow   = false;

    void emit(const se::Descriptor &d)
    {
        if (count >= limit)
        {
            overflow = true;
            return;
        }
        const se::DescriptorWords w = se::encode_descriptor(d);
        uint16_t *p = table + count * se::DESC_WORDS;
        for (unsigned i = 0; i < se::DESC_WORDS; i++)
        {
            p[i] = w.w[i];
        }
        count++;
        words += d.count;
        busy += d.wait_hblank
                    ? se::engine_wait_descriptor_cycles(se::ENGINE_ARBITRATION_CYCLES, d.count)
                    : se::engine_descriptor_cycles(se::ENGINE_ARBITRATION_CYCLES, d.count);
    }

    // The cheapest possible way to spend one scanline: wait for the HBLANK
    // edge, deposit one word into no FIFO at all.
    void pacer(bool stop_after)
    {
        se::Descriptor d;
        d.src         = reinterpret_cast<uint32_t>(&g_fb[0][0]);
        d.count       = 1;
        d.destination = se::DEST_NONE;
        d.wait_hblank = true;
        d.stop_after  = stop_after;
        emit(d);
    }

    // `n` words from `src` into one FIFO, split into descriptors of at most
    // `chunk` words.  Only the first may wait for the HBLANK edge.
    void stream(uint32_t src, unsigned n, uint8_t destination, bool wait_first, unsigned chunk)
    {
        if (chunk == 0 || chunk > se::DESC_MAX_COUNT)
        {
            chunk = se::DESC_MAX_COUNT;
        }
        unsigned done = 0;
        while (done < n)
        {
            const unsigned take = (n - done > chunk) ? chunk : (n - done);
            se::Descriptor d;
            d.src         = src + done * 2u;
            d.count       = static_cast<uint16_t>(take);
            d.destination = destination;
            d.wait_hblank = wait_first && (done == 0);
            emit(d);
            done += take;
        }
    }
};

uint32_t vidcmd_addr(unsigned line)
{
    return reinterpret_cast<uint32_t>(&g_vidcmd[line][0]);
}

// Write `count` palette entries, then pix_base and pix_mode, into g_preamble and
// emit the one VIDCMD descriptor that deposits them.  No wait_hblank: the list
// is armed by the vsync ISR, so this runs in vertical blanking, where a palette
// write's four disturbed pixel slots do not exist.
unsigned put_preamble(Builder &b, const se::Rgb444 *entries, unsigned count,
                      unsigned pix_base, uint32_t pix_mode, bool with_run)
{
    unsigned n = 0;
    for (unsigned i = 0; i < count; i++)
    {
        const se::PaletteEntryWords e = se::vidcmd_palette_entry(i, entries[i]);
        for (unsigned w = 0; w < se::PALETTE_WORDS_PER_ENTRY; w++)
        {
            g_preamble[n++] = e.w[w];
        }
    }
    g_preamble[n++] = se::vidcmd_set(se::SET_PIX_BASE, pix_base);
    g_preamble[n++] = se::vidcmd_set(se::SET_PIX_MODE, pix_mode);
    if (with_run)
    {
        g_preamble[n++] = se::vidcmd_run(se::RUN_SRC_PASSTHROUGH, 1);
    }

    se::Descriptor d;
    d.src         = reinterpret_cast<uint32_t>(&g_preamble[0]);
    d.count       = static_cast<uint16_t>(n);
    d.destination = se::DEST_VIDCMD_FIFO_W;
    b.emit(d);
    return n;
}

uint32_t fb_addr(unsigned line)
{
    return reinterpret_cast<uint32_t>(&g_fb[line][0]);
}

// --- C0: nothing at all -----------------------------------------------------

void build_none(Builder &)
{
}

// --- C0B: a blank screen from the smallest legal list ------------------------
//
// One VIDCMD word painting the whole frame black (COMPOSITOR holds the last
// RUN's source to the end of a line, and a line that receives no records keeps
// holding, so a single one-slot HOLD covers all 480 lines: KEEP is RGB_OUT fed
// back, which is black at every line's start and so stays black; it reads no
// register and no palette entry), and one wait_hblank/stop_after pacer to
// disarm the engine and raise its IRQ.  The HOLD is a one-slot run-class
// record exactly as the RUN_COLOR it replaces was, so the timing is identical.
void build_blank(Builder &b)
{
    g_vidcmd[0][0] = se::vidcmd_hold(1);

    se::Descriptor d;
    d.src         = vidcmd_addr(0);
    d.count       = 1;
    d.destination = se::DEST_VIDCMD_FIFO_W;
    b.emit(d);
    b.pacer(true);
}

// --- C1: the firmware console, 640x480 1bpp PIXEL ---------------------------
//
// Byte-for-byte the shape firmware/rom.cpp's dl_build_console_list() emits: a
// nine-word VIDCMD preamble in vblank (palette entry 0, palette entry 1,
// SET pix_base, SET pix_mode, one-slot passthrough RUN), the 34-line walk to the
// top of frame, then 40 PIXELS words per line as two 20-word descriptors, the
// first waiting on the line's HBLANK edge.  The 1bpp plane at pix_base 0 selects
// palette ENTRY 1 for a set bit and ENTRY 0 for a clear one.
constexpr unsigned CONSOLE_1BPP_WORDS = se::PIXELS_WORDS_1BPP;   // 40
constexpr unsigned CONSOLE_1BPP_CHUNK = CONSOLE_1BPP_WORDS / 2;  // 20

void build_1bpp(Builder &b)
{
    const se::Rgb444 palette[2] = { se::rgb444(0x0, 0x0, 0x0), se::rgb444(0xF, 0xF, 0xF) };
    put_preamble(b, palette, 2, 0, se::pixel_mode(se::PIXEL_DEPTH_1BPP, false), true);

    for (unsigned i = 0; i < VBLANK_WALK_LINES; i++)
    {
        b.pacer(false);
    }
    for (unsigned line = 0; line < LINES; line++)
    {
        b.stream(fb_addr(line), CONSOLE_1BPP_WORDS, se::DEST_PIXELS_FIFO_W, true,
                 CONSOLE_1BPP_CHUNK);
    }
    b.pacer(true);
}

// The 1bpp framebuffer content: the same text, rendered as a bitmap, so C1 and
// C2 show the same screen by two different routes.
void fill_1bpp_framebuffer()
{
    for (unsigned line = 0; line < LINES; line++)
    {
        uint16_t ink[GROUPS_PER_LINE];
        text_line_ink(line, ink);
        for (unsigned g = 0; g < GROUPS_PER_LINE; g++)
        {
            g_fb[line][g] = ink[g];
        }
        for (unsigned w = GROUPS_PER_LINE; w < PIXELS_MAX_WORDS; w++)
        {
            g_fb[line][w] = 0;
        }
    }
}

// --- C2: the pure-VIDCMD MASK text console ----------------------------------
//
// Every one of the forty 16-pixel groups on every line is a MASK record, even a
// group that is all background: that is exactly the 82 words/line shape
// vidcmd-spec.md section 4.5 prices, and it makes the authored slot sum exactly
// 640 by construction (40 records x 16 slots, chained gaplessly) with the two
// colour SETs consumed free in the preceding HBLANK.
//
// Ink is cmp_color1 and background is cmp_color0, both written per line.  The
// third colour (dibit 00, passthrough, resolving to a palette entry with the
// PIXELS FIFO never written) is deliberately NOT used for the picture: section
// 4.5 flags it as model behaviour not yet verified against hardware, this
// benchmark should not depend on it, and since 2026-09-20 it would be an
// UNWRITTEN palette entry -- the RAM powers up undefined and nothing here loads
// it.  pix_base and pix_mode are still SET once per frame on line 0, which is
// where the line-0 word count of 84 comes from; they cost the same two words the
// retired pal_bg/mode pair did, so C2's line is unchanged at 82/84 words.
void build_text(Builder &b)
{
    for (unsigned line = 0; line < LINES; line++)
    {
        uint16_t *w = &g_vidcmd[line][0];
        unsigned  n = 0;

        if (line == 0)
        {
            w[n++] = se::vidcmd_set(se::SET_PIX_BASE, 0);
            w[n++] = se::vidcmd_set(se::SET_PIX_MODE,
                                    se::pixel_mode(se::PIXEL_DEPTH_1BPP, false));
        }
        w[n++] = se::vidcmd_set(se::SET_CMP_COLOR1, text_row_color1(line / CELL_H));
        w[n++] = se::vidcmd_set(se::SET_CMP_COLOR0, TEXT_BG);

        uint16_t ink[GROUPS_PER_LINE];
        text_line_ink(line, ink);
        for (unsigned g = 0; g < GROUPS_PER_LINE; g++)
        {
            // Opacity all ones, select = the ink: dibit 11 (cmp_color1) where
            // there is ink, dibit 10 (cmp_color0) where there is not.  Bit 15
            // of the select plane is always clear because cell column 0 is
            // always blank, so the record's implicit pixel 0 agrees with the
            // picture it is part of.
            const se::VidcmdMask m = se::vidcmd_mask_from_planes(0xFFFF, ink[g]);
            w[n++] = m.header;
            w[n++] = m.data;
        }
    }

    for (unsigned i = 0; i < VBLANK_WALK_LINES; i++)
    {
        b.pacer(false);
    }
    for (unsigned line = 0; line < LINES; line++)
    {
        const unsigned n = (line == 0) ? TEXT_WORDS_LINE0 : TEXT_WORDS;
        b.stream(vidcmd_addr(line), n, se::DEST_VIDCMD_FIFO_W, true, se::DESC_MAX_COUNT);
    }
    b.pacer(true);
}

// --- C3: 2bpp, full rate, with a per-line VIDCMD packet ----------------------
//
// 80 PIXELS words plus SIX VIDCMD words per line: the stand-in for a 320x480
// 4bpp or a 640-wide 2bpp screen with a per-line palette.  The word count is
// what RESULTS.md's analytic column is computed from and is deliberately
// UNCHANGED by the move to an indexed PIXEL -- what changed is what the six
// words are.  Before: five SETs (two COMPOSITOR colours, pal_fg, pal_bg,
// ham_held) and one 640-slot passthrough RUN.  Now: a THREE-WORD PALETTE ENTRY
// (SET clut_index, SET clut_data, the same-value filler -- vidcmd-spec.md
// §3.2), the two COMPOSITOR colour SETs and the same RUN.  So the line still
// occupies exactly H_ACTIVE slots and still reloads one colour of its palette
// every scanline, which is the thing C3 exists to price.
//
// The other three of the four entries a 2bpp line reaches are loaded once per
// frame by the vblank preamble; entry 3 is the one the per-line packet rewrites.
// The write is consumed in the line's HBLANK, so its four disturbed pixel slots
// do not exist, and the word behind the filler is a COMPOSITOR colour SET rather
// than the RUN that resumes passthrough -- both of §3.2's author rules.
//
// VIDCMD goes ahead of PIXELS in the group, as super-engine/author.cpp orders it.
constexpr unsigned IDX2_VIDCMD_WORDS = se::PALETTE_WORDS_PER_ENTRY + 3;   // 6
static_assert(IDX2_VIDCMD_WORDS == 6, "C3 is priced at six VIDCMD words per line");

// The four entries a 2bpp line at pix_base 0 reaches, dibit 0..3.  Entry 3 is
// rewritten per line with a green ramp, so the screenshot shows the ramp running
// down the frame as well as all four codes across it.
constexpr se::Rgb444 idx2_entry3(unsigned line)
{
    const unsigned g = 4u + (line * 11u) / (LINES - 1u);
    return se::rgb444(g, 0x2, 0xF);
}

void build_2bpp(Builder &b)
{
    const se::Rgb444 palette[4] = {
        se::rgb444(0x0, 0x0, 0x0),
        se::rgb444(0xF, 0x8, 0x2),
        se::rgb444(0x1, 0x1, 0x8),
        idx2_entry3(0),
    };
    put_preamble(b, palette, 4, 0, se::pixel_mode(se::PIXEL_DEPTH_2BPP, false), false);

    for (unsigned line = 0; line < LINES; line++)
    {
        uint16_t *w = &g_vidcmd[line][0];
        unsigned  n = 0;

        const se::PaletteEntryWords e = se::vidcmd_palette_entry(3, idx2_entry3(line));
        w[n++] = e.w[0];
        w[n++] = e.w[1];
        w[n++] = e.w[2];
        w[n++] = se::vidcmd_set(se::SET_CMP_COLOR1, se::rgb444(0xF, 0xF, 0xF));
        w[n++] = se::vidcmd_set(se::SET_CMP_COLOR0, se::rgb444(0x0, 0x0, 0x0));
        w[n++] = se::vidcmd_run(se::RUN_SRC_PASSTHROUGH, se::H_ACTIVE);

        // Four indexed codes across the line, so the screenshot shows all four
        // palette entries.
        for (unsigned word = 0; word < se::PIXELS_WORDS_2BPP; word++)
        {
            const unsigned code = (word + line / 8u) & 3u;
            uint16_t v = 0;
            for (unsigned k = 0; k < 8; k++)
            {
                v = static_cast<uint16_t>((v << 2) | code);
            }
            g_fb[line][word] = v;
        }
    }

    for (unsigned i = 0; i < VBLANK_WALK_LINES; i++)
    {
        b.pacer(false);
    }
    for (unsigned line = 0; line < LINES; line++)
    {
        b.stream(vidcmd_addr(line), IDX2_VIDCMD_WORDS, se::DEST_VIDCMD_FIFO_W, true,
                 se::DESC_MAX_COUNT);
        b.stream(fb_addr(line), se::PIXELS_WORDS_2BPP, se::DEST_PIXELS_FIFO_W, false,
                 se::DESC_MAX_COUNT);
    }
    b.pacer(true);
}

// --- C4: 20 PIXELS words and ~60 VIDCMD words per line ----------------------
//
// Roughly the shape of a sprite game's list: a cheap half-rate 1bpp playfield
// with a lot of per-line VIDCMD on top.  Slot arithmetic: 3 SETs (free) +
// 28 chained MASK records (448 slots) + one 192-slot passthrough RUN = 640.
//
// The playfield's two palette entries and its mode are loaded once per frame by
// the vblank preamble, which keeps the line at the 60 words RESULTS.md prices it
// at: before the indexed PIXEL the line authored FOUR SETs (two COMPOSITOR
// colours plus pal_fg and pal_bg) against a streamed count of 60, so the
// trailing RUN was silently dropped on every line.  Now the authored and the
// streamed counts are both 60, with pix_base as the third SET.
constexpr unsigned SPRITE_RECORDS      = 28;
constexpr unsigned SPRITE_VIDCMD_WORDS = 3 + SPRITE_RECORDS * 2 + 1;   // 60
constexpr unsigned SPRITE_RUN_SLOTS    = se::H_ACTIVE - SPRITE_RECORDS * se::MASK_SLOTS;

static_assert(SPRITE_VIDCMD_WORDS == 60, "C4 is specified as ~60 VIDCMD words per line");
static_assert(SPRITE_RUN_SLOTS == 192, "C4's records plus its run must occupy H_ACTIVE");

void build_sprites(Builder &b)
{
    const se::Rgb444 palette[2] = { se::rgb444(0x0, 0x0, 0x2), se::rgb444(0x0, 0x8, 0xF) };
    put_preamble(b, palette, 2, 0, se::pixel_mode(se::PIXEL_DEPTH_1BPP, true), false);

    for (unsigned line = 0; line < LINES; line++)
    {
        uint16_t *w = &g_vidcmd[line][0];
        unsigned  n = 0;
        w[n++] = se::vidcmd_set(se::SET_CMP_COLOR1, se::rgb444(0xF, 0xF, 0x0));
        w[n++] = se::vidcmd_set(se::SET_CMP_COLOR0, se::rgb444(0x8, 0x0, 0x0));
        w[n++] = se::vidcmd_set(se::SET_PIX_BASE, 0);

        for (unsigned r = 0; r < SPRITE_RECORDS; r++)
        {
            // Bit 15 of select is clear so the implicit pixel 0 (cmp_color0)
            // matches the art; opacity bit 15 is set for the same reason.
            const unsigned phase  = (r + line / 4u) & 7u;
            const uint16_t select = static_cast<uint16_t>(0x0AAAu >> (phase & 3u));
            const uint16_t opacity =
                static_cast<uint16_t>(0x8000u | (0x7FFEu >> (phase & 3u)) | select);
            const se::VidcmdMask m = se::vidcmd_mask_from_planes(opacity, select);
            w[n++] = m.header;
            w[n++] = m.data;
        }
        w[n++] = se::vidcmd_run(se::RUN_SRC_PASSTHROUGH, SPRITE_RUN_SLOTS);

        if (n != SPRITE_VIDCMD_WORDS)
        {
            printf("dmabench: C4 authored %u words, priced at %u\n", n, SPRITE_VIDCMD_WORDS);
        }

        for (unsigned word = 0; word < se::PIXELS_WORDS_1BPP_HALF; word++)
        {
            g_fb[line][word] = static_cast<uint16_t>(0x0F0Fu << ((line / 8u) & 1u));
        }
    }

    for (unsigned i = 0; i < VBLANK_WALK_LINES; i++)
    {
        b.pacer(false);
    }
    for (unsigned line = 0; line < LINES; line++)
    {
        b.stream(vidcmd_addr(line), SPRITE_VIDCMD_WORDS, se::DEST_VIDCMD_FIFO_W, true,
                 se::DESC_MAX_COUNT);
        b.stream(fb_addr(line), se::PIXELS_WORDS_1BPP_HALF, se::DEST_PIXELS_FIFO_W, false,
                 se::DESC_MAX_COUNT);
    }
    b.pacer(true);
}

// ---------------------------------------------------------------------------
// The configuration table
// ---------------------------------------------------------------------------

struct Config
{
    const char *tag;
    const char *what;
    void      (*build)(Builder &);
    bool        arm;
};

constexpr Config CONFIGS[] = {
    {"C0",  "ENGINE never armed (no DMA at all)",            build_none,     false},
    {"C0B", "blank screen, smallest legal list (2 desc)",    build_blank,    true},
    {"C1",  "640x480 1bpp PIXEL console, as firmware does",  build_1bpp,     true},
    {"C2",  "pure-VIDCMD MASK text, 82 words/line",          build_text,     true},
    {"C3",  "2bpp, 80 PIXELS + 6 VIDCMD per line",           build_2bpp,     true},
    {"C4",  "half-rate 1bpp, 20 PIXELS + 60 VIDCMD/line",    build_sprites,  true},
};

constexpr unsigned CONFIG_COUNT = sizeof(CONFIGS) / sizeof(CONFIGS[0]);

struct Measurement
{
    unsigned descriptors = 0;
    unsigned words       = 0;
    uint32_t busy        = 0;
    uint32_t units       = 0;
    uint32_t ms          = 0;
    bool     overflow    = false;
};

// One frame of SYSCLK: 525 scanlines of LINE_SYSCLK each.
constexpr uint32_t FRAME_SYSCLK = static_cast<uint32_t>(se::V_TOTAL * se::LINE_SYSCLK);

uint32_t g_desc_base = 0;
uint32_t g_desc_bytes = 0;

uint16_t desc_word_addr()
{
    return static_cast<uint16_t>((g_desc_base - se::DESC_TABLE_BASE) >> 1);
}

Builder build(const Config &cfg)
{
    Builder b;
    b.table = reinterpret_cast<uint16_t *>(g_desc_base);
    b.limit = g_desc_bytes / se::DESC_BYTES;
    cfg.build(b);
    return b;
}

Measurement run(const Config &cfg, unsigned fields)
{
    const Builder b = build(cfg);

    Measurement m;
    m.descriptors = b.count;
    m.words       = b.words;
    m.busy        = b.busy;
    m.overflow    = b.overflow;

    const uint16_t desc = desc_word_addr();

    // Two settling fields with the list live, so nothing below is measuring the
    // transition from the previous configuration.
    for (unsigned i = 0; i < 2; i++)
    {
        griffin_vsync_wait();
        if (cfg.arm)
        {
            ENGINE_DESC = desc;
        }
    }

    uint32_t units = 0;
    uint32_t acc    = 0x12345678u;
    unsigned seen   = 0;
    unsigned chunk  = 0;

    const uint32_t t0 = griffin_getticks();
    while (seen < fields)
    {
        acc = kernel_chunk(acc, chunk);
        chunk = (chunk + 1u) % KERNEL_CHUNKS;
        units++;
        if (griffin_vsync_pending())
        {
            griffin_vsync_ack();
            if (cfg.arm)
            {
                ENGINE_DESC = desc;
            }
            seen++;
        }
    }
    const uint32_t t1 = griffin_getticks();

    g_kernel_sink = acc;
    m.units = units;
    m.ms     = t1 - t0;
    return m;
}

void print_row(const Config &cfg, const Measurement &m, const Measurement &ref)
{
    // Percentages in tenths, integer arithmetic only: the firmware's printf has
    // no float and the app has no business pulling one in.
    const uint32_t share10 = (m.busy * 1000u + FRAME_SYSCLK / 2u) / FRAME_SYSCLK;
    const uint32_t ratio10 = (ref.units == 0)
                                 ? 0u
                                 : static_cast<uint32_t>((static_cast<uint64_t>(m.units) * 1000u +
                                                          ref.units / 2u) / ref.units);

    printf("%-4s %5u %6u %8lu %5lu.%lu%% %9lu %5lu.%lu%% %6lu%s\n",
           cfg.tag, m.descriptors, m.words,
           static_cast<unsigned long>(m.busy),
           static_cast<unsigned long>(share10 / 10u), static_cast<unsigned long>(share10 % 10u),
           static_cast<unsigned long>(m.units),
           static_cast<unsigned long>(ratio10 / 10u), static_cast<unsigned long>(ratio10 % 10u),
           static_cast<unsigned long>(m.ms),
           m.overflow ? "  DESCRIPTOR TABLE OVERFLOW" : "");
}

}   // namespace

int main(int argc, char **argv)
{
    unsigned fields   = 300;
    int      shot     = -1;

    if (argc > 2 && strcmp(argv[1], "shot") == 0)
    {
        shot = atoi(argv[2]);
        if (shot < 0 || shot >= static_cast<int>(CONFIG_COUNT))
        {
            printf("dmabench: no such configuration %d (0..%u)\n", shot, CONFIG_COUNT - 1);
            return 1;
        }
    }
    else if (argc > 1)
    {
        const int n = atoi(argv[1]);
        if (n > 0)
        {
            fields = static_cast<unsigned>(n);
        }
    }

    GriffinVideoDirectInfo info;
    info.desc_table_base  = 0;
    info.desc_table_bytes = 0;
    if (griffin_video_direct_start(&info) != 0)
    {
        printf("dmabench: SYS_VIDEO_DIRECT_START failed\n");
        return 1;
    }
    g_desc_base  = info.desc_table_base;
    g_desc_bytes = info.desc_table_bytes;

    fill_1bpp_framebuffer();

    if (shot >= 0)
    {
        const Config &cfg = CONFIGS[shot];
        const Builder b   = build(cfg);
        printf("dmabench: holding %s (%s): %u descriptors, %u words, %lu SYSCLK/frame\n",
               cfg.tag, cfg.what, b.count, b.words, static_cast<unsigned long>(b.busy));
        // Hold the screen forever; the emulator's --run-cycles ends the run and
        // writes the screenshot while this app still owns the display.
        for (;;)
        {
            griffin_vsync_wait();
            if (cfg.arm)
            {
                ENGINE_DESC = desc_word_addr();
            }
        }
    }

    printf("dmabench: %u fields per configuration, kernel = %u-word RMW pass over "
           "%u bytes of RAM\n",
           fields, KERNEL_WORDS, static_cast<unsigned>(sizeof(g_kernel)));
    printf("dmabench: carve 0x%06lX + %lu bytes (%lu descriptors), frame = %lu SYSCLK\n",
           static_cast<unsigned long>(g_desc_base),
           static_cast<unsigned long>(g_desc_bytes),
           static_cast<unsigned long>(g_desc_bytes / se::DESC_BYTES),
           static_cast<unsigned long>(FRAME_SYSCLK));
    printf("cfg   desc  words   SYSCLK   DMA/fr    units  vs C0      ms\n");

    Measurement results[CONFIG_COUNT];
    for (unsigned i = 0; i < CONFIG_COUNT; i++)
    {
        results[i] = run(CONFIGS[i], fields);
        print_row(CONFIGS[i], results[i], results[0]);
    }

    printf("\n");
    for (unsigned i = 0; i < CONFIG_COUNT; i++)
    {
        printf("%-4s %s\n", CONFIGS[i].tag, CONFIGS[i].what);
    }

    // The headline: what the MASK console leaves the CPU, against what the 1bpp
    // console leaves it.  C1 is index 2, C2 is index 3.
    if (results[2].units > 0)
    {
        const uint32_t pct10 = static_cast<uint32_t>(
            (static_cast<uint64_t>(results[3].units) * 1000u + results[2].units / 2u) /
            results[2].units);
        printf("\ndmabench: a MASK text console leaves the CPU %lu.%lu%% of the throughput "
               "a 1bpp console leaves it\n",
               static_cast<unsigned long>(pct10 / 10u), static_cast<unsigned long>(pct10 % 10u));
    }

    (void)griffin_video_direct_end();
    printf("dmabench: console restored\n");
    return 0;
}
