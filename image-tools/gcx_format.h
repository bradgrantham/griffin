// gcx_format.h — the .gcx picture: 4bpp half rate with COMPOSITOR fix-ups, as a
// memory image ENGINE can walk
//
// Host side.  gcx_target.h is the layout a 68000 reads and holds the section
// list; this file holds the model a tool builds one from and the rules.
//
// WHAT A .gcx IS.  A 320 x 480 sixteen-colour picture (PIXEL's 4bpp half rate)
// whose every line may also carry
//
//   palette loads   three VIDCMD words each, executed in that line's HBLANK
//   held colours    SET cmp_color0 / cmp_color1, in HBLANK, on lines that use
//                   MASK records
//   fix-up records  COMPOSITOR records that replace stretches of the indexed
//                   picture with direct RGB444: LITERAL (one word, one exact
//                   320-density pixel), LITERAL + HOLD (two words, a flat span),
//                   MASK (two words, sixteen slots at 640 resolution, each
//                   passthrough or one of the two held colours) and
//                   RUN(passthrough) for the stretches left alone
//
// and the ENGINE descriptor list that delivers all of it, prebuilt.  The encoder
// ships the list it verified, so the file's timing is the timing that was checked.
//
// THE DELIVERY SCHEDULE, which is the format's one piece of timing design:
//
//   arm (vsync line)   preamble: sixteen palette entries, SET pix_base,
//                      SET pix_mode -- /RS clears mode and base at every vsync --
//                      then LINE 0's packet, which waits in the FIFO behind
//                      nothing because playback is gated by H_ACTIVE
//   WALK_LINES pacers  one wait_hblank each, down to the top of the frame
//   group g            [wait_hblank] line g's 80 PIXELS words, then line g+1's
//                      VIDCMD packet
//   stop
//
// A line's packet is therefore in the FIFO a whole line before it plays, and its
// PIXELS words start arriving 9 SYSCLK after the HBLANK edge against a preload
// deadline of 80.  That only works because EVERY LINE IS FRAMED AT EXACTLY 640
// SLOTS: the next line's words sit behind this line's last record, and a line
// that came up short would let them start early.  validate_picture() checks it
// with descriptor.h's own slot planner.
//
// A LINE NEVER ENDS IN A STREAM RUN.  The next line's leading palette SETs are
// prefetched behind it and a stream run would execute them in active video, over
// passthrough pixels.
//
// One group costs 208 + 14 d + 2 V SYSCLK for V VIDCMD words in d descriptors (at
// the cost model's arbitration of 4), against a 444-SYSCLK line.  82 words is the
// most that still fits at arbitration 8, and it is the default budget.

#pragma once

#include "clut_color.h"
#include "gcx_target.h"

#include "super-engine/descriptor.h"

#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

namespace Gcx
{

namespace se = SuperEngine;

using ClutColor::PaletteLoad;
using ClutColor::Rgb444;

inline constexpr int PALETTE_SIZE   = 16;
inline constexpr int PIXEL_WIDTH    = 320;
inline constexpr int OUT_WIDTH      = 640;
inline constexpr int WORDS_PER_LINE = 80;
inline constexpr int HEIGHT         = 480;

// The vsync ISR arms inside line V_SYNC_START; lines 491..524 are 34 HBLANK edges
// before line 0's group (firmware/rom.cpp VBLANK_WALK_LINES).
inline constexpr int WALK_LINES = 34;

inline constexpr int PREAMBLE_WORDS =
    PALETTE_SIZE * static_cast<int>(se::PALETTE_WORDS_PER_ENTRY) + 2;   // 50

// The per-line VIDCMD budget, in words.  See the header comment.
inline constexpr int DEFAULT_LINE_WORDS = 82;

static_assert(PIXEL_WIDTH == static_cast<int>(GcxTarget::PIXEL_WIDTH));
static_assert(WORDS_PER_LINE == static_cast<int>(GcxTarget::WORDS_PER_LINE));
static_assert(WORDS_PER_LINE == static_cast<int>(se::PIXELS_WORDS_4BPP_HALF));
static_assert(OUT_WIDTH == static_cast<int>(se::H_ACTIVE));
static_assert(HEIGHT == static_cast<int>(se::V_ACTIVE));
static_assert(GcxTarget::DESC_WORDS == se::DESC_WORDS);
static_assert(PREAMBLE_WORDS + DEFAULT_LINE_WORDS <= static_cast<int>(se::VIDCMD_FIFO_WORDS));

// One line as a tool holds it.  `records` is everything after the palette loads:
// any held-colour SETs, then the line's active-video records, which must tile
// exactly 640 slots.
struct Line
{
    std::vector<PaletteLoad> loads;
    std::vector<uint16_t>    records;
    std::vector<uint16_t>    pixels;   // WORDS_PER_LINE, MSB-first indices
};

struct Picture
{
    Rgb444            initial[PALETTE_SIZE] = {};
    std::vector<Line> lines;
};

// A line's packet exactly as COMPOSITOR reads it.
inline std::vector<uint16_t> line_packet(const Line &line)
{
    std::vector<uint16_t> out;
    for (const PaletteLoad &l : line.loads)
    {
        for (uint16_t w : se::vidcmd_palette_entry(l.index, l.colour))
        {
            out.push_back(w);
        }
    }
    out.insert(out.end(), line.records.begin(), line.records.end());
    return out;
}

// The rules a picture must meet before a list is built from it.
inline bool validate_picture(const Picture &p, std::string &error)
{
    char buf[160];
    if (p.lines.size() != static_cast<size_t>(HEIGHT))
    {
        error = "a .gcx picture has exactly 480 lines";
        return false;
    }
    for (size_t y = 0; y < p.lines.size(); y++)
    {
        const Line &line = p.lines[y];
        if (line.pixels.size() != static_cast<size_t>(WORDS_PER_LINE))
        {
            std::snprintf(buf, sizeof(buf), "line %zu has %zu pixel words", y, line.pixels.size());
            error = buf;
            return false;
        }
        if (y == 0 && !line.loads.empty())
        {
            error = "line 0's palette is the preamble's; it carries no loads";
            return false;
        }
        for (const PaletteLoad &l : line.loads)
        {
            if (l.index >= PALETTE_SIZE || (l.colour & 0xF000) != 0)
            {
                std::snprintf(buf, sizeof(buf), "line %zu has a malformed palette load", y);
                error = buf;
                return false;
            }
        }
        if (line.records.empty())
        {
            std::snprintf(buf, sizeof(buf), "line %zu has no records", y);
            error = buf;
            return false;
        }
        // Step by records: a MASK's data word is not a lead word and must not
        // be decoded as one.
        size_t lead = 0;
        for (size_t i = 0; i < line.records.size();
             i += se::vidcmd_record_words(line.records[i]))
        {
            lead = i;
        }
        const uint16_t last = line.records[lead];
        if (se::vidcmd_type_of(last) == se::VidcmdType::RUN
            && se::vidcmd_run_kind(last) == se::VidcmdRunKind::STREAM)
        {
            std::snprintf(buf, sizeof(buf), "line %zu ends in a STREAM RUN", y);
            error = buf;
            return false;
        }
        const std::vector<uint16_t> packet = line_packet(line);
        const se::VidcmdSlotPlan    plan   = se::vidcmd_plan_line(packet);
        if (plan.slots != se::H_ACTIVE || plan.starved != 0 || plan.stream_leftovers != 0
            || plan.mask_stalls != 0)
        {
            std::snprintf(buf, sizeof(buf),
                          "line %zu plans to %u slots (must be %u), %u starved, %u stream "
                          "leftovers, %u mask stalls",
                          y, plan.slots, se::H_ACTIVE, plan.starved, plan.stream_leftovers,
                          plan.mask_stalls);
            error = buf;
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Serialisation
// ---------------------------------------------------------------------------

struct BuildOptions
{
    // 1 is the format.  0 puts a line's packet in its own group, behind its
    // PIXELS words, and exists only so the checker can be shown to catch it.
    int lookahead = 1;
};

inline void put_be16(std::vector<uint8_t> &v, uint32_t w)
{
    v.push_back(static_cast<uint8_t>((w >> 8) & 0xFF));
    v.push_back(static_cast<uint8_t>(w & 0xFF));
}

inline void put_be32(std::vector<uint8_t> &v, uint32_t w)
{
    put_be16(v, w >> 16);
    put_be16(v, w & 0xFFFF);
}

inline void poke_be16(std::vector<uint8_t> &v, size_t at, uint32_t w)
{
    v[at]     = static_cast<uint8_t>((w >> 8) & 0xFF);
    v[at + 1] = static_cast<uint8_t>(w & 0xFF);
}

// Does not validate: validate_picture() is the rule book, and the negative
// controls need to write files that break it.
inline std::vector<uint8_t> picture_bytes(const Picture &p, const BuildOptions &options = {})
{
    const uint32_t height = static_cast<uint32_t>(p.lines.size());

    // The VIDCMD stream: preamble, then the packets in line order.
    std::vector<uint16_t> vidcmd;
    for (int i = 0; i < PALETTE_SIZE; i++)
    {
        for (uint16_t w : se::vidcmd_palette_entry(static_cast<uint32_t>(i), p.initial[i]))
        {
            vidcmd.push_back(w);
        }
    }
    vidcmd.push_back(se::vidcmd_set(se::SET_PIX_BASE, 0));
    vidcmd.push_back(se::vidcmd_set(se::SET_PIX_MODE, se::pixel_mode(se::PIXEL_DEPTH_4BPP, true)));

    std::vector<uint32_t> packet_at(height, 0);
    std::vector<uint32_t> packet_words(height, 0);
    uint32_t              longest = 0;
    for (uint32_t y = 0; y < height; y++)
    {
        const std::vector<uint16_t> packet = line_packet(p.lines[y]);
        packet_at[y]    = static_cast<uint32_t>(vidcmd.size());
        packet_words[y] = static_cast<uint32_t>(packet.size());
        longest         = (packet_words[y] > longest) ? packet_words[y] : longest;
        vidcmd.insert(vidcmd.end(), packet.begin(), packet.end());
    }

    // Descriptors, with sources relative to their own section for now.
    enum class Section { NONE, VIDCMD, PIXELS };
    struct Pending
    {
        se::Descriptor d;
        Section        section = Section::NONE;
    };
    std::vector<Pending> list;

    auto stream = [&](Section section, uint32_t word, uint32_t words, uint8_t destination,
                      bool wait_first)
    {
        uint32_t done = 0;
        while (done < words)
        {
            const uint32_t left = words - done;
            const uint32_t take = (left > se::DESC_MAX_COUNT) ? se::DESC_MAX_COUNT : left;
            Pending        e;
            e.d.src         = (word + done) * 2;
            e.d.count       = static_cast<uint16_t>(take);
            e.d.destination = destination;
            e.d.wait_hblank = wait_first && done == 0;
            e.section       = section;
            list.push_back(e);
            done += take;
        }
    };
    auto pacer = [&](bool stop_after)
    {
        Pending e;
        e.d.src         = 0;   // the file's own first word: read, strobed nowhere
        e.d.count       = 1;
        e.d.destination = se::DEST_NONE;
        e.d.wait_hblank = true;
        e.d.stop_after  = stop_after;
        list.push_back(e);
    };

    const uint32_t bootstrap =
        (options.lookahead > 0 && height > 0) ? packet_words[0] : 0;
    stream(Section::VIDCMD, 0, PREAMBLE_WORDS + bootstrap, se::DEST_VIDCMD_FIFO_W, false);
    for (int i = 0; i < WALK_LINES; i++)
    {
        pacer(false);
    }
    for (uint32_t y = 0; y < height; y++)
    {
        stream(Section::PIXELS, y * WORDS_PER_LINE, WORDS_PER_LINE, se::DEST_PIXELS_FIFO_W, true);
        const uint32_t carried = y + static_cast<uint32_t>(options.lookahead);
        if (options.lookahead == 0 || carried < height)
        {
            stream(Section::VIDCMD, packet_at[carried], packet_words[carried],
                   se::DEST_VIDCMD_FIFO_W, false);
        }
    }
    pacer(true);

    const uint32_t desc_offset   = GcxTarget::HEADER_WORDS * 2;
    const uint32_t vidcmd_offset = desc_offset + static_cast<uint32_t>(list.size()) * se::DESC_BYTES;
    const uint32_t pixels_offset = vidcmd_offset + static_cast<uint32_t>(vidcmd.size()) * 2;
    const uint32_t index_offset  = pixels_offset + height * WORDS_PER_LINE * 2;
    const uint32_t file_bytes    = index_offset + height * 2;

    std::vector<uint8_t> out;
    out.reserve(file_bytes);
    put_be32(out, GcxTarget::MAGIC);
    put_be16(out, GcxTarget::VERSION);
    put_be16(out, GcxTarget::HEADER_WORDS);
    put_be16(out, PIXEL_WIDTH);
    put_be16(out, height);
    put_be16(out, WORDS_PER_LINE);
    put_be16(out, se::pixel_mode(se::PIXEL_DEPTH_4BPP, true));
    put_be32(out, file_bytes);
    put_be32(out, desc_offset);
    put_be32(out, static_cast<uint32_t>(list.size()));
    put_be32(out, vidcmd_offset);
    put_be32(out, static_cast<uint32_t>(vidcmd.size()));
    put_be32(out, pixels_offset);
    put_be32(out, index_offset);
    put_be16(out, PREAMBLE_WORDS);
    put_be16(out, WALK_LINES);
    put_be16(out, 0);   // HDR_GROUP_SYSCLK, filled in after verification
    put_be16(out, 0);   // HDR_FIFO_HIGH, likewise
    put_be16(out, longest);
    while (out.size() < desc_offset)
    {
        put_be16(out, 0);
    }

    for (const Pending &e : list)
    {
        se::Descriptor d = e.d;
        if (e.section == Section::VIDCMD)
        {
            d.src += vidcmd_offset;
        }
        else if (e.section == Section::PIXELS)
        {
            d.src += pixels_offset;
        }
        const se::DescriptorWords w = se::encode_descriptor(d);
        for (uint32_t i = 0; i < se::DESC_WORDS; i++)
        {
            put_be16(out, w.w[i]);
        }
    }
    for (uint16_t w : vidcmd)
    {
        put_be16(out, w);
    }
    for (const Line &line : p.lines)
    {
        for (uint16_t w : line.pixels)
        {
            put_be16(out, w);
        }
    }
    for (uint32_t y = 0; y < height; y++)
    {
        put_be16(out, packet_words[y]);
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

// The file as the 68000 sees it: big-endian bytes to host words.
inline std::vector<uint16_t> file_words(const std::vector<uint8_t> &data)
{
    std::vector<uint16_t> words(data.size() / 2, 0);
    for (size_t i = 0; i < words.size(); i++)
    {
        words[i] = static_cast<uint16_t>((data[i * 2] << 8) | data[i * 2 + 1]);
    }
    return words;
}

}   // namespace Gcx
