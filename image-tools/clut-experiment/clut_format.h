// clut_format.h — the .clut stream format for the CLUT photo experiment
//
// DESIGN.md §3 is the specification; this header is its only implementation and
// the ONLY thing the encoder and the decoder share.  It deliberately contains
// no rendering: unpacking a pixel word into colours is the decoder's job and
// the encoder's prediction is written by a separate piece of code, so that
// "decoder output == encoder prediction" is a real cross-check between two
// implementations rather than a tautology.
//
// Byte layout, all multi-byte fields big-endian (68000 order, as micro-HAM):
//
//   0  'C' 'L' 'U' 'T'
//   4  u8  version (1)
//   5  u8  mode id (Mode, see MODES below)
//   6  u16 pixel width      the mode's own pixel domain: 640 or 320
//   8  u16 height           480
//  10  u8  K                CLUT entries the encoder was allowed per line
//  11  u8  N                refinement spans the encoder was allowed per line
//  12  u16 words_per_line   the mode's pixel words, repeated for cross-check
//
//   then `height` lines of:
//     u8  load_count,  load_count x { u8 index, u16 colour }
//     u8  span_count,  span_count x { u16 x, u16 length, u16 colour }
//     words_per_line x u16 pixel data
//
// Colours are 12-bit R4G4B4 in the low bits of a u16, the packing
// descriptor.h uses.

#pragma once

#include "clut_color.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace Clut
{

using Rgb444 = ClutColor::Rgb444;

inline constexpr int OUT_WIDTH  = 640;   // what the CRT sees, after any doubling
inline constexpr int HEIGHT     = 480;
inline constexpr int CLUT_SIZE  = 16;
inline constexpr int VERSION    = 1;

// vidcmd-spec.md §4.5: ENGINE delivers a word every 2 SYSCLK, the suite pins
// VIDCMD_WORDS_PER_LINE_CAP at 147 words for a 445-SYSCLK line.  Pixel words
// and VIDCMD words come out of the same budget, so a mode's words_per_line
// plus SPAN_WORDS per span must fit under it.  c4f (160 pixel words) does not,
// which is exactly why DESIGN.md calls it a flagged ceiling.
inline constexpr int VIDCMD_WORDS_PER_LINE_CAP = 147;

inline constexpr int SPAN_WORDS      = 3;   // SET cmp_color, RUN colour, RUN passthrough
inline constexpr int SPAN_MIN_LENGTH = 4;
inline constexpr int SPAN_MIN_GAP    = 4;

// ---------------------------------------------------------------------------
// Colour helpers — one home, ../clut_color.h (moved there 2026-09-20 with the
// blue-noise mask, when the c4h rung was promoted to image-tools/clut_encode)
// ---------------------------------------------------------------------------

using ClutColor::clamp_8;
using ClutColor::colour_error;
using ClutColor::nibble_to_8;
using ClutColor::quantize_8_to_4;
using ClutColor::rgb444;
using ClutColor::rgb444_b;
using ClutColor::rgb444_channel_8;
using ClutColor::rgb444_g;
using ClutColor::rgb444_r;

// ---------------------------------------------------------------------------
// Modes (DESIGN.md §2)
// ---------------------------------------------------------------------------

enum class Mode : uint8_t
{
    C4H = 0,   // 320 x 4bpp, half rate, 80 words/line
    C2F = 1,   // 640 x 2bpp, full rate, 80 words/line, CLUT entries 0..3 only
    C4F = 2,   // 640 x 4bpp, full rate, 160 words/line, over the 147-word cap
    ATTR = 3,  // 320 px as 40 cells of { fg4, bg4, pattern8 }, 40 words/line
};

struct ModeInfo
{
    Mode        mode;
    const char *name;
    int         pixel_width;     // the mode's own pixel domain
    int         bits_per_pixel;  // 0 for attr, which is not a plain index stream
    int         words_per_line;
    bool        half_rate;       // decoded by pixel doubling to OUT_WIDTH
    int         usable_entries;  // CLUT entries a pixel code can reach
};

inline constexpr ModeInfo MODES[] = {
    { Mode::C4H,  "c4h",  320, 4,  80, true,  16 },
    { Mode::C2F,  "c2f",  640, 2,  80, false,  4 },
    { Mode::C4F,  "c4f",  640, 4, 160, false, 16 },
    { Mode::ATTR, "attr", 320, 0,  40, true,  16 },
};

inline constexpr int MODE_COUNT = static_cast<int>(sizeof(MODES) / sizeof(MODES[0]));

inline const ModeInfo *mode_info(Mode m)
{
    for (int i = 0; i < MODE_COUNT; i++)
    {
        if (MODES[i].mode == m)
        {
            return &MODES[i];
        }
    }
    return nullptr;
}

inline const ModeInfo *mode_by_name(const char *name)
{
    for (int i = 0; i < MODE_COUNT; i++)
    {
        if (std::strcmp(MODES[i].name, name) == 0)
        {
            return &MODES[i];
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// In-memory stream
// ---------------------------------------------------------------------------

using Load = ClutColor::PaletteLoad;

struct Span
{
    uint16_t x      = 0;   // in OUT_WIDTH pixels: spans are a COMPOSITOR overlay
    uint16_t length = 0;
    Rgb444   colour = 0;
};

struct Line
{
    std::vector<Load>     loads;
    std::vector<Span>     spans;
    std::vector<uint16_t> words;
};

struct Stream
{
    Mode              mode = Mode::C4F;
    int               k    = CLUT_SIZE;
    int               n    = 0;
    std::vector<Line> lines;

    const ModeInfo &info() const { return *mode_info(mode); }
};

// ---------------------------------------------------------------------------
// Serialisation.  The writer does not validate — the corruption tests in
// `make check` need to be able to write a stream that breaks the rules.
// ---------------------------------------------------------------------------

inline void put_u8(std::vector<uint8_t> &v, int b)
{
    v.push_back(static_cast<uint8_t>(b & 0xFF));
}

inline void put_be16(std::vector<uint8_t> &v, int w)
{
    v.push_back(static_cast<uint8_t>((w >> 8) & 0xFF));
    v.push_back(static_cast<uint8_t>(w & 0xFF));
}

inline std::vector<uint8_t> stream_bytes(const Stream &s)
{
    const ModeInfo      &mi = s.info();
    std::vector<uint8_t> out;

    out.push_back('C');
    out.push_back('L');
    out.push_back('U');
    out.push_back('T');
    put_u8(out, VERSION);
    put_u8(out, static_cast<int>(s.mode));
    put_be16(out, mi.pixel_width);
    put_be16(out, HEIGHT);
    put_u8(out, s.k);
    put_u8(out, s.n);
    put_be16(out, mi.words_per_line);

    for (const Line &line : s.lines)
    {
        put_u8(out, static_cast<int>(line.loads.size()));
        for (const Load &l : line.loads)
        {
            put_u8(out, l.index);
            put_be16(out, l.colour);
        }
        put_u8(out, static_cast<int>(line.spans.size()));
        for (const Span &sp : line.spans)
        {
            put_be16(out, sp.x);
            put_be16(out, sp.length);
            put_be16(out, sp.colour);
        }
        for (uint16_t w : line.words)
        {
            put_be16(out, w);
        }
    }
    return out;
}

inline bool write_bytes(const char *path, const std::vector<uint8_t> &data)
{
    std::FILE *f = std::fopen(path, "wb");
    if (f == nullptr)
    {
        return false;
    }
    const bool ok = data.empty() || std::fwrite(data.data(), 1, data.size(), f) == data.size();
    std::fclose(f);
    return ok;
}

inline bool write_stream(const char *path, const Stream &s)
{
    return write_bytes(path, stream_bytes(s));
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

// A cursor that refuses to run off the end, so a truncated file is a clean
// error naming the line rather than a crash.
class Reader
{
public:
    Reader(const std::vector<uint8_t> &data) : data_(data) {}

    bool u8(int &out)
    {
        if (pos_ + 1 > data_.size())
        {
            return false;
        }
        out = data_[pos_];
        pos_ += 1;
        return true;
    }

    bool be16(int &out)
    {
        if (pos_ + 2 > data_.size())
        {
            return false;
        }
        out = (data_[pos_] << 8) | data_[pos_ + 1];
        pos_ += 2;
        return true;
    }

    size_t remaining() const { return data_.size() - pos_; }
    size_t position() const { return pos_; }

private:
    const std::vector<uint8_t> &data_;
    size_t                      pos_ = 0;
};

// Structural parse only: shape errors (bad magic, truncation, a mode id that
// does not exist) are reported here, rule violations by validate_stream.
inline bool parse_stream(const std::vector<uint8_t> &data, Stream &out, std::string &error)
{
    Reader reader(data);

    if (data.size() < 14 || data[0] != 'C' || data[1] != 'L' || data[2] != 'U' || data[3] != 'T')
    {
        error = "not a .clut file (bad magic)";
        return false;
    }

    int magic0 = 0;
    int magic1 = 0;
    int magic2 = 0;
    int magic3 = 0;
    reader.u8(magic0);
    reader.u8(magic1);
    reader.u8(magic2);
    reader.u8(magic3);

    int version = 0;
    int mode_id = 0;
    int width   = 0;
    int height  = 0;
    int k       = 0;
    int n       = 0;
    int words   = 0;
    if (!reader.u8(version) || !reader.u8(mode_id) || !reader.be16(width) || !reader.be16(height)
        || !reader.u8(k) || !reader.u8(n) || !reader.be16(words))
    {
        error = "header truncated";
        return false;
    }
    if (version != VERSION)
    {
        error = "unsupported version " + std::to_string(version);
        return false;
    }
    if (mode_id >= MODE_COUNT || mode_info(static_cast<Mode>(mode_id)) == nullptr)
    {
        error = "unknown mode id " + std::to_string(mode_id);
        return false;
    }

    out.mode                = static_cast<Mode>(mode_id);
    const ModeInfo &mi      = out.info();
    out.k                   = k;
    out.n                   = n;

    if (width != mi.pixel_width)
    {
        error = "header width " + std::to_string(width) + " disagrees with mode " + mi.name;
        return false;
    }
    if (height != HEIGHT)
    {
        error = "header height " + std::to_string(height) + " is not " + std::to_string(HEIGHT);
        return false;
    }
    if (words != mi.words_per_line)
    {
        error = "header words_per_line " + std::to_string(words) + " disagrees with mode "
                + mi.name;
        return false;
    }

    out.lines.assign(HEIGHT, Line());
    for (int y = 0; y < HEIGHT; y++)
    {
        Line &line       = out.lines[y];
        int   load_count = 0;
        if (!reader.u8(load_count))
        {
            error = "line " + std::to_string(y) + ": truncated before load count";
            return false;
        }
        line.loads.resize(static_cast<size_t>(load_count));
        for (int i = 0; i < load_count; i++)
        {
            int index  = 0;
            int colour = 0;
            if (!reader.u8(index) || !reader.be16(colour))
            {
                error = "line " + std::to_string(y) + ": truncated in CLUT loads";
                return false;
            }
            line.loads[static_cast<size_t>(i)].index  = static_cast<uint8_t>(index);
            line.loads[static_cast<size_t>(i)].colour = static_cast<Rgb444>(colour);
        }

        int span_count = 0;
        if (!reader.u8(span_count))
        {
            error = "line " + std::to_string(y) + ": truncated before span count";
            return false;
        }
        line.spans.resize(static_cast<size_t>(span_count));
        for (int i = 0; i < span_count; i++)
        {
            int x      = 0;
            int length = 0;
            int colour = 0;
            if (!reader.be16(x) || !reader.be16(length) || !reader.be16(colour))
            {
                error = "line " + std::to_string(y) + ": truncated in spans";
                return false;
            }
            line.spans[static_cast<size_t>(i)].x      = static_cast<uint16_t>(x);
            line.spans[static_cast<size_t>(i)].length = static_cast<uint16_t>(length);
            line.spans[static_cast<size_t>(i)].colour = static_cast<Rgb444>(colour);
        }

        line.words.resize(static_cast<size_t>(mi.words_per_line));
        for (int i = 0; i < mi.words_per_line; i++)
        {
            int w = 0;
            if (!reader.be16(w))
            {
                error = "line " + std::to_string(y) + ": expected "
                        + std::to_string(mi.words_per_line) + " pixel words, file ended after "
                        + std::to_string(i);
                return false;
            }
            line.words[static_cast<size_t>(i)] = static_cast<uint16_t>(w);
        }
    }

    if (reader.remaining() != 0)
    {
        error = "trailing garbage: " + std::to_string(reader.remaining()) + " bytes after line "
                + std::to_string(HEIGHT - 1);
        return false;
    }
    return true;
}

// The §3 rules.  `warning` collects the things DESIGN.md says to flag rather
// than reject (c4f's word budget).
inline bool validate_stream(const Stream &s, std::string &error, std::string &warning)
{
    const ModeInfo &mi = s.info();

    if (s.k < 1 || s.k > CLUT_SIZE)
    {
        error = "header K " + std::to_string(s.k) + " out of range 1.." + std::to_string(CLUT_SIZE);
        return false;
    }
    if (s.n < 0 || s.n > 255)
    {
        error = "header N " + std::to_string(s.n) + " out of range";
        return false;
    }
    if (static_cast<int>(s.lines.size()) != HEIGHT)
    {
        error = "stream has " + std::to_string(s.lines.size()) + " lines, not "
                + std::to_string(HEIGHT);
        return false;
    }

    const int budget_words = mi.words_per_line + SPAN_WORDS * s.n;
    if (budget_words > VIDCMD_WORDS_PER_LINE_CAP)
    {
        warning = std::string("mode ") + mi.name + " with N=" + std::to_string(s.n) + " needs "
                  + std::to_string(budget_words) + " words/line, over the "
                  + std::to_string(VIDCMD_WORDS_PER_LINE_CAP) + "-word cap";
        if (s.mode != Mode::C4F)
        {
            error = warning;
            return false;
        }
    }

    for (int y = 0; y < HEIGHT; y++)
    {
        const Line &line  = s.lines[static_cast<size_t>(y)];
        const int   limit = (y == 0) ? CLUT_SIZE : s.k;
        const std::string where = "line " + std::to_string(y) + ": ";

        if (static_cast<int>(line.loads.size()) > limit)
        {
            error = where + "load_count " + std::to_string(line.loads.size()) + " exceeds "
                    + std::to_string(limit);
            return false;
        }
        bool seen[CLUT_SIZE] = { false };
        for (const Load &l : line.loads)
        {
            if (l.index >= CLUT_SIZE)
            {
                error = where + "CLUT index " + std::to_string(l.index) + " out of range";
                return false;
            }
            if (seen[l.index])
            {
                error = where + "CLUT index " + std::to_string(l.index) + " loaded twice";
                return false;
            }
            seen[l.index] = true;
            if ((l.colour & 0xF000) != 0)
            {
                error = where + "CLUT colour has bits above R4G4B4";
                return false;
            }
        }

        if (static_cast<int>(line.spans.size()) > s.n)
        {
            error = where + "span_count " + std::to_string(line.spans.size()) + " exceeds N="
                    + std::to_string(s.n);
            return false;
        }
        int previous_end = -SPAN_MIN_GAP;
        for (const Span &sp : line.spans)
        {
            if (sp.length < SPAN_MIN_LENGTH)
            {
                error = where + "span length " + std::to_string(sp.length) + " below minimum "
                        + std::to_string(SPAN_MIN_LENGTH);
                return false;
            }
            if (static_cast<int>(sp.x) + static_cast<int>(sp.length) > OUT_WIDTH)
            {
                error = where + "span [" + std::to_string(sp.x) + ","
                        + std::to_string(sp.x + sp.length) + ") runs past "
                        + std::to_string(OUT_WIDTH);
                return false;
            }
            if (static_cast<int>(sp.x) < previous_end + SPAN_MIN_GAP)
            {
                error = where + "span at x=" + std::to_string(sp.x)
                        + " overlaps or crowds the previous span ending at "
                        + std::to_string(previous_end) + " (minimum gap "
                        + std::to_string(SPAN_MIN_GAP) + ")";
                return false;
            }
            if ((sp.colour & 0xF000) != 0)
            {
                error = where + "span colour has bits above R4G4B4";
                return false;
            }
            previous_end = static_cast<int>(sp.x) + static_cast<int>(sp.length);
        }

        if (static_cast<int>(line.words.size()) != mi.words_per_line)
        {
            error = where + "has " + std::to_string(line.words.size()) + " pixel words, not "
                    + std::to_string(mi.words_per_line);
            return false;
        }
    }
    return true;
}

}  // namespace Clut
