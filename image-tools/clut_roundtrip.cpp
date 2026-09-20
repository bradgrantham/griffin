// clut_roundtrip.cpp — encode, then render through the REAL suite model
//
// Build: see the Makefile in this directory.
// Usage: clut_roundtrip <in.gc4> <preview.png>
//
// The suite model (super-engine/render.cpp) is the semantics truth, and this is
// the harness that holds clut_encode to it.  It takes the encoder's .gc4 file,
// builds the DISPLAY LIST a program would build from it out of descriptor.h's own
// authoring helpers -- vidcmd_palette_entry() for each load, vidcmd_set() for the
// mode and base, vidcmd_run() for the line's one passthrough record -- feeds it
// through PixelUnit/CompositorUnit clock by clock, and requires the rendered
// frame to equal the encoder's --preview PNG PIXEL FOR PIXEL.  A disagreement is
// a bug in clut_encode or in the format, never in the suite.
//
// It replaces microham_roundtrip, which did the same job for micro-HAM and went
// with it; the structure is deliberately the same one.
//
// WHAT THE LIST LOOKS LIKE, and why the shape is part of the test:
//
//   vertical blanking   sixteen palette entries (48 words) + SET pix_base +
//                       SET pix_mode.  Fifty words is more than an HBLANK
//                       delivers, and vertical blanking has no pixel slots for a
//                       palette write to disturb, so this is where a full load
//                       belongs -- and /RS clears mode and base every frame, so
//                       they are re-sent from here too.
//   each line's HBLANK  that line's K loads, three words each, then one
//                       RUN(passthrough, 1).  COMPOSITOR holds the last source to
//                       the end of the line, so one slot paints 640 pixels.
//
//   the PIXELS words go in BEFORE the blanking runs, because PIX_PRELOAD reads
//   the line's first word PIXEL_FETCH_LEAD clocks before pixel 0 -- 16 clocks
//   inside the blanking, not at pixel 0.  A list that delivers later tiles a
//   word, and the model can see that now.
//
// Three of the model's counters are asserted zero for every frame, and each one
// is a real failure mode of this format: palette_unwritten_reads (a line showed
// an entry nobody ever loaded -- the RAM powers up random, so that is noise on
// screen), palette_bad_filler (a three-word entry whose third word was not the
// same value, which stores the wrong colour) and palette_disturbed_visible (a
// write whose four disturbed pixel clocks reached the DAC through passthrough).

#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

#include "gc4_format.h"

#include "super-engine/render.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace se = SuperEngine;

using ClutColor::PaletteLoad;
using ClutColor::nibble_to_8;
using ClutColor::rgb444_b;
using ClutColor::rgb444_g;
using ClutColor::rgb444_r;

// The mode byte the format hard-codes has to be the one descriptor.h builds, or
// the file would name a mode the chip does not implement.
static_assert(Gc4::PIX_MODE_4BPP_HALF
                  == static_cast<int>(se::pixel_mode(se::PIXEL_DEPTH_4BPP, true)),
              "gc4's pix_mode must be descriptor.h's 4bpp half rate");
static_assert(Gc4::WORDS_PER_LINE == static_cast<int>(se::PIXELS_WORDS_4BPP_HALF),
              "gc4's words/line must be the mode's record length");
static_assert(Gc4::PALETTE_SIZE == static_cast<int>(se::PALETTE_ENTRIES));
static_assert(Gc4::OUT_WIDTH == static_cast<int>(se::H_ACTIVE));

namespace
{

// Render the picture the way a display list would, on the model's own clocks.
std::vector<se::Rgb444> suite_render(const Gc4::Picture &picture, se::RenderStats &stats)
{
    se::PixelUnit      pix;
    se::CompositorUnit cmp;

    // Power-on first: the palette RAM comes up with the model's reproducible
    // random contents, so a list that forgets an entry paints noise rather than a
    // convenient black.  Then /RS, which is what vsync does and which clears
    // pix_mode, pix_base and pixel_skip but NEVER the RAM.
    pix.power_on_reset();
    cmp.power_on_reset();
    pix.reset();
    cmp.reset();
    pix.attach_stats(&stats);
    cmp.attach_stats(&stats);
    pix.set_counting(true);
    cmp.set_counting(true);

    const int height = picture.height;
    std::vector<se::Rgb444> frame(static_cast<size_t>(se::H_ACTIVE) * height, 0);

    // The frame preamble, in vertical blanking.
    for (int i = 0; i < Gc4::PALETTE_SIZE; i++)
    {
        const se::PaletteEntryWords e =
            se::vidcmd_palette_entry(static_cast<uint32_t>(i), picture.initial[i]);
        for (uint32_t w = 0; w < se::PALETTE_WORDS_PER_ENTRY; w++)
        {
            cmp.push_word(e.w[w]);
        }
    }
    cmp.push_word(se::vidcmd_set(se::SET_PIX_BASE, 0));
    cmp.push_word(se::vidcmd_set(se::SET_PIX_MODE,
                                 se::pixel_mode(se::PIXEL_DEPTH_4BPP, true)));
    se::run_blanking(pix, cmp, false);

    for (int y = 0; y < height; y++)
    {
        const Gc4::Line &line = picture.lines[static_cast<size_t>(y)];

        // PIXELS first: the preload reads word 0 sixteen clocks before pixel 0.
        for (int w = 0; w < Gc4::WORDS_PER_LINE; w++)
        {
            if (!pix.push_word(line.words[static_cast<size_t>(w)]))
            {
                std::fprintf(stderr, "PIXELS FIFO overflow at line %d\n", y);
            }
        }

        for (const PaletteLoad &l : line.loads)
        {
            const se::PaletteEntryWords e = se::vidcmd_palette_entry(l.index, l.colour);
            for (uint32_t w = 0; w < se::PALETTE_WORDS_PER_ENTRY; w++)
            {
                cmp.push_word(e.w[w]);
            }
        }
        cmp.push_word(se::vidcmd_run(se::RUN_SRC_PASSTHROUGH, 1));

        se::run_blanking(pix, cmp, true);
        cmp.set_pixel_position(static_cast<uint64_t>(y) * se::H_TOTAL);
        se::render_active_line(pix, cmp, &frame[static_cast<size_t>(y) * se::H_ACTIVE]);
    }
    return frame;
}

bool load_png(const char *path, std::vector<uint8_t> &out, int &width, int &height)
{
    int            comps = 0;
    unsigned char *image = stbi_load(path, &width, &height, &comps, 3);
    if (image == nullptr)
    {
        return false;
    }
    out.assign(image, image + static_cast<size_t>(width) * height * 3);
    stbi_image_free(image);
    return true;
}

}   // namespace

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        std::fprintf(stderr, "usage: %s <in.gc4> <preview.png>\n", argv[0]);
        return 1;
    }

    std::vector<uint8_t> data;
    if (!Gc4::read_bytes(argv[1], data))
    {
        std::fprintf(stderr, "%s: cannot read %s\n", argv[0], argv[1]);
        return 1;
    }

    Gc4::Picture picture;
    std::string  error;
    if (!Gc4::parse_picture(data, picture, error) || !Gc4::validate_picture(picture, error))
    {
        std::fprintf(stderr, "%s: %s\n", argv[1], error.c_str());
        return 1;
    }

    std::vector<uint8_t> preview;
    int                  pw = 0;
    int                  ph = 0;
    if (!load_png(argv[2], preview, pw, ph))
    {
        std::fprintf(stderr, "%s: cannot read %s\n", argv[0], argv[2]);
        return 1;
    }
    if (pw != Gc4::OUT_WIDTH || ph != picture.height)
    {
        std::fprintf(stderr, "%s: preview is %dx%d, the picture is %dx%d\n", argv[0], pw, ph,
                     Gc4::OUT_WIDTH, picture.height);
        return 1;
    }

    se::RenderStats               stats;
    const std::vector<se::Rgb444> frame = suite_render(picture, stats);

    size_t mismatches = 0;
    size_t first      = 0;
    for (size_t i = 0; i < frame.size(); i++)
    {
        const uint8_t r = nibble_to_8(rgb444_r(frame[i]));
        const uint8_t g = nibble_to_8(rgb444_g(frame[i]));
        const uint8_t b = nibble_to_8(rgb444_b(frame[i]));
        if (r != preview[i * 3 + 0] || g != preview[i * 3 + 1] || b != preview[i * 3 + 2])
        {
            if (mismatches == 0)
            {
                first = i;
            }
            mismatches++;
        }
    }

    bool ok = true;
    if (mismatches != 0)
    {
        std::printf("MISMATCH: %zu of %zu pixels; first at (%zu,%zu) suite 0x%03X "
                    "preview %02X%02X%02X\n",
                    mismatches, frame.size(), first % Gc4::OUT_WIDTH, first / Gc4::OUT_WIDTH,
                    frame[first], preview[first * 3 + 0], preview[first * 3 + 1],
                    preview[first * 3 + 2]);
        ok = false;
    }

    struct Counter
    {
        const char *name;
        uint32_t    value;
    };
    const Counter counters[] = {
        { "palette_unwritten_reads",   stats.palette_unwritten_reads },
        { "palette_bad_filler",        stats.palette_bad_filler },
        { "palette_disturbed_visible", stats.palette_disturbed_visible },
        { "pixels_tiled_words",        stats.pixels_tiled_words },
        { "vidcmd_overruns",           stats.vidcmd_overruns },
    };
    for (const Counter &c : counters)
    {
        if (c.value != 0)
        {
            std::printf("MODEL COUNTER %s = %u, expected 0\n", c.name, c.value);
            ok = false;
        }
    }

    if (ok)
    {
        std::printf("%-40s suite==preview EXACT (%zu pixels), %u palette writes\n", argv[1],
                    frame.size(), stats.palette_writes);
    }
    std::printf("%s\n", ok ? "ROUND TRIP OK" : "ROUND TRIP FAILED");
    return ok ? 0 : 1;
}
