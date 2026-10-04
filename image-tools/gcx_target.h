// gcx_target.h — showing a .gcx picture on the 68000
//
// The freestanding half of gcx_format.h: layout arithmetic and the one loop a
// viewer runs, with no allocation and no host-only types.  gcx_format.h
// static_asserts that the two agree; the format's prose lives over there.
//
// A .gcx file is a MEMORY IMAGE.  It carries the pixel plane, every line's
// VIDCMD words exactly as COMPOSITOR reads them, and the ENGINE descriptor list
// that streams both, already built and already verified against the suite model
// by the encoder.  The only thing a file cannot know is where it will be loaded,
// so each descriptor's source is an offset from the start of the file and the
// viewer adds the load address once (relocate_table()).  After that the per-frame
// work is the vsync re-arm and nothing else.
//
//   header        32 words
//   descriptors   desc_count x 4 words, sources file-relative
//   vidcmd        preamble (16 palette entries, pix_base, pix_mode), then every
//                 line's packet in line order
//   pixels        height x 80 words
//   index         height words: each line's packet length (for decoders; the
//                 viewer never reads it)
//
// Everything is big-endian, so a word read out of the loaded image is its value.

#ifndef GRIFFIN_GCX_TARGET_H
#define GRIFFIN_GCX_TARGET_H

#include <stdint.h>

namespace GcxTarget
{

inline constexpr uint32_t MAGIC          = 0x47435831u;   // 'G','C','X','1'
inline constexpr unsigned VERSION        = 1;
inline constexpr unsigned HEADER_WORDS   = 32;            // 64 bytes
inline constexpr unsigned PIXEL_WIDTH    = 320;
inline constexpr unsigned WORDS_PER_LINE = 80;
inline constexpr unsigned MAX_HEIGHT     = 480;
inline constexpr unsigned DESC_WORDS     = 4;
inline constexpr unsigned DESC_BYTES     = DESC_WORDS * 2;

// Header word offsets.  Two-word fields are high word first.
inline constexpr unsigned HDR_MAGIC          = 0;    // 2 words
inline constexpr unsigned HDR_VERSION        = 2;
inline constexpr unsigned HDR_HEADER_WORDS   = 3;
inline constexpr unsigned HDR_WIDTH          = 4;
inline constexpr unsigned HDR_HEIGHT         = 5;
inline constexpr unsigned HDR_WORDS_PER_LINE = 6;
inline constexpr unsigned HDR_PIX_MODE       = 7;
inline constexpr unsigned HDR_FILE_BYTES     = 8;    // 2 words
inline constexpr unsigned HDR_DESC_OFFSET    = 10;   // 2 words, bytes from the file's start
inline constexpr unsigned HDR_DESC_COUNT     = 12;   // 2 words
inline constexpr unsigned HDR_VIDCMD_OFFSET  = 14;   // 2 words
inline constexpr unsigned HDR_VIDCMD_WORDS   = 16;   // 2 words
inline constexpr unsigned HDR_PIXELS_OFFSET  = 18;   // 2 words
inline constexpr unsigned HDR_INDEX_OFFSET   = 20;   // 2 words
inline constexpr unsigned HDR_PREAMBLE_WORDS = 22;
inline constexpr unsigned HDR_WALK_LINES     = 23;   // wait_hblank pacers ahead of line 0
inline constexpr unsigned HDR_GROUP_SYSCLK   = 24;   // worst line group, arbitration 4
inline constexpr unsigned HDR_FIFO_HIGH      = 25;   // VIDCMD FIFO high water the encoder saw
inline constexpr unsigned HDR_LINE_WORDS_MAX = 26;   // longest line packet
// 27..31 reserved, zero

constexpr uint32_t long_at(const uint16_t *base, unsigned word)
{
    return (static_cast<uint32_t>(base[word]) << 16) | base[word + 1];
}

// A loaded file, addressed rather than copied.
struct Picture
{
    const uint16_t *base       = nullptr;
    unsigned        height     = 0;
    uint32_t        desc_count = 0;

    const uint16_t *descriptors() const
    {
        return base + long_at(base, HDR_DESC_OFFSET) / 2;
    }
    uint32_t table_bytes() const { return desc_count * DESC_BYTES; }
};

// Check the header of a file already read into memory.  `words` is how many
// 16-bit words were read.  Returns false and leaves *out untouched if anything
// disagrees; *why gets a short reason for the caller to print.
inline bool open_picture(const uint16_t *base, uint32_t words, Picture *out, const char **why)
{
    if (words < HEADER_WORDS)
    {
        *why = "shorter than a header";
        return false;
    }
    if (long_at(base, HDR_MAGIC) != MAGIC)
    {
        *why = "not a .gcx picture (bad magic)";
        return false;
    }
    if (base[HDR_VERSION] != VERSION || base[HDR_HEADER_WORDS] != HEADER_WORDS)
    {
        *why = "unsupported .gcx version";
        return false;
    }
    const unsigned height = base[HDR_HEIGHT];
    if (height == 0 || height > MAX_HEIGHT)
    {
        *why = "height out of range";
        return false;
    }
    if (base[HDR_WIDTH] != PIXEL_WIDTH || base[HDR_WORDS_PER_LINE] != WORDS_PER_LINE)
    {
        *why = "not a 320-wide 80-word-per-line picture";
        return false;
    }
    if (words * 2u != long_at(base, HDR_FILE_BYTES))
    {
        *why = "file length disagrees with its own header";
        return false;
    }
    const uint32_t desc_offset = long_at(base, HDR_DESC_OFFSET);
    const uint32_t desc_count  = long_at(base, HDR_DESC_COUNT);
    if ((desc_offset & 1u) != 0 || desc_count == 0
        || desc_offset + desc_count * DESC_BYTES > words * 2u)
    {
        *why = "descriptor table outside the file";
        return false;
    }

    out->base       = base;
    out->height     = height;
    out->desc_count = desc_count;
    *why            = nullptr;
    return true;
}

// Copy the descriptor table to `table` (inside ENGINE's descriptor page) with
// every source turned from a file offset into an address.  Words 1 and 2 of a
// descriptor are the source's high and low halves, so the relocation is one
// 32-bit add; `load_address` is where the file's first byte sits, and it is even.
//
// Indexed, not pointer-bumped: the project's GCC miscompiles
// move.w (a0)+,0(a0,d0.l) copy loops (griffin.log 2026-10-02).
inline void relocate_table(uint16_t *table, const Picture &picture, uint32_t load_address)
{
    const uint16_t *source = picture.descriptors();
    for (uint32_t i = 0; i < picture.desc_count; i++)
    {
        const uint32_t at  = i * DESC_WORDS;
        const uint32_t src = long_at(source, at + 1) + load_address;
        table[at + 0] = source[at + 0];
        table[at + 1] = static_cast<uint16_t>(src >> 16);
        table[at + 2] = static_cast<uint16_t>(src);
        table[at + 3] = source[at + 3];
    }
}

}   // namespace GcxTarget

#endif /* GRIFFIN_GCX_TARGET_H */
