// smurf -- a playable flip-screen rescue game on the display-list pipeline.
//
// This is the first application that puts a REAL PICTURE through the direct
// video surface: three micro-HAM backdrops off the CF card, sprite overlays
// composed as VIDCMD span lists on top of them, and a square/noise soundtrack
// deposited into the audio FIFO by the same display list that draws the frame.
// apps/dtest proved the surface; this proves it carries a game.
//
// ===========================================================================
// WHY THE LIST IS SHAPED THE WAY IT IS
// ===========================================================================
//
// A 14 MHz 68000 has ~233,000 SYSCLK in a frame and the ENGINE is already
// spending ~228 of every 445 SYSCLK line on DMA, so roughly half of that is
// left for the CPU.  A full-screen micro-HAM frame is
//
//   34 pacers                       walk from the vsync line (490) to line 0
//   480 x (1 VIDCMD + 3 PIXELS)     80 words of HAM per line = 32+32+16
//   9 audio bursts                  262 or 263 stereo pairs per frame
//   1 trailing stop_after pacer
//   -------------------------------------------------------------------
//   1964 descriptors = 15712 bytes, and two of those fit the 32 KB carve.
//
// which is FAR too much to author from scratch every frame.  So the list is a
// STATIC SKELETON, built once when a screen is entered (that build takes about
// 300 ms, and a blank flip between screens is authentic anyway):
//
//   * every line's PIXELS descriptors point straight at the .ham plane in the
//     heap and never change while a screen is up;
//   * every line's VIDCMD descriptor points at that line's packet in a pool,
//     {SET pix_mode, SET pix_pal_fg, SET pix_pal_bg, RUN(passthrough,1)},
//     which paints the whole line from the HAM plane;
//   * screen furniture that does not move -- the meadow's fence hazard,
//     Smurfette by her house, the title text, the lives indicator -- is drawn
//     INTO those static packets, so it costs nothing per frame;
//   * the audio descriptors point at that table's own sample buffer, so even
//     they never need rewriting -- only the samples do.
//
// Per frame the app touches ONLY the lines a MOVING sprite covers: 32 for the
// player alone, 48 when the forest bat is out too.  Those lines get a longer
// packet -- {SETs, RUN(pt,x0), spans..., RUN(pt,1)} -- built into a
// double-buffered pool in .bss, and their one VIDCMD descriptor is repointed at
// it; a line shared with a static sprite gets both, because the fixed sprites
// are handed to the per-frame builder as well as baked in.  The previous
// frame's lines in the same table are handed back to their static packets
// first, and when nothing moved at all (a death pose, a win pose) the whole
// pass is skipped on a signature compare.  Everything else in the 15712 bytes
// is left exactly as it was.
//
// ===========================================================================
// SPAN ARITHMETIC (the part that is easy to get one pixel wrong)
// ===========================================================================
//
// COMPOSITOR gives every record one slot per pixel, INCLUDING a SET.  A span
// in one of the eight saturated colours is a single RUN_COLOR word and costs
// exactly its own width; any other colour needs SET(cmp_held_fg) then
// RUN(held_fg,w) and therefore costs one pixel MORE than it covers.  That
// pixel is stolen from the passthrough gap in front of the span, so the sprite
// still lands where the game put it and the stolen pixel shows the backdrop
// (the source register still says passthrough while the SET is consumed).
//
// A packet is capped at 24 words, and that number is a TIMING result, not a
// guess.  A line's VIDCMD descriptor must be the first thing in the line's
// descriptor group -- its palette SETs have to commit before pixel 0, and a
// packet deposited any EARLIER than the preceding HBLANK would be eaten by the
// previous line's spare slots instead (COMPOSITOR holds only while nothing is
// staged).  But the same group's first PIXELS descriptor has to land word 0
// before pixel 0 as well, or PIXEL re-reads the previous line's last word and
// the whole line slides right.  From descriptor.h's cost model at 14 MHz:
//
//   HBLANK                     160 pixel clocks   = 89 SYSCLK
//   minus the engine's HBLANK synchroniser                -2
//   wait_hblank VIDCMD of W words   20 + 2W SYSCLK
//   PIXELS descriptor, first word           +15 SYSCLK
//   -----------------------------------------------------------
//   20 + 2W + 15 <= 87  =>  W <= 26
//
// 24 leaves four SYSCLK of margin, and it is enough: the widest packet any
// sprite pair in this game can produce is 23 words and the widest text line is
// 22 (both measured over every frame, row and offset).  A line that somehow
// wants more is truncated on the right and counted, not allowed to slide.
//
// Everything on a sprite line's packet must end by pixel 638 so the trailing
// RUN(passthrough,1) can hand the rest of the line back to the HAM plane; the
// PIXEL decoder runs underneath the whole time, so the backdrop is never
// disturbed by an overlay, on either side of it.
//
// ===========================================================================
// AUDIO
// ===========================================================================
//
// PORTS pops one stereo pair every second scanline: 15734.375 Hz, exactly
// 262.5 pairs per 525-line frame.  A frame's list cannot deposit half a pair,
// so table 0 carries 262 and table 1 carries 263 and the loop alternates them
// -- the average is exact and the FIFO neither dries nor overflows.  The FIFO
// is primed by the CPU (writes to AUDIO_FIFO) until PORTS reports half full
// plus a margin, which keeps the level above the half-full mark all frame and
// therefore keeps the firmware's level-2 HF_IRQ from firing at all.  Priming
// is repeated after every screen rebuild, since a rebuild is 300 ms with no
// list armed and the FIFO empties long before the end of it.  (Feeding the
// FIFO from the CPU across a rebuild was tried and is a false economy: a
// CPU-written pair costs enough that the rebuild has to service more pops than
// it saves, and 300 ms of rebuild became 1.8 s.  The flip is silent.)
//
// Silence is 0x80: the DAC is unsigned and 0x00 is the negative rail.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../../griffin.generated.h"
#include "../../griffin.generated.refs.h"
#include "../../super-engine/descriptor.h"
#include "../../image-tools/smurf-assets/smurf_sprites.h"
#include "../lib/griffin_app.h"
#include "../lib/griffin_input.h"
#include "../lib/griffin_video.h"

namespace se = SuperEngine;
using namespace Griffin::reg;

namespace
{

// ===========================================================================
// Raster and display-list geometry
// ===========================================================================

constexpr unsigned LINES               = se::V_ACTIVE;              // 480
constexpr unsigned VBLANK_WALK_LINES   = 34;                        // 491..524
constexpr unsigned HAM_WORDS_PER_LINE  = se::PIXELS_WORDS_MICROHAM; // 80
constexpr unsigned HAM_BYTES_PER_LINE  = HAM_WORDS_PER_LINE * 2;    // 160
constexpr unsigned PIXEL_DESCS_PER_LINE = 3;                        // 32 + 32 + 16
constexpr unsigned AUDIO_BURSTS        = 9;

constexpr unsigned DESCS_PER_LINE = 1 + PIXEL_DESCS_PER_LINE;
constexpr unsigned FRAME_DESCRIPTORS =
    VBLANK_WALK_LINES + LINES * DESCS_PER_LINE + AUDIO_BURSTS + 1;

// Two tables in the 32 KB app carve, so frame N+1 is authored while frame N is
// still being walked.
constexpr uint32_t TABLE_STRIDE = 0x4000;

static_assert(FRAME_DESCRIPTORS * se::DESC_BYTES <= TABLE_STRIDE,
              "a frame's descriptors must fit one half of the carve");
static_assert(2 * TABLE_STRIDE <= GRIFFIN_APP_DESC_BYTES,
              "two descriptor tables must fit the app's descriptor carve");

// One VIDCMD descriptor per line, capped so the line's first PIXELS word still
// makes it into HBLANK -- see the derivation at the top of this file.
constexpr unsigned PACKET_MAX_WORDS = 24;
static_assert(PACKET_MAX_WORDS <= se::DESC_MAX_COUNT,
              "a line's packet must be one descriptor");
static_assert(20 + 2 * PACKET_MAX_WORDS + 15 <= se::HBLANK_SYSCLK - 2,
              "the line's first PIXELS word must be deposited before pixel 0");

// The last slot a span may occupy: the trailing RUN(passthrough,1) needs one.
constexpr unsigned LAST_SLOT = se::H_ACTIVE - 1;            // 639

// ===========================================================================
// Audio pacing
// ===========================================================================

constexpr unsigned AUDIO_PAIRS[2] = { 262, 263 };   // per table; averages 262.5
constexpr unsigned AUDIO_PAIRS_MAX = 263;

// Pairs written past the half-full mark when priming, so the running level
// stays above half full and the firmware's HF_IRQ never latches.
constexpr unsigned AUDIO_PRIME_MARGIN = 40;

// ===========================================================================
// Game geometry
// ===========================================================================

constexpr int GROUND_Y   = 408;    // y of the player's feet when standing
constexpr int PLAYER_W   = 24;
constexpr int PLAYER_H   = 32;
constexpr int WALK_SPEED = 3;

// Subpixel vertical motion in quarter pixels: apex = 40*40/(2*3) = 266
// quarters = 66 px, airtime 28 frames.  The apex is what makes the fence
// jumpable with a margin rather than on a single frame -- the player's box
// clears the fence's top for 19 of those 28 frames, i.e. 57 px of travel
// against a 39 px hazard, so the jump has a six-frame launch window.
constexpr int JUMP_VELOCITY = -40;
constexpr int GRAVITY       = 3;

constexpr int PLAYER_X_MIN = 0;
constexpr int PLAYER_X_MAX = static_cast<int>(se::H_ACTIVE) - PLAYER_W;   // 616

// Collision boxes are gameplay constants, not sprite extents: the art has a
// few transparent rows the player should not be punished for.
constexpr int PLAYER_BOX_DX     = 4;
constexpr int PLAYER_BOX_W      = 16;
constexpr int PLAYER_BOX_H      = 30;
constexpr int PLAYER_DUCK_BOX_H = 24;

// The meadow's fence hazard stands in the gap the backdrop leaves at x
// 352..448 (image-tools/make_smurf_assets.cpp).
constexpr int FENCE_X = 384;
constexpr int FENCE_Y = GROUND_Y - 40;
constexpr int FENCE_BOX_DX = 4;
constexpr int FENCE_BOX_W  = 24;
constexpr int FENCE_BOX_DY = 2;
constexpr int FENCE_BOX_H  = 38;

// The forest bat swoops on a sine.  Its lowest box bottom (368+14 = 382) is
// under a standing player's box top (408-30 = 378) and over a ducking one's
// (408-24 = 384), which is what makes DOWN the answer to it.
constexpr int BAT_Y_MID  = 351;
constexpr int BAT_Y_SWING = 17;                 // 334 .. 368
constexpr int BAT_X_MID  = 316;
constexpr int BAT_X_SWING = 250;                // 66 .. 566
constexpr int BAT_BOX_DX = 4;
constexpr int BAT_BOX_W  = 16;
constexpr int BAT_BOX_DY = 4;
constexpr int BAT_BOX_H  = 10;

// Smurfette waits at the mushroom house on the clearing screen.
constexpr int SMURFETTE_X = 520;
constexpr int SMURFETTE_Y = GROUND_Y - 32;

constexpr unsigned SCREEN_COUNT = 3;
constexpr unsigned START_LIVES  = 3;

constexpr unsigned DEATH_FRAMES  = 72;    // death pose before the respawn
constexpr unsigned EXIT_HOLD     = 120;   // ~2 s of FIRE on the title exits

// A safety net so an unattended run cannot spin forever.  An optional argument
// overrides it, which is how a scripted session ends with the frame-rate report
// instead of running until someone presses something.
constexpr unsigned FRAME_LIMIT = 60u * 60u * 12u;   // 12 minutes

// ===========================================================================
// Assets
// ===========================================================================

struct Backdrop
{
    const uint16_t *ham = nullptr;   // 480 x 80 big-endian words
    const uint16_t *pal = nullptr;   // 480 x {pal_fg, pal_bg}
};

Backdrop g_backdrop[SCREEN_COUNT];

struct AssetName
{
    const char *ham;
    const char *pal;
};

constexpr AssetName ASSET_NAMES[SCREEN_COUNT] = {
    { "MEADOW.HAM",   "MEADOW.PAL"   },
    { "FOREST.HAM",   "FOREST.PAL"   },
    { "CLEARING.HAM", "CLEARING.PAL" },
};

const uint16_t *load_file(const char *path, unsigned bytes)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
    {
        printf("smurf: cannot open %s\n", path);
        return nullptr;
    }
    // malloc, not new[]: a throwing array-new drags the libstdc++ exception
    // machinery into the binary for no gain here.
    void *buf = malloc(bytes);
    if (!buf)
    {
        printf("smurf: out of memory for %s (%u bytes)\n", path, bytes);
        fclose(fp);
        return nullptr;
    }
    const size_t got = fread(buf, 1, bytes, fp);
    fclose(fp);
    if (got != bytes)
    {
        printf("smurf: %s short read (%lu of %u bytes)\n", path,
               static_cast<unsigned long>(got), bytes);
        free(buf);
        return nullptr;
    }
    return static_cast<const uint16_t *>(buf);
}

bool load_assets()
{
    for (unsigned i = 0; i < SCREEN_COUNT; i++)
    {
        g_backdrop[i].ham = load_file(ASSET_NAMES[i].ham, LINES * HAM_BYTES_PER_LINE);
        g_backdrop[i].pal = load_file(ASSET_NAMES[i].pal, LINES * 4);
        if (!g_backdrop[i].ham || !g_backdrop[i].pal)
        {
            return false;
        }
    }
    return true;
}

// ===========================================================================
// 5x7 font
//
// Text is drawn as spans exactly like a sprite, so a text line obeys the same
// 32-word packet cap.  Five bits per row means at most three spans per glyph
// (M and W are the only ones that reach three), which is what bounds a text
// line: the longest string drawn here is six characters.
// ===========================================================================

constexpr unsigned FONT_W = 5;
constexpr unsigned FONT_H = 7;

constexpr uint8_t FONT[38][FONT_H] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },   // ' '
    { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },   // A
    { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E },   // B
    { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E },   // C
    { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E },   // D
    { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F },   // E
    { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 },   // F
    { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F },   // G
    { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },   // H
    { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F },   // I
    { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C },   // J
    { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 },   // K
    { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F },   // L
    { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 },   // M
    { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 },   // N
    { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },   // O
    { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 },   // P
    { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D },   // Q
    { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 },   // R
    { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E },   // S
    { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 },   // T
    { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },   // U
    { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 },   // V
    { 0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11 },   // W
    { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 },   // X
    { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 },   // Y
    { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F },   // Z
    { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E },   // 0
    { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },   // 1
    { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F },   // 2
    { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },   // 3
    { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 },   // 4
    { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },   // 5
    { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E },   // 6
    { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },   // 7
    { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E },   // 8
    { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },   // 9
    { 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04 },   // '!'
};

unsigned font_index(char c)
{
    if (c >= 'A' && c <= 'Z')
    {
        return 1u + static_cast<unsigned>(c - 'A');
    }
    if (c >= '0' && c <= '9')
    {
        return 27u + static_cast<unsigned>(c - '0');
    }
    if (c == '!')
    {
        return 37u;
    }
    return 0u;   // space, and anything unmapped
}

// ===========================================================================
// Spans and packet emission
// ===========================================================================

struct Span
{
    int16_t  x;
    int16_t  w;
    uint16_t rgb;
};

constexpr unsigned MAX_SPANS = 48;

// Diagnostics, all reported at exit rather than silently swallowed.
unsigned g_slow_lines = 0;          // lines that needed the general emitter
unsigned g_packet_truncations = 0;   // a line wanted more than 32 words
unsigned g_static_clobbers    = 0;   // a moving sprite landed on a static-span line

bool saturated_code(uint16_t rgb, unsigned &code)
{
    const unsigned r = (rgb >> 8) & 0xF;
    const unsigned g = (rgb >> 4) & 0xF;
    const unsigned b = rgb & 0xF;
    if ((r == 0 || r == 0xF) && (g == 0 || g == 0xF) && (b == 0 || b == 0xF))
    {
        code = (r != 0 ? 4u : 0u) | (g != 0 ? 2u : 0u) | (b != 0 ? 1u : 0u);
        return true;
    }
    return false;
}

// One scanline's VIDCMD packet.  `spans` must be sorted by x; overlaps are
// resolved in favour of the earlier span, which is what keeps the slot
// arithmetic monotone.
//
// held_fg is tracked WITHIN a packet and assumed unknown at its start, so a
// colour that recurs on the same line (the smurf's skin either side of his
// black eyes, say) costs one word instead of two while every packet stays
// independent of whatever the previous line left in the register.
unsigned emit_packet(uint16_t *out, const Span *spans, unsigned n,
                     uint16_t pal_fg, uint16_t pal_bg)
{
    unsigned k = 0;
    out[k++] = se::vidcmd_set(se::SET_PIX_MODE, se::PIXEL_MODE_MICRO_HAM);
    out[k++] = se::vidcmd_set(se::SET_PIX_PAL_FG, pal_fg);
    out[k++] = se::vidcmd_set(se::SET_PIX_PAL_BG, pal_bg);

    int cursor = 0;
    int32_t held_fg = -1;      // no cmp_held_fg SET issued in this packet yet
    for (unsigned i = 0; i < n; i++)
    {
        int x = spans[i].x;
        int w = spans[i].w;
        if (x < cursor)
        {
            w -= (cursor - x);
            x  = cursor;
        }
        if (x + w > static_cast<int>(LAST_SLOT))
        {
            w = static_cast<int>(LAST_SLOT) - x;
        }
        if (w <= 0)
        {
            continue;
        }

        unsigned   code = 0;
        const bool sat  = saturated_code(spans[i].rgb, code);
        const bool need_set = !sat && (held_fg != static_cast<int32_t>(spans[i].rgb));
        int        gap  = x - cursor;
        const unsigned body = need_set ? 2u : 1u;

        if (need_set)
        {
            // The SET owns a pixel slot.  Steal it from the gap in front, where
            // it still shows the backdrop; if the span butts up against its
            // neighbour, steal the span's own first pixel instead.
            if (gap > 0)
            {
                gap--;
            }
            else if (w >= 2)
            {
                w--;
                x++;
            }
            else
            {
                continue;
            }
        }

        if (k + (gap > 0 ? 1u : 0u) + body + 1u > PACKET_MAX_WORDS)
        {
            g_packet_truncations++;
            break;
        }
        if (gap > 0)
        {
            out[k++] = se::vidcmd_run(se::RUN_SRC_PASSTHROUGH, static_cast<unsigned>(gap));
        }
        if (sat)
        {
            out[k++] = se::vidcmd_run_color(code, static_cast<unsigned>(w));
        }
        else
        {
            if (need_set)
            {
                out[k++] = se::vidcmd_set(se::SET_CMP_HELD_FG, spans[i].rgb);
                held_fg  = spans[i].rgb;
            }
            out[k++] = se::vidcmd_run(se::RUN_SRC_HELD_FG, static_cast<unsigned>(w));
        }
        cursor = x + w;
    }

    // Hand the rest of the line back to the HAM plane; COMPOSITOR holds the
    // last source for every remaining slot.
    out[k++] = se::vidcmd_run(se::RUN_SRC_PASSTHROUGH, 1);
    return k;
}

// Insertion sort by x.  n is at most a couple of dozen and the input is almost
// always already ordered (each source contributes an ordered run), so this
// beats anything cleverer.
void sort_spans(Span *s, unsigned n)
{
    for (unsigned i = 1; i < n; i++)
    {
        const Span v = s[i];
        unsigned j = i;
        while (j > 0 && s[j - 1].x > v.x)
        {
            s[j] = s[j - 1];
            j--;
        }
        s[j] = v;
    }
}

// ===========================================================================
// Sprite instances
// ===========================================================================

struct Instance
{
    const SpriteFrame *frame;
    int16_t            x;
    int16_t            y;
    uint16_t           row_base;   // index of row 0 in the g_row table
};

constexpr unsigned MAX_INSTANCES = 4;

// ---------------------------------------------------------------------------
// Precompiled sprite rows
//
// The general emitter above is the reference, but running it 32 times a frame
// is what actually blew the budget: a 68000 spends a few hundred cycles per
// span on the colour test, the gap arithmetic and the branches, and 32 lines
// of that is most of a frame.
//
// Everything it computes is FIXED for a given sprite row except where the
// sprite is: a row's records are the same words at every x, and only the
// passthrough gap in front of them changes.  So each row is compiled ONCE at
// startup into
//
//     lead    where the row's first record starts, relative to the sprite's x
//             (one less when that record needs a SET, which owns a slot)
//     words   the row's records verbatim, gaps between spans included
//     end     where the cursor ends up, relative to the sprite's x
//
// and a frame's line becomes: three SETs, then per sprite one RUN(passthrough,
// x + lead - cursor) and a copy of `words`, then the trailing RUN.  Two sprites
// concatenate as long as their extents do not overlap, which on this playfield
// means everything except the frame a collision happens on -- and that falls
// back to the general emitter, which handles overlap by clipping.
// ---------------------------------------------------------------------------

struct RowPacket
{
    uint16_t first;   // index into g_row_pool
    uint16_t words;
    int16_t  lead;
    int16_t  end;
};

constexpr unsigned ROW_TABLE_MAX = 384;
constexpr unsigned ROW_POOL_WORDS = 4096;

RowPacket g_row[ROW_TABLE_MAX];
uint16_t  g_row_pool[ROW_POOL_WORDS];
uint16_t  g_frame_row_base[SPRITE_FRAME_COUNT];
unsigned  g_row_words_max = 0;

// Compile every shipped sprite frame.  Returns false if the pools are too
// small, which is a build-time sizing error rather than a runtime condition.
bool init_row_packets()
{
    unsigned pool = 0;
    unsigned idx  = 0;

    for (int fi = 0; fi < SPRITE_FRAME_COUNT; fi++)
    {
        const SpriteFrame *f = SPRITE_FRAMES[fi];
        g_frame_row_base[fi] = static_cast<uint16_t>(idx);

        for (unsigned r = 0; r < f->height; r++)
        {
            if (idx >= ROW_TABLE_MAX || pool + PACKET_MAX_WORDS > ROW_POOL_WORDS)
            {
                return false;
            }
            const SpriteRow &row = f->rows[r];
            uint16_t *out = &g_row_pool[pool];
            unsigned  k   = 0;
            int       cursor  = 0;
            int32_t   held_fg = -1;
            int       lead    = 0;
            bool      first   = true;

            for (unsigned j = 0; j < static_cast<unsigned>(row.count); j++)
            {
                const SpriteSpan &sp = f->spans[row.first + j];
                int x = sp.x;
                int w = sp.width;
                if (x < cursor)
                {
                    w -= cursor - x;
                    x  = cursor;
                }
                if (w <= 0)
                {
                    continue;
                }
                unsigned   code = 0;
                const bool sat  = saturated_code(sp.rgb444, code);
                const bool need_set = !sat && (held_fg != static_cast<int32_t>(sp.rgb444));

                if (first)
                {
                    lead  = x - (need_set ? 1 : 0);
                    first = false;
                }
                else
                {
                    int gap = x - cursor - (need_set ? 1 : 0);
                    if (gap > 0)
                    {
                        out[k++] = se::vidcmd_run(se::RUN_SRC_PASSTHROUGH,
                                                  static_cast<unsigned>(gap));
                    }
                    else if (gap < 0)
                    {
                        // need_set with no gap: the SET takes the span's own
                        // first pixel instead (one pixel of the neighbour).
                        w--;
                        x++;
                        if (w <= 0)
                        {
                            continue;
                        }
                    }
                }

                if (sat)
                {
                    out[k++] = se::vidcmd_run_color(code, static_cast<unsigned>(w));
                }
                else
                {
                    if (need_set)
                    {
                        out[k++] = se::vidcmd_set(se::SET_CMP_HELD_FG, sp.rgb444);
                        held_fg  = sp.rgb444;
                    }
                    out[k++] = se::vidcmd_run(se::RUN_SRC_HELD_FG, static_cast<unsigned>(w));
                }
                cursor = x + w;
            }

            g_row[idx].first = static_cast<uint16_t>(pool);
            g_row[idx].words = static_cast<uint16_t>(k);
            g_row[idx].lead  = static_cast<int16_t>(lead);
            g_row[idx].end   = static_cast<int16_t>(cursor);
            if (k > g_row_words_max)
            {
                g_row_words_max = k;
            }
            pool += k;
            idx++;
        }
    }
    return true;
}

unsigned frame_row_base(const SpriteFrame *f)
{
    for (int i = 0; i < SPRITE_FRAME_COUNT; i++)
    {
        if (SPRITE_FRAMES[i] == f)
        {
            return g_frame_row_base[i];
        }
    }
    return 0;
}

Instance make_instance(const SpriteFrame *f, int x, int y)
{
    Instance in;
    in.frame    = f;
    in.x        = static_cast<int16_t>(x);
    in.y        = static_cast<int16_t>(y);
    in.row_base = static_cast<uint16_t>(frame_row_base(f));
    return in;
}

// Appends this line's spans from every instance that covers it; `max` is the
// room left in `out`, since callers stack several gathers into one buffer.
unsigned gather_sprite_spans(Span *out, unsigned max, const Instance *inst,
                             unsigned ninst, int line)
{
    unsigned n = 0;
    for (unsigned i = 0; i < ninst; i++)
    {
        const SpriteFrame *f = inst[i].frame;
        const int row = line - inst[i].y;
        if (row < 0 || row >= static_cast<int>(f->height))
        {
            continue;
        }
        const SpriteRow &r = f->rows[row];
        for (unsigned j = 0; j < static_cast<unsigned>(r.count) && n < max; j++)
        {
            const SpriteSpan &s = f->spans[r.first + j];
            out[n].x   = static_cast<int16_t>(inst[i].x + s.x);
            out[n].w   = static_cast<int16_t>(s.width);
            out[n].rgb = s.rgb444;
            n++;
        }
    }
    return n;
}

// ===========================================================================
// Display-list storage
// ===========================================================================

// Static per-line packets.  Variable length (a text line is longer than a
// plain backdrop line), bump-allocated into one pool at scene build.
constexpr unsigned STATIC_POOL_WORDS = 8192;

uint16_t g_static_pool[STATIC_POOL_WORDS];
uint32_t g_static_src[LINES];
uint8_t  g_static_count[LINES];
uint8_t  g_static_text_line[LINES];   // lines carrying text/HUD spans, which the
                                      // per-frame builder cannot reproduce and a
                                      // moving sprite must therefore stay off

// Per-frame packets for the lines a moving sprite covers, double buffered
// alongside the descriptor tables.
constexpr unsigned MAX_DIRTY = 128;

uint16_t g_dyn_pool[2][MAX_DIRTY * PACKET_MAX_WORDS];
uint16_t g_dirty[2][MAX_DIRTY];
unsigned g_dirty_count[2] = { 0, 0 };

// Signature of the dynamic content last authored into each table; 0 means
// "nothing, rebuild unconditionally".
uint32_t g_frame_sig[2] = { 0, 0 };

// Which lines this frame has already claimed, stamped with the frame number so
// no clearing pass is needed.  32 bits so the stamp cannot wrap onto a stale
// one inside a session.
uint32_t g_line_stamp[LINES];
uint32_t g_stamp = 0;

// Audio sample buffers, one per table for the same reason.
uint16_t g_audio_buf[2][AUDIO_PAIRS_MAX];

// Word offsets into a table, computed once.
uint16_t g_line_desc_word[LINES];
uint16_t g_audio_desc_word[AUDIO_BURSTS];
uint16_t g_stop_desc_word = 0;
uint8_t  g_audio_burst[2][AUDIO_BURSTS];

uint32_t g_table_base = 0;

se::DescriptorWords g_pacer;

void put_desc(uint16_t *p, const se::DescriptorWords &w)
{
    p[0] = w.w[0];
    p[1] = w.w[1];
    p[2] = w.w[2];
    p[3] = w.w[3];
}

uint16_t *table_at(unsigned which, unsigned word_offset)
{
    return reinterpret_cast<uint16_t *>(g_table_base + which * TABLE_STRIDE) + word_offset;
}

// ENGINE_DESC is a word address inside the hard-wired 0x3F0000 descriptor page.
uint16_t desc_word_addr(unsigned which)
{
    return static_cast<uint16_t>(
        ((g_table_base + which * TABLE_STRIDE) - se::DESC_TABLE_BASE) >> 1);
}

// Line `line` of a group ends that group when it is the last one before the
// next audio burst; the boundaries are evenly spread so the FIFO level stays
// inside a narrow band all frame.
unsigned group_end(unsigned g)
{
    return ((g + 1) * LINES) / AUDIO_BURSTS;
}

void init_list_layout()
{
    se::Descriptor d;
    d.src         = reinterpret_cast<uint32_t>(&g_static_pool[0]);
    d.count       = 1;
    d.signal_mask = se::SIGNAL_NONE;
    d.wait_hblank = true;
    g_pacer = se::encode_descriptor(d);

    unsigned idx = VBLANK_WALK_LINES;
    unsigned g   = 0;
    for (unsigned line = 0; line < LINES; line++)
    {
        g_line_desc_word[line] = static_cast<uint16_t>(idx * se::DESC_WORDS);
        idx += DESCS_PER_LINE;
        if (g < AUDIO_BURSTS && line + 1 == group_end(g))
        {
            g_audio_desc_word[g] = static_cast<uint16_t>(idx * se::DESC_WORDS);
            idx++;
            g++;
        }
    }
    g_stop_desc_word = static_cast<uint16_t>(idx * se::DESC_WORDS);
    idx++;

    // Evenly spread bursts that sum to the table's pair count.
    for (unsigned t = 0; t < 2; t++)
    {
        for (unsigned b = 0; b < AUDIO_BURSTS; b++)
        {
            const unsigned lo = (AUDIO_PAIRS[t] * b) / AUDIO_BURSTS;
            const unsigned hi = (AUDIO_PAIRS[t] * (b + 1)) / AUDIO_BURSTS;
            g_audio_burst[t][b] = static_cast<uint8_t>(hi - lo);
        }
    }
}

void put_line_vidcmd(unsigned which, unsigned line, uint32_t src, unsigned count)
{
    se::Descriptor d;
    d.src         = src;
    d.count       = static_cast<uint16_t>(count);
    d.signal_mask = se::SIGNAL_VIDCMD_FIFO_W;
    d.wait_hblank = true;
    put_desc(table_at(which, g_line_desc_word[line]), se::encode_descriptor(d));
}

// ===========================================================================
// Scene (screen) build
// ===========================================================================

void audio_render(uint16_t *dst, unsigned pairs);

struct TextItem
{
    const char *text;
    int16_t     x;
    int16_t     y;
    uint8_t     sx;
    uint8_t     sy;
    uint16_t    rgb;
};

constexpr unsigned MAX_TEXT_ITEMS = 4;

constexpr unsigned MAX_FIXED_INSTANCES = 2;

struct SceneSpec
{
    unsigned screen = 0;
    TextItem text[MAX_TEXT_ITEMS] = {};
    unsigned text_count = 0;
    unsigned hud_lives = 0;         // 0 = no lives indicator

    // Sprites that do not move while the screen is up.  They are BAKED INTO
    // the static packets -- which is what keeps the per-frame line count down
    // to the moving sprites alone -- and are ALSO handed to the per-frame
    // builder, so a line the player happens to share with them still gets
    // both.  Never baking a moving sprite in is the whole discipline.
    Instance fixed[MAX_FIXED_INSTANCES] = {};
    unsigned fixed_count = 0;
};

// The lives indicator: one white block per life, top left.
constexpr int HUD_Y = 16;
constexpr int HUD_H = 24;
constexpr int HUD_X = 16;
constexpr int HUD_PITCH = 32;
constexpr int HUD_W = 24;

// Spans that belong to the line no matter what the player is doing.  `text`
// reports whether any of them came from text or the HUD, i.e. from something
// the per-frame builder could NOT reproduce if a sprite landed on the line.
unsigned scene_static_spans(Span *out, const SceneSpec &spec, int line, bool *text = nullptr)
{
    unsigned n = 0;

    for (unsigned t = 0; t < spec.text_count && n < MAX_SPANS; t++)
    {
        const TextItem &ti = spec.text[t];
        const int rel = line - ti.y;
        if (rel < 0 || rel >= static_cast<int>(FONT_H) * ti.sy)
        {
            continue;
        }
        const unsigned row = static_cast<unsigned>(rel) / ti.sy;
        const int cell = FONT_W * ti.sx + ti.sx;     // one blank column between glyphs
        int cx = ti.x;
        for (const char *p = ti.text; *p != '\0'; p++, cx += cell)
        {
            const uint8_t bits = FONT[font_index(*p)][row];
            unsigned c = 0;
            while (c < FONT_W && n < MAX_SPANS)
            {
                if ((bits & (0x10u >> c)) == 0)
                {
                    c++;
                    continue;
                }
                unsigned run = 1;
                while (c + run < FONT_W && (bits & (0x10u >> (c + run))) != 0)
                {
                    run++;
                }
                out[n].x   = static_cast<int16_t>(cx + static_cast<int>(c) * ti.sx);
                out[n].w   = static_cast<int16_t>(run * ti.sx);
                out[n].rgb = ti.rgb;
                n++;
                c += run;
            }
        }
    }

    if (spec.hud_lives != 0 && line >= HUD_Y && line < HUD_Y + HUD_H)
    {
        for (unsigned i = 0; i < spec.hud_lives && n < MAX_SPANS; i++)
        {
            out[n].x   = static_cast<int16_t>(HUD_X + static_cast<int>(i) * HUD_PITCH);
            out[n].w   = static_cast<int16_t>(HUD_W);
            out[n].rgb = se::RGB444_WHITE;
            n++;
        }
    }

    if (text != nullptr)
    {
        *text = (n != 0);
    }
    n += gather_sprite_spans(out + n, MAX_SPANS - n, spec.fixed, spec.fixed_count, line);
    sort_spans(out, n);
    return n;
}

// Build both descriptor tables and every static packet for `spec`.  Costs about
// 300 ms with no list armed, so a screen flip blanks and goes quiet; that is
// what a flip-screen game of this vintage did anyway.
bool build_scene(const SceneSpec &spec)
{
    const Backdrop &bd = g_backdrop[spec.screen];
    unsigned pool = 0;
    Span spans[MAX_SPANS];

    for (unsigned line = 0; line < LINES; line++)
    {
        bool has_text = false;
        const unsigned n = scene_static_spans(spans, spec, static_cast<int>(line), &has_text);
        g_static_text_line[line] = has_text ? 1u : 0u;
        if (pool + PACKET_MAX_WORDS > STATIC_POOL_WORDS)
        {
            printf("smurf: static packet pool exhausted at line %u\n", line);
            return false;
        }
        const unsigned k = emit_packet(&g_static_pool[pool], spans, n,
                                       bd.pal[2 * line], bd.pal[2 * line + 1]);
        g_static_src[line]   = reinterpret_cast<uint32_t>(&g_static_pool[pool]);
        g_static_count[line] = static_cast<uint8_t>(k);
        pool += k;
    }

    for (unsigned which = 0; which < 2; which++)
    {
        uint16_t *p = table_at(which, 0);
        for (unsigned i = 0; i < VBLANK_WALK_LINES; i++)
        {
            put_desc(p, g_pacer);
            p += se::DESC_WORDS;
        }

        for (unsigned line = 0; line < LINES; line++)
        {
            put_line_vidcmd(which, line, g_static_src[line], g_static_count[line]);

            const uint32_t src = reinterpret_cast<uint32_t>(bd.ham) + line * HAM_BYTES_PER_LINE;
            uint16_t *d = table_at(which, g_line_desc_word[line] + se::DESC_WORDS);
            unsigned  done = 0;
            for (unsigned part = 0; part < PIXEL_DESCS_PER_LINE; part++)
            {
                const unsigned count = (HAM_WORDS_PER_LINE - done > se::DESC_MAX_COUNT)
                                           ? se::DESC_MAX_COUNT
                                           : (HAM_WORDS_PER_LINE - done);
                se::Descriptor pd;
                pd.src         = src + done * 2;
                pd.count       = static_cast<uint16_t>(count);
                pd.signal_mask = se::SIGNAL_PIXELS_FIFO_W;
                pd.wait_hblank = false;
                put_desc(d, se::encode_descriptor(pd));
                d += se::DESC_WORDS;
                done += count;
            }
        }

        for (unsigned b = 0; b < AUDIO_BURSTS; b++)
        {
            unsigned before = 0;
            for (unsigned i = 0; i < b; i++)
            {
                before += g_audio_burst[which][i];
            }
            se::Descriptor ad;
            ad.src         = reinterpret_cast<uint32_t>(&g_audio_buf[which][before]);
            ad.count       = g_audio_burst[which][b];
            ad.signal_mask = se::SIGNAL_AUDIO_FIFO_W;
            ad.wait_hblank = false;
            put_desc(table_at(which, g_audio_desc_word[b]), se::encode_descriptor(ad));
        }

        se::Descriptor sd;
        sd.src         = reinterpret_cast<uint32_t>(&g_static_pool[0]);
        sd.count       = 1;
        sd.signal_mask = se::SIGNAL_NONE;
        sd.wait_hblank = true;
        sd.stop_after  = true;
        put_desc(table_at(which, g_stop_desc_word), se::encode_descriptor(sd));

        g_dirty_count[which] = 0;
        g_frame_sig[which]   = 0;
    }

    return true;
}

// Rewrite only the lives blocks, which is the one static element that changes
// without a screen change.  A life is only ever LOST, so the new packet is
// never longer than the one already in the pool and can be written over it in
// place; the check is there so that stays true by construction rather than by
// argument.
void update_hud(const SceneSpec &spec)
{
    Span spans[MAX_SPANS];
    uint16_t packet[PACKET_MAX_WORDS];
    for (int line = HUD_Y; line < HUD_Y + HUD_H; line++)
    {
        bool has_text = false;
        const unsigned n = scene_static_spans(spans, spec, line, &has_text);
        const unsigned k = emit_packet(packet, spans, n,
                                       g_backdrop[spec.screen].pal[2 * line],
                                       g_backdrop[spec.screen].pal[2 * line + 1]);
        if (k > g_static_count[line])
        {
            continue;   // would run into the next line's packet; leave it be
        }
        g_static_text_line[line] = has_text ? 1u : 0u;
        uint16_t *const dst = reinterpret_cast<uint16_t *>(g_static_src[line]);
        for (unsigned w = 0; w < k; w++)
        {
            dst[w] = packet[w];
        }
        g_static_count[line] = static_cast<uint8_t>(k);
        for (unsigned which = 0; which < 2; which++)
        {
            put_line_vidcmd(which, static_cast<unsigned>(line), g_static_src[line], k);
        }
    }
}

// ===========================================================================
// Per-frame authoring: the moving sprites and this table's samples
// ===========================================================================


// What the dynamic half of a frame depends on.  When it is unchanged from the
// last frame authored into THIS table -- a death animation, a win pose, any
// held still -- the table's dynamic lines are already right and the whole
// restore/rebuild pass can be skipped.  Only the samples still have to be made.
uint32_t frame_signature(const SceneSpec &spec, const Instance *inst, unsigned ninst)
{
    uint32_t sig = 1u + spec.screen * 0x9E3779B9u;
    for (unsigned i = 0; i < ninst; i++)
    {
        sig = sig * 33u + reinterpret_cast<uint32_t>(inst[i].frame);
        sig = sig * 33u + static_cast<uint32_t>(static_cast<uint16_t>(inst[i].x));
        sig = sig * 33u + static_cast<uint32_t>(static_cast<uint16_t>(inst[i].y));
    }
    for (unsigned i = 0; i < spec.fixed_count; i++)
    {
        sig = sig * 33u + reinterpret_cast<uint32_t>(spec.fixed[i].frame);
        sig = sig * 33u + static_cast<uint32_t>(static_cast<uint16_t>(spec.fixed[i].x));
    }
    return sig;
}

void build_frame(unsigned which, const SceneSpec &spec,
                 const Instance *inst, unsigned ninst)
{
    const uint32_t sig = frame_signature(spec, inst, ninst);
    if (sig == g_frame_sig[which])
    {
        audio_render(g_audio_buf[which], AUDIO_PAIRS[which]);
        return;
    }
    g_frame_sig[which] = sig;

    // Hand back the lines this table dirtied two frames ago.
    for (unsigned i = 0; i < g_dirty_count[which]; i++)
    {
        const unsigned line = g_dirty[which][i];
        put_line_vidcmd(which, line, g_static_src[line], g_static_count[line]);
    }
    g_dirty_count[which] = 0;

    // Claim this frame's lines, once each.
    g_stamp++;
    unsigned dirty = 0;
    for (unsigned i = 0; i < ninst; i++)
    {
        const int y0 = inst[i].y;
        const int y1 = y0 + static_cast<int>(inst[i].frame->height);
        for (int line = y0; line < y1; line++)
        {
            if (line < 0 || line >= static_cast<int>(LINES))
            {
                continue;
            }
            if (g_line_stamp[line] == g_stamp)
            {
                continue;
            }
            if (dirty >= MAX_DIRTY)
            {
                break;
            }
            g_line_stamp[line] = g_stamp;
            g_dirty[which][dirty++] = static_cast<uint16_t>(line);
        }
    }
    g_dirty_count[which] = dirty;

    // The instances that can appear on a dirty line, in x order.  Two at most
    // in practice (the player and one piece of screen furniture), so this is a
    // straight insertion rather than a sort.
    Instance all[MAX_INSTANCES];
    unsigned nall = 0;
    for (unsigned i = 0; i < ninst && nall < MAX_INSTANCES; i++)
    {
        all[nall++] = inst[i];
    }
    for (unsigned i = 0; i < spec.fixed_count && nall < MAX_INSTANCES; i++)
    {
        all[nall++] = spec.fixed[i];
    }
    for (unsigned i = 1; i < nall; i++)
    {
        const Instance v = all[i];
        unsigned j = i;
        while (j > 0 && all[j - 1].x > v.x)
        {
            all[j] = all[j - 1];
            j--;
        }
        all[j] = v;
    }

    const uint16_t *pal = g_backdrop[spec.screen].pal;
    Span spans[MAX_SPANS];
    for (unsigned i = 0; i < dirty; i++)
    {
        const unsigned line = g_dirty[which][i];
        if (g_static_text_line[line] != 0)
        {
            g_static_clobbers++;
        }
        uint16_t *const out = &g_dyn_pool[which][i * PACKET_MAX_WORDS];

        // Fast path: paste each covering row's precompiled records behind a
        // passthrough gap.  Bails out to the general emitter if two sprites
        // overlap, if one runs off an edge, or if the packet would not fit.
        out[0] = se::vidcmd_set(se::SET_PIX_MODE, se::PIXEL_MODE_MICRO_HAM);
        out[1] = se::vidcmd_set(se::SET_PIX_PAL_FG, pal[2 * line]);
        out[2] = se::vidcmd_set(se::SET_PIX_PAL_BG, pal[2 * line + 1]);
        unsigned k = 3;
        int  cursor = 0;
        bool fast   = true;
        for (unsigned j = 0; j < nall; j++)
        {
            const int row = static_cast<int>(line) - all[j].y;
            if (row < 0 || row >= static_cast<int>(all[j].frame->height))
            {
                continue;
            }
            const RowPacket &rp = g_row[all[j].row_base + row];
            if (rp.words == 0)
            {
                continue;
            }
            const int gap = all[j].x + rp.lead - cursor;
            if (gap < 0 || all[j].x + rp.end > static_cast<int>(LAST_SLOT) ||
                k + 1u + rp.words + 1u > PACKET_MAX_WORDS)
            {
                fast = false;
                break;
            }
            if (gap > 0)
            {
                out[k++] = se::vidcmd_run(se::RUN_SRC_PASSTHROUGH,
                                          static_cast<unsigned>(gap));
            }
            const uint16_t *src = &g_row_pool[rp.first];
            for (unsigned w = 0; w < rp.words; w++)
            {
                out[k++] = src[w];
            }
            cursor = all[j].x + rp.end;
        }

        if (fast)
        {
            out[k++] = se::vidcmd_run(se::RUN_SRC_PASSTHROUGH, 1);
        }
        else
        {
            const unsigned n = gather_sprite_spans(spans, MAX_SPANS, all, nall,
                                                   static_cast<int>(line));
            sort_spans(spans, n);
            k = emit_packet(out, spans, n, pal[2 * line], pal[2 * line + 1]);
            g_slow_lines++;
        }
        put_line_vidcmd(which, line, reinterpret_cast<uint32_t>(out), k);
    }

    audio_render(g_audio_buf[which], AUDIO_PAIRS[which]);
}

// ===========================================================================
// Audio: two squares and an LFSR, SN76489 in spirit
// ===========================================================================

// Half-periods in samples at 15734.375 Hz, i.e. rate / (2 * frequency).
constexpr uint8_t NOTE_HALF_PERIOD[] = {
    30,   // 0  C4
    27,   // 1  D4
    24,   // 2  E4
    23,   // 3  F4
    20,   // 4  G4
    18,   // 5  A4
    16,   // 6  B4
    15,   // 7  C5
    13,   // 8  D5
    12,   // 9  E5
    10,   // 10 G5
    8,    // 11 C6
};
constexpr uint8_t NOTE_REST = 0xFF;

struct SeqStep
{
    uint8_t note;
    uint8_t units;   // AUDIO_SEQ_UNIT samples each
};

constexpr unsigned AUDIO_SEQ_UNIT = 1024;   // ~65 ms

// A sixteen-step background loop.  Deliberately plain: it has to survive being
// heard for minutes and it has to be cheap.
constexpr SeqStep MELODY[] = {
    { 7, 2 }, { 9, 2 }, { 10, 2 }, { 9, 2 },
    { 7, 2 }, { 4, 2 }, { 7,  2 }, { NOTE_REST, 2 },
    { 5, 2 }, { 7, 2 }, { 9,  2 }, { 7, 2 },
    { 4, 2 }, { 2, 2 }, { 0,  2 }, { NOTE_REST, 2 },
};

constexpr SeqStep SFX_JUMP[]  = { { 10, 1 }, { 11, 1 } };
constexpr SeqStep SFX_DEATH[] = { { 7, 2 }, { 5, 2 }, { 4, 2 }, { 0, 6 } };
constexpr SeqStep SFX_WIN[]   = { { 7, 1 }, { 9, 1 }, { 10, 1 }, { 11, 4 },
                                  { 10, 1 }, { 11, 6 } };

// 262 samples a frame is 262 trips round an inner loop that has to leave room
// for the display list, so the synthesiser is written as RUNS: the sequencer
// only ever runs at a note boundary, and between boundaries the whole state is
// three counters and three amplitudes living in registers.  A "rest" is an
// ordinary note with amplitude zero, so the inner loop has no channel-idle
// branch to take.
constexpr unsigned REST_HALF_PERIOD = 32;      // any period; the amplitude is 0
constexpr uint32_t RUN_FOREVER = 0x7FFFFFFFu;

struct Channel
{
    const SeqStep *seq   = nullptr;
    unsigned       len   = 0;
    unsigned       step  = 0;
    uint32_t       left  = RUN_FOREVER;   // samples left in this step
    unsigned       half  = REST_HALF_PERIOD;
    unsigned       count = REST_HALF_PERIOD;
    int            level = 0;             // current output, toggles sign
    int            amp   = 0;             // this channel's peak
    bool           loop  = false;

    void start(const SeqStep *s, unsigned n, unsigned amplitude, bool repeat)
    {
        seq  = s;
        len  = n;
        step = 0;
        left = 0;
        amp  = static_cast<int>(amplitude);
        loop = repeat;
    }

    void silence()
    {
        seq   = nullptr;
        left  = RUN_FOREVER;
        half  = REST_HALF_PERIOD;
        count = REST_HALF_PERIOD;
        level = 0;
    }

    // Advance to the next step if this one is spent, then report how many
    // samples may be rendered before the sequencer has to run again.
    unsigned prepare(unsigned want)
    {
        while (left == 0)
        {
            if (step >= len)
            {
                if (!loop)
                {
                    silence();
                    break;
                }
                step = 0;
            }
            const SeqStep &s = seq[step++];
            left  = s.units * AUDIO_SEQ_UNIT;
            half  = (s.note == NOTE_REST) ? REST_HALF_PERIOD : NOTE_HALF_PERIOD[s.note];
            count = half;              // samples until the next toggle
            level = (s.note == NOTE_REST) ? 0 : amp;
        }
        return (left < want) ? static_cast<unsigned>(left) : want;
    }
};

Channel  g_music;
Channel  g_sfx;
uint16_t g_noise_lfsr   = 0xACE1;
uint32_t g_noise_left   = 0;     // samples of noise burst remaining
unsigned g_noise_div    = 0;
unsigned g_noise_period = 4;
int      g_noise_amp    = 0;

constexpr unsigned MUSIC_AMPLITUDE = 22;
constexpr unsigned SFX_AMPLITUDE   = 34;

void audio_noise_burst(unsigned samples, int amplitude, unsigned period)
{
    g_noise_left   = samples;
    g_noise_amp    = amplitude;
    g_noise_period = period;
    g_noise_div    = 0;
}

void audio_render(uint16_t *dst, unsigned pairs)
{
    while (pairs != 0)
    {
        unsigned run = g_music.prepare(pairs);
        run = g_sfx.prepare(run);
        if (g_noise_left != 0 && g_noise_left < run)
        {
            run = static_cast<unsigned>(g_noise_left);
        }

        unsigned mc = g_music.count;
        unsigned sc = g_sfx.count;
        unsigned nc = g_noise_div;
        int      ml = g_music.level;
        int      sl = g_sfx.level;
        const unsigned mh = g_music.half;
        const unsigned sh = g_sfx.half;
        const unsigned np = g_noise_period;
        const int      na = (g_noise_left != 0) ? g_noise_amp : 0;
        uint16_t lfsr = g_noise_lfsr;

        // Two square waves hold their value between toggles, so the common
        // case (no noise burst) writes CONSTANT BLOCKS: work out how long the
        // mix cannot change for and fill that many words.  262 samples a frame
        // through a per-sample mixer was 4 ms of the 16.6 ms budget; this is a
        // store and a loop branch per sample.
        unsigned left = run;
        while (left != 0)
        {
            unsigned n = left;
            if (mc < n)
            {
                n = mc;
            }
            if (sc < n)
            {
                n = sc;
            }

            if (na != 0)
            {
                // Noise changes every np samples, so the block is one sample.
                if (nc == 0)
                {
                    nc = np;
                    // 16-bit maximal-length LFSR, taps 16,14,13,11.
                    const uint16_t bit = static_cast<uint16_t>(
                        ((lfsr >> 0) ^ (lfsr >> 2) ^ (lfsr >> 3) ^ (lfsr >> 5)) & 1u);
                    lfsr = static_cast<uint16_t>((lfsr >> 1) | (bit << 15));
                }
                nc--;
                n = 1;
            }

            int v = 0x80 + ml + sl + ((na != 0) ? (((lfsr & 1u) != 0) ? na : -na) : 0);
            if (v < 8)
            {
                v = 8;
            }
            else if (v > 248)
            {
                v = 248;
            }
            const uint16_t word = static_cast<uint16_t>((v << 8) | v);
            for (unsigned i = 0; i < n; i++)
            {
                *dst++ = word;
            }

            mc -= n;
            sc -= n;
            left -= n;
            if (mc == 0)
            {
                mc = mh;
                ml = -ml;
            }
            if (sc == 0)
            {
                sc = sh;
                sl = -sl;
            }
        }

        g_music.count = mc;
        g_sfx.count   = sc;
        g_music.level = ml;
        g_sfx.level   = sl;
        g_noise_div   = nc;
        g_noise_lfsr  = lfsr;
        g_music.left -= run;
        g_sfx.left   -= run;
        if (g_noise_left != 0)
        {
            g_noise_left -= run;
        }

        pairs -= run;
    }
}

// Fill the FIFO to just over half full, then a margin further.  PORTS reports
// only the half-full flag, so this is the one measurement available -- and
// staying above the mark is exactly what keeps the firmware's level-2 HF_IRQ
// from latching every frame.
void audio_prime()
{
    unsigned guard = 2 * Griffin::AUDIO_FIFO_DEPTH;
    uint16_t pair = 0;
    while ((PORTS_AUDIO_STATUS & Griffin::PORTS_AUDIO_STATUS_HALF_FULL_MASK) == 0 && guard-- != 0)
    {
        audio_render(&pair, 1);
        AUDIO_FIFO = pair;
    }
    for (unsigned i = 0; i < AUDIO_PRIME_MARGIN; i++)
    {
        audio_render(&pair, 1);
        AUDIO_FIFO = pair;
    }
}

void audio_enable(bool on)
{
    PORTS_AUDIO_CONTROL = static_cast<uint8_t>(
        Griffin::PORTS_AUDIO_CONTROL_CLEAR_HF_IRQ_MASK |
        (on ? Griffin::PORTS_AUDIO_CONTROL_ENABLE_MASK : 0u));
}

// ===========================================================================
// Game
// ===========================================================================

constexpr int8_t SINE64[64] = {
       0,    6,   12,   19,   24,   30,   36,   41,
      45,   49,   53,   56,   59,   61,   63,   64,
      64,   64,   63,   61,   59,   56,   53,   49,
      45,   41,   36,   30,   24,   19,   12,    6,
       0,   -6,  -12,  -19,  -24,  -30,  -36,  -41,
     -45,  -49,  -53,  -56,  -59,  -61,  -63,  -64,
     -64,  -64,  -63,  -61,  -59,  -56,  -53,  -49,
     -45,  -41,  -36,  -30,  -24,  -19,  -12,   -6,
};

int sine(unsigned phase, int amplitude)
{
    return (SINE64[phase & 63u] * amplitude) / 64;
}

enum class Mode : uint8_t
{
    Title,
    Play,
    Dying,
    Won,
    Exit,
};

struct Game
{
    Mode     mode = Mode::Title;
    unsigned screen = 0;
    unsigned lives  = START_LIVES;
    unsigned frame  = 0;         // free-running, the only timebase

    int  x      = 40;
    int  feet_q = GROUND_Y * 4;  // quarter pixels
    int  vy     = 0;
    bool airborne = false;
    bool ducking  = false;
    bool facing_right = true;

    unsigned timer = 0;          // death / win dwell
    unsigned fire_held = 0;
    bool     fire_prev = false;
    bool     game_over_banner = false;
};

Game g_game;

int player_feet() { return g_game.feet_q / 4; }

bool boxes_overlap(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh)
{
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

// The frame the player is drawn with, and the one collision uses.
const SpriteFrame *player_frame()
{
    if (g_game.mode == Mode::Dying)
    {
        return &SMURF_DEATH;
    }
    if (g_game.mode == Mode::Won)
    {
        return &SMURF_JUMP;
    }
    if (g_game.airborne)
    {
        return &SMURF_JUMP;
    }
    if (g_game.ducking)
    {
        return &SMURF_DUCK;
    }
    return ((g_game.frame >> 3) & 1u) != 0 ? &SMURF_WALK1 : &SMURF_WALK0;
}

void player_box(int &bx, int &by, int &bw, int &bh)
{
    const int h = g_game.ducking && !g_game.airborne ? PLAYER_DUCK_BOX_H : PLAYER_BOX_H;
    bx = g_game.x + PLAYER_BOX_DX;
    bw = PLAYER_BOX_W;
    by = player_feet() - h;
    bh = h;
}

int bat_x() { return BAT_X_MID + sine(g_game.frame / 2u, BAT_X_SWING); }
int bat_y() { return BAT_Y_MID + sine(g_game.frame, BAT_Y_SWING); }

SceneSpec make_scene()
{
    SceneSpec s;
    s.screen = g_game.screen;
    switch (g_game.mode)
    {
        case Mode::Title:
            s.screen = 0;
            // Black, not white: the meadow's clouds sit right behind these
            // two lines and white on white is unreadable.
            s.text[0] = { "SMURF",  200,  60, 6, 6, se::RGB444_BLACK };
            s.text[1] = { "RESCUE", 180, 120, 6, 6, se::RGB444_BLACK };
            if (g_game.game_over_banner)
            {
                s.text[2] = { "GAME", 215, 270, 5, 5, se::rgb444(0xF, 0xF, 0x0) };
                s.text[3] = { "OVER", 230, 330, 5, 5, se::rgb444(0xF, 0xF, 0x0) };
            }
            else
            {
                s.text[2] = { "PRESS", 215, 270, 5, 5, se::rgb444(0xF, 0xF, 0x0) };
                s.text[3] = { "FIRE",  240, 330, 5, 5, se::rgb444(0xF, 0xF, 0x0) };
            }
            s.text_count = 4;
            s.hud_lives  = 0;
            break;

        case Mode::Won:
            s.text[0] = { "YOU", 260,  60, 6, 6, se::RGB444_WHITE };
            s.text[1] = { "WIN", 260, 120, 6, 6, se::RGB444_WHITE };
            s.text_count = 2;
            s.hud_lives  = g_game.lives;
            break;

        default:
            s.text_count = 0;
            s.hud_lives  = g_game.lives;
            break;
    }

    // Screen furniture that never moves goes in the static packets: the meadow
    // fence and, on the clearing, Smurfette waiting by the mushroom house.
    if (g_game.mode != Mode::Title)
    {
        if (g_game.screen == 0)
        {
            s.fixed[s.fixed_count++] = make_instance(&FENCE, FENCE_X, FENCE_Y);
        }
        else if (g_game.screen == 2)
        {
            s.fixed[s.fixed_count++] = make_instance(&SMURFETTE_WALK0,
                                                     SMURFETTE_X, SMURFETTE_Y);
        }
    }
    return s;
}

unsigned collect_instances(Instance *inst)
{
    unsigned n = 0;

    if (g_game.mode == Mode::Play || g_game.mode == Mode::Dying ||
        g_game.mode == Mode::Won || g_game.mode == Mode::Title)
    {
        inst[n++] = make_instance(player_frame(), g_game.x, player_feet() - PLAYER_H);
    }

    // Only things that actually move: everything else is in the static packets
    // (see SceneSpec::fixed) and costs no per-frame work at all.
    if (g_game.mode != Mode::Title && g_game.screen == 1)
    {
        inst[n++] = make_instance(((g_game.frame >> 3) & 1u) != 0 ? &BAT_FLAP1 : &BAT_FLAP0,
                                  bat_x(), bat_y());
    }

    return n;
}

}   // namespace

// ===========================================================================
// main
// ===========================================================================

int main(int argc, char **argv)
{
    unsigned frame_limit = FRAME_LIMIT;
    if (argc > 1)
    {
        const unsigned n = static_cast<unsigned>(atoi(argv[1]));
        if (n != 0)
        {
            frame_limit = n;
        }
    }

    printf("smurf: micro-HAM flip-screen game on the display-list pipeline\n");

    if (!load_assets())
    {
        return 1;
    }
    printf("smurf: 3 backdrops loaded (%u bytes each plane)\n",
           LINES * HAM_BYTES_PER_LINE);

    GriffinVideoDirectInfo info;
    info.desc_table_base  = 0;
    info.desc_table_bytes = 0;
    if (griffin_video_direct_start(&info) != 0)
    {
        printf("smurf: SYS_VIDEO_DIRECT_START failed\n");
        return 1;
    }
    g_table_base = info.desc_table_base;

    init_list_layout();
    if (!init_row_packets())
    {
        printf("smurf: sprite row pools too small\n");
        (void)griffin_video_direct_end();
        return 1;
    }
    printf("smurf: %u descriptors/frame, %u bytes/table, carve %lu bytes, t=%lu ms\n",
           FRAME_DESCRIPTORS,
           static_cast<unsigned>(FRAME_DESCRIPTORS * se::DESC_BYTES),
           static_cast<unsigned long>(info.desc_table_bytes),
           static_cast<unsigned long>(griffin_getticks()));

    SceneSpec spec = make_scene();
    if (!build_scene(spec))
    {
        (void)griffin_video_direct_end();
        return 1;
    }

    // The tables' sample buffers are DMA'd from the very first armed frame, and
    // 0x0000 is the DAC's negative rail, not silence.
    for (unsigned t = 0; t < 2; t++)
    {
        for (unsigned i = 0; i < AUDIO_PAIRS_MAX; i++)
        {
            g_audio_buf[t][i] = se::audio_pair_word(0x80, 0x80);
        }
    }

    g_music.start(MELODY, sizeof MELODY / sizeof MELODY[0], MUSIC_AMPLITUDE, true);
    audio_prime();
    audio_enable(true);

    unsigned which       = 0;
    unsigned iterations  = 0;
    unsigned late_frames = 0;
    unsigned rebuilds    = 0;
    uint32_t rebuild_ms_max = 0;
    // Frames that did something known to cost more than a frame -- a screen
    // rebuild, or a serial print -- are not held against the budget; the
    // steady-state number is the one that has to be one frame per iteration.
    bool     slow_event  = true;
    const uint32_t ticks_start = griffin_getticks();

    while (g_game.mode != Mode::Exit && iterations < frame_limit)
    {
        // Probed BEFORE the wait: if vblank is already latched, this loop
        // iteration took longer than a frame and the picture just repeated.
        const bool late = griffin_vsync_pending() != 0;
        if (late && !slow_event)
        {
            late_frames++;
        }
        slow_event = false;

        griffin_vsync_wait();
        ENGINE_DESC = desc_word_addr(which);
        iterations++;
        g_game.frame++;

        GriffinInput in;
        griffin_input_read(&in);
        const bool fire  = griffin_joy_fire(in.joy1) != 0;
        const bool left  = griffin_joy_left(in.joy1) != 0;
        const bool right = griffin_joy_right(in.joy1) != 0;
        const bool up    = griffin_joy_up(in.joy1) != 0;
        const bool down  = griffin_joy_down(in.joy1) != 0;
        const bool fire_edge = fire && !g_game.fire_prev;
        const bool fire_release = !fire && g_game.fire_prev;
        g_game.fire_held = fire ? g_game.fire_held + 1 : 0;

        bool need_scene = false;

        switch (g_game.mode)
        {
            case Mode::Title:
            {
                // A demo walk across the meadow so the title screen moves.
                g_game.x += g_game.facing_right ? 2 : -2;
                if (g_game.x >= PLAYER_X_MAX)
                {
                    g_game.x = PLAYER_X_MAX;
                    g_game.facing_right = false;
                }
                if (g_game.x <= PLAYER_X_MIN)
                {
                    g_game.x = PLAYER_X_MIN;
                    g_game.facing_right = true;
                }
                g_game.feet_q = GROUND_Y * 4;
                g_game.airborne = false;
                g_game.ducking  = false;

                if (g_game.game_over_banner)
                {
                    g_game.timer++;
                    if (g_game.timer >= 180)
                    {
                        g_game.game_over_banner = false;
                        need_scene = true;
                    }
                    break;
                }
                if (g_game.fire_held >= EXIT_HOLD)
                {
                    printf("smurf: FIRE held on the title, exiting\n");
                    g_game.mode = Mode::Exit;
                    break;
                }
                if (fire_release)
                {
                    g_game.mode   = Mode::Play;
                    g_game.screen = 0;
                    g_game.lives  = START_LIVES;
                    g_game.x      = 24;
                    g_game.feet_q = GROUND_Y * 4;
                    g_game.vy     = 0;
                    g_game.airborne = false;
                    need_scene = true;
                    printf("smurf: start, lives %u\n", g_game.lives);
                }
                break;
            }

            case Mode::Play:
            {
                g_game.ducking = down && !g_game.airborne;

                if (!g_game.ducking)
                {
                    if (left)
                    {
                        g_game.x -= WALK_SPEED;
                        g_game.facing_right = false;
                    }
                    if (right)
                    {
                        g_game.x += WALK_SPEED;
                        g_game.facing_right = true;
                    }
                }
                if ((up || fire_edge) && !g_game.airborne)
                {
                    g_game.airborne = true;
                    g_game.vy = JUMP_VELOCITY;
                    g_sfx.start(SFX_JUMP, sizeof SFX_JUMP / sizeof SFX_JUMP[0], SFX_AMPLITUDE, false);
                }
                if (g_game.airborne)
                {
                    g_game.feet_q += g_game.vy;
                    g_game.vy += GRAVITY;
                    if (g_game.feet_q >= GROUND_Y * 4)
                    {
                        g_game.feet_q = GROUND_Y * 4;
                        g_game.vy = 0;
                        g_game.airborne = false;
                    }
                }
                else if ((left || right) && ((g_game.frame & 7u) == 0))
                {
                    audio_noise_burst(700, 9, 6);   // footstep tick
                }

                // Flip screens at the edges.
                if (g_game.x > PLAYER_X_MAX)
                {
                    if (g_game.screen + 1 < SCREEN_COUNT)
                    {
                        g_game.screen++;
                        g_game.x = 8;
                        need_scene = true;
                        printf("smurf: screen %u\n", g_game.screen);
                    }
                    else
                    {
                        g_game.x = PLAYER_X_MAX;
                    }
                }
                else if (g_game.x < PLAYER_X_MIN)
                {
                    if (g_game.screen > 0)
                    {
                        g_game.screen--;
                        g_game.x = PLAYER_X_MAX - 8;
                        need_scene = true;
                        printf("smurf: screen %u\n", g_game.screen);
                    }
                    else
                    {
                        g_game.x = PLAYER_X_MIN;
                    }
                }

                if (!need_scene)
                {
                    int bx, by, bw, bh;
                    player_box(bx, by, bw, bh);
                    bool hit = false;
                    bool win = false;
                    if (g_game.screen == 0)
                    {
                        hit = boxes_overlap(bx, by, bw, bh,
                                            FENCE_X + FENCE_BOX_DX, FENCE_Y + FENCE_BOX_DY,
                                            FENCE_BOX_W, FENCE_BOX_H);
                    }
                    else if (g_game.screen == 1)
                    {
                        hit = boxes_overlap(bx, by, bw, bh,
                                            bat_x() + BAT_BOX_DX, bat_y() + BAT_BOX_DY,
                                            BAT_BOX_W, BAT_BOX_H);
                    }
                    else
                    {
                        win = boxes_overlap(bx, by, bw, bh,
                                            SMURFETTE_X + PLAYER_BOX_DX,
                                            SMURFETTE_Y + (PLAYER_H - PLAYER_BOX_H),
                                            PLAYER_BOX_W, PLAYER_BOX_H);
                    }

                    if (hit)
                    {
                        g_game.mode  = Mode::Dying;
                        g_game.timer = 0;
                        g_game.airborne = false;
                        g_game.ducking  = false;
                        g_game.feet_q   = GROUND_Y * 4;
                        g_sfx.start(SFX_DEATH, sizeof SFX_DEATH / sizeof SFX_DEATH[0], SFX_AMPLITUDE, false);
                        audio_noise_burst(3000, 24, 3);
                        printf("smurf: hit on screen %u at x=%d feet=%d, lives %u\n",
                               g_game.screen, bx, by + bh, g_game.lives - 1);
                        slow_event = true;
                    }
                    else if (win)
                    {
                        g_game.mode  = Mode::Won;
                        g_game.timer = 0;
                        g_sfx.start(SFX_WIN, sizeof SFX_WIN / sizeof SFX_WIN[0], SFX_AMPLITUDE, false);
                        need_scene = true;
                        printf("smurf: rescued Smurfette, you win\n");
                    }
                }
                break;
            }

            case Mode::Dying:
            {
                g_game.timer++;
                if (g_game.timer >= DEATH_FRAMES)
                {
                    g_game.lives--;
                    if (g_game.lives == 0)
                    {
                        printf("smurf: game over\n");
                        g_game.mode = Mode::Title;
                        g_game.game_over_banner = true;
                        g_game.timer = 0;
                        g_game.x = 24;
                        need_scene = true;
                    }
                    else
                    {
                        printf("smurf: lives %u\n", g_game.lives);
                        slow_event = true;
                        g_game.mode = Mode::Play;
                        g_game.x = 24;
                        g_game.feet_q = GROUND_Y * 4;
                        g_game.vy = 0;
                        spec.hud_lives = g_game.lives;
                        update_hud(spec);
                    }
                }
                break;
            }

            case Mode::Won:
            {
                g_game.timer++;
                if (g_game.timer > 60 && fire_edge)
                {
                    g_game.mode = Mode::Title;
                    g_game.x = 24;
                    g_game.timer = 0;
                    need_scene = true;
                }
                break;
            }

            case Mode::Exit:
                break;
        }

        g_game.fire_prev = fire;

        if (g_game.mode == Mode::Exit)
        {
            break;
        }

        if (need_scene)
        {
            const uint32_t t0 = griffin_getticks();
            spec = make_scene();
            if (!build_scene(spec))
            {
                break;
            }
            audio_prime();
            const uint32_t dt = griffin_getticks() - t0;
            if (dt > rebuild_ms_max)
            {
                rebuild_ms_max = dt;
            }
            rebuilds++;
            slow_event = true;
            continue;
        }

        Instance inst[MAX_INSTANCES];
        const unsigned ninst = collect_instances(inst);
        which ^= 1;
        build_frame(which, spec, inst, ninst);
    }

    const uint32_t ticks_end = griffin_getticks();

    audio_enable(false);
    (void)griffin_video_direct_end();

    const uint32_t ms = ticks_end - ticks_start;
    printf("smurf: %u loop iterations in %lu ms", iterations,
           static_cast<unsigned long>(ms));
    if (ms != 0)
    {
        printf(" (%lu.%02lu frames/s)",
               static_cast<unsigned long>(iterations * 1000u / ms),
               static_cast<unsigned long>((iterations * 100000u / ms) % 100u));
    }
    printf("\n");
    printf("smurf: %u late frames (loop iterations that missed a vblank), "
           "%u scene rebuilds (worst %lu ms)\n", late_frames, rebuilds,
           static_cast<unsigned long>(rebuild_ms_max));
    printf("smurf: %u general-emitter lines, %u packet truncations, "
           "%u static-line clobbers, widest sprite row %u words\n",
           g_slow_lines, g_packet_truncations, g_static_clobbers, g_row_words_max);
    printf("smurf: console restored\n");
    return 0;
}
