// gc4_format.h — the .gc4 file: a 4bpp half-rate picture an app can DMA
//
// What a Griffin program has to hand PIXEL for a photograph, in the layout it
// wants to hand it over in, and nothing else.  The mode is 4bpp at HALF RATE
// (vidcmd-spec.md §2): 320 groups across the 640-clock line, each group a
// four-bit palette index, 80 words per line — the same bandwidth micro-HAM used
// before it was retired with the indexed PIXEL.
//
// WHY TWO FIXED-STRIDE SECTIONS RATHER THAN ONE INTERLEAVED STREAM.  ENGINE
// streams a line's pixels out of RAM at a constant stride, so the pixel plane
// has to be contiguous and evenly spaced: a program reads the plane section
// straight into its framebuffer and points descriptors at it without touching a
// word.  The palette section is the same shape for the same reason — one fixed
// (1 + K)-word record per line, so line n's loads are at a multiply, not a walk.
// The experiment's .clut format (clut-experiment/clut_format.h) interleaves
// them, which is fine for a host decoder and useless to a display list.
//
// A LOAD IS ONE WORD, and it is one word because that is exactly the payload the
// two SETs of a palette entry carry: index in bits [15:12], colour in [11:0].
// The reader turns each into the three-word entry of vidcmd-spec.md §3.2 (SET
// clut_index, SET clut_data, the same-value filler) and deposits it in that
// line's HBLANK packet, where a palette write's four disturbed pixel slots do
// not exist.
//
// LAYOUT, every multi-byte field BIG-ENDIAN (68000 order):
//
//    0   4   'G' 'C' 'L' '4'
//    4   2   version (1)
//    6   2   k                 max palette loads per line, 0..8
//    8   2   width             320, the mode's own pixel domain
//   10   2   height            480 or 240
//   12   2   words_per_line    80
//   14   2   pix_mode          the SET(pix_mode) value: 4bpp | half rate
//   16  32   initial palette   16 x u16 R4G4B4, loaded once in vertical blanking
//   48       palette section   height x (1 + k) u16: count, then count loads,
//                              unused slots zero
//    +       pixel section     height x words_per_line u16, MSB-first indices
//
// Both sections start on a word boundary and are fixed stride, so the file is
// addressable arithmetic all the way down.  K = 0 is legal and means "the
// initial palette is the whole palette": one screen, sixteen colours, no per-line
// cost at all, which is what a game's backdrop wants.

#pragma once

#include "clut_color.h"
#include "gc4_target.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace Gc4
{

using ClutColor::PaletteLoad;
using ClutColor::Rgb444;

inline constexpr int VERSION        = 1;
inline constexpr int HEADER_BYTES   = 48;
inline constexpr int PALETTE_SIZE   = 16;
inline constexpr int PIXEL_WIDTH    = 320;   // 4bpp half rate: 320 groups per line
inline constexpr int OUT_WIDTH      = 640;   // what the CRT sees, each group held 2 clocks
inline constexpr int WORDS_PER_LINE = 80;
inline constexpr int BITS_PER_PIXEL = 4;

// The per-line reload budget.  The default is 4 because a 26-word HBLANK packet
// is the firmware viewer's budget (three words an entry plus the RUN), and 8 is
// the ceiling for the same reason: 8 * 3 + 1 = 25 words still fits, 9 does not.
inline constexpr int DEFAULT_K = 4;
inline constexpr int MAX_K     = 8;

static_assert(MAX_K * 3 + 1 <= 26, "K must fit a 26-word HBLANK VIDCMD packet");

// gc4_target.h is the same layout for the 68000, and the two must not drift.
static_assert(VERSION == static_cast<int>(Gc4Target::VERSION));
static_assert(HEADER_BYTES == 2 * static_cast<int>(Gc4Target::HEADER_WORDS));
static_assert(PALETTE_SIZE == static_cast<int>(Gc4Target::PALETTE_SIZE));
static_assert(PIXEL_WIDTH == static_cast<int>(Gc4Target::PIXEL_WIDTH));
static_assert(WORDS_PER_LINE == static_cast<int>(Gc4Target::WORDS_PER_LINE));
static_assert(MAX_K == static_cast<int>(Gc4Target::MAX_K));

// SET(pix_mode) for 4bpp half rate: depth 2 (4bpp) with mode[2] (half rate) set.
// Spelled out here rather than included from super-engine/descriptor.h so a host
// tool needs no generated headers; clut_roundtrip asserts the two agree.
inline constexpr int PIX_MODE_4BPP_HALF = 2 | 4;

struct Line
{
    std::vector<PaletteLoad> loads;
    std::vector<uint16_t>    words;
};

struct Picture
{
    int                 k      = DEFAULT_K;
    int                 height = 480;
    Rgb444              initial[PALETTE_SIZE] = {};
    std::vector<Line>   lines;
};

// ---------------------------------------------------------------------------
// Serialisation.  The writer does not validate: validate_picture() is the rule
// book, and the corruption tests need to be able to write a file that breaks it.
// ---------------------------------------------------------------------------

inline void put_be16(std::vector<uint8_t> &v, int w)
{
    v.push_back(static_cast<uint8_t>((w >> 8) & 0xFF));
    v.push_back(static_cast<uint8_t>(w & 0xFF));
}

inline std::vector<uint8_t> picture_bytes(const Picture &p)
{
    std::vector<uint8_t> out;
    out.push_back('G');
    out.push_back('C');
    out.push_back('L');
    out.push_back('4');
    put_be16(out, VERSION);
    put_be16(out, p.k);
    put_be16(out, PIXEL_WIDTH);
    put_be16(out, p.height);
    put_be16(out, WORDS_PER_LINE);
    put_be16(out, PIX_MODE_4BPP_HALF);
    for (int i = 0; i < PALETTE_SIZE; i++)
    {
        put_be16(out, p.initial[i]);
    }

    for (const Line &line : p.lines)
    {
        put_be16(out, static_cast<int>(line.loads.size()));
        for (int i = 0; i < p.k; i++)
        {
            if (i < static_cast<int>(line.loads.size()))
            {
                const PaletteLoad &l = line.loads[static_cast<size_t>(i)];
                put_be16(out, ((l.index & 0xF) << 12) | (l.colour & 0xFFF));
            }
            else
            {
                put_be16(out, 0);
            }
        }
    }
    for (const Line &line : p.lines)
    {
        for (uint16_t w : line.words)
        {
            put_be16(out, w);
        }
    }
    return out;
}

inline bool write_picture(const char *path, const Picture &p)
{
    const std::vector<uint8_t> data = picture_bytes(p);
    std::FILE                 *f    = std::fopen(path, "wb");
    if (f == nullptr)
    {
        return false;
    }
    const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    std::fclose(f);
    return ok;
}

inline bool read_bytes(const char *path, std::vector<uint8_t> &out)
{
    std::FILE *f = std::fopen(path, "rb");
    if (f == nullptr)
    {
        return false;
    }
    uint8_t buffer[65536];
    size_t  got = 0;
    out.clear();
    while ((got = std::fread(buffer, 1, sizeof(buffer), f)) > 0)
    {
        out.insert(out.end(), buffer, buffer + got);
    }
    std::fclose(f);
    return true;
}

// Shape only: a bad magic, a size that does not match the header's own geometry,
// an unknown version.  Rule violations are validate_picture()'s.
inline bool parse_picture(const std::vector<uint8_t> &data, Picture &out, std::string &error)
{
    auto be16 = [&](size_t at) { return (data[at] << 8) | data[at + 1]; };

    if (data.size() < static_cast<size_t>(HEADER_BYTES) || data[0] != 'G' || data[1] != 'C'
        || data[2] != 'L' || data[3] != '4')
    {
        error = "not a .gc4 file (bad magic)";
        return false;
    }
    const int version = be16(4);
    const int k       = be16(6);
    const int width   = be16(8);
    const int height  = be16(10);
    const int words   = be16(12);
    const int mode    = be16(14);

    if (version != VERSION)
    {
        error = "unsupported version " + std::to_string(version);
        return false;
    }
    if (k < 0 || k > MAX_K)
    {
        error = "header K " + std::to_string(k) + " out of range 0.." + std::to_string(MAX_K);
        return false;
    }
    if (width != PIXEL_WIDTH)
    {
        error = "header width " + std::to_string(width) + " is not " + std::to_string(PIXEL_WIDTH);
        return false;
    }
    if (height <= 0 || height > 480)
    {
        error = "header height " + std::to_string(height) + " out of range 1..480";
        return false;
    }
    if (words != WORDS_PER_LINE)
    {
        error = "header words_per_line " + std::to_string(words) + " is not "
                + std::to_string(WORDS_PER_LINE);
        return false;
    }
    if (mode != PIX_MODE_4BPP_HALF)
    {
        error = "header pix_mode " + std::to_string(mode) + " is not 4bpp half rate";
        return false;
    }

    const size_t expected = static_cast<size_t>(HEADER_BYTES)
                            + static_cast<size_t>(height) * (1 + k) * 2
                            + static_cast<size_t>(height) * WORDS_PER_LINE * 2;
    if (data.size() != expected)
    {
        error = "file is " + std::to_string(data.size()) + " bytes; its header implies "
                + std::to_string(expected);
        return false;
    }

    out.k      = k;
    out.height = height;
    for (int i = 0; i < PALETTE_SIZE; i++)
    {
        out.initial[i] = static_cast<Rgb444>(be16(static_cast<size_t>(16 + i * 2)));
    }

    out.lines.assign(static_cast<size_t>(height), Line());
    size_t at = HEADER_BYTES;
    for (int y = 0; y < height; y++)
    {
        Line     &line  = out.lines[static_cast<size_t>(y)];
        const int count = be16(at);
        at += 2;
        for (int i = 0; i < k; i++)
        {
            const int w = be16(at);
            at += 2;
            if (i < count)
            {
                PaletteLoad l;
                l.index  = static_cast<uint8_t>((w >> 12) & 0xF);
                l.colour = static_cast<Rgb444>(w & 0xFFF);
                line.loads.push_back(l);
            }
        }
        if (count > k)
        {
            error = "line " + std::to_string(y) + ": load count " + std::to_string(count)
                    + " exceeds K=" + std::to_string(k);
            return false;
        }
    }
    for (int y = 0; y < height; y++)
    {
        Line &line = out.lines[static_cast<size_t>(y)];
        line.words.assign(WORDS_PER_LINE, 0);
        for (int i = 0; i < WORDS_PER_LINE; i++)
        {
            line.words[static_cast<size_t>(i)] = static_cast<uint16_t>(be16(at));
            at += 2;
        }
    }
    return true;
}

// The rules a reader is entitled to rely on.
inline bool validate_picture(const Picture &p, std::string &error)
{
    if (p.k < 0 || p.k > MAX_K)
    {
        error = "K " + std::to_string(p.k) + " out of range";
        return false;
    }
    if (static_cast<int>(p.lines.size()) != p.height)
    {
        error = "picture has " + std::to_string(p.lines.size()) + " lines, not "
                + std::to_string(p.height);
        return false;
    }
    for (int i = 0; i < PALETTE_SIZE; i++)
    {
        if ((p.initial[i] & 0xF000) != 0)
        {
            error = "initial palette entry " + std::to_string(i) + " has bits above R4G4B4";
            return false;
        }
    }
    for (int y = 0; y < p.height; y++)
    {
        const Line       &line  = p.lines[static_cast<size_t>(y)];
        const std::string where = "line " + std::to_string(y) + ": ";
        if (static_cast<int>(line.loads.size()) > p.k)
        {
            error = where + "load count " + std::to_string(line.loads.size()) + " exceeds K="
                    + std::to_string(p.k);
            return false;
        }
        bool seen[PALETTE_SIZE] = { false };
        for (const PaletteLoad &l : line.loads)
        {
            if (l.index >= PALETTE_SIZE)
            {
                error = where + "palette index " + std::to_string(l.index) + " out of range";
                return false;
            }
            if (seen[l.index])
            {
                error = where + "palette index " + std::to_string(l.index) + " loaded twice";
                return false;
            }
            seen[l.index] = true;
            if ((l.colour & 0xF000) != 0)
            {
                error = where + "load colour has bits above R4G4B4";
                return false;
            }
        }
        if (static_cast<int>(line.words.size()) != WORDS_PER_LINE)
        {
            error = where + "has " + std::to_string(line.words.size()) + " pixel words, not "
                    + std::to_string(WORDS_PER_LINE);
            return false;
        }
    }
    return true;
}

}   // namespace Gc4
