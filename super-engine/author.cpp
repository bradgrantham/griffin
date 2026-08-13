// author.cpp — build the validation display lists, their pixel payloads and
// their VIDCMD instruction streams.
//
// Bare-metal rules apply (see author.h): no allocation, no I/O, no exceptions,
// no host-only headers.  Everything is written through the caller's Memory.

#include "author.h"

#include <array>

namespace SuperEngine
{

namespace
{

// ---------------------------------------------------------------------------
// Descriptor-table cursor
// ---------------------------------------------------------------------------

struct ListWriter
{
    Memory   ram;
    uint32_t base          = 0;
    uint32_t limit         = 0;
    uint32_t cursor        = 0;
    uint32_t count         = 0;
    uint32_t payload_words = 0;
    uint32_t pacing_words  = 0;
    bool     overflow      = false;

    void emit(const Descriptor &d);
};

void ListWriter::emit(const Descriptor &d)
{
    if (cursor + DESC_BYTES > limit)
    {
        overflow = true;
        return;
    }

    const DescriptorWords enc = encode_descriptor(d);
    for (uint32_t i = 0; i < DESC_WORDS; i++)
    {
        ram[cursor + i * 2] = enc.w[i];
    }

    cursor += DESC_BYTES;
    count++;
    payload_words += d.count;
    if (d.signal_mask == SIGNAL_NONE)
    {
        pacing_words += d.count;
    }
}

// Emit `words` payload words through `mask`, split into descriptors of at most
// `max_chunk` words each, sized as evenly as the split allows.
void emit_stream(ListWriter &w, uint8_t mask, uint32_t src, uint32_t words,
                 bool wait_first, uint32_t max_chunk)
{
    if (words == 0)
    {
        return;
    }
    if (max_chunk == 0 || max_chunk > DESC_MAX_COUNT)
    {
        max_chunk = DESC_MAX_COUNT;
    }

    const uint32_t pieces = (words + max_chunk - 1) / max_chunk;
    const uint32_t share  = words / pieces;
    const uint32_t extra  = words % pieces;

    uint32_t addr = src;
    for (uint32_t i = 0; i < pieces; i++)
    {
        Descriptor d;
        d.src         = addr;
        d.count       = static_cast<uint16_t>(share + (i < extra ? 1u : 0u));
        d.signal_mask = mask;
        d.wait_hblank = wait_first && (i == 0);
        w.emit(d);
        addr += static_cast<uint32_t>(d.count) * 2;
    }
}

// Two descriptors per line is the shape the plan calls for — 20w + 20w for a
// plain 1bpp line, which is exactly griffin.yml's ENGINE_WORDS_PER_BURST — and
// it stays two when pixel_skip adds a 41st word.  Past 64 words the 5-bit
// count field is what sets the floor, so an 80-word micro-HAM line splits into
// three 27ish-word descriptors rather than four 20-word ones.
uint32_t pixel_chunk_for(uint32_t words)
{
    if (words <= 2 * DESC_MAX_COUNT)
    {
        return (words + 1) / 2;
    }
    return DESC_MAX_COUNT;
}

// ---------------------------------------------------------------------------
// Bit packing for the pixel streams
// ---------------------------------------------------------------------------

// MSB-first into consecutive 16-bit words, which is exactly how PIXEL shifts
// them back out.
struct BitPacker
{
    Memory   ram;
    uint32_t base       = 0;
    uint32_t word_index = 0;
    uint32_t acc        = 0;
    uint32_t held_bits  = 0;

    void put(uint32_t value, uint32_t count)
    {
        for (uint32_t i = 0; i < count; i++)
        {
            acc = (acc << 1) | ((value >> (count - 1 - i)) & 1u);
            held_bits++;
            if (held_bits == 16)
            {
                ram[base + word_index * 2] = static_cast<uint16_t>(acc);
                word_index++;
                acc       = 0;
                held_bits = 0;
            }
        }
    }
};

// ---------------------------------------------------------------------------
// VIDCMD tile bitmaps
// ---------------------------------------------------------------------------

// A 16x16 arrow.  MSB of each word is the leftmost pixel, matching both the
// pixel stream and the tile words.
constexpr std::array<uint16_t, 16> CURSOR_MASK = {
    0x8000, 0xC000, 0xE000, 0xF000,
    0xF800, 0xFC00, 0xFE00, 0xFF00,
    0xFF80, 0xFFC0, 0xFE00, 0xEE00,
    0xCF00, 0x0700, 0x0780, 0x0380,
};

// The select word says which held colour an opaque pixel takes; eroding the
// mask by one pixel horizontally leaves a held_bg outline around a held_fg
// interior, which is the classic always-legible cursor and needs no second
// bitmap to maintain.
constexpr uint16_t erode_horizontal(uint16_t m)
{
    return static_cast<uint16_t>(m & static_cast<uint16_t>(m << 1) & static_cast<uint16_t>(m >> 1));
}

// A 16x16 diamond, used for the 4-sprites-per-line worst case.  Computed
// rather than tabulated so the shape is obviously symmetric.
constexpr uint16_t sprite_mask(uint32_t row)
{
    const uint32_t half  = (row < 8) ? row : (15 - row);
    const uint32_t width = 2 * (half + 1);
    const uint32_t shift = (16 - width) / 2;
    return static_cast<uint16_t>((((1u << width) - 1u) << shift) & 0xFFFFu);
}

// Horizontal fg/bg banding inside the sprite: unmistakable in a PPM, and it
// exercises both held colours on every sprite line.
constexpr uint16_t sprite_select(uint32_t row)
{
    return (row & 1u) ? 0x0000 : 0xFFFF;
}

// ---------------------------------------------------------------------------
// Tempest-web moving objects
// ---------------------------------------------------------------------------
//
// This is the per-frame CPU authoring path in miniature: object positions in,
// a sorted non-overlapping list of coloured spans for one scanline out.  It
// allocates nothing and touches no bitmap — moving an object is re-running
// this, which is the whole point of the hybrid architecture.

struct ColorSpan
{
    int32_t  x     = 0;
    int32_t  w     = 0;
    uint32_t color = 0;
};

constexpr std::array<uint32_t, TEMPEST_FLIPPERS> FLIPPER_LANE       = {2, 7, 11};
constexpr std::array<int32_t, TEMPEST_FLIPPERS>  FLIPPER_DEPTH_BASE = {46, 58, 70};
constexpr int32_t                                FLIPPER_DEPTH_STEP = 13;

constexpr std::array<uint32_t, TEMPEST_SHOTS> SHOT_LANE       = {5, 13};
constexpr std::array<int32_t, TEMPEST_SHOTS>  SHOT_DEPTH_BASE = {52, 64};
constexpr int32_t                             SHOT_DEPTH_STEP = 21;

constexpr uint32_t CLAW_LANE_BASE = 4;

// Insert one span keeping the list sorted by x, then clip it against whatever
// is already there.  Overlapping objects would otherwise double-count pixels
// and blow the line's slot sum, which is the failure mode that slides the whole
// stream until vsync — so the rasterizer, not the caller, guarantees disjoint.
void insert_span(std::span<ColorSpan> list, uint32_t &n, ColorSpan s)
{
    if (s.x < 0)
    {
        s.w += s.x;
        s.x = 0;
    }
    if (s.x + s.w > static_cast<int32_t>(H_ACTIVE))
    {
        s.w = static_cast<int32_t>(H_ACTIVE) - s.x;
    }
    if (s.w <= 0 || n >= list.size())
    {
        return;
    }

    uint32_t at = n;
    for (uint32_t i = 0; i < n; i++)
    {
        if (list[i].x > s.x)
        {
            at = i;
            break;
        }
    }
    for (uint32_t i = n; i > at; i--)
    {
        list[i] = list[i - 1];
    }
    list[at] = s;
    n++;

    // Sweep once and push any overlap out of the later span.
    uint32_t kept = 0;
    int32_t  edge = 0;
    for (uint32_t i = 0; i < n; i++)
    {
        ColorSpan c = list[i];
        if (c.x < edge)
        {
            c.w -= (edge - c.x);
            c.x = edge;
        }
        if (c.w <= 0)
        {
            continue;
        }
        edge = c.x + c.w;
        list[kept] = c;
        kept++;
    }
    n = kept;
}

// All coloured spans covering `line`, sorted and disjoint.
uint32_t tempest_spans_for_line(const FrameParams &p, uint32_t line, std::span<ColorSpan> out)
{
    uint32_t n = 0;

    // Player claw: two prongs straddling the lane's outer-rim vertex, riding
    // just inside the near rim.  One lane per frame.
    {
        const uint32_t lane = (CLAW_LANE_BASE + p.frame_index) % TEMPEST_LANES;
        const int32_t  cx   = tempest_depth_x(lane, 92);
        const int32_t  cy   = tempest_depth_y(lane, 92);
        if (static_cast<int32_t>(line) >= cy &&
            static_cast<int32_t>(line) < cy + static_cast<int32_t>(TEMPEST_CLAW_ROWS))
        {
            const int32_t row = static_cast<int32_t>(line) - cy;
            insert_span(out, n, {cx - 11 + row, TEMPEST_CLAW_WIDTH, TEMPEST_CLAW_COLOR});
            insert_span(out, n, {cx + 8 - row, TEMPEST_CLAW_WIDTH, TEMPEST_CLAW_COLOR});
        }
    }

    // Flippers climbing their lanes toward the near rim.
    for (uint32_t i = 0; i < TEMPEST_FLIPPERS; i++)
    {
        const int32_t depth =
            FLIPPER_DEPTH_BASE[i] + static_cast<int32_t>(p.frame_index) * FLIPPER_DEPTH_STEP;
        const uint32_t lane = FLIPPER_LANE[i];
        const int32_t  ox   = tempest_depth_x(lane, depth);
        const int32_t  oy   = tempest_depth_y(lane, depth);
        if (static_cast<int32_t>(line) >= oy &&
            static_cast<int32_t>(line) < oy + static_cast<int32_t>(TEMPEST_FLIPPER_ROWS))
        {
            const int32_t row = static_cast<int32_t>(line) - oy;
            insert_span(out, n, {ox - 1 + row, TEMPEST_FLIPPER_WIDTH, TEMPEST_FLIPPER_COLOR});
        }
    }

    // Shots travelling outward.
    for (uint32_t i = 0; i < TEMPEST_SHOTS; i++)
    {
        const int32_t depth =
            SHOT_DEPTH_BASE[i] + static_cast<int32_t>(p.frame_index) * SHOT_DEPTH_STEP;
        const uint32_t lane = SHOT_LANE[i];
        const int32_t  ox   = tempest_depth_x(lane, depth);
        const int32_t  oy   = tempest_depth_y(lane, depth);
        if (static_cast<int32_t>(line) >= oy &&
            static_cast<int32_t>(line) < oy + static_cast<int32_t>(TEMPEST_SHOT_ROWS))
        {
            insert_span(out, n, {ox, TEMPEST_SHOT_WIDTH, TEMPEST_SHOT_COLOR});
        }
    }

    // Density stress, off by default: N one-pixel spans at a two-pixel pitch,
    // i.e. span/gap/span/gap.  This is the densest arrangement the format can
    // express — every record is one word and every playback is one slot — so it
    // is where the object budget actually gets measured.
    if (p.tempest_stress_spans > 0 && line >= TEMPEST_STRESS_ROW &&
        line < TEMPEST_STRESS_ROW + TEMPEST_STRESS_ROWS)
    {
        uint32_t want = p.tempest_stress_spans;
        if (want > TEMPEST_MAX_STRESS_SPANS)
        {
            want = TEMPEST_MAX_STRESS_SPANS;
        }
        const uint32_t pitch = H_ACTIVE / (want + 1);
        for (uint32_t i = 0; i < want; i++)
        {
            insert_span(out, n, {static_cast<int32_t>(pitch * (i + 1)), 1, TEMPEST_STRESS_COLOR});
        }
    }

    return n;
}

}  // namespace

// ---------------------------------------------------------------------------
// Pixel payloads
// ---------------------------------------------------------------------------

void write_test_pattern_1bpp(Memory ram, uint32_t base, uint32_t stride_bytes,
                             uint32_t words_per_line, uint32_t lines)
{
    for (uint32_t y = 0; y < lines; y++)
    {
        const uint32_t row = base + y * stride_bytes;
        for (uint32_t w = 0; w < words_per_line; w++)
        {
            uint16_t bits = 0;
            for (uint32_t b = 0; b < 16; b++)
            {
                const uint32_t x = w * 16 + b;

                // 16-pixel diagonal bands: horizontal scroll slides them left,
                // vertical scroll slides them up, and the two are visually
                // distinguishable because the bands are diagonal.
                bool on = (((x + y) / 16u) & 1u) != 0;

                // A 2-line marker bar every 32 lines pins down the vertical
                // scroll offset exactly, for the programmatic frame compare.
                if ((y % 32u) < 2u)
                {
                    on = (x % 8u) < 4u;
                }

                if (on)
                {
                    bits = static_cast<uint16_t>(bits | (0x8000u >> b));
                }
            }
            ram[row + w * 2] = bits;
        }
    }
}

void write_solid_pattern_1bpp(Memory ram, uint32_t base, uint32_t stride_bytes,
                              uint32_t words_per_line, uint32_t lines)
{
    for (uint32_t y = 0; y < lines; y++)
    {
        for (uint32_t w = 0; w < words_per_line; w++)
        {
            ram[base + y * stride_bytes + w * 2] = 0xFFFF;
        }
    }
}

void write_test_pattern_microham(Memory ram, uint32_t base, uint32_t stride_bytes,
                                 uint32_t lines)
{
    for (uint32_t y = 0; y < lines; y++)
    {
        BitPacker bp;
        bp.ram  = ram;
        bp.base = base + y * stride_bytes;

        // 160 pixels of "held <- pix_pal_fg", one 2-bit code each.
        for (uint32_t i = 0; i < 160; i++)
        {
            bp.put(0b01, HAM_CODE_PALETTE_BITS);
        }

        // 160 pixels of "held <- pix_pal_bg".
        for (uint32_t i = 0; i < 160; i++)
        {
            bp.put(0b00, HAM_CODE_PALETTE_BITS);
        }

        // 320 pixels as 160 four-bit chroma codes.  Alternating between the
        // green/red and green/blue forms means every channel gets driven, and
        // stepping the phase every 8 lines makes the blocks visibly staircase
        // down the frame.
        for (uint32_t k = 0; k < 160; k++)
        {
            const uint32_t kk = k + (y >> 3);
            const uint32_t g  = (kk >> 1) & 1u;
            if ((kk & 1u) == 0)
            {
                const uint32_t r = (kk >> 2) & 1u;
                bp.put(0b1000u | (g << 1) | r, HAM_CODE_CHROMA_BITS);   // 10_g_r
            }
            else
            {
                const uint32_t b = (kk >> 3) & 1u;
                bp.put(0b1100u | (g << 1) | b, HAM_CODE_CHROMA_BITS);   // 11_g_b
            }
        }
    }
}

void write_audio_source(Memory ram, uint32_t base, uint32_t pairs)
{
    // ~437 Hz sawtooth left / ~218 Hz triangle right at 15734.375 Hz.  Integer
    // only: no <cmath> (the target toolchain would pull in libm) and no
    // rounding differences between hosts.  Audio samples are unaffected by the
    // move to 12-bit colour — they are still 8-bit unsigned per channel.
    constexpr uint32_t SAW_PERIOD = 36;   // 15734 / 36 = 437 Hz
    constexpr uint32_t TRI_PERIOD = 72;

    for (uint32_t i = 0; i < pairs; i++)
    {
        const uint32_t saw_phase = i % SAW_PERIOD;
        const uint8_t  left = static_cast<uint8_t>((saw_phase * 256u) / SAW_PERIOD);

        const uint32_t tri_phase = i % TRI_PERIOD;
        const uint32_t up = (tri_phase < TRI_PERIOD / 2) ? tri_phase : (TRI_PERIOD - 1 - tri_phase);
        const uint8_t  right = static_cast<uint8_t>((up * 512u) / TRI_PERIOD);

        ram[base + i * 2] = audio_pair_word(left, right);
    }
}

uint32_t pixel_words_for(PixelMode mode, uint32_t pixel_skip)
{
    const uint32_t m = (mode == PixelMode::MICRO_HAM) ? PIXEL_MODE_MICRO_HAM
                                                      : PIXEL_MODE_DIRECT_1BPP;
    return pixels_words_per_line(m, pixel_skip);
}

// ---------------------------------------------------------------------------
// VIDCMD records
// ---------------------------------------------------------------------------

uint32_t write_vidcmd_records(const FrameParams &p, Memory ram,
                              std::span<uint8_t> line_words,
                              std::span<uint8_t> line_records,
                              std::span<uint16_t> line_slots)
{
    // Four sprites at the plan's worst case, spaced so no two tiles touch and
    // the trailing run is nonzero.
    constexpr std::array<uint32_t, 4> SPRITE_X = {64, 200, 360, 500};

    const uint32_t pixel_skip = p.h_scroll_pixels % 16u;
    const uint32_t mode_value = (p.mode == PixelMode::MICRO_HAM) ? PIXEL_MODE_MICRO_HAM
                                                                 : PIXEL_MODE_DIRECT_1BPP;

    uint32_t total = 0;

    for (uint32_t line = 0; line < V_ACTIVE; line++)
    {
        const uint32_t rec_base = p.vidcmd_base + line * VIDCMD_LINE_STRIDE_BYTES;
        uint32_t n       = 0;
        uint32_t records = 0;
        uint32_t slots   = 0;

        auto put = [&](uint16_t v)
        {
            ram[rec_base + n * 2] = v;
            n++;
        };

        // Lead words start a record; a tile's two mask words do not.
        auto put_record = [&](uint16_t v)
        {
            put(v);
            records++;
        };

        // --- blank-region SETs -----------------------------------------------
        // Everything emitted before the line's first RUN/TILE is consumed while
        // the beam is in horizontal blanking, where the fetch machine pops as
        // fast as the FIFO allows and a SET commits immediately at zero slot
        // cost.  That is the cheap place to put per-line configuration; only a
        // SET that has to land on a *particular pixel* belongs in the run of
        // active records below.
        if (p.slot_regression)
        {
            put_record(vidcmd_set(SET_PIX_PAL_FG, p.regression_c1));
            put_record(vidcmd_set(SET_PIX_PAL_BG, RGB444_BLACK));
            put_record(vidcmd_set(SET_PIX_MODE, PIXEL_MODE_DIRECT_1BPP));
            put_record(vidcmd_set(SET_PIX_PIXEL_SKIP, 0));
        }
        else
        {
            // Held colours are reset to 0xFFF/0x000 by /RS at vsync, so every
            // frame has to re-establish them before the first tile.
            if (p.tiles != TileStyle::NONE && line == 0)
            {
                put_record(vidcmd_set(SET_CMP_HELD_FG, p.held_fg));
                put_record(vidcmd_set(SET_CMP_HELD_BG, p.held_bg));
            }
            if (p.per_line_palette)
            {
                const Rgb444 fg = p.depth_fade_palette ? tempest_palette_fg(line)
                                                       : line_palette_fg(line);
                const Rgb444 bg = p.depth_fade_palette ? tempest_palette_bg(line)
                                                       : line_palette_bg(line);
                put_record(vidcmd_set(SET_PIX_PAL_FG, fg));
                put_record(vidcmd_set(SET_PIX_PAL_BG, bg));
            }
            if (p.per_line_mode)
            {
                put_record(vidcmd_set(SET_PIX_MODE, mode_value));
                put_record(vidcmd_set(SET_PIX_PIXEL_SKIP, pixel_skip));
            }
        }

        // --- active-slot records: must total exactly H_ACTIVE -----------------
        if (p.slot_regression)
        {
            // The normative case, written literally as specified.  Slot 0 is
            // the RUN, slot 1 is the SET (which commits on the edge that begins
            // it, so that pixel already shows C2), slots 2..639 are the tail
            // RUN.  1 + 1 + 638 = 640.
            put_record(vidcmd_run(RUN_SRC_PASSTHROUGH, 1));
            slots += 1;
            put_record(vidcmd_set(SET_PIX_PAL_FG, p.regression_c2));
            slots += 1;
            put_record(vidcmd_run(RUN_SRC_PASSTHROUGH, H_ACTIVE - 2));
            slots += H_ACTIVE - 2;
        }
        else if (p.mid_line_split)
        {
            // Place the SETs `skew_pix` slots early so they *take effect* at
            // p.split_pixel.  Each SET costs one slot, so a two-register change
            // is inherently two pixels wide: the background lands at
            // split_pixel and the foreground one pixel later.
            const uint32_t skew  = p.skew_pix;
            const uint32_t lead  = (p.split_pixel > skew) ? (p.split_pixel - skew) : 0u;
            if (lead > 0)
            {
                put_record(vidcmd_run(RUN_SRC_PASSTHROUGH, lead));
                slots += lead;
            }
            put_record(vidcmd_set(SET_PIX_PAL_BG, p.split_bg));
            slots += 1;
            put_record(vidcmd_set(SET_PIX_PAL_FG, p.split_fg));
            slots += 1;
            put_record(vidcmd_run(RUN_SRC_PASSTHROUGH, H_ACTIVE - slots));
            slots = H_ACTIVE;
        }
        else if (p.tiles == TileStyle::CURSOR)
        {
            if (line >= p.cursor_y && line < p.cursor_y + 16)
            {
                const uint32_t row = line - p.cursor_y;
                put_record(vidcmd_tile(p.cursor_x, 0));
                put(erode_horizontal(CURSOR_MASK[row]));
                put(CURSOR_MASK[row]);
                slots += p.cursor_x + VIDCMD_TILE_PIXELS;
            }
            if (slots < H_ACTIVE)
            {
                put_record(vidcmd_run(RUN_SRC_PASSTHROUGH, H_ACTIVE - slots));
                slots = H_ACTIVE;
            }
        }
        else if (p.tiles == TileStyle::FOUR_SPRITES)
        {
            const uint32_t row = line % 16u;
            uint32_t x = 0;
            for (uint32_t s = 0; s < SPRITE_X.size(); s++)
            {
                put_record(vidcmd_tile(SPRITE_X[s] - x, 0));
                put(sprite_select(row));
                put(sprite_mask(row));
                slots += (SPRITE_X[s] - x) + VIDCMD_TILE_PIXELS;
                x = SPRITE_X[s] + VIDCMD_TILE_PIXELS;
            }
            if (slots < H_ACTIVE)
            {
                put_record(vidcmd_run(RUN_SRC_PASSTHROUGH, H_ACTIVE - slots));
                slots = H_ACTIVE;
            }
        }
        else if (p.tempest_objects)
        {
            // Moving objects as coloured spans over the static wireframe.  Each
            // span is one RUN_COLOR word; the gaps between them are ordinary
            // passthrough RUNs, so the wireframe and its depth-faded palette
            // show through untouched.  Nothing here writes a pixel.
            std::array<ColorSpan, TEMPEST_MAX_SPANS> spans{};
            const uint32_t count = tempest_spans_for_line(p, line, spans);

            int32_t x = 0;
            for (uint32_t i = 0; i < count; i++)
            {
                if (spans[i].x > x)
                {
                    put_record(vidcmd_run(RUN_SRC_PASSTHROUGH,
                                          static_cast<uint32_t>(spans[i].x - x)));
                    slots += static_cast<uint32_t>(spans[i].x - x);
                }
                put_record(vidcmd_run_color(spans[i].color, static_cast<uint32_t>(spans[i].w)));
                slots += static_cast<uint32_t>(spans[i].w);
                x = spans[i].x + spans[i].w;
            }
            if (slots < H_ACTIVE)
            {
                put_record(vidcmd_run(RUN_SRC_PASSTHROUGH, H_ACTIVE - slots));
                slots = H_ACTIVE;
            }
        }
        else
        {
            // The floor: with no drain-to-N and no trailing-run-repeat, even a
            // line that wants nothing from the compositor must still spend one
            // word saying so.
            put_record(vidcmd_run(RUN_SRC_PASSTHROUGH, H_ACTIVE));
            slots += H_ACTIVE;
        }

        line_words[line]   = static_cast<uint8_t>(n);
        line_records[line] = static_cast<uint8_t>(records);
        line_slots[line]   = static_cast<uint16_t>(slots);
        total += n;
    }

    return total;
}

// ---------------------------------------------------------------------------
// The display list
// ---------------------------------------------------------------------------

AuthorResult author_frame(const FrameParams &p, Memory ram,
                          std::span<const uint8_t> vidcmd_line_words)
{
    ListWriter w;
    w.ram    = ram;
    w.base   = p.table_base;
    w.limit  = p.table_base + p.table_bytes;
    w.cursor = p.table_base;

    // The source address advances in whole 16-bit words, so the coarse part of
    // a horizontal scroll is a word offset and the fine part is pixel_skip.
    // Now that pixel_skip is a 12-bit SET value rather than a 3-bit MODE field,
    // 0..15 is fully representable and every pixel offset is reachable.
    const uint32_t word_offset = p.h_scroll_pixels / 16u;
    uint32_t pixel_skip = p.h_scroll_pixels % 16u;
    if (pixel_skip > PIXELS_SKIP_MAX)
    {
        pixel_skip = PIXELS_SKIP_MAX;
    }

    const uint32_t line_words  = pixel_words_for(p.mode, pixel_skip);
    const uint32_t pixel_chunk = pixel_chunk_for(line_words);

    uint32_t audio_pair = p.audio_frame_pair_base;
    uint32_t audio_pairs_emitted = 0;

    // --- VBLANK preamble -----------------------------------------------------
    // The list is armed during vertical blanking, so this runs immediately with
    // no HBLANK wait.  PORTS keeps popping audio through the 45 blanked lines,
    // and no per-line group exists to feed it there, so the preamble carries
    // vblank's share: 45 lines / 2 = 22.5 pairs, rounded up to 23.
    if (p.audio && p.audio_preamble_pairs > 0)
    {
        emit_stream(w, SIGNAL_AUDIO_FIFO_W, p.audio_base + audio_pair * 2,
                    p.audio_preamble_pairs, false, DESC_MAX_COUNT);
        audio_pair += p.audio_preamble_pairs;
        audio_pairs_emitted += p.audio_preamble_pairs;
    }

    // --- walk to the top of frame -------------------------------------------
    for (uint32_t i = 0; i < p.vblank_pacing_lines; i++)
    {
        Descriptor d;
        d.src         = p.fb_base;
        d.count       = 1;
        d.signal_mask = SIGNAL_NONE;
        d.wait_hblank = true;
        w.emit(d);
    }

    // --- one group per visible line ------------------------------------------
    for (uint32_t line = 0; line < V_ACTIVE; line++)
    {
        // The group's first descriptor waits for the HBLANK edge of the line
        // *before* the one it feeds, so everything below lands in the 160
        // pixel-clock gap (or spills a little into the next line's active
        // video, which the FIFOs absorb).
        bool wait = true;

        const uint32_t vidcmd_words =
            (line < vidcmd_line_words.size()) ? vidcmd_line_words[line] : 0u;
        const uint32_t vidcmd_addr = p.vidcmd_base + line * VIDCMD_LINE_STRIDE_BYTES;

        // How much of the line's VIDCMD stream goes ahead of the pixels.
        uint32_t head = vidcmd_words;
        if (p.vidcmd_after_pixels)
        {
            head = 0;
        }
        else if (p.vidcmd_head_words > 0 && p.vidcmd_head_words < vidcmd_words)
        {
            head = p.vidcmd_head_words;
        }

        const uint32_t src = p.fb_base + (p.v_scroll_lines + line) * p.fb_stride_bytes +
                             word_offset * 2;

        // Optional pixel head, ahead of everything: cheap immunity for PIXEL.
        uint32_t pix_head = 0;
        if (p.pixels_head_words > 0 && p.pixels_head_words < line_words)
        {
            pix_head = p.pixels_head_words;
            emit_stream(w, SIGNAL_PIXELS_FIFO_W, src, pix_head, wait, pixel_chunk);
            wait = false;
        }

        if (head > 0)
        {
            emit_stream(w, SIGNAL_VIDCMD_FIFO_W, vidcmd_addr, head, wait, DESC_MAX_COUNT);
            wait = false;
        }

        emit_stream(w, SIGNAL_PIXELS_FIFO_W, src + pix_head * 2, line_words - pix_head, wait,
                    pixel_chunk);
        wait = false;

        if (vidcmd_words > head)
        {
            emit_stream(w, SIGNAL_VIDCMD_FIFO_W, vidcmd_addr + head * 2, vidcmd_words - head,
                        false, DESC_MAX_COUNT);
        }

        // Audio last in the group: the audio FIFO is 1024 pairs deep and is
        // being drained at one pair per two lines, so it is the one consumer
        // with enough slack to be scheduled behind everything else.
        if (p.audio && (line % p.audio_burst_interval) == 0)
        {
            emit_stream(w, SIGNAL_AUDIO_FIFO_W, p.audio_base + audio_pair * 2,
                        p.audio_burst_pairs, false, DESC_MAX_COUNT);
            audio_pair += p.audio_burst_pairs;
            audio_pairs_emitted += p.audio_burst_pairs;
        }
    }

    // The list ends on a wait_hblank/mask-0/stop_after descriptor rather than
    // by setting stop_after on display line 479's last deposit.  That one extra
    // descriptor pins nENGINE_IRQ to a fixed point — just after line 479's
    // HBLANK edge — instead of letting it drift with however long the last
    // line's group happened to take, which would make the ISR's re-arm deadline
    // depend on how heavy the frame was.
    {
        Descriptor d;
        d.src         = p.fb_base;
        d.count       = 1;
        d.signal_mask = SIGNAL_NONE;
        d.wait_hblank = true;
        d.stop_after  = true;
        w.emit(d);
    }

    AuthorResult r;
    r.first_descriptor = p.table_base;
    r.descriptor_count = w.count;
    r.table_bytes      = w.cursor - w.base;
    r.payload_words    = w.payload_words;
    r.pacing_words     = w.pacing_words;
    r.audio_pairs      = audio_pairs_emitted;
    r.table_overflow   = w.overflow;
    return r;
}

}  // namespace SuperEngine
