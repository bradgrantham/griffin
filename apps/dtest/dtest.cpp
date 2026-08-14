// dtest -- end-to-end exercise of the direct-video application surface.
//
// This is the first application to take the display hardware away from the
// firmware, so it deliberately touches every part of that surface once:
//
//   SYS_VIDEO_DIRECT_START / _END   apps/lib/griffin_video.h
//   poll-mode vblank pacing         griffin_vsync_wait()
//   app-owned ENGINE arming         ENGINE_DESC written from the app
//   joystick and paddle sampling    apps/lib/griffin_input.h
//   clean restore to the console    griffin_video_direct_end()
//
// and, with the `noend` argument, the path where an app exits still holding
// direct access so the loader has to reclaim it.
//
// THE PICTURE IS ALL-VIDCMD.  There is no framebuffer and no PIXELS DMA at all:
// every visible pixel comes out of COMPOSITOR's held colours or a RUN_COLOR
// span.  That is the smallest correct display list -- one descriptor per
// scanline, a handful of payload words -- and it proves the whole
// ENGINE -> VIDCMD FIFO -> COMPOSITOR path without dragging pixel data in.
//
// FRAME SHAPE (see super-engine/descriptor.h for the encodings):
//
//   34 wait_hblank pacers        walk from the vsync line (490) to line 0
//   480 per-line descriptors     a VIDCMD packet where there is content,
//                                a bare wait_hblank pacer where there is not
//   1 trailing stop_after pacer  disarms ENGINE and pins the level-3 IRQ
//
// A line that receives no VIDCMD words HOLDS: COMPOSITOR keeps replaying the
// last RUN's source for the whole line.  So line 0's packet -- two SETs and a
// one-slot RUN of held_bg -- paints the entire background, and a content line
// only has to describe its own spans and then hand the rest of the line back
// to held_bg with a final one-slot RUN.
//
// Held colours reset at every /RS (vsync), so line 0 re-SETs them every frame;
// they are not sticky across the frame boundary.
//
// AUTHORING COST IS PART OF THE DESIGN.  A frame is 515 descriptors and a
// 14 MHz 68000 has ~233,000 SYSCLK to build the next one in, so the inner loop
// has to stay in the tens of cycles per descriptor:
//
//   - every pacer descriptor is byte-identical, so its four words are encoded
//     once at startup and the loop that spends a band of blank scanlines is
//     four moves through a cursor;
//   - scanlines are authored in BANDS (background, square, bar) rather than by
//     testing each of the 480 lines against every shape -- at 480 iterations
//     the test chain costs more than the descriptors do;
//   - all 32 lines of the square share ONE payload packet and all 24 lines of
//     the bar share another, because a descriptor source is just a DMA read
//     address and nothing stops several descriptors naming the same words.
//     A frame's whole VIDCMD payload is ten words.
//
// The first version of this app built each descriptor through a generic
// emit(se::Descriptor) call per scanline; that took 30 ms per frame, the loop
// ran at 1.9 video frames per iteration, and the list was armed at a drifting
// mid-frame scanline instead of in vblank -- a torn, rolling picture.  The
// numbers above are what keeps the arm inside the vblank it was paced to.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../griffin.generated.h"
#include "../../griffin.generated.refs.h"
#include "../../super-engine/descriptor.h"
#include "../lib/griffin_input.h"
#include "../lib/griffin_video.h"

namespace se = SuperEngine;
using namespace Griffin::reg;

namespace
{

// ---------------------------------------------------------------------------
// Picture geometry
// ---------------------------------------------------------------------------

// The joystick square.
constexpr unsigned SQUARE_SIZE = 32;
constexpr unsigned SQUARE_STEP = 4;      // pixels per frame of dpad travel

// The paddle bar: a horizontal span from x=0 whose width follows paddle A.
constexpr unsigned BAR_TOP    = 432;
constexpr unsigned BAR_HEIGHT = 24;
constexpr unsigned BAR_SCALE  = 2;       // 0..255 counts -> 0..510 pixels

// A line's records must total no more than H_ACTIVE slots or the leftover
// record plays at the start of the NEXT line (the JIT overrun hazard in
// descriptor.h).  Every content line ends with a one-slot RUN that hands the
// rest of the line to held_bg, so the widest span has to stop one pixel short.
constexpr unsigned SQUARE_X_MAX = se::H_ACTIVE - SQUARE_SIZE - 1;   // 607

// The square and the bar are kept in disjoint bands so no scanline ever needs
// to describe both.  One shape per line is what lets each band share a single
// payload packet.
constexpr unsigned SQUARE_Y_MAX = BAR_TOP - SQUARE_SIZE;            // 400

static_assert(BAR_SCALE * 255u + 1u <= se::H_ACTIVE,
              "the paddle bar plus its trailing one-slot RUN must fit a line");
static_assert(BAR_SCALE * 255u <= se::RUN_COLOR_MAX_COUNT,
              "the paddle bar is one RUN_COLOR record, so it is capped at 511 px");
static_assert(BAR_TOP + BAR_HEIGHT <= se::V_ACTIVE, "the paddle bar runs off the screen");

constexpr se::Rgb444 BACKGROUND = se::rgb444(0x0, 0x1, 0x4);   // dark blue
constexpr se::Rgb444 FOREGROUND = se::rgb444(0xF, 0xF, 0xF);   // not used by the
                                                               // picture; SET so
                                                               // both COMPOSITOR
                                                               // held registers
                                                               // are exercised

// ---------------------------------------------------------------------------
// Display-list storage
// ---------------------------------------------------------------------------

// One descriptor per scanline, plus the vblank walk and the trailing stop.
constexpr unsigned VBLANK_WALK_LINES = 34;   // lines 491..524, as firmware/rom.cpp
constexpr unsigned FRAME_DESCRIPTORS = VBLANK_WALK_LINES + se::V_ACTIVE + 1;

// Two tables, so frame N+1 is built while frame N is still being walked.  Only
// DESCRIPTOR TABLES have to live in the carve (griffin_abi.h); the VIDCMD words
// are ordinary DMA sources and stay in the app's own .bss below.
constexpr uint32_t TABLE_STRIDE = 0x4000;    // 2048 descriptors of room each

static_assert(FRAME_DESCRIPTORS * se::DESC_BYTES <= TABLE_STRIDE,
              "a frame's descriptors must fit one half of the carve");
static_assert(2 * TABLE_STRIDE <= GRIFFIN_APP_DESC_BYTES,
              "two descriptor tables must fit the app's descriptor carve");

// A frame's whole payload: the line-0 packet (two SETs plus at most a square),
// the square band's packet and the bar band's packet.
constexpr unsigned PACKET_MAX  = 5;
constexpr unsigned PAYLOAD_MAX = 3 * PACKET_MAX;

// Double-buffered for the same reason the tables are: the engine is still
// reading frame N's words while frame N+1 is authored.
uint16_t g_payload[2][PAYLOAD_MAX];

// Pre-encoded descriptors whose every field is frame-invariant.  Only the
// per-line VIDCMD descriptors are built at authoring time.
se::DescriptorWords g_pacer;
se::DescriptorWords g_stop;

void init_descriptor_templates()
{
    se::Descriptor d;
    // A pacer strobes nothing, so the source only has to be a readable word.
    d.src         = reinterpret_cast<uint32_t>(&g_payload[0][0]);
    d.count       = 1;
    d.signal_mask = se::SIGNAL_NONE;
    d.wait_hblank = true;
    g_pacer = se::encode_descriptor(d);

    d.stop_after = true;
    g_stop = se::encode_descriptor(d);
}

// What the picture looks like this frame.
struct Scene
{
    unsigned square_x = 48;
    unsigned square_y = 48;
    unsigned bar_width = 0;
};

// ---------------------------------------------------------------------------
// Authoring
// ---------------------------------------------------------------------------

void put_desc(uint16_t *&p, const se::DescriptorWords &w)
{
    p[0] = w.w[0];
    p[1] = w.w[1];
    p[2] = w.w[2];
    p[3] = w.w[3];
    p += se::DESC_WORDS;
}

// A wait_hblank descriptor that strobes nothing spends exactly one scanline and
// deposits nothing.  edma3 has no wait-VBLANK, so a run of these is how a list
// walks from the arming point to the line it wants.
void put_pacers(uint16_t *&p, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
    {
        put_desc(p, g_pacer);
    }
}

// One scanline that receives `n` VIDCMD words from `src`.  wait_hblank puts the
// deposit in that line's HBLANK, so the words are staged before its pixel 0.
void put_vidcmd_line(uint16_t *&p, uint32_t src, unsigned n)
{
    se::Descriptor d;
    d.src         = src;
    d.count       = static_cast<uint16_t>(n);
    d.signal_mask = se::SIGNAL_VIDCMD_FIFO_W;
    d.wait_hblank = true;
    put_desc(p, se::encode_descriptor(d));
}

// The records that draw the square on one of its scanlines.  Identical for all
// 32 of them, which is why the band shares a single packet.
unsigned square_records(uint16_t *out, const Scene &s)
{
    unsigned n = 0;
    if (s.square_x > 0)
    {
        out[n++] = se::vidcmd_run(se::RUN_SRC_HELD_BG, s.square_x);
    }
    out[n++] = se::vidcmd_run_color(se::RUN_COLOR_WHITE, SQUARE_SIZE);
    out[n++] = se::vidcmd_run(se::RUN_SRC_HELD_BG, 1);
    return n;
}

unsigned bar_records(uint16_t *out, const Scene &s)
{
    unsigned n = 0;
    if (s.bar_width > 0)
    {
        out[n++] = se::vidcmd_run_color(se::RUN_COLOR_YELLOW, s.bar_width);
    }
    out[n++] = se::vidcmd_run(se::RUN_SRC_HELD_BG, 1);
    return n;
}

// Build a whole frame's list into table `which`.  Returns the descriptor count
// so the caller can report it once.
unsigned build_frame(uint32_t table_base, unsigned which, const Scene &s)
{
    uint16_t *const table = reinterpret_cast<uint16_t *>(table_base + which * TABLE_STRIDE);
    uint16_t *const pay   = g_payload[which];
    uint16_t       *p     = table;

    // --- payload: three packets, whatever the scene needs ------------------
    //
    // Line 0 carries the frame's held-colour SETs.  They are consumed during
    // blanking, ahead of the first RUN, so they cost no active slot.
    unsigned n0 = 0;
    pay[n0++] = se::vidcmd_set(se::SET_CMP_HELD_BG, BACKGROUND);
    pay[n0++] = se::vidcmd_set(se::SET_CMP_HELD_FG, FOREGROUND);
    if (s.square_y == 0)
    {
        n0 += square_records(pay + n0, s);
    }
    else
    {
        // Nothing else on line 0, but the SETs still need a RUN behind them to
        // point COMPOSITOR at held_bg for the rest of the frame.
        pay[n0++] = se::vidcmd_run(se::RUN_SRC_HELD_BG, 1);
    }

    uint16_t *const sq_pay = pay + PACKET_MAX;
    const unsigned  n_sq   = square_records(sq_pay, s);

    uint16_t *const bar_pay = pay + 2 * PACKET_MAX;
    const unsigned  n_bar   = bar_records(bar_pay, s);

    const uint32_t pay_addr     = reinterpret_cast<uint32_t>(pay);
    const uint32_t sq_pay_addr  = reinterpret_cast<uint32_t>(sq_pay);
    const uint32_t bar_pay_addr = reinterpret_cast<uint32_t>(bar_pay);

    // --- descriptors: the vblank walk, then the scanlines in bands ---------
    put_pacers(p, VBLANK_WALK_LINES);

    put_vidcmd_line(p, pay_addr, n0);
    unsigned line = 1;

    // Background down to the square, then the square band.  When square_y is 0
    // the square's first line was line 0 above, and this band picks up at 1.
    if (line < s.square_y)
    {
        put_pacers(p, s.square_y - line);
        line = s.square_y;
    }
    for (; line < s.square_y + SQUARE_SIZE; line++)
    {
        put_vidcmd_line(p, sq_pay_addr, n_sq);
    }

    put_pacers(p, BAR_TOP - line);
    line = BAR_TOP;
    for (; line < BAR_TOP + BAR_HEIGHT; line++)
    {
        put_vidcmd_line(p, bar_pay_addr, n_bar);
    }

    put_pacers(p, se::V_ACTIVE - line);
    put_desc(p, g_stop);

    return static_cast<unsigned>(p - table) / se::DESC_WORDS;
}

// ENGINE_DESC is a word address inside the hard-wired 0x3F0000 descriptor page.
uint16_t desc_word_addr(uint32_t table_base, unsigned which)
{
    return static_cast<uint16_t>(
        ((table_base + which * TABLE_STRIDE) - se::DESC_TABLE_BASE) >> 1);
}

unsigned clamp_up(unsigned v, unsigned step, unsigned hi)
{
    return (v + step > hi) ? hi : v + step;
}

unsigned clamp_down(unsigned v, unsigned step)
{
    return (v < step) ? 0 : v - step;
}

}   // namespace

int main(int argc, char **argv)
{
    const bool skip_end = (argc > 1) && (strcmp(argv[1], "noend") == 0);
    const unsigned frame_limit = skip_end ? 120u : 3600u;

    printf("dtest: all-VIDCMD direct-video test%s\n",
           skip_end ? " (noend: exits still holding direct access)" : "");

    GriffinVideoDirectInfo info;
    info.desc_table_base  = 0;
    info.desc_table_bytes = 0;
    if (griffin_video_direct_start(&info) != 0)
    {
        printf("dtest: SYS_VIDEO_DIRECT_START failed\n");
        return 1;
    }

    printf("dtest: carve base 0x%06lX, %lu bytes; tables at 0x%06lX and 0x%06lX\n",
           static_cast<unsigned long>(info.desc_table_base),
           static_cast<unsigned long>(info.desc_table_bytes),
           static_cast<unsigned long>(info.desc_table_base),
           static_cast<unsigned long>(info.desc_table_base + TABLE_STRIDE));

    init_descriptor_templates();

    Scene scene;
    unsigned which = 0;
    const unsigned descriptors = build_frame(info.desc_table_base, which, scene);

    printf("dtest: %u descriptors/frame, DESC=0x%04X/0x%04X\n",
           descriptors,
           static_cast<unsigned>(desc_word_addr(info.desc_table_base, 0)),
           static_cast<unsigned>(desc_word_addr(info.desc_table_base, 1)));

    unsigned frames = 0;
    bool fired = false;
    while (frames < frame_limit && !fired)
    {
        // Vblank has just started; the 34 pacers at the head of the list absorb
        // whatever poll latency got us here, so arming now lands content line 0
        // on scanline 0.  ENGINE_DESC is a true 16-bit register: one move.w.
        griffin_vsync_wait();
        ENGINE_DESC = desc_word_addr(info.desc_table_base, which);
        frames++;

        // Exactly once per observed vblank: the paddle dump pulse inside this
        // call is what starts the next frame's ramp.
        GriffinInput in;
        griffin_input_read(&in);

        if (griffin_joy_left(in.joy1))
        {
            scene.square_x = clamp_down(scene.square_x, SQUARE_STEP);
        }
        if (griffin_joy_right(in.joy1))
        {
            scene.square_x = clamp_up(scene.square_x, SQUARE_STEP, SQUARE_X_MAX);
        }
        if (griffin_joy_up(in.joy1))
        {
            scene.square_y = clamp_down(scene.square_y, SQUARE_STEP);
        }
        if (griffin_joy_down(in.joy1))
        {
            scene.square_y = clamp_up(scene.square_y, SQUARE_STEP, SQUARE_Y_MAX);
        }
        scene.bar_width = in.paddle_a * BAR_SCALE;
        fired = griffin_joy_fire(in.joy1) != 0;

        // Author the next frame into the other table while ENGINE walks this
        // one.  Both halves are ours for the whole run, so nothing else can be
        // reading the one being written.
        which ^= 1;
        (void)build_frame(info.desc_table_base, which, scene);
    }

    if (skip_end)
    {
        printf("dtest: %u frames, exiting WITHOUT griffin_video_direct_end()\n", frames);
        return 0;
    }

    (void)griffin_video_direct_end();
    printf("dtest: %u frames, square at (%u,%u), bar %u px; console restored\n",
           frames, scene.square_x, scene.square_y, scene.bar_width);
    return 0;
}
