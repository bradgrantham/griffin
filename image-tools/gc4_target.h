// gc4_target.h — reading a .gc4 picture on the 68000
//
// The freestanding half of gc4_format.h: the file's layout arithmetic, with no
// allocation, no <vector> and no host-only types, so firmware and applications
// can include it.  gc4_format.h static_asserts that the two agree; this file is
// the one that runs on the target and the format's prose lives over there.
//
// The 68000 is big-endian and every field in the file is big-endian, so a word
// read straight out of the loaded image is already the value: there is no
// byte swapping anywhere below, and that is the point of the format.
//
//   header      48 bytes, word aligned
//   palette     height x (1 + K) words: count, then K load slots
//   pixels      height x 80 words, contiguous -- ENGINE streams it in place
//
// A load slot is { index[15:12], colour[11:0] }, which is exactly the payload
// the two SETs of a palette entry carry (vidcmd-spec.md §3.2).

#ifndef GRIFFIN_GC4_TARGET_H
#define GRIFFIN_GC4_TARGET_H

#include <stdint.h>

namespace Gc4Target
{

inline constexpr uint32_t MAGIC          = 0x47434C34u;   // 'G','C','L','4'
inline constexpr unsigned VERSION        = 1;
inline constexpr unsigned HEADER_WORDS   = 24;            // 48 bytes
inline constexpr unsigned PALETTE_SIZE   = 16;
inline constexpr unsigned PIXEL_WIDTH    = 320;
inline constexpr unsigned WORDS_PER_LINE = 80;
inline constexpr unsigned MAX_K          = 8;
inline constexpr unsigned MAX_HEIGHT     = 480;

// Header word offsets.
inline constexpr unsigned HDR_MAGIC_HI       = 0;
inline constexpr unsigned HDR_MAGIC_LO       = 1;
inline constexpr unsigned HDR_VERSION        = 2;
inline constexpr unsigned HDR_K              = 3;
inline constexpr unsigned HDR_WIDTH          = 4;
inline constexpr unsigned HDR_HEIGHT         = 5;
inline constexpr unsigned HDR_WORDS_PER_LINE = 6;
inline constexpr unsigned HDR_PIX_MODE       = 7;
inline constexpr unsigned HDR_PALETTE        = 8;   // 16 words

// The whole file's size, so a reader can check it against the directory entry
// before it allocates anything.
constexpr uint32_t file_bytes(unsigned k, unsigned height)
{
    return 2u * (HEADER_WORDS + height * (1u + k) + height * WORDS_PER_LINE);
}

// A loaded file, addressed rather than copied.
struct Picture
{
    const uint16_t *base   = nullptr;
    unsigned        k      = 0;
    unsigned        height = 0;

    const uint16_t *palette() const { return base + HDR_PALETTE; }
    uint16_t        pix_mode() const { return base[HDR_PIX_MODE]; }

    // Line `line`'s palette record: word 0 is the load count, then k slots.
    const uint16_t *loads(unsigned line) const
    {
        return base + HEADER_WORDS + line * (1u + k);
    }
    unsigned load_count(unsigned line) const { return loads(line)[0]; }

    const uint16_t *pixels(unsigned line) const
    {
        return base + HEADER_WORDS + height * (1u + k) + line * WORDS_PER_LINE;
    }
};

constexpr unsigned load_index(uint16_t slot)  { return (slot >> 12) & 0xFu; }
constexpr unsigned load_colour(uint16_t slot) { return slot & 0x0FFFu; }

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
    const uint32_t magic =
        (static_cast<uint32_t>(base[HDR_MAGIC_HI]) << 16) | base[HDR_MAGIC_LO];
    if (magic != MAGIC)
    {
        *why = "not a .gc4 picture (bad magic)";
        return false;
    }
    if (base[HDR_VERSION] != VERSION)
    {
        *why = "unsupported .gc4 version";
        return false;
    }
    const unsigned k      = base[HDR_K];
    const unsigned height = base[HDR_HEIGHT];
    if (k > MAX_K)
    {
        *why = "K above the eight a 26-word HBLANK packet holds";
        return false;
    }
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
    if (words * 2u != file_bytes(k, height))
    {
        *why = "file length disagrees with its own header";
        return false;
    }

    out->base   = base;
    out->k      = k;
    out->height = height;
    *why        = nullptr;
    return true;
}

}   // namespace Gc4Target

#endif /* GRIFFIN_GC4_TARGET_H */
