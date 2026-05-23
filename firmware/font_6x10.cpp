// 6x10 placeholder font.  Yields 106 columns x 48 rows on 640x480.
//
// Provides a minimal printable-ASCII subset so the 6-wide general blitter
// path is instantiated and exercised at compile time.  Replace
// font_6x10_bits[] with a real 6x10 font (e.g. the 6x10 in
// textport_code/cpm-bitmap-test.cpp around line 5398) to get readable text
// in this mode.

#include "font.h"

namespace griffin::textport
{

extern const uint8_t font_6x10_bits[96 * 10];

constinit const Font font_6x10 = {
    /*width =*/ 6,
    /*height=*/ 10,
    /*stride=*/ 1,
    /*mask  =*/ 0xFC,        // top 6 bits significant
    /*first =*/ 0x20,        // space
    /*count =*/ 96,          // 0x20..0x7F
    /*bits  =*/ font_6x10_bits,
};

extern const FontRenderer font_6x10_renderer;

constinit const FontRenderer font_6x10_renderer = {
    /*font        =*/ &font_6x10,
    /*draw_cell   =*/ &GlyphBlit<6, 10, 1, 0xFC>::draw_cell,
    /*invert_cell =*/ &GlyphBlit<6, 10, 1, 0xFC>::invert_cell,
    /*clear_row   =*/ &GlyphBlit<6, 10, 1, 0xFC>::clear_row,
};

// Placeholder data: 96 entries of 10 rows each.  Glyph 0x20 (space) is
// blank; every other glyph is a 4-wide block centered in the cell.
// Distinguishable from blanks but obviously a stub — replace with real
// font data when 6x10 text becomes interesting.
//
// Bit layout: MSB-left, top 6 bits used (mask 0xFC).
namespace
{
constexpr uint8_t kGlyphRows[10] = {
    0x00, 0x00, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78, 0x00, 0x00,
};
}

#define G0  0,0,0,0,0,0,0,0,0,0,
#define GX  kGlyphRows[0], kGlyphRows[1], kGlyphRows[2], kGlyphRows[3], \
            kGlyphRows[4], kGlyphRows[5], kGlyphRows[6], kGlyphRows[7], \
            kGlyphRows[8], kGlyphRows[9],

// 8 rows of 12 glyph slots each = 96 entries; first slot (' ') blank.
const uint8_t font_6x10_bits[96 * 10] = {
    G0 GX GX GX GX GX GX GX GX GX GX GX
    GX GX GX GX GX GX GX GX GX GX GX GX
    GX GX GX GX GX GX GX GX GX GX GX GX
    GX GX GX GX GX GX GX GX GX GX GX GX
    GX GX GX GX GX GX GX GX GX GX GX GX
    GX GX GX GX GX GX GX GX GX GX GX GX
    GX GX GX GX GX GX GX GX GX GX GX GX
    GX GX GX GX GX GX GX GX GX GX GX GX
};

#undef G0
#undef GX

} // namespace griffin::textport
