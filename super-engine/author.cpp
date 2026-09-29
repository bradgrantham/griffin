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
    if (d.destination == DEST_NONE)
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
        d.destination = mask;
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
// Sprite art
// ---------------------------------------------------------------------------
//
// With TILE gone the compositor has no mask shifters, so this is authoring-side
// art that gets decomposed into RUN spans below rather than a bitmap the
// hardware walks.
//
// A 16x16 arrow.  MSB of each word is the leftmost pixel.
constexpr std::array<uint16_t, 16> CURSOR_MASK = {
    0x8000, 0xC000, 0xE000, 0xF000,
    0xF800, 0xFC00, 0xFE00, 0xFF00,
    0xFF80, 0xFFC0, 0xFE00, 0xEE00,
    0xCF00, 0x0700, 0x0780, 0x0380,
};

// Eroding the mask by one pixel horizontally leaves a held_bg outline around a
// held_fg interior — the classic always-legible cursor, and it now costs a
// couple of extra runs rather than a second 16-bit mask word.
constexpr uint16_t erode_horizontal(uint16_t m)
{
    return static_cast<uint16_t>(m & static_cast<uint16_t>(m << 1) & static_cast<uint16_t>(m >> 1));
}

// A 16x16 diamond for the 4-sprites-per-line worst case.  Computed rather than
// tabulated so the shape is obviously symmetric, and contiguous by construction
// so each sprite row decomposes into exactly one run.
constexpr uint16_t sprite_mask(uint32_t row)
{
    const uint32_t half  = (row < 8) ? row : (15 - row);
    const uint32_t width = 2 * (half + 1);
    const uint32_t shift = (16 - width) / 2;
    return static_cast<uint16_t>((((1u << width) - 1u) << shift) & 0xFFFFu);
}

// Horizontal fg/bg banding inside the sprite: unmistakable in a PPM, and it
// exercises both held colours on every sprite line.
constexpr uint32_t sprite_row_src(uint32_t row)
{
    return (row & 1u) ? RUN_SRC_HELD_BG : RUN_SRC_HELD_FG;
}

// ---------------------------------------------------------------------------
// Tempest-web moving objects
// ---------------------------------------------------------------------------
//
// This is the per-frame CPU authoring path in miniature: object positions in,
// a sorted non-overlapping list of coloured spans for one scanline out.  It
// allocates nothing and touches no bitmap — moving an object is re-running
// this, which is the whole point of the hybrid architecture.

// One painted span on a line.  `src` is a RUN source select, so a span is
// either a held colour (restylable by a later SET without touching the list) or
// a RUN_COLOR literal (atomic, one word, immune to held-colour changes).  Every
// sprite, cursor and game object in the suite is now a list of these.
struct ColorSpan
{
    int32_t  x     = 0;
    int32_t  w     = 0;
    uint32_t src   = RUN_SRC_COLOR;
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
            insert_span(out, n, {cx - 11 + row, TEMPEST_CLAW_WIDTH, RUN_SRC_COLOR, TEMPEST_CLAW_COLOR});
            insert_span(out, n, {cx + 8 - row, TEMPEST_CLAW_WIDTH, RUN_SRC_COLOR, TEMPEST_CLAW_COLOR});
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
            insert_span(out, n, {ox - 1 + row, TEMPEST_FLIPPER_WIDTH, RUN_SRC_COLOR, TEMPEST_FLIPPER_COLOR});
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
            insert_span(out, n, {ox, TEMPEST_SHOT_WIDTH, RUN_SRC_COLOR, TEMPEST_SHOT_COLOR});
        }
    }

    // Density stress, off by default: N one-pixel spans on a band of rows,
    // SPREAD evenly across the line.  That measures DELIVERY, which is what
    // this sweep is for; the compositor's own fetch cadence is a separate
    // ceiling and the spread pitch stays well clear of it (see author.h).
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
            insert_span(out, n, {static_cast<int32_t>(pitch * (i + 1)), 1, RUN_SRC_COLOR,
                                 TEMPEST_STRESS_COLOR});
        }
    }

    return n;
}

// ---------------------------------------------------------------------------
// Pure-VIDCMD screens
// ---------------------------------------------------------------------------
//
// PASSTHROUGH IS THE THIRD COLOUR — MODEL BEHAVIOUR, NOT YET HARDWARE.  These
// screens author no PIXELS descriptors at all, so the PIXELS FIFO is never
// written and PIXEL's shifter re-delivers its reset word forever.  That word is
// all zeroes, and a zero bit in 1bpp direct mode selects pix_pal_bg, so
// RGB_IN — and therefore a dibit-00 pixel and a passthrough RUN — resolves to
// pal_bg, a register a VIDCMD SET can write.  Dibit 00 becomes a THIRD settable
// colour per record, free of any SET between groups.
//
// That chain is asserted here because it is what super-engine/render.cpp
// models, and render.cpp models pixel.v's documented behaviour (no empty flag,
// a 7200 holds Q while empty, so an unfed stream tiles its last word).  It has
// NOT been confirmed against the RTL or against silicon: what a 7200 pair
// presents on Q after a /RS with no write at all, and what pixel.v's shifter
// holds before its first fetch, are the two questions to settle.  Until then
// the third colour is a MODEL FINDING.

// The character at `i` of a NUL-terminated line, blank past its end.
char text_char(const char *s, uint32_t i)
{
    for (uint32_t k = 0; k <= i; k++)
    {
        if (s[k] == '\0')
        {
            return ' ';
        }
        if (k == i)
        {
            return s[k];
        }
    }
    return ' ';
}

constexpr const char *CONSOLE_TITLE =
    "GRIFFIN VIDCMD CONSOLE - 80 COLUMNS BY 60 ROWS - THE DISPLAY LIST IS THE SCREEN";
constexpr const char *CONSOLE_STATUS =
    "STATUS: 40 MASK RECORDS PER FULL LINE = 80 WORDS + 2 SETS - PIXELS WORDS: 0";

constexpr std::array<const char *, 12> CONSOLE_PHRASES = {
    "GRIFFIN 68000 - VIDCMD PURE DISPLAY LIST CONSOLE",
    "EVERY GLYPH PAIR IS ONE 16 PIXEL MASK RECORD",
    "PIXEL 0 OF A RECORD IS IMPLICIT OPAQUE COLOR0",
    "THE 8 PIXEL CELL LEAVES COLUMN 0 BLANK ON PURPOSE",
    "MASK TO MASK CHAINING COSTS ZERO SLOTS",
    "RECOLOURING BETWEEN TWO MASKS COSTS TWO SLOTS",
    "BLANK CELLS COLLAPSE INTO ONE BACKGROUND RUN",
    "NO PIXELS DESCRIPTORS ARE AUTHORED AT ALL",
    "PASSTHROUGH RESOLVES TO PAL BG IN THIS MODEL",
    "SO DIBIT 00 IS A THIRD SETTABLE COLOUR HERE",
    "PENDING HARDWARE VERIFICATION AGAINST PIXEL V",
    "0123456789 .,:-/>()!*+= ABCDEFGHIJKLMNOPQRSTUVWXYZ",
};

// --- the kiosk's fixed layout ----------------------------------------------

constexpr uint32_t KIOSK_TITLE_Y = 48;
constexpr uint32_t KIOSK_TITLE_X = 216;    // 13 glyphs x 16 = 208, centred
constexpr uint32_t KIOSK_ITEM_X  = 128;
constexpr uint32_t KIOSK_ITEMS   = 4;
constexpr std::array<uint32_t, KIOSK_ITEMS> KIOSK_ITEM_Y = {144, 208, 272, 336};
constexpr uint32_t KIOSK_HIGHLIGHT_ITEM = 2;

// One record per glyph, so a segment's width in pixels is records * 16.
uint32_t kiosk_text_records(const char *s)
{
    uint32_t n = 0;
    while (s[n] != '\0' && n < KIOSK_MAX_TEXT)
    {
        n++;
    }
    return n;
}

void kiosk_fill_segment(KioskSegment &seg, uint32_t x0, const char *text,
                        Rgb444 color1, Rgb444 color0)
{
    seg.x0      = x0;
    seg.records = kiosk_text_records(text);
    seg.color1  = color1;
    seg.color0  = color0;
    for (uint32_t i = 0; i < seg.records; i++)
    {
        seg.text[i] = text[i];
    }
    // The hotkey: the item's first glyph is drawn in PASSTHROUGH, so a kiosk
    // line shows three colours with no SET between the groups that carry them.
    seg.alt[0] = 1;
}

// One MASK record's worth of a glyph row, as a dibit array.  x is the record's
// pixel 0 on the line.
VidcmdMask mask_from_classes(std::span<const InkClass> cls, uint32_t x)
{
    std::array<uint32_t, MASK_SLOTS> d{};
    for (uint32_t i = 0; i < MASK_SLOTS; i++)
    {
        d[i] = ink_class_dibit(cls[x + i]);
    }
    return vidcmd_mask(d);
}

bool group_is_background(std::span<const InkClass> cls, uint32_t x)
{
    for (uint32_t i = 0; i < MASK_SLOTS; i++)
    {
        if (cls[x + i] != InkClass::BG)
        {
            return false;
        }
    }
    return true;
}

// Writes one line's records, counting words and records separately because a
// MASK is two words and one record.
struct RecordWriter
{
    Memory   ram;
    uint32_t base    = 0;
    uint32_t words   = 0;
    uint32_t records = 0;

    void word(uint16_t v)
    {
        ram[base + words * 2] = v;
        words++;
    }

    void record(uint16_t v)
    {
        word(v);
        records++;
    }

    void mask(const VidcmdMask &m)
    {
        record(m.header);
        word(m.data);
    }

    // One palette entry: the index SET, the data SET and the same-value FILLER.
    // Three records, and the filler is not optional — COMPOSITOR re-registers
    // set_pix_value from every staged word, so without it the RAM stores the
    // next word's payload (vidcmd-spec.md §3.2).  Legal in blanking, or where
    // the four slots after the data SET are covered.
    void palette_entry(uint32_t index, Rgb444 colour)
    {
        const PaletteEntryWords e = vidcmd_palette_entry(index, colour);
        for (uint16_t v : e)
        {
            record(v);
        }
    }

    // A RUN_COLOR spans at most RUN_COLOR_MAX_COUNT pixels.  A chunked span is
    // still exact: the first chunk is long enough for the fetch to bank the
    // second, so the pair law lands it on the very next slot.
    void run_color_span(uint32_t colour, uint32_t n)
    {
        while (n > 0)
        {
            const uint32_t chunk = (n > RUN_COLOR_MAX_COUNT) ? RUN_COLOR_MAX_COUNT : n;
            record(vidcmd_run_color(colour, chunk));
            n -= chunk;
        }
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// The console screen
// ---------------------------------------------------------------------------

char console_char(uint32_t row, uint32_t col)
{
    if (col >= CONSOLE_COLS || row >= CONSOLE_ROWS)
    {
        return ' ';
    }
    if (row == 0)
    {
        return text_char(CONSOLE_TITLE, col);
    }
    if (row == CONSOLE_ROWS - 1)
    {
        return text_char(CONSOLE_STATUS, col);
    }
    // "NN> " and then the row's phrase.
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
    return text_char(CONSOLE_PHRASES[(row - 1) % CONSOLE_PHRASES.size()], col - 4);
}

InkClass console_ink_class(uint32_t row, uint32_t col)
{
    // The title, the status line and every row's "NN>" prefix are drawn in the
    // third colour — dibit 00, passthrough — so they cost no extra SET and no
    // extra record.  Everything else is cmp_color1.
    if (row == 0 || row == CONSOLE_ROWS - 1 || col < 3)
    {
        return InkClass::ALT;
    }
    return InkClass::INK;
}

void console_line_classes(uint32_t line, std::span<InkClass> out)
{
    for (uint32_t x = 0; x < H_ACTIVE; x++)
    {
        out[x] = InkClass::BG;
    }

    const uint32_t row       = line / CONSOLE_CELL_H;
    const uint32_t glyph_row = line % CONSOLE_CELL_H;

    for (uint32_t col = 0; col < CONSOLE_COLS; col++)
    {
        const char c = console_char(row, col);
        if (c == ' ')
        {
            continue;
        }
        const InkClass ink = console_ink_class(row, col);
        for (uint32_t fc = 0; fc < FONT_COLS; fc++)
        {
            if (font_pixel(c, fc, glyph_row))
            {
                // Column 0 of the cell is the inter-character gap, which is
                // what keeps every record's implicit pixel 0 on background.
                out[col * CONSOLE_CELL_W + 1 + fc] = ink;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// The kiosk screen
// ---------------------------------------------------------------------------

KioskLinePlan kiosk_plan_line(uint32_t line)
{
    KioskLinePlan plan;

    const uint32_t hy = KIOSK_ITEM_Y[KIOSK_HIGHLIGHT_ITEM];
    const bool     highlighted = (line >= hy && line < hy + KIOSK_CELL_H);
    plan.background_color = highlighted ? RUN_COLOR_CYAN : RUN_COLOR_BLUE;

    const Rgb444 bg = run_colour_to_rgb444(plan.background_color);

    if (line >= KIOSK_TITLE_Y && line < KIOSK_TITLE_Y + KIOSK_CELL_H)
    {
        plan.cell_top = KIOSK_TITLE_Y;
        kiosk_fill_segment(plan.seg[0], KIOSK_TITLE_X, "GRIFFIN KIOSK",
                           rgb444(15, 15, 0), bg);
        plan.seg_n = 1;
        return plan;
    }

    for (uint32_t i = 0; i < KIOSK_ITEMS; i++)
    {
        if (line < KIOSK_ITEM_Y[i] || line >= KIOSK_ITEM_Y[i] + KIOSK_CELL_H)
        {
            continue;
        }
        plan.cell_top = KIOSK_ITEM_Y[i];
        if (i == 0)
        {
            // The mid-item recolour, and the reason this case exists: the SET
            // pair sits BETWEEN two mask groups, so it costs
            // MASK_GAP_AFTER_MASK_SET_SET slots and the second word's glyphs
            // start that many pixels later.  The geometry below is that
            // constant; if the seam were any other number the rendered image
            // would not match the reference.
            // The trailing space is a real MASK record whose sixteen pixels are
            // all background — legal, and it keeps the two words readable
            // either side of the seam.
            kiosk_fill_segment(plan.seg[0], KIOSK_ITEM_X, "PLAY ",
                               rgb444(15, 15, 15), bg);
            const uint32_t after = plan.seg[0].x0 + plan.seg[0].records * KIOSK_CELL_W;
            kiosk_fill_segment(plan.seg[1], after + MASK_GAP_AFTER_MASK_SET_SET, "DEMO",
                               rgb444(0, 15, 15), bg);
            plan.seg_n = 2;
        }
        else if (i == 1)
        {
            kiosk_fill_segment(plan.seg[0], KIOSK_ITEM_X, "SETTINGS",
                               rgb444(15, 12, 4), bg);
            plan.seg_n = 1;
        }
        else if (i == 2)
        {
            kiosk_fill_segment(plan.seg[0], KIOSK_ITEM_X, "DIAGNOSTICS",
                               rgb444(0, 0, 0), bg);
            plan.seg_n = 1;
        }
        else
        {
            kiosk_fill_segment(plan.seg[0], KIOSK_ITEM_X, "ABOUT",
                               rgb444(4, 15, 4), bg);
            plan.seg_n = 1;
        }
        break;
    }

    return plan;
}

void kiosk_line_classes(const KioskLinePlan &plan, uint32_t line, std::span<InkClass> out)
{
    for (uint32_t x = 0; x < H_ACTIVE; x++)
    {
        out[x] = InkClass::BG;
    }

    const uint32_t cell_row = line - plan.cell_top;

    for (uint32_t s = 0; s < plan.seg_n; s++)
    {
        const KioskSegment &seg = plan.seg[s];

        for (uint32_t g = 0; g < seg.records; g++)
        {
            const InkClass ink = (seg.alt[g] != 0) ? InkClass::ALT : InkClass::INK;
            for (uint32_t px = 1; px < KIOSK_CELL_W; px++)
            {
                // x3 horizontally into columns 1..15, x4 vertically into rows
                // 0..27; column 0 and rows 28..31 are the cell's blank margin,
                // which is what keeps the implicit pixel 0 on background.
                if (font_pixel(seg.text[g], (px - 1) / KIOSK_SCALE_X, cell_row / KIOSK_SCALE_Y))
                {
                    out[seg.x0 + g * KIOSK_CELL_W + px] = ink;
                }
            }
        }
    }
}

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

void write_test_pattern_indexed2(Memory ram, uint32_t base, uint32_t stride_bytes,
                                 uint32_t lines)
{
    for (uint32_t y = 0; y < lines; y++)
    {
        BitPacker bp;
        bp.ram  = ram;
        bp.base = base + y * stride_bytes;

        for (uint32_t x = 0; x < H_ACTIVE; x++)
        {
            bp.put(idx2_pattern_code(x, y), 2);
        }
    }
}

void write_test_pattern_indexed4_half(Memory ram, uint32_t base, uint32_t stride_bytes,
                                      uint32_t lines)
{
    for (uint32_t y = 0; y < lines; y++)
    {
        BitPacker bp;
        bp.ram  = ram;
        bp.base = base + y * stride_bytes;

        // 320 groups, one nibble each: a half-rate 4bpp line.
        for (uint32_t g = 0; g < H_ACTIVE / 2u; g++)
        {
            bp.put(idx4_pattern_code(g, y), 4);
        }
    }
}

void write_test_pattern_attr(Memory ram, uint32_t base, uint32_t stride_bytes,
                             uint32_t cells_per_line, uint32_t lines)
{
    for (uint32_t y = 0; y < lines; y++)
    {
        for (uint32_t cell = 0; cell < cells_per_line; cell++)
        {
            const uint32_t fg  = attr_cell_fg(cell, y);
            const uint32_t bg  = attr_cell_bg(cell, y);
            const uint32_t pat = attr_cell_pattern(cell, y);
            ram[base + y * stride_bytes + cell * 2] =
                static_cast<uint16_t>((fg << 12) | (bg << 8) | pat);
        }
    }
}

void write_audio_source(Memory ram, uint32_t base, uint32_t pairs)
{
    // ~437 Hz sawtooth left / ~218 Hz triangle right at 15734.375 Hz.  Integer
    // only: no <cmath> (the target toolchain would pull in libm) and no
    // rounding differences between hosts.  Computed as unsigned 8-bit and
    // stored two's complement (bit 7 flipped), as the AUDIO chip takes it.
    constexpr uint32_t SAW_PERIOD = 36;   // 15734 / 36 = 437 Hz
    constexpr uint32_t TRI_PERIOD = 72;

    for (uint32_t i = 0; i < pairs; i++)
    {
        const uint32_t saw_phase = i % SAW_PERIOD;
        const uint8_t  left = static_cast<uint8_t>((saw_phase * 256u) / SAW_PERIOD);

        const uint32_t tri_phase = i % TRI_PERIOD;
        const uint32_t up = (tri_phase < TRI_PERIOD / 2) ? tri_phase : (TRI_PERIOD - 1 - tri_phase);
        const uint8_t  right = static_cast<uint8_t>((up * 512u) / TRI_PERIOD);

        ram[base + i * 2] = audio_byte_word(static_cast<uint8_t>(left ^ 0x80));
        ram[audio_right_base(base, pairs) + i * 2] = audio_byte_word(static_cast<uint8_t>(right ^ 0x80));
    }

    // PERIOD 99 and VOLUME 63 for channels 0 and 2 (registers 0, 2, 4, 6).
    const uint8_t control[AUDIO_CONTROL_WORDS] = { 0, 99, 2, 99, 4, 63, 6, 63 };
    for (uint32_t i = 0; i < AUDIO_CONTROL_WORDS; i++)
    {
        ram[audio_control_base(base, pairs) + i * 2] = audio_byte_word(control[i]);
    }
}

uint32_t pixel_words_for(PixelMode mode, uint32_t pixel_skip)
{
    return pixels_words_per_line(pixel_mode_bits(mode), pixel_skip);
}

// A 16x16 bordered box with a diamond inside.  Column 0 and column 15 and rows
// 0 and 15 are cmp_color0 (the border), the diamond is cmp_color1, and
// everything else is PASSTHROUGH so the picture underneath shows through.
// Column 0 being unconditionally opaque is what makes the record legal at all:
// a MASK's pixel 0 is an implicit opaque cmp_color0 and there is no bit for it.
uint32_t mask_sprite_dibit(uint32_t row, uint32_t col)
{
    if (row == 0 || row == MASK_SPRITE_H - 1 || col == 0 || col == MASK_SLOTS - 1)
    {
        return MASK_DIBIT_COLOR0;
    }
    return ((sprite_mask(row) >> (15u - col)) & 1u) != 0 ? MASK_DIBIT_COLOR1
                                                         : MASK_DIBIT_PASSTHROUGH;
}

// ---------------------------------------------------------------------------
// VIDCMD records
// ---------------------------------------------------------------------------

// One line of a pure-VIDCMD screen.  Returns nothing: the writer carries the
// word and record counts out.
//
// CONSOLE.  Two per-line SETs in the blank region (free, zero slots), then the
// line's forty 16-pixel groups: a MASK for every group that has ink and ONE
// merged RUN for every stretch of groups that has none.  Chained masks are
// gapless and a RUN-to-mask boundary is gapless too, so the authored slot sum
// is exactly 640 by construction — 40 groups of 16 — however the groups split
// between masks and runs.
static void write_console_line(const FrameParams &p, RecordWriter &w, uint32_t line,
                               uint32_t &slots)
{
    const uint32_t row = line / CONSOLE_CELL_H;

    // Blank-region setup.  The third colour is PALETTE ENTRY 0 — with no PIXELS
    // word ever fetched the shifter reads zero, and 1bpp at pix_base 0 makes
    // that entry 0 — and it is written once per FRAME.  /RS clears PIXEL's mode
    // and base registers but NOT the RAM, so one entry per frame is belt and
    // braces rather than a requirement; what /RS really forces is the mode SET.
    if (line == 0)
    {
        w.palette_entry(0, CONSOLE_ALT_COLOR);
        w.record(vidcmd_set(SET_PIX_BASE, 0));
        w.record(vidcmd_set(SET_PIX_MODE, pixel_mode_bits(PixelMode::DIRECT_1BPP)));
    }
    w.record(vidcmd_set(SET_CMP_COLOR1, console_row_color1(row)));
    w.record(vidcmd_set(SET_CMP_COLOR0, console_row_color0(row)));
    (void)p;

    std::array<InkClass, H_ACTIVE> cls{};
    console_line_classes(line, cls);

    constexpr uint32_t GROUPS = H_ACTIVE / MASK_SLOTS;
    uint32_t           g      = 0;
    while (g < GROUPS)
    {
        if (group_is_background(cls, g * MASK_SLOTS))
        {
            uint32_t end = g;
            while (end < GROUPS && group_is_background(cls, end * MASK_SLOTS))
            {
                end++;
            }
            const uint32_t n = (end - g) * MASK_SLOTS;
            w.record(vidcmd_run(RUN_SRC_COLOR0, n));
            slots += n;
            g = end;
        }
        else
        {
            w.mask(mask_from_classes(cls, g * MASK_SLOTS));
            slots += MASK_SLOTS;
            g++;
        }
    }
}

// KIOSK.  A RUN_COLOR background, one big glyph per record, and a SET pair in
// front of every recoloured segment.  The seam the SET pair costs depends on
// what is in front of it and is priced from the derived constants:
//
//   behind a RUN   the fetch banks BOTH SETs, so the pair law lands them on
//                  consecutive slots and the header on the next —
//                  MASK_GAP_AFTER_RUN_SET_SET slots, absorbed by shortening
//                  the background run.
//   behind a MASK  only ONE word can be parked while a mask plays, so the SETs
//                  cannot be a pair: each costs its own slot plus the cadence's
//                  HOLD — MASK_GAP_AFTER_MASK_SET_SET slots, which the art's
//                  geometry has to leave room for.
static void write_kiosk_line(const FrameParams &p, RecordWriter &w, uint32_t line,
                             uint32_t &slots)
{
    if (line == 0)
    {
        // The kiosk's third colour is palette entry 0, for the same reason as
        // the console's (write_console_line).
        w.palette_entry(0, KIOSK_ALT_COLOR);
        w.record(vidcmd_set(SET_PIX_BASE, 0));
        w.record(vidcmd_set(SET_PIX_MODE, pixel_mode_bits(PixelMode::DIRECT_1BPP)));
    }
    (void)p;

    const KioskLinePlan plan = kiosk_plan_line(line);

    std::array<InkClass, H_ACTIVE> cls{};
    kiosk_line_classes(plan, line, cls);

    uint32_t x         = 0;
    bool     after_mask = false;
    for (uint32_t s = 0; s < plan.seg_n; s++)
    {
        const KioskSegment &seg = plan.seg[s];
        const uint32_t      gap = seg.x0 - x;

        // The background run in front of the SET pair, shortened by whatever
        // seam the pair is about to cost.  Zero when the recolour sits directly
        // between two mask groups.
        const uint32_t run = (after_mask && gap == MASK_GAP_AFTER_MASK_SET_SET)
                                 ? 0u
                                 : (gap - MASK_GAP_AFTER_RUN_SET_SET);
        if (run > 0)
        {
            w.run_color_span(plan.background_color, run);
        }
        w.record(vidcmd_set(SET_CMP_COLOR1, seg.color1));
        w.record(vidcmd_set(SET_CMP_COLOR0, seg.color0));
        slots += gap;   // the run plus the pair's own seam slots

        for (uint32_t g = 0; g < seg.records; g++)
        {
            w.mask(mask_from_classes(cls, seg.x0 + g * KIOSK_CELL_W));
            slots += MASK_SLOTS;
        }
        x          = seg.x0 + seg.records * KIOSK_CELL_W;
        after_mask = true;
    }

    if (x < H_ACTIVE)
    {
        w.run_color_span(plan.background_color, H_ACTIVE - x);
        slots += H_ACTIVE - x;
    }
}

static uint32_t write_screen_records(const FrameParams &p, Memory ram,
                                     std::span<uint8_t> line_words,
                                     std::span<uint8_t> line_records,
                                     std::span<uint16_t> line_slots,
                                     std::span<uint16_t> line_stretch)
{
    uint32_t total = 0;

    for (uint32_t line = 0; line < V_ACTIVE; line++)
    {
        RecordWriter w;
        w.ram  = ram;
        w.base = p.vidcmd_base + line * VIDCMD_LINE_STRIDE_BYTES;

        uint32_t slots = 0;
        if (p.screen == ScreenStyle::CONSOLE)
        {
            write_console_line(p, w, line, slots);
        }
        else
        {
            write_kiosk_line(p, w, line, slots);
        }

        // Occupancy, not the authored sum — the same rule every other case is
        // held to.  These screens are built so the two agree (chained masks and
        // RUN-to-mask boundaries are gapless, and the SET seams are priced into
        // the geometry), and the CUSHION check is what proves it.
        const VidcmdSlotPlan plan = vidcmd_plan_line(
            std::span<const uint16_t>(&ram.words[w.base >> 1], w.words), H_BLANK);

        line_words[line]   = static_cast<uint8_t>(w.words);
        line_records[line] = static_cast<uint8_t>(w.records);
        line_slots[line]   = static_cast<uint16_t>(plan.slots);
        line_stretch[line] = static_cast<uint16_t>(plan.stretch);
        total += w.words;
    }

    return total;
}

uint32_t write_vidcmd_records(const FrameParams &p, Memory ram,
                              std::span<uint8_t> line_words,
                              std::span<uint8_t> line_records,
                              std::span<uint16_t> line_slots,
                              std::span<uint16_t> line_stretch)
{
    if (p.screen != ScreenStyle::NONE)
    {
        return write_screen_records(p, ram, line_words, line_records, line_slots,
                                    line_stretch);
    }

    constexpr std::array<uint32_t, 4> SPRITE_X = {64, 200, 360, 500};

    const uint32_t pixel_skip = p.h_scroll_pixels % 16u;
    const uint32_t mode_value = pixel_mode_bits(p.mode);

    uint32_t total = 0;

    for (uint32_t line = 0; line < V_ACTIVE; line++)
    {
        const uint32_t rec_base = p.vidcmd_base + line * VIDCMD_LINE_STRIDE_BYTES;
        uint32_t n       = 0;
        uint32_t records = 0;
        uint32_t slots   = 0;

        auto put_word = [&](uint16_t v)
        {
            ram[rec_base + n * 2] = v;
            n++;
        };

        auto put = [&](uint16_t v)
        {
            put_word(v);
            records++;
        };

        // A MASK is two words and one record.
        auto put_mask = [&](const VidcmdMask &m)
        {
            put(m.header);
            put_word(m.data);
        };

        // What the compositor will ACTUALLY do with the words written so far.
        // The authored sum `slots` below is the lower bound; this is the truth,
        // because the 2-clock fetch cadence buys a HOLD slot behind every
        // record that is not half of a banked pair (descriptor.h,
        // vidcmd_plan_line).
        auto plan_now = [&]()
        {
            return vidcmd_plan_line(
                std::span<const uint16_t>(&ram.words[rec_base >> 1], n), H_BLANK);
        };

        // Close an exact-640 line.  The filler RUN is emitted provisionally and
        // then resized against the slot the fetch engine actually delivers it
        // on, so the cadence's stretch comes out of the filler rather than
        // overrunning into the next line and stealing its eager SETs.  A filler
        // is the LAST record, so its own count cannot move its start slot —
        // one pass is exact, not a fixed point.
        auto close_exact = [&]()
        {
            put(vidcmd_run(RUN_SRC_PASSTHROUGH, 1));
            const VidcmdSlotPlan plan = plan_now();
            if (plan.last_slot < H_ACTIVE)
            {
                ram[rec_base + (n - 1) * 2] =
                    vidcmd_run(RUN_SRC_PASSTHROUGH, H_ACTIVE - plan.last_slot);
            }
            else
            {
                // The list already reaches the end of the line on its own; a
                // filler would only overrun.  (If the records BEFORE it already
                // overran, the checker's slot-sum test is what says so.)
                n--;
                records--;
            }
        };

        // Every span this line paints, sorted and disjoint.
        std::array<ColorSpan, TEMPEST_MAX_SPANS> spans{};
        uint32_t span_n = 0;

        // --- blank-region SETs -----------------------------------------------
        // Everything emitted before the line's first RUN is consumed while the
        // beam is in horizontal blanking, where compositor.v pops as fast as the
        // FIFO allows and a staged SET executes immediately at zero slot cost.
        // Only a SET that has to land on a PARTICULAR pixel belongs in the run
        // of active records below.
        bool want_records = true;

        // One palette entry: three consecutive words, and in the BLANK region so
        // the four pixel slots the write disturbs are all inside blanking.
        auto put_entry = [&](uint32_t index, Rgb444 colour)
        {
            const PaletteEntryWords e = vidcmd_palette_entry(index, colour);
            for (uint16_t v : e)
            {
                put(v);
            }
        };

        if (p.slot_regression)
        {
            // The palette this case needs — base 0 is the PAIR {c1, c1} and base
            // REGRESSION_BASE_HI the pair {c2, c2}, so the mid-line
            // SET(pix_base) below repaints whatever the bitmap says with one
            // colour either way and the pixel it lands on is unambiguous — is
            // loaded once per frame in the VBLANK preamble (author_frame), not
            // here: twelve more words in an HBLANK would push this line's own
            // SETs past pixel 0.
            put(vidcmd_set(SET_PIX_BASE, 0));
            put(vidcmd_set(SET_PIX_MODE, pixel_mode_bits(PixelMode::DIRECT_1BPP)));
            put(vidcmd_set(SET_PIX_PIXEL_SKIP, 0));
        }
        else if (p.jit_frame_preamble)
        {
            // The firmware console's whole VIDCMD budget: one preamble on line 0,
            // nothing on the other 479.  compositor.v's hold does the rest —
            // {RUN(passthrough,1)} paints a line, and a line that receives no
            // fill keeps holding the source it already had.  The preamble is now
            // the two palette entries the console's 1bpp plane selects (entry 1
            // for a set bit, entry 0 for a clear one) plus base and mode: nine
            // words instead of three, all of them free in blanking.
            if (line == 0)
            {
                put_entry(0, p.held_bg);
                put_entry(1, p.held_fg);
                put(vidcmd_set(SET_PIX_BASE, 0));
                put(vidcmd_set(SET_PIX_MODE, pixel_mode_bits(PixelMode::DIRECT_1BPP)));
                put(vidcmd_run(RUN_SRC_PASSTHROUGH, 1));
                slots += 1;
            }
            want_records = false;
        }
        else
        {
            if (p.sprites != SpriteStyle::NONE && line == 0)
            {
                put(vidcmd_set(SET_CMP_HELD_FG, p.held_fg));
                put(vidcmd_set(SET_CMP_HELD_BG, p.held_bg));
            }
            // A PER-LINE RELOAD of the two entries a 1bpp playfield selects:
            // six words in the HBLANK ahead of the line, entry 1 for a set bit
            // and entry 0 for a clear one.
            if (p.per_line_palette)
            {
                const Rgb444 fg = p.depth_fade_palette ? tempest_palette_fg(line)
                                                       : line_palette_fg(line);
                const Rgb444 bg = p.depth_fade_palette ? tempest_palette_bg(line)
                                                       : line_palette_bg(line);
                put_entry(0, bg);
                put_entry(1, fg);
            }
            if (p.per_line_mode)
            {
                put(vidcmd_set(SET_PIX_MODE, mode_value));
                put(vidcmd_set(SET_PIX_BASE, p.pix_base));
                put(vidcmd_set(SET_PIX_PIXEL_SKIP, pixel_skip));
            }
        }

        // --- gather this line's painted spans ---------------------------------
        if (want_records && !p.slot_regression && !p.mid_line_split)
        {
            if (p.sprites == SpriteStyle::CURSOR)
            {
                if (line >= p.cursor_y && line < p.cursor_y + 16)
                {
                    const uint16_t mask     = CURSOR_MASK[line - p.cursor_y];
                    const uint16_t interior = erode_horizontal(mask);
                    // Walk the 16 columns and emit maximal runs of equal class.
                    // Contiguity is what makes a mask cheap as runs: the arrow
                    // costs 1-3 records per row rather than a 3-word tile.
                    uint32_t col = 0;
                    while (col < 16)
                    {
                        const bool on = ((mask >> (15 - col)) & 1) != 0;
                        if (!on)
                        {
                            col++;
                            continue;
                        }
                        const bool fg = ((interior >> (15 - col)) & 1) != 0;
                        uint32_t run = 1;
                        while (col + run < 16)
                        {
                            const bool on2 = ((mask >> (15 - (col + run))) & 1) != 0;
                            const bool fg2 = ((interior >> (15 - (col + run))) & 1) != 0;
                            if (!on2 || fg2 != fg)
                            {
                                break;
                            }
                            run++;
                        }
                        insert_span(spans, span_n,
                                    {static_cast<int32_t>(p.cursor_x + col),
                                     static_cast<int32_t>(run),
                                     fg ? RUN_SRC_HELD_FG : RUN_SRC_HELD_BG, 0});
                        col += run;
                    }
                }
            }
            else if (p.sprites == SpriteStyle::FOUR_SPRITES)
            {
                const uint32_t row  = line % 16u;
                const uint16_t mask = sprite_mask(row);
                // The diamond is one contiguous run, so each sprite is exactly
                // one record: four sprites cost four colour runs plus their
                // gaps, where a tile cost three words each.
                uint32_t first = 0;
                while (first < 16 && ((mask >> (15 - first)) & 1) == 0)
                {
                    first++;
                }
                uint32_t width = 0;
                while (first + width < 16 && ((mask >> (15 - (first + width))) & 1) != 0)
                {
                    width++;
                }
                if (width > 0)
                {
                    for (uint32_t s = 0; s < SPRITE_X.size(); s++)
                    {
                        insert_span(spans, span_n,
                                    {static_cast<int32_t>(SPRITE_X[s] + first),
                                     static_cast<int32_t>(width), sprite_row_src(row), 0});
                    }
                }
            }
            else if (p.tempest_objects)
            {
                span_n = tempest_spans_for_line(p, line, spans);
            }
        }

        // --- active-slot records ---------------------------------------------
        if (p.slot_regression)
        {
            // compositor_tb's NORMATIVE_M0.  The RUN and the SET are banked as a
            // pair in HBLANK, so slot 0 is the RUN and slot 1 is the SET
            // (visible in its own slot).  The tail RUN is a fresh fetch and
            // lands on slot 3 at the 2-slot cadence, with slot 2 holding the
            // same passthrough — so it is 637 slots long, not 638, and
            // close_exact() is what works that out.
            put(vidcmd_run(RUN_SRC_PASSTHROUGH, 1));
            slots += 1;
            // A mid-line SET(pix_base) is the pixel-exact recolour the indexed
            // chip has: it changes which PAIR of palette entries the playfield
            // selects and disturbs nothing, where a mid-line palette WRITE would
            // steal four pixel slots (vidcmd-spec.md §3.2).
            put(vidcmd_set(SET_PIX_BASE, REGRESSION_BASE_HI));
            slots += 1;
            close_exact();
        }
        else if (p.mid_line_split)
        {
            const uint32_t skew = p.skew_pix;
            const uint32_t lead = (p.split_pixel > skew) ? (p.split_pixel - skew) : 0u;
            if (lead > 0)
            {
                put(vidcmd_run(RUN_SRC_PASSTHROUGH, lead));
                slots += lead;
            }
            // The two SETs are the leading RUN's banked pair, so they land on
            // consecutive slots — the whole point of the case, and the property
            // the 2-clock cadence preserves.  The record AFTER them is a fresh
            // fetch and slips one slot behind the authored sum.
            //
            // Both are SET(pix_base), to two pairs of palette entries loaded
            // with one colour each, so "which pixel did this SET land on" is
            // answered by the colour of that pixel and nothing else.  A mid-line
            // palette WRITE is illegal under passthrough and is not what this
            // case is about.
            put(vidcmd_set(SET_PIX_BASE, SPLIT_BASE_MID));
            slots += 1;
            put(vidcmd_set(SET_PIX_BASE, SPLIT_BASE_HI));
            slots += 1;
            close_exact();
        }
        else if (want_records && p.sprites == SpriteStyle::MASK_SPRITES)
        {
            // Four 16x16 sprites as MASK records on 16-pixel cell boundaries,
            // with passthrough RUNs between them.  RUN-to-mask and mask-to-RUN
            // are both zero-slot boundaries, so the authored sum is exactly
            // H_ACTIVE by construction and no filler arithmetic is needed —
            // the same property the console screen is built on.
            //
            // The point of the case is the dibits' RESOLUTION.  A mask steps
            // one dibit per PIXEL CLOCK slot, and COMPOSITOR has no idea what
            // rate PIXEL is running at, so these sprites stay 640-wide over a
            // 320-wide half-rate playfield showing through their holes.
            const uint32_t row = line % MASK_SPRITE_H;
            uint32_t       x   = 0;
            for (uint32_t s = 0; s < MASK_SPRITE_COUNT; s++)
            {
                const uint32_t x0 = MASK_SPRITE_X[s];
                if (x0 > x)
                {
                    put(vidcmd_run(RUN_SRC_PASSTHROUGH, x0 - x));
                    slots += x0 - x;
                }

                std::array<uint32_t, MASK_SLOTS> d{};
                for (uint32_t c = 0; c < MASK_SLOTS; c++)
                {
                    d[c] = mask_sprite_dibit(row, c);
                }
                put_mask(vidcmd_mask(d));
                slots += MASK_SLOTS;
                x = x0 + MASK_SLOTS;
            }
            if (x < H_ACTIVE)
            {
                put(vidcmd_run(RUN_SRC_PASSTHROUGH, H_ACTIVE - x));
                slots += H_ACTIVE - x;
            }
        }
        else if (want_records)
        {
            int32_t x = 0;
            for (uint32_t i = 0; i < span_n; i++)
            {
                if (spans[i].x > x)
                {
                    put(vidcmd_run(RUN_SRC_PASSTHROUGH, static_cast<uint32_t>(spans[i].x - x)));
                    slots += static_cast<uint32_t>(spans[i].x - x);
                }
                if (spans[i].src == RUN_SRC_COLOR)
                {
                    put(vidcmd_run_color(spans[i].color, static_cast<uint32_t>(spans[i].w)));
                }
                else
                {
                    put(vidcmd_run(spans[i].src, static_cast<uint32_t>(spans[i].w)));
                }
                slots += static_cast<uint32_t>(spans[i].w);
                x = spans[i].x + spans[i].w;
            }

            if (p.framing == FramingMode::CUSHION)
            {
                // Exact-640 discipline: the FIFO never empties mid-line, so the
                // occupancy has to close — and occupancy, not the authored sum,
                // is what close_exact() measures.
                if (slots < H_ACTIVE)
                {
                    close_exact();
                }
            }
            else if (slots < H_ACTIVE)
            {
                // JIT: hand the line back to passthrough and let hold replicate
                // it to HBLANK.  One word, however much line is left.
                if (span_n > 0 || slots == 0)
                {
                    put(vidcmd_run(RUN_SRC_PASSTHROUGH, 1));
                    slots += 1;
                }
            }
        }

        // Report the OCCUPANCY, not the authored sum: what the checker has to
        // hold the list to is the number of slots the records really take.
        const VidcmdSlotPlan plan = plan_now();
        line_words[line]   = static_cast<uint8_t>(n);
        line_records[line] = static_cast<uint8_t>(records);
        line_slots[line]   = static_cast<uint16_t>(plan.slots);
        line_stretch[line] = static_cast<uint16_t>(plan.stretch);
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

    // --- the frame's PALETTE, in vertical blanking ---------------------------
    //
    // Sixteen three-word entries is 48 words, well past what one HBLANK can
    // deliver, and vertical blanking is where it belongs anyway: there are no
    // pixel slots for a write to disturb.  Nothing in hardware clears the RAM,
    // so this load is also the assertion that the entries survive 480 lines and
    // the /RS at the next vsync.
    if (p.palette_entries > 0)
    {
        const uint32_t addr = p.vidcmd_base + VIDCMD_PREAMBLE_SLOT * VIDCMD_LINE_STRIDE_BYTES;
        uint32_t n = 0;
        for (uint32_t i = 0; i < p.palette_entries; i++)
        {
            for (uint16_t v : vidcmd_palette_entry(i, p.palette[i]))
            {
                ram[addr + n * 2] = v;
                n++;
            }
        }
        emit_stream(w, DEST_VIDCMD_FIFO_W, addr, n, false, DESC_MAX_COUNT);
    }

    // --- VBLANK preamble -----------------------------------------------------
    // The list is armed during vertical blanking, so this runs immediately with
    // no HBLANK wait.  AUDIO keeps popping through the 45 blanked lines, and
    // no per-line group exists to feed it there, so the preamble carries
    // vblank's share: 45 lines / 2 = 22.5 samples per channel, rounded up to
    // 23.  The control records go first, every frame: re-sending PERIOD and
    // VOLUME is harmless, and the first frame needs them before any sample.
    if (p.audio)
    {
        emit_stream(w, DEST_AUDCTL_FIFO_W, audio_control_base(p.audio_base, p.audio_source_pairs),
                    AUDIO_CONTROL_WORDS, false, DESC_MAX_COUNT);
    }
    if (p.audio && p.audio_preamble_pairs > 0)
    {
        emit_stream(w, DEST_SAMPLES0_FIFO_W, p.audio_base + audio_pair * 2,
                    p.audio_preamble_pairs, false, DESC_MAX_COUNT);
        emit_stream(w, DEST_SAMPLES2_FIFO_W, audio_right_base(p.audio_base, p.audio_source_pairs) + audio_pair * 2,
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
        d.destination = DEST_NONE;
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

        // A pure-VIDCMD screen has no pixel stream at all: the display list IS
        // the framebuffer, so the group is one run of VIDCMD descriptors and
        // nothing else.  PIXEL keeps re-delivering its reset word, which is
        // what makes a passthrough dibit a settable third colour (see the note
        // at the top of the screen section).
        if (p.pure_vidcmd)
        {
            emit_stream(w, DEST_VIDCMD_FIFO_W, vidcmd_addr, vidcmd_words, wait,
                        DESC_MAX_COUNT);
            wait = false;
        }
        else
        {
            // Optional pixel head, ahead of everything: cheap immunity for PIXEL.
            uint32_t pix_head = 0;
            if (p.pixels_head_words > 0 && p.pixels_head_words < line_words)
            {
                pix_head = p.pixels_head_words;
                emit_stream(w, DEST_PIXELS_FIFO_W, src, pix_head, wait, pixel_chunk);
                wait = false;
            }

            if (head > 0)
            {
                emit_stream(w, DEST_VIDCMD_FIFO_W, vidcmd_addr, head, wait, DESC_MAX_COUNT);
                wait = false;
            }

            emit_stream(w, DEST_PIXELS_FIFO_W, src + pix_head * 2, line_words - pix_head,
                        wait, pixel_chunk);
            wait = false;

            if (vidcmd_words > head)
            {
                emit_stream(w, DEST_VIDCMD_FIFO_W, vidcmd_addr + head * 2,
                            vidcmd_words - head, false, DESC_MAX_COUNT);
            }
        }

        // Audio last in the group: each channel FIFO is 256 samples deep and
        // drains at one sample per two lines, so audio is the one consumer
        // with enough slack to be scheduled behind everything else.
        if (p.audio && (line % p.audio_burst_interval) == 0)
        {
            emit_stream(w, DEST_SAMPLES0_FIFO_W, p.audio_base + audio_pair * 2,
                        p.audio_burst_pairs, false, DESC_MAX_COUNT);
            emit_stream(w, DEST_SAMPLES2_FIFO_W, audio_right_base(p.audio_base, p.audio_source_pairs) + audio_pair * 2,
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
        d.destination = DEST_NONE;
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

// ===========================================================================
// STATIC PIECES
// ===========================================================================

uint32_t sp_program_words(uint32_t i, std::span<uint16_t> out)
{
    uint32_t n = 0;
    auto     put = [&](uint16_t v)
    {
        if (n < out.size())
        {
            out[n] = v;
        }
        n++;
    };

    // The left cap: the line's pix_base SET and the left border, one static
    // two-word fragment per pix_base value.
    put(vidcmd_set(SET_PIX_BASE, sp_pix_base_for(i)));
    put(vidcmd_run_color(RUN_COLOR_BLACK, SP_BORDER_PIXELS));
    put(vidcmd_run(RUN_SRC_PASSTHROUGH, sp_gap_for(i)));
    for (uint32_t j = 0; j < SP_POSE_WORDS; j++)
    {
        put(sp_pose_word(i, j));
    }
    put(vidcmd_run(RUN_SRC_PASSTHROUGH, sp_tail_for(i)));
    put(vidcmd_run_color(RUN_COLOR_BLACK, SP_BORDER_PIXELS));
    return n;
}

void write_static_pieces_data(const StaticPiecesParams &p, Memory ram,
                              std::span<const Rgb444> palette)
{
    // The gap/tail table: RUN(passthrough, n) for every n a 640-pixel line can
    // want.  Entry H_ACTIVE doubles as the whole program of a plain line, which
    // is why a non-sprite line costs one descriptor and no data of its own.
    for (uint32_t n = 1; n <= SP_GAP_TABLE_ENTRIES; n++)
    {
        ram[sp_gap_word_addr(p.data_base, n)] = vidcmd_run(RUN_SRC_PASSTHROUGH, n);
    }

    for (uint32_t sel = 0; sel < 2; sel++)
    {
        const uint32_t addr = sp_left_cap_addr(p.data_base, sel);
        ram[addr + 0] = vidcmd_set(SET_PIX_BASE, sel * 2u);
        ram[addr + 2] = vidcmd_run_color(RUN_COLOR_BLACK, SP_BORDER_PIXELS);
    }

    ram[sp_right_border_addr(p.data_base)] = vidcmd_run_color(RUN_COLOR_BLACK, SP_BORDER_PIXELS);

    for (uint32_t i = 0; i < SP_SPRITE_LINES; i++)
    {
        const uint32_t addr = sp_pose_row_addr(p.data_base, i);
        for (uint32_t j = 0; j < SP_POSE_WORDS; j++)
        {
            ram[addr + j * 2] = sp_pose_word(i, j);
        }
    }

    // The frame's palette, as three-word entries, deposited in vertical
    // blanking where a write disturbs no pixel slot.
    {
        uint32_t       n    = 0;
        const uint32_t addr = sp_palette_addr(p.data_base);
        for (uint32_t e = 0; e < SP_PALETTE_ENTRIES; e++)
        {
            const Rgb444 c = (e < palette.size()) ? palette[e] : RGB444_BLACK;
            for (uint16_t v : vidcmd_palette_entry(e, c))
            {
                ram[addr + n * 2] = v;
                n++;
            }
        }
    }

    // MONOLITHIC's programs: the same thirteen words gathered into one place,
    // padded to the frozen SP_PROGRAM_WORDS shape with filler SETs.  Target 7
    // is decoded nowhere, and a pad is consumed eagerly in the NEXT line's
    // blanking, so it costs neither a slot nor a pixel — see STATIC-PIECES.md.
    for (uint32_t i = 0; i < SP_SPRITE_LINES; i++)
    {
        std::array<uint16_t, SP_SEMANTIC_WORDS> prog{};
        const uint32_t n = sp_program_words(i, prog);
        const uint32_t addr = sp_program_addr(p.program_base, i);
        for (uint32_t w = 0; w < n; w++)
        {
            ram[addr + w * 2] = prog[w];
        }
        for (uint32_t w = n; w < SP_PROGRAM_WORDS; w++)
        {
            ram[addr + w * 2] = vidcmd_set(SET_FILLER, 0);
        }
    }
}

AuthorResult author_static_pieces(const StaticPiecesParams &p, Memory ram)
{
    ListWriter w;
    w.ram    = ram;
    w.base   = p.table_base;
    w.limit  = p.table_base + p.table_bytes;
    w.cursor = p.table_base;

    auto pacer = [&](bool stop)
    {
        Descriptor d;
        d.src         = p.data_base;
        d.count       = 1;
        d.destination = DEST_NONE;
        d.wait_hblank = true;
        d.stop_after  = stop;
        w.emit(d);
    };

    // One line's VIDCMD program.  `wait` puts the group's wait_hblank on the
    // first descriptor of the program; at k > 0 the PIXELS run owns it instead.
    auto emit_program = [&](uint32_t line, bool wait)
    {
        if (!sp_is_sprite_line(line))
        {
            Descriptor d;
            d.src         = sp_gap_word_addr(p.data_base, H_ACTIVE);
            d.count       = SP_PLAIN_LINE_WORDS;
            d.destination = DEST_VIDCMD_FIFO_W;
            d.wait_hblank = wait;
            w.emit(d);
            return;
        }

        const uint32_t i = sp_sprite_index(line);

        if (p.form == StaticPiecesForm::MONOLITHIC)
        {
            Descriptor d;
            d.src         = sp_program_addr(p.program_base, i);
            d.count       = static_cast<uint16_t>(SP_PROGRAM_WORDS);
            d.destination = DEST_VIDCMD_FIFO_W;
            d.wait_hblank = wait;
            w.emit(d);
            return;
        }

        const uint32_t srcs[5] = {
            sp_left_cap_addr(p.data_base, sp_pix_base_sel(i)),
            sp_gap_word_addr(p.data_base, sp_gap_for(i)),
            sp_pose_row_addr(p.data_base, i),
            sp_gap_word_addr(p.data_base, sp_tail_for(i)),
            sp_right_border_addr(p.data_base),
        };
        const uint16_t counts[5] = {2, 1, static_cast<uint16_t>(SP_POSE_WORDS), 1, 1};

        for (uint32_t piece = 0; piece < 5; piece++)
        {
            Descriptor d;
            d.src         = srcs[piece];
            d.count       = counts[piece];
            d.destination = DEST_VIDCMD_FIFO_W;
            d.wait_hblank = wait && (piece == 0);
            w.emit(d);
        }
    };

    // --- the vblank walk, split around /RS -----------------------------------
    uint32_t pacers = 0;
    for (; pacers < p.pacers_before_preamble && pacers < p.vblank_pacing_lines; pacers++)
    {
        pacer(false);
    }

    {
        Descriptor d;
        d.src         = sp_palette_addr(p.data_base);
        d.count       = static_cast<uint16_t>(SP_PALETTE_WORDS);
        d.destination = DEST_VIDCMD_FIFO_W;
        w.emit(d);
    }

    // The lookahead's own bootstrap: with line y's program deposited in group
    // y-k, the first k lines have no group to be deposited in.  Vertical
    // blanking after /RS is where they go, and the FIFO holds them because
    // playback is gated by H_ACTIVE and there is none until line 0.
    for (uint32_t y = 0; y < p.lookahead && y < V_ACTIVE; y++)
    {
        emit_program(y, false);
    }

    for (; pacers < p.vblank_pacing_lines; pacers++)
    {
        pacer(false);
    }

    // --- one group per visible line ------------------------------------------
    for (uint32_t g = 0; g < V_ACTIVE; g++)
    {
        const uint32_t target      = g + p.lookahead;
        const bool     has_program = target < V_ACTIVE;
        bool           wait        = true;

        if (!p.pixels_first && has_program)
        {
            emit_program(target, wait);
            wait = false;
        }

        const uint32_t src = p.fb_base + g * p.fb_stride_bytes;
        emit_stream(w, DEST_PIXELS_FIFO_W, src, PIXELS_WORDS_1BPP, wait, SP_PIXELS_CHUNK);
        wait = false;

        if (p.pixels_first && has_program)
        {
            emit_program(target, false);
        }
    }

    pacer(true);

    AuthorResult r;
    r.first_descriptor = p.table_base;
    r.descriptor_count = w.count;
    r.table_bytes      = w.cursor - w.base;
    r.payload_words    = w.payload_words;
    r.pacing_words     = w.pacing_words;
    r.audio_pairs      = 0;
    r.table_overflow   = w.overflow;
    return r;
}

}  // namespace SuperEngine
