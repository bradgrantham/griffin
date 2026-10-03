// Mandelbrot explorer.
//
//     mandel              explore: left click zooms in about the cursor, right
//                         click zooms out; on the keyboard `a` toggles the
//                         palette animation, `p` steps through the palettes
//                         (rainbow, ochre, grey, black and white), `q` quits
//     mandel nocycle      the same with the palette animation off at start
//     mandel selftest     check the 68000 inner loop against its C++ definition
//                         and report its speed; no video
//
// WHAT IS ON THE SCREEN
//
//   * A 320x240 sixteen-colour playfield: PIXEL's 4bpp half-rate mode, each
//     framebuffer row shown on two scanlines by pointing both lines'
//     descriptors at the same eighty words.  The display list is built once
//     and never changes.
//   * A 640x480 arrow cursor that is NOT in the framebuffer.  It is a
//     COMPOSITOR MASK overlay delivered as one record chain in vertical
//     blanking (apps/lib/griffin_cursor.h), in COMPOSITOR's own two colours,
//     which bypass the palette.
//   * Palette cycling: indices 1..15 are a closed colour loop that rotates
//     under the picture by rewriting fifteen palette entries in the same
//     vblank preamble.  The framebuffer is not touched.
//
// So everything that moves -- the cursor and the colours -- lives in the
// preamble's 177 words, and everything that is expensive -- the picture -- is
// written exactly once per pixel.
//
// WHO ARMS THE DISPLAY LIST
//
// The list's vblank pacers count HBLANK edges from the moment it is armed, so
// arming N lines late shows the whole picture N lines low.  This program
// computes straight through vblank and could only poll between chunks of
// arithmetic thousands of cycles long, so it uses griffin_abi.h's INTERRUPT
// pacing model: its own level-6 handler re-arms ENGINE at every vsync and
// counts the frame, and the mainline notices the count change at its leisure.
//
// THE ONE RACE, and why it is not guarded: the mainline rewrites preamble
// words (cursor moved, palette cycled) and the handler arms a list that
// delivers them.  The rewrite happens only right after the mainline has seen
// the frame count change -- within one arithmetic chunk, a few milliseconds
// after the preamble went out -- and takes well under a millisecond, so it is
// finished most of a frame before the next vsync.  If it ever were caught, the
// damage is one frame of a torn cursor or a half-rotated palette; the picture
// itself cannot be affected.
//
// THE ARITHMETIC is mandel_ref.h (the definition) and mandel_iter.s (the
// 68000 inner loop).  host_ref.cpp renders the same definition on the host and
// is compared with the emulator's screenshot pixel for pixel.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unistd.h>

#include "../../griffin.generated.h"
#include "../../griffin.generated.refs.h"
#include "../../super-engine/descriptor.h"
#include "../lib/griffin_app.h"
#include "../lib/griffin_cursor.h"
#include "../lib/griffin_mouse.h"
#include "../lib/griffin_video.h"

#include "mandel_ref.h"

namespace se = SuperEngine;
using namespace Griffin::reg;

extern "C" int mandel_step(Mandel::State *state, unsigned count);

// mandel_iter.s indexes the state by hand.
static_assert(offsetof(Mandel::State, x) == 0 && offsetof(Mandel::State, y) == 4 &&
              offsetof(Mandel::State, cx) == 8 && offsetof(Mandel::State, cy) == 12 &&
              offsetof(Mandel::State, n) == 16,
              "mandel_iter.s STATE_* offsets");

namespace
{

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

constexpr unsigned WIDTH  = Mandel::WIDTH;      // 320
constexpr unsigned HEIGHT = Mandel::HEIGHT;     // 240
constexpr unsigned LINES  = se::V_ACTIVE;       // 480
constexpr unsigned LINE_WORDS = se::PIXELS_WORDS_4BPP_HALF;     // 80

static_assert(WIDTH * 2 == se::H_ACTIVE && HEIGHT * 2 == LINES);
static_assert(LINE_WORDS * 4 == WIDTH, "four 4-bit pixels per PIXELS word");

// Lines 491..524: the handler arms inside line 490 (the vsync line, just after
// /RS has emptied both FIFOs), and thirty-four HBLANK edges later the first
// line group's own wait is the edge that feeds line 0.  Same walk as
// firmware/rom.cpp and apps/dmabench.
constexpr unsigned VBLANK_WALK_LINES = 34;

// ---------------------------------------------------------------------------
// The preamble: every VIDCMD word the frame gets, delivered in vblank
// ---------------------------------------------------------------------------
//
//     [0, 48)     sixteen palette entries, three words each
//     48          SET pix_base 0
//     49          SET pix_mode 4bpp half rate
//     50, 51      SET cmp_color0 (cursor outline), SET cmp_color1 (cursor fill)
//     [52, 177)   the cursor chain, front-padded with repeats of word 49
//
// /RS clears pix_mode and pix_base at every vsync and nothing ever initialises
// the palette RAM, so all of it goes out every frame regardless; cycling and
// cursor motion only change which words.

constexpr unsigned PALETTE_WORDS  = se::PALETTE_ENTRIES * se::PALETTE_WORDS_PER_ENTRY;   // 48
constexpr unsigned PRE_PIX_BASE   = PALETTE_WORDS;
constexpr unsigned PRE_PIX_MODE   = PALETTE_WORDS + 1;
constexpr unsigned PRE_COLOR0     = PALETTE_WORDS + 2;
constexpr unsigned PRE_COLOR1     = PALETTE_WORDS + 3;
constexpr unsigned PRE_CHAIN      = PALETTE_WORDS + 4;
constexpr unsigned PREAMBLE_WORDS = PRE_CHAIN + GriffinCursor::CHAIN_WORDS;              // 177

static_assert(PREAMBLE_WORDS == 177);
// The chain's RUNs and MASKs wait in the FIFO for line 0; the SETs in front of
// them are consumed in blanking as they arrive.
static_assert(PREAMBLE_WORDS < se::VIDCMD_FIFO_WORDS);

constexpr uint16_t PIX_MODE_WORD =
    se::vidcmd_set(se::SET_PIX_MODE, se::pixel_mode(se::PIXEL_DEPTH_4BPP, true));
constexpr se::Rgb444 CURSOR_OUTLINE = se::rgb444(0x0, 0x0, 0x0);
constexpr se::Rgb444 CURSOR_FILL    = se::rgb444(0xF, 0xF, 0xF);

constexpr unsigned PREAMBLE_DESCRIPTORS =
    (PREAMBLE_WORDS + se::DESC_MAX_COUNT - 1) / se::DESC_MAX_COUNT;                      // 6
constexpr unsigned LINE_DESCRIPTORS =
    (LINE_WORDS + se::DESC_MAX_COUNT - 1) / se::DESC_MAX_COUNT;                          // 3
constexpr unsigned TABLE_DESCRIPTORS =
    PREAMBLE_DESCRIPTORS + VBLANK_WALK_LINES + LINES * LINE_DESCRIPTORS + 1;             // 1481

static_assert(TABLE_DESCRIPTORS * se::DESC_BYTES <= GRIFFIN_APP_DESC_BYTES);

// Payload storage.  Only the descriptor table has to live in the firmware's
// carve; these are ordinary DMA sources in the app's own .bss.  Static, never
// on the stack (firmware/rom.cpp's dl_build_gc4_list note).
uint16_t g_framebuffer[HEIGHT][LINE_WORDS];
uint16_t g_preamble[PREAMBLE_WORDS];

// ---------------------------------------------------------------------------
// The vsync handler
// ---------------------------------------------------------------------------

volatile uint16_t g_desc_word  = 0;     // what the handler writes to ENGINE_DESC
volatile uint32_t g_frames     = 0;     // vsyncs seen

// Level 6.  The latch must be cleared or the level-triggered IRQ re-enters
// forever; the arm must be here, not in the mainline, for the reason at the
// top of this file.  Three absolute-address moves and an rte.
__attribute__((interrupt)) void vsync_handler()
{
    GLUE_VSYNC_CLEAR = static_cast<uint8_t>(Griffin::GLUE_VSYNC_CLEAR_VSYNC_PENDING_MASK);
    ENGINE_DESC      = g_desc_word;
    g_frames         = g_frames + 1;
}

// The RAM vector table is at address 0, which the compiler assumes is not an
// object; pass the address through an empty asm so it cannot reason about it
// (the same laundering firmware/rom.cpp's vector_slot does).
constexpr uint32_t VECTOR_LEVEL6_ADDR = 30 * 4;

void install_vsync_handler()
{
    uint32_t addr = VECTOR_LEVEL6_ADDR;
    asm("" : "+r"(addr));
    *reinterpret_cast<volatile uint32_t *>(addr) = reinterpret_cast<uint32_t>(&vsync_handler);
}

// ---------------------------------------------------------------------------
// Display-list construction (once)
// ---------------------------------------------------------------------------

struct Builder
{
    uint16_t *table = nullptr;
    unsigned  count = 0;

    void emit(const se::Descriptor &d)
    {
        const se::DescriptorWords w = se::encode_descriptor(d);
        uint16_t *p = table + count * se::DESC_WORDS;
        for (unsigned i = 0; i < se::DESC_WORDS; i++)
        {
            p[i] = w.w[i];
        }
        count++;
    }

    // One scanline spent: wait for the HBLANK edge, deposit one word nowhere.
    void pacer(bool stop_after)
    {
        se::Descriptor d;
        d.src         = reinterpret_cast<uint32_t>(&g_framebuffer[0][0]);
        d.count       = 1;
        d.destination = se::DEST_NONE;
        d.wait_hblank = true;
        d.stop_after  = stop_after;
        emit(d);
    }

    void stream(uint32_t src, unsigned words, uint8_t destination, bool wait_first)
    {
        unsigned done = 0;
        while (done < words)
        {
            const unsigned take =
                (words - done > se::DESC_MAX_COUNT) ? se::DESC_MAX_COUNT : (words - done);
            se::Descriptor d;
            d.src         = src + done * 2u;
            d.count       = static_cast<uint16_t>(take);
            d.destination = destination;
            d.wait_hblank = wait_first && done == 0;
            emit(d);
            done += take;
        }
    }
};

unsigned build_table(uint32_t table_base)
{
    Builder b;
    b.table = reinterpret_cast<uint16_t *>(table_base);

    // No wait: this runs the moment the handler arms, inside the vsync line.
    b.stream(reinterpret_cast<uint32_t>(&g_preamble[0]), PREAMBLE_WORDS, se::DEST_VIDCMD_FIFO_W,
             false);
    for (unsigned i = 0; i < VBLANK_WALK_LINES; i++)
    {
        b.pacer(false);
    }
    for (unsigned line = 0; line < LINES; line++)
    {
        b.stream(reinterpret_cast<uint32_t>(&g_framebuffer[line / 2][0]), LINE_WORDS,
                 se::DEST_PIXELS_FIFO_W, true);
    }
    b.pacer(true);
    return b.count;
}

// ---------------------------------------------------------------------------
// Preamble maintenance
// ---------------------------------------------------------------------------

void write_palette(unsigned palette, unsigned phase)
{
    for (unsigned i = 0; i < se::PALETTE_ENTRIES; i++)
    {
        const se::PaletteEntryWords e =
            se::vidcmd_palette_entry(i, Mandel::palette_colour(palette, i, phase));
        for (unsigned w = 0; w < se::PALETTE_WORDS_PER_ENTRY; w++)
        {
            g_preamble[i * se::PALETTE_WORDS_PER_ENTRY + w] = e.w[w];
        }
    }
}

void write_cursor(unsigned x, unsigned y)
{
    GriffinCursor::build_chain(&g_preamble[PRE_CHAIN], GriffinCursor::ARROW, x, y, PIX_MODE_WORD);
}

void write_preamble_fixed()
{
    g_preamble[PRE_PIX_BASE] = se::vidcmd_set(se::SET_PIX_BASE, 0);
    g_preamble[PRE_PIX_MODE] = PIX_MODE_WORD;
    g_preamble[PRE_COLOR0]   = se::vidcmd_set(se::SET_CMP_COLOR0, CURSOR_OUTLINE);
    g_preamble[PRE_COLOR1]   = se::vidcmd_set(se::SET_CMP_COLOR1, CURSOR_FILL);
}

// ---------------------------------------------------------------------------
// Framebuffer: aligned square blocks of one colour index
// ---------------------------------------------------------------------------
//
// The picture is refined in passes of 8x8, 4x4, 2x2 and 1x1 blocks, and those
// sizes line up with a word (four pixels), so each is a different width of the
// same store: two words, one word, one byte, one nibble.

void fill_block(unsigned px, unsigned py, unsigned size, unsigned index)
{
    const uint16_t solid = static_cast<uint16_t>(index * 0x1111u);
    if (size == 8)
    {
        for (unsigned r = 0; r < 8; r++)
        {
            uint16_t *p = &g_framebuffer[py + r][px / 4];
            p[0] = solid;
            p[1] = solid;
        }
    }
    else if (size == 4)
    {
        for (unsigned r = 0; r < 4; r++)
        {
            g_framebuffer[py + r][px / 4] = solid;
        }
    }
    else if (size == 2)
    {
        // Big-endian: the word's high byte is its first two pixels.
        const uint8_t pair = static_cast<uint8_t>(index * 0x11u);
        for (unsigned r = 0; r < 2; r++)
        {
            uint8_t *bytes = reinterpret_cast<uint8_t *>(&g_framebuffer[py + r][0]);
            bytes[px / 2] = pair;
        }
    }
    else
    {
        const unsigned shift = 12u - 4u * (px & 3u);
        uint16_t      &word  = g_framebuffer[py][px / 4];
        word = static_cast<uint16_t>((word & ~(0xFu << shift)) | (index << shift));
    }
}

// ---------------------------------------------------------------------------
// Per-frame service: mouse, cursor, palette, keyboard
// ---------------------------------------------------------------------------

enum class Action : uint8_t
{
    None,
    ZoomIn,
    ZoomOut,
    Quit,
};

struct Ui
{
    uint32_t           seen_frames  = 0;
    GriffinMouseReport last_report  = {};
    int                cursor_x     = se::H_ACTIVE / 2;
    int                cursor_y     = se::V_ACTIVE / 2;
    unsigned           phase        = 0;
    unsigned           palette      = 0;
    bool               cycle        = true;
    bool               key_break    = false;   // the last scancode was the F0 break prefix
    Action             pending      = Action::None;
    unsigned           click_px     = 0;    // playfield pixel under the cursor at the click
    unsigned           click_py     = 0;
};

Ui g_ui;

constexpr unsigned CYCLE_FRAMES = 4;        // palette advances every fourth frame

// PS/2 scan-code set 2 make codes (firmware/keymap.cpp's table, by position).
constexpr uint8_t SCAN_A     = 0x1C;
constexpr uint8_t SCAN_P     = 0x4D;
constexpr uint8_t SCAN_Q     = 0x15;
constexpr uint8_t SCAN_BREAK = 0xF0;        // prefix: the next code is a release
constexpr uint8_t SCAN_EXT   = 0xE0;        // prefix: an extended key follows

// One raw scancode from the console.  Only make codes of unextended keys do
// anything; a release is the code after F0 and is dropped, and E0 is just
// skipped (an extended key's own code is then taken as a plain make, which
// is harmless: none of them is a, p or q).
void handle_key(uint8_t code)
{
    if (code == SCAN_BREAK)
    {
        g_ui.key_break = true;
        return;
    }
    if (g_ui.key_break)
    {
        g_ui.key_break = false;
        return;
    }
    if (code == SCAN_A)
    {
        g_ui.cycle = !g_ui.cycle;
    }
    else if (code == SCAN_P)
    {
        g_ui.palette = (g_ui.palette + 1) % Mandel::PALETTE_COUNT;
        write_palette(g_ui.palette, g_ui.phase);
    }
    else if (code == SCAN_Q)
    {
        g_ui.pending = Action::Quit;
    }
}

int clamp(int v, int lo, int hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

// Called between chunks of arithmetic.  Almost always one compare; once a
// frame, the work below.  Returns the action waiting to be taken, if any.
Action service()
{
    const uint32_t frames = g_frames;
    if (frames == g_ui.seen_frames)
    {
        return g_ui.pending;
    }
    g_ui.seen_frames = frames;

    // The preamble for this frame has just gone out, so now is the safe time
    // to change it (see "THE ONE RACE" above).
    GriffinMouseReport now;
    if (griffin_mouse_read(&now) == 0)
    {
        const int dx = static_cast<int>(now.x - g_ui.last_report.x);
        const int dy = static_cast<int>(g_ui.last_report.y - now.y);     // PS/2 Y is up
        if (dx != 0 || dy != 0)
        {
            g_ui.cursor_x = clamp(g_ui.cursor_x + dx, 0, static_cast<int>(se::H_ACTIVE) - 1);
            g_ui.cursor_y = clamp(g_ui.cursor_y + dy, 0, static_cast<int>(se::V_ACTIVE) - 1);
            write_cursor(static_cast<unsigned>(g_ui.cursor_x),
                         static_cast<unsigned>(g_ui.cursor_y));
        }

        // A press edge, and only while nothing is waiting: one click, one zoom.
        const uint8_t pressed =
            static_cast<uint8_t>(now.buttons & ~g_ui.last_report.buttons);
        if (g_ui.pending == Action::None &&
            (pressed & (GRIFFIN_MOUSE_BUTTON_LEFT | GRIFFIN_MOUSE_BUTTON_RIGHT)) != 0)
        {
            g_ui.pending  = (pressed & GRIFFIN_MOUSE_BUTTON_LEFT) != 0 ? Action::ZoomIn
                                                                        : Action::ZoomOut;
            g_ui.click_px = static_cast<unsigned>(g_ui.cursor_x) / 2u;
            g_ui.click_py = static_cast<unsigned>(g_ui.cursor_y) / 2u;
        }
        g_ui.last_report = now;
    }

    if (g_ui.cycle && frames % CYCLE_FRAMES == 0)
    {
        g_ui.phase = (g_ui.phase + 1) % Mandel::CYCLE_COLOURS;
        write_palette(g_ui.palette, g_ui.phase);
    }

    // The console is in raw mode: whatever is waiting is scancodes.
    while (griffin_inputready() > 0)
    {
        uint8_t codes[16];
        const int got = static_cast<int>(read(0, codes, sizeof(codes)));
        for (int i = 0; i < got; i++)
        {
            handle_key(codes[i]);
        }
        if (got <= 0)
        {
            break;
        }
    }
    return g_ui.pending;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

// A chunk is short against a frame so service() notices each vsync within a
// few milliseconds: 32 iterations at mandel_iter.s's worst 758 cycles is about
// 24,000 CPU cycles, and a frame is 233,625 SYSCLK.
constexpr unsigned CHUNK_ITERATIONS = 32;

// Compute one pixel's colour index.  Returns false if an action arrived and
// the pixel was abandoned.
bool compute_pixel(const Mandel::View &view, unsigned px, unsigned py, unsigned max_iter,
                   unsigned &index)
{
    const int32_t cx = Mandel::pixel_cx(view, px);
    const int32_t cy = Mandel::pixel_cy(view, py);
    if (Mandel::in_main_bulbs(cx, cy))
    {
        index = 0;
        return service() == Action::None;
    }

    Mandel::State state = Mandel::start(cx, cy);
    while (state.n < max_iter)
    {
        const unsigned left  = max_iter - state.n;
        const unsigned chunk = (left > CHUNK_ITERATIONS) ? CHUNK_ITERATIONS : left;
        if (mandel_step(&state, chunk) != 0)
        {
            break;
        }
        if (service() != Action::None)
        {
            return false;
        }
    }
    index = Mandel::colour_index(state.n, max_iter);
    return service() == Action::None;
}

// Progressive refinement.  Every pixel is computed exactly once: a pass at
// block size b skips the points the pass at 2b already did.  Returns false if
// an action interrupted it.
bool render(const Mandel::View &view)
{
    const unsigned max_iter = Mandel::max_iterations(view);
    for (unsigned size = 8; size != 0; size /= 2)
    {
        const unsigned coarse = size * 2;
        for (unsigned py = 0; py < HEIGHT; py += size)
        {
            for (unsigned px = 0; px < WIDTH; px += size)
            {
                if (size != 8 && (px % coarse) == 0 && (py % coarse) == 0)
                {
                    continue;
                }
                unsigned index = 0;
                if (!compute_pixel(view, px, py, max_iter, index))
                {
                    return false;
                }
                fill_block(px, py, size, index);
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Self-test: the assembly against its definition
// ---------------------------------------------------------------------------

// Run both to the same limit and compare everything observable.  The assembly
// is driven in chunks of seven so a chunk boundary falls at every phase of
// the reference's iteration count, which is what exercises the resume path.
bool selftest_point(int32_t cx, int32_t cy, unsigned max_iter)
{
    Mandel::State want = Mandel::start(cx, cy);
    bool want_escaped  = false;
    while (want.n < max_iter && !want_escaped)
    {
        want_escaped = Mandel::step(want);
    }

    Mandel::State got = Mandel::start(cx, cy);
    bool got_escaped  = false;
    while (got.n < max_iter && !got_escaped)
    {
        const unsigned left  = max_iter - got.n;
        const unsigned chunk = (left > 7u) ? 7u : left;
        got_escaped = mandel_step(&got, chunk) != 0;
    }

    return got.n == want.n && got.x == want.x && got.y == want.y &&
           got_escaped == want_escaped;
}

int selftest()
{
    constexpr unsigned MAX_ITER = 200;
    unsigned points = 0;
    unsigned failed = 0;

    // The top view on a coarse grid: interior, exterior and boundary.
    const Mandel::View home;
    for (unsigned py = 0; py < HEIGHT; py += 6)
    {
        for (unsigned px = 0; px < WIDTH; px += 5)
        {
            points++;
            failed += selftest_point(Mandel::pixel_cx(home, px), Mandel::pixel_cy(home, py),
                                     MAX_ITER) ? 0u : 1u;
        }
    }

    // Scattered points with every low bit in play, some outside the escape
    // radius from the start (c up to +-4 exercises the axis tests).
    uint32_t seed = 0x2545F491u;
    for (unsigned i = 0; i < 1500; i++)
    {
        seed = seed * 1664525u + 1013904223u;
        const int32_t cx = static_cast<int32_t>(seed) >> 2;     // +-2.0
        seed = seed * 1664525u + 1013904223u;
        const int32_t cy = static_cast<int32_t>(seed) >> ((i % 8 == 0) ? 1 : 3);
        points++;
        failed += selftest_point(cx, cy, MAX_ITER) ? 0u : 1u;
    }

    printf("mandel selftest: %u points, %u mismatches against mandel_ref.h: %s\n", points, failed,
           (failed == 0) ? "PASS" : "FAIL");

    // Speed, on a point that never escapes and whose orbit has busy low bits.
    constexpr unsigned TIMED_CHUNKS = 400;
    constexpr unsigned TIMED_CHUNK  = 100;
    Mandel::State state = Mandel::start(-(Mandel::ONE / 5), Mandel::ONE / 2);
    const uint32_t t0 = griffin_getticks();
    for (unsigned i = 0; i < TIMED_CHUNKS; i++)
    {
        (void)mandel_step(&state, TIMED_CHUNK);
    }
    const uint32_t ms = griffin_getticks() - t0;
    const uint32_t iterations = TIMED_CHUNKS * TIMED_CHUNK;
    if (ms > 0)
    {
        // SYSCLK per iteration as wall time, so it includes whatever the
        // console's display DMA takes from the CPU while this runs.
        const uint32_t sysclk_per_iteration =
            (ms * (Griffin::SYSCLK_HZ / 1000u)) / iterations;
        printf("mandel selftest: %lu iterations in %lu ms: %lu iterations/s, %lu SYSCLK each "
               "with the text console's DMA running\n",
               static_cast<unsigned long>(iterations), static_cast<unsigned long>(ms),
               static_cast<unsigned long>((iterations * 1000u) / ms),
               static_cast<unsigned long>(sysclk_per_iteration));
    }
    return (failed == 0) ? 0 : 1;
}

}   // namespace

int main(int argc, char **argv)
{
    bool cycle = true;
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "selftest") == 0)
        {
            return selftest();
        }
        if (strcmp(argv[i], "nocycle") == 0)
        {
            cycle = false;
        }
        else
        {
            printf("usage: %s [nocycle | selftest]\n", argv[0]);
            return 1;
        }
    }

    GriffinMouseReport report;
    if (griffin_mouse_read(&report) != 0)
    {
        printf("mandel: this firmware has no mouse syscall\n");
        return 1;
    }
    if (report.present == 0)
    {
        printf("mandel: no PS/2 mouse answered at boot\n");
        return 1;
    }
    printf("mandel: left click zooms in, right click zooms out; a animates the palette, "
           "p changes it, q quits\n");

    GriffinVideoDirectInfo info;
    info.desc_table_base  = 0;
    info.desc_table_bytes = 0;
    if (griffin_video_direct_start(&info) != 0)
    {
        printf("mandel: SYS_VIDEO_DIRECT_START failed\n");
        return 1;
    }
    if (info.desc_table_bytes < TABLE_DESCRIPTORS * se::DESC_BYTES)
    {
        (void)griffin_video_direct_end();
        printf("mandel: descriptor carve too small\n");
        return 1;
    }

    g_ui.cycle       = cycle;
    g_ui.last_report = report;
    // The cursor convention of griffin_mouse.h: centre plus everything the
    // mouse has reported since boot, so the emulator's pointer is already on
    // it, and on hardware nothing worse than a cursor not quite centred.
    g_ui.cursor_x = clamp(static_cast<int>(se::H_ACTIVE / 2) + static_cast<int>(report.x), 0,
                          static_cast<int>(se::H_ACTIVE) - 1);
    g_ui.cursor_y = clamp(static_cast<int>(se::V_ACTIVE / 2) - static_cast<int>(report.y), 0,
                          static_cast<int>(se::V_ACTIVE) - 1);
    write_palette(g_ui.palette, g_ui.phase);
    write_preamble_fixed();
    write_cursor(static_cast<unsigned>(g_ui.cursor_x), static_cast<unsigned>(g_ui.cursor_y));
    (void)build_table(info.desc_table_base);

    // Vector first, then the enable: the handler must exist before the IRQ can.
    g_desc_word = static_cast<uint16_t>((info.desc_table_base - se::DESC_TABLE_BASE) >> 1);
    install_vsync_handler();
    if (griffin_video_vsync_irq(1) != 0)
    {
        (void)griffin_video_direct_end();
        printf("mandel: this firmware cannot enable the vsync IRQ for an application\n");
        return 1;
    }
    if (griffin_console_raw(1) != 0)
    {
        (void)griffin_video_direct_end();
        printf("mandel: this firmware cannot give an application raw keyboard input\n");
        return 1;
    }

    Mandel::View view;
    bool         finished = false;
    for (;;)
    {
        if (!finished)
        {
            // The console is hidden behind the picture but still reaches the
            // serial port, where the timing is worth having.
            const uint32_t started = g_frames;
            finished = render(view);
            if (finished)
            {
                printf("mandel: level %u, %u iterations max, rendered in %lu frames\n",
                       view.level, Mandel::max_iterations(view),
                       static_cast<unsigned long>(g_frames - started));
            }
        }

        // A finished picture just waits for the next click; an interrupted
        // render already has its action.
        Action action = g_ui.pending;
        while (finished && action == Action::None)
        {
            action = service();
        }
        if (action == Action::Quit)
        {
            break;
        }

        // A zoom that cannot go further leaves the view, and a finished
        // picture of it, alone.
        bool changed = false;
        if (action == Action::ZoomIn)
        {
            changed = Mandel::zoom_in(view, g_ui.click_px, g_ui.click_py);
        }
        else if (action == Action::ZoomOut)
        {
            changed = Mandel::zoom_out(view, g_ui.click_px, g_ui.click_py);
        }
        g_ui.pending = Action::None;
        if (changed)
        {
            finished = false;
        }
    }

    // END restores the firmware's level-6 vector and its own IRQ enable; the
    // loader would restore line editing too, but the app leaves things as it
    // found them.
    (void)griffin_console_raw(0);
    (void)griffin_video_direct_end();
    return 0;
}
