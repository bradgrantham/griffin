// Bitmap font rendering for 1bpp linear framebuffers.
//
// Layered as:
//
//   Font          : runtime descriptor (geometry + pointer to glyph bits).
//   GlyphBlit<>   : compile-time-specialized blitter template.
//   FontRenderer  : runtime handle holding font data + function pointers
//                   to the specialized GlyphBlit static functions.
//
// Textport holds a `const FontRenderer*` and dispatches through it so the
// active font can be switched at runtime while the inner blit loops stay
// fully specialized at compile time.

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>

namespace griffin::textport
{

// Geometry/data descriptor for one font.  Stored in ROM (.rodata).
struct Font
{
    uint8_t  width;        // glyph width in pixels
    uint8_t  height;       // glyph height in pixel rows
    uint8_t  stride;       // bytes per glyph row (1 if width <= 8, 2 if 9..16)
    uint8_t  mask;         // active bits in stored byte (e.g. 0xFC for 6-wide)
    uint8_t  first;        // first encoded code point
    uint16_t count;        // glyphs encoded
    const uint8_t* bits;   // [count * height * stride], MSB-left
};

// Runtime polymorphic handle.  One static instance per font lives in ROM.
struct FontRenderer
{
    using DrawCellFn   = void (*)(uint8_t* fb, unsigned pitch,
                                  const FontRenderer& fr,
                                  unsigned cx, unsigned cy,
                                  uint8_t glyph, bool inverse);
    using InvertCellFn = void (*)(uint8_t* fb, unsigned pitch,
                                  const FontRenderer& fr,
                                  unsigned cx, unsigned cy);
    using ClearRowFn   = void (*)(uint8_t* fb, unsigned pitch,
                                  const FontRenderer& fr,
                                  unsigned cy, unsigned cols);

    const Font*  font;
    DrawCellFn   draw_cell;
    InvertCellFn invert_cell;
    ClearRowFn   clear_row;
};

// Compile-time-specialized blitter.  Each font instantiates one of these
// and stores pointers to its statics in its FontRenderer.
//
// Template parameters are the font geometry, lifted to compile time so the
// hot loops collapse to straight-line m68k code on the aligned 8-wide path.
template<unsigned W, unsigned H, unsigned Stride, uint8_t Mask>
struct GlyphBlit
{
    static_assert(W >= 1 && W <= 16, "GlyphBlit width must be 1..16");
    static_assert(Stride == 1 || Stride == 2, "Stride must be 1 or 2");
    static_assert(Stride * 8 >= W, "Stride must hold W bits");

    // Compute the cell's pixel offset within the framebuffer row.
    // Inlined into every blit so the divisions vanish when W is a constant.
    static constexpr unsigned cell_bit_offset(unsigned cx)
    {
        return cx * W;
    }

    static void draw_cell(uint8_t* fb, unsigned pitch,
                          const FontRenderer& fr,
                          unsigned cx, unsigned cy,
                          uint8_t glyph, bool inverse)
    {
        const uint8_t first = fr.font->first;
        if (glyph < first || glyph >= first + fr.font->count)
        {
            glyph = static_cast<uint8_t>(' ' < first ? first : ' ');
        }
        const uint8_t* g = fr.font->bits
                         + (static_cast<unsigned>(glyph - first) * H * Stride);
        const uint8_t xorMask = inverse ? Mask : uint8_t{0};

        if constexpr (W == 8 && Stride == 1)
        {
            // Aligned 8-wide: 1 store per row, no shift, no read-modify-write.
            uint8_t* row = fb + cy * H * pitch + cx;
            for (unsigned r = 0; r < H; ++r)
            {
                *row = static_cast<uint8_t>(g[r] ^ xorMask);
                row += pitch;
            }
        }
        else if constexpr (W == 16 && Stride == 2)
        {
            // Aligned 16-wide: 2 stores per row, no shift.
            uint8_t* row = fb + cy * H * pitch + (cx * 2);
            for (unsigned r = 0; r < H; ++r)
            {
                row[0] = static_cast<uint8_t>(g[r * 2    ] ^ xorMask);
                row[1] = static_cast<uint8_t>(g[r * 2 + 1] ^ xorMask);
                row += pitch;
            }
        }
        else
        {
            // General path: glyph row may straddle a byte boundary.
            // Compile-time Stride decides whether the source uses 1 or 2 bytes.
            const unsigned bit   = cell_bit_offset(cx);
            const unsigned shift = bit & 7u;
            uint8_t* row = fb + cy * H * pitch + (bit >> 3);

            // mask of bits inside the cell that we are about to write
            // in each of up to two destination bytes.
            const uint8_t cellMaskHi = static_cast<uint8_t>(Mask >> shift);
            const uint8_t cellMaskLo = (shift == 0)
                ? uint8_t{0}
                : static_cast<uint8_t>(Mask << (8u - shift));
            const bool do_byte2 = (shift + W) > 8;

            for (unsigned r = 0; r < H; ++r)
            {
                uint16_t glyph_word;
                if constexpr (Stride == 1)
                {
                    glyph_word = static_cast<uint16_t>(
                        static_cast<uint16_t>(g[r] ^ xorMask) << 8);
                }
                else
                {
                    const uint8_t b0 = static_cast<uint8_t>(g[r * 2    ] ^ xorMask);
                    const uint8_t b1 = static_cast<uint8_t>(g[r * 2 + 1] ^ xorMask);
                    glyph_word = static_cast<uint16_t>(
                        (static_cast<uint16_t>(b0) << 8) | b1);
                }

                const uint8_t hi = static_cast<uint8_t>(glyph_word >> (8u + shift)) & cellMaskHi;
                row[0] = static_cast<uint8_t>((row[0] & ~cellMaskHi) | hi);

                if (do_byte2)
                {
                    const uint8_t lo = static_cast<uint8_t>(glyph_word >> shift) & cellMaskLo;
                    row[1] = static_cast<uint8_t>((row[1] & ~cellMaskLo) | lo);
                }

                row += pitch;
            }
        }
    }

    static void invert_cell(uint8_t* fb, unsigned pitch,
                            const FontRenderer& /*fr*/,
                            unsigned cx, unsigned cy)
    {
        if constexpr (W == 8 && Stride == 1)
        {
            uint8_t* row = fb + cy * H * pitch + cx;
            for (unsigned r = 0; r < H; ++r)
            {
                *row = static_cast<uint8_t>(*row ^ 0xFFu);
                row += pitch;
            }
        }
        else if constexpr (W == 16 && Stride == 2)
        {
            uint8_t* row = fb + cy * H * pitch + (cx * 2);
            for (unsigned r = 0; r < H; ++r)
            {
                row[0] = static_cast<uint8_t>(row[0] ^ 0xFFu);
                row[1] = static_cast<uint8_t>(row[1] ^ 0xFFu);
                row += pitch;
            }
        }
        else
        {
            const unsigned bit   = cell_bit_offset(cx);
            const unsigned shift = bit & 7u;
            uint8_t* row = fb + cy * H * pitch + (bit >> 3);

            const uint8_t cellMaskHi = static_cast<uint8_t>(Mask >> shift);
            const uint8_t cellMaskLo = (shift == 0)
                ? uint8_t{0}
                : static_cast<uint8_t>(Mask << (8u - shift));
            const bool do_byte2 = (shift + W) > 8;

            for (unsigned r = 0; r < H; ++r)
            {
                row[0] = static_cast<uint8_t>(row[0] ^ cellMaskHi);
                if (do_byte2)
                {
                    row[1] = static_cast<uint8_t>(row[1] ^ cellMaskLo);
                }
                row += pitch;
            }
        }
    }

    static void clear_row(uint8_t* fb, unsigned pitch,
                          const FontRenderer& /*fr*/,
                          unsigned cy, unsigned cols)
    {
        // Clears the pixel rectangle covering cells [0..cols) at row cy.
        // Used by scroll/insert/delete-line/erase paths.
        if constexpr (W == 8 && Stride == 1)
        {
            uint8_t* p = fb + cy * H * pitch;
            for (unsigned r = 0; r < H; ++r)
            {
                std::memset(p, 0, cols);
                p += pitch;
            }
        }
        else if constexpr (W == 16 && Stride == 2)
        {
            uint8_t* p = fb + cy * H * pitch;
            for (unsigned r = 0; r < H; ++r)
            {
                std::memset(p, 0, cols * 2u);
                p += pitch;
            }
        }
        else
        {
            // Width is not a multiple of 8: clear by repeatedly invalidating
            // every cell.  Used only on the slow path so simplicity wins.
            for (unsigned r = 0; r < H; ++r)
            {
                uint8_t* row = fb + (cy * H + r) * pitch;
                const unsigned total_bits = cols * W;
                const unsigned full_bytes = total_bits / 8u;
                const unsigned tail_bits  = total_bits % 8u;
                std::memset(row, 0, full_bytes);
                if (tail_bits)
                {
                    const uint8_t tail_mask =
                        static_cast<uint8_t>(0xFFu << (8u - tail_bits));
                    row[full_bytes] = static_cast<uint8_t>(
                        row[full_bytes] & ~tail_mask);
                }
            }
        }
    }
};

} // namespace griffin::textport
