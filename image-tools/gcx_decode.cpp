// gcx_decode.cpp — a .gcx file -> the picture it shows, by an independent reading
//
// Build: see the Makefile in this directory.
// Usage: gcx_decode [--quiet] <in.gcx> [out.png]
//
// The second opinion on fixup_encode.  It shares NOTHING with the encoder or the
// suite model beyond gcx_target.h's header offsets: the record bit patterns
// below are written out again from griffin-video-datasheet.html §3.2, and the
// picture is painted from them directly, with no FIFOs and no clocks.  `make
// check` requires its PNG to equal the encoder's preview and the suite model's
// render byte for byte.
//
// It is also the format's validator, and it is strict about the subset of
// COMPOSITOR the format uses.  A line's packet is
//
//   palette loads   whole three-word entries (SET clut_index, SET clut_data,
//                   SET filler with the same value), executed in HBLANK
//   held colours    SET cmp_color0 / SET cmp_color1, also in HBLANK; they
//                   persist from line to line and reset at vsync
//   records         RUN(passthrough, n), LITERAL, HOLD (only behind a LITERAL,
//                   count >= 2) and MASK (two words, sixteen slots, each
//                   passthrough, colour 0 or colour 1, slot 0 always colour 0),
//                   tiling exactly 640 slots on the 2-slot grid
//
// and anything else -- any other SET, a STREAM RUN, a held-colour RUN, a reserved
// MASK dibit, an odd width -- is rejected, because the encoder does not emit it and a file that
// carries it was not made by the rules this decoder can vouch for.
//
// The descriptor list is checked for SHAPE, not timing (timing is the suite
// model's): the VIDCMD and PIXELS descriptors must between them deliver each
// section exactly once and in order, the list must end on a stop, and it must
// wait for exactly walk + height + 1 HBLANK edges.

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#include "clut_color.h"
#include "gcx_target.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using ClutColor::Rgb444;
using ClutColor::nibble_to_8;
using ClutColor::rgb444_b;
using ClutColor::rgb444_g;
using ClutColor::rgb444_r;

namespace
{

constexpr int OUT_WIDTH    = 640;
constexpr int PALETTE_SIZE = 16;

// VIDCMD word decode, datasheet §3.2.
constexpr bool     is_set(uint16_t w) { return (w & 0x8000u) != 0; }
constexpr unsigned set_target(uint16_t w) { return (w >> 12) & 0x7u; }
constexpr unsigned set_value(uint16_t w) { return w & 0x0FFFu; }
constexpr bool     is_mask(uint16_t w) { return (w & 0xC000u) == 0x4000u; }
constexpr unsigned run_source(uint16_t w) { return (w >> 12) & 0x3u; }
constexpr bool     is_literal(uint16_t w) { return (w & 0xC000u) == 0 && run_source(w) == 3; }
constexpr bool     is_hold(uint16_t w) { return (w & 0xC000u) == 0 && run_source(w) != 3 && (w & 0x0800u) == 0; }
constexpr bool     is_plain_run(uint16_t w) { return (w & 0xCC00u) == 0x0C00u && run_source(w) != 3; }
constexpr unsigned run_count(uint16_t w)
{
    const unsigned n = (~w) & 0x3FFu;
    return (n == 0) ? 1024u : n;
}

constexpr unsigned TARGET_COLOUR1    = 0;
constexpr unsigned TARGET_COLOUR0    = 1;
constexpr unsigned TARGET_CLUT_INDEX = 2;
constexpr unsigned TARGET_CLUT_DATA  = 3;
constexpr unsigned TARGET_PIX_BASE   = 4;
constexpr unsigned TARGET_PIX_MODE   = 5;
constexpr unsigned TARGET_FILLER     = 7;

constexpr unsigned DEST_NONE   = 0;
constexpr unsigned DEST_PIXELS = 1;
constexpr unsigned DEST_VIDCMD = 2;

bool read_bytes(const char *path, std::vector<uint8_t> &out)
{
    std::FILE *f = std::fopen(path, "rb");
    if (f == nullptr)
    {
        return false;
    }
    uint8_t buffer[65536];
    size_t  got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), f)) > 0)
    {
        out.insert(out.end(), buffer, buffer + got);
    }
    std::fclose(f);
    return true;
}

std::string text(const char *format, unsigned a = 0, unsigned b = 0, unsigned c = 0)
{
    char buf[200];
    std::snprintf(buf, sizeof(buf), format, a, b, c);
    return buf;
}

// One whole palette entry at words[at..at+2], applied to `palette`.
bool take_entry(const uint16_t *words, Rgb444 *palette)
{
    if (!is_set(words[0]) || set_target(words[0]) != TARGET_CLUT_INDEX
        || !is_set(words[1]) || set_target(words[1]) != TARGET_CLUT_DATA
        || !is_set(words[2]) || set_target(words[2]) != TARGET_FILLER
        || set_value(words[1]) != set_value(words[2]) || set_value(words[0]) >= PALETTE_SIZE)
    {
        return false;
    }
    palette[set_value(words[0])] = static_cast<Rgb444>(set_value(words[1]));
    return true;
}

bool decode(const std::vector<uint16_t> &file, std::vector<Rgb444> &frame, unsigned &height,
            std::string &error)
{
    GcxTarget::Picture picture;
    const char        *why = nullptr;
    if (!GcxTarget::open_picture(file.data(), static_cast<uint32_t>(file.size()), &picture, &why))
    {
        error = why;
        return false;
    }
    height = picture.height;

    const uint16_t *h            = file.data();
    const uint32_t  file_bytes   = static_cast<uint32_t>(file.size()) * 2;
    const uint32_t  desc_offset  = GcxTarget::long_at(h, GcxTarget::HDR_DESC_OFFSET);
    const uint32_t  vidcmd_at    = GcxTarget::long_at(h, GcxTarget::HDR_VIDCMD_OFFSET);
    const uint32_t  vidcmd_words = GcxTarget::long_at(h, GcxTarget::HDR_VIDCMD_WORDS);
    const uint32_t  pixels_at    = GcxTarget::long_at(h, GcxTarget::HDR_PIXELS_OFFSET);
    const uint32_t  index_at     = GcxTarget::long_at(h, GcxTarget::HDR_INDEX_OFFSET);
    const unsigned  preamble     = h[GcxTarget::HDR_PREAMBLE_WORDS];
    const unsigned  walk         = h[GcxTarget::HDR_WALK_LINES];
    const uint32_t  pixel_words  = height * GcxTarget::WORDS_PER_LINE;

    if (((vidcmd_at | pixels_at | index_at) & 1u) != 0
        || vidcmd_at != desc_offset + picture.table_bytes()
        || pixels_at != vidcmd_at + vidcmd_words * 2
        || index_at != pixels_at + pixel_words * 2
        || file_bytes != index_at + height * 2)
    {
        error = "the sections do not tile the file";
        return false;
    }
    if (h[GcxTarget::HDR_PIX_MODE] != (2u | 4u))
    {
        error = "pix_mode is not 4bpp half rate";
        return false;
    }

    const uint16_t *vidcmd = file.data() + vidcmd_at / 2;
    const uint16_t *pixels = file.data() + pixels_at / 2;
    const uint16_t *index  = file.data() + index_at / 2;

    // The list's shape: each section delivered once, in order.
    {
        const uint16_t *table  = picture.descriptors();
        uint32_t        v_done = 0;
        uint32_t        p_done = 0;
        unsigned        waits  = 0;
        for (uint32_t i = 0; i < picture.desc_count; i++)
        {
            const uint16_t *d     = table + i * GcxTarget::DESC_WORDS;
            const bool      wait  = (d[0] & 0x8000u) != 0;
            const bool      stop  = (d[0] & 0x4000u) != 0;
            const unsigned  count = ((d[0] >> 9) & 0x1Fu) + 1;
            const unsigned  dest  = d[0] & 0x7u;
            const uint32_t  src   = GcxTarget::long_at(d, 1);
            if ((d[0] & 0x01F8u) != 0 || (src & 1u) != 0 || src + count * 2 > file_bytes)
            {
                error = text("descriptor %u is malformed", i);
                return false;
            }
            waits += wait ? 1u : 0u;
            if (stop != (i + 1 == picture.desc_count))
            {
                error = text("descriptor %u: the list must stop at its last descriptor and "
                             "nowhere else", i);
                return false;
            }
            if (dest == DEST_VIDCMD)
            {
                if (src != vidcmd_at + v_done * 2)
                {
                    error = text("descriptor %u delivers VIDCMD words out of order", i);
                    return false;
                }
                v_done += count;
            }
            else if (dest == DEST_PIXELS)
            {
                if (src != pixels_at + p_done * 2)
                {
                    error = text("descriptor %u delivers PIXELS words out of order", i);
                    return false;
                }
                p_done += count;
            }
            else if (dest != DEST_NONE)
            {
                error = text("descriptor %u has a destination a picture does not use", i);
                return false;
            }
        }
        if (v_done != vidcmd_words || p_done != pixel_words)
        {
            error = text("the list delivers %u VIDCMD and %u PIXELS words, the file holds %u",
                         v_done, p_done, vidcmd_words);
            return false;
        }
        if (waits != walk + height + 1)
        {
            error = text("the list waits for %u HBLANK edges, expected %u", waits,
                         walk + height + 1);
            return false;
        }
    }

    // The preamble: every entry, then base and mode.
    Rgb444 palette[PALETTE_SIZE] = {};
    bool   loaded[PALETTE_SIZE]  = {};
    if (preamble != PALETTE_SIZE * 3 + 2 || preamble > vidcmd_words)
    {
        error = "the preamble is not sixteen entries, pix_base and pix_mode";
        return false;
    }
    for (int i = 0; i < PALETTE_SIZE; i++)
    {
        if (!take_entry(vidcmd + i * 3, palette))
        {
            error = text("preamble entry %u is malformed", static_cast<unsigned>(i));
            return false;
        }
        loaded[set_value(vidcmd[i * 3])] = true;
    }
    for (int i = 0; i < PALETTE_SIZE; i++)
    {
        if (!loaded[i])
        {
            error = text("the preamble never loads entry %u", static_cast<unsigned>(i));
            return false;
        }
    }
    const uint16_t base_word = vidcmd[PALETTE_SIZE * 3];
    const uint16_t mode_word = vidcmd[PALETTE_SIZE * 3 + 1];
    if (!is_set(base_word) || set_target(base_word) != TARGET_PIX_BASE || set_value(base_word) != 0
        || !is_set(mode_word) || set_target(mode_word) != TARGET_PIX_MODE
        || set_value(mode_word) != h[GcxTarget::HDR_PIX_MODE])
    {
        error = "the preamble does not end in SET pix_base 0, SET pix_mode";
        return false;
    }

    // /RS at vsync leaves colour 1 white and colour 0 black.
    Rgb444 colour0 = 0x000;
    Rgb444 colour1 = 0xFFF;

    frame.assign(static_cast<size_t>(OUT_WIDTH) * height, 0);
    uint32_t at = preamble;
    for (unsigned y = 0; y < height; y++)
    {
        const unsigned  words  = index[y];
        const uint16_t *packet = vidcmd + at;
        if (at + words > vidcmd_words)
        {
            error = text("line %u's packet runs past the VIDCMD section", y);
            return false;
        }
        at += words;

        unsigned i = 0;
        unsigned entries = 0;
        while (i < words && is_set(packet[i]))
        {
            if (set_target(packet[i]) == TARGET_COLOUR0)
            {
                colour0 = static_cast<Rgb444>(set_value(packet[i]));
                i += 1;
                continue;
            }
            if (set_target(packet[i]) == TARGET_COLOUR1)
            {
                colour1 = static_cast<Rgb444>(set_value(packet[i]));
                i += 1;
                continue;
            }
            if (i + 3 > words || !take_entry(packet + i, palette))
            {
                error = text("line %u has a SET that is neither a whole palette entry nor a "
                             "held colour", y);
                return false;
            }
            i += 3;
            entries++;
        }
        if (y == 0 && entries != 0)
        {
            error = "line 0 carries palette loads; its palette is the preamble's";
            return false;
        }

        Rgb444        *row          = &frame[static_cast<size_t>(y) * OUT_WIDTH];
        const uint16_t *line_pixels = pixels + y * GcxTarget::WORDS_PER_LINE;
        unsigned        slot        = 0;
        bool            after_literal = false;
        Rgb444          kept        = 0;
        for (; i < words; i++)
        {
            const uint16_t w     = packet[i];
            unsigned       width = 0;
            if (is_set(w))
            {
                error = text("line %u word %u: a SET among the records", y, i);
                return false;
            }
            if (is_mask(w))
            {
                if (i + 1 >= words)
                {
                    error = text("line %u word %u: a MASK without its data word", y, i);
                    return false;
                }
                const uint16_t data = packet[++i];
                width = 16;
                for (unsigned j = 0; j < 16 && slot + j < OUT_WIDTH; j++)
                {
                    // Slot 0 has no bits: it is colour 0.  Slots 1..7 are the
                    // header's dibits from bit 13 down, 8..15 the data word's
                    // from bit 15 down.
                    unsigned dibit = 2;
                    if (j >= 8)
                    {
                        dibit = (data >> (14 - 2 * (j - 8))) & 3u;
                    }
                    else if (j >= 1)
                    {
                        dibit = (w >> (14 - 2 * j)) & 3u;
                    }
                    const unsigned s     = slot + j;
                    const unsigned group = s / 2;
                    if (dibit == 1)
                    {
                        error = text("line %u word %u: a reserved MASK dibit", y, i);
                        return false;
                    }
                    row[s] = (dibit == 2) ? colour0
                             : (dibit == 3) ? colour1
                             : palette[(line_pixels[group / 4] >> (12 - 4 * (group % 4))) & 0xFu];
                }
                after_literal = false;
            }
            else if (is_literal(w))
            {
                width = 2;
                kept  = static_cast<Rgb444>(w & 0x0FFFu);
                if (slot + width <= OUT_WIDTH)
                {
                    row[slot] = row[slot + 1] = kept;
                }
                after_literal = true;
            }
            else if (is_hold(w))
            {
                width = run_count(w);
                if (!after_literal || width < 2)
                {
                    error = text("line %u word %u: a HOLD that is not behind a LITERAL, or "
                                 "shorter than 2", y, i);
                    return false;
                }
                for (unsigned s = slot; s < slot + width && s < OUT_WIDTH; s++)
                {
                    row[s] = kept;
                }
                after_literal = false;
            }
            else if (is_plain_run(w) && run_source(w) == 0)
            {
                width = run_count(w);
                for (unsigned s = slot; s < slot + width && s < OUT_WIDTH; s++)
                {
                    const unsigned group = s / 2;
                    const unsigned code  =
                        (line_pixels[group / 4] >> (12 - 4 * (group % 4))) & 0xFu;
                    row[s] = palette[code];
                }
                after_literal = false;
            }
            else
            {
                error = text("line %u word %u: 0x%04X is not a record this format uses", y, i, w);
                return false;
            }
            if ((width & 1u) != 0)
            {
                error = text("line %u word %u: a record off the 2-slot grid", y, i);
                return false;
            }
            slot += width;
        }
        if (slot != OUT_WIDTH)
        {
            error = text("line %u's records cover %u slots, must be exactly %u", y, slot,
                         OUT_WIDTH);
            return false;
        }
    }
    if (at != vidcmd_words)
    {
        error = "VIDCMD words left over after the last line";
        return false;
    }
    return true;
}

}   // namespace

int main(int argc, char **argv)
{
    bool quiet = false;
    int  arg   = 1;
    if (arg < argc && std::strcmp(argv[arg], "--quiet") == 0)
    {
        quiet = true;
        arg++;
    }
    if (argc - arg < 1 || argc - arg > 2)
    {
        std::fprintf(stderr, "usage: %s [--quiet] <in.gcx> [out.png]\n", argv[0]);
        return 1;
    }

    std::vector<uint8_t> data;
    if (!read_bytes(argv[arg], data) || (data.size() & 1u) != 0)
    {
        std::fprintf(stderr, "%s: cannot read %s\n", argv[0], argv[arg]);
        return 1;
    }
    std::vector<uint16_t> file(data.size() / 2);
    for (size_t i = 0; i < file.size(); i++)
    {
        file[i] = static_cast<uint16_t>((data[i * 2] << 8) | data[i * 2 + 1]);
    }

    std::vector<Rgb444> frame;
    unsigned            height = 0;
    std::string         error;
    if (!decode(file, frame, height, error))
    {
        std::fprintf(stderr, "%s: %s\n", argv[arg], error.c_str());
        return 1;
    }

    if (argc - arg == 2)
    {
        std::vector<uint8_t> rgb(frame.size() * 3);
        for (size_t i = 0; i < frame.size(); i++)
        {
            rgb[i * 3 + 0] = nibble_to_8(rgb444_r(frame[i]));
            rgb[i * 3 + 1] = nibble_to_8(rgb444_g(frame[i]));
            rgb[i * 3 + 2] = nibble_to_8(rgb444_b(frame[i]));
        }
        if (stbi_write_png(argv[arg + 1], OUT_WIDTH, static_cast<int>(height), 3, rgb.data(),
                           OUT_WIDTH * 3) == 0)
        {
            std::fprintf(stderr, "%s: cannot write %s\n", argv[0], argv[arg + 1]);
            return 1;
        }
    }
    if (!quiet)
    {
        std::printf("%s: valid, 320x%u\n", argv[arg], height);
    }
    return 0;
}
