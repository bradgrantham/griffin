// clut_decode.cpp — .gc4 -> PNG, and the file's validator
//
// Build: see the Makefile in this directory.
// Usage: clut_decode [--quiet] <in.gc4> [out.png]
//
// Exit 0 = the file obeys gc4_format.h's rules; exit 2 = it does not, and the
// message names the line.  With an output path it also renders the picture.
//
// THIS SHARES NOTHING WITH THE ENCODER BUT gc4_format.h.  The unpacking below is
// written from the format's prose, not called out of the encoder, so "decoder
// output == the encoder's own preview" is a cross-check between two
// implementations rather than a tautology.  The third implementation is
// super-engine/render.cpp, driven by clut_roundtrip, and it is the one that
// speaks for the hardware.
//
// The palette a line displays is the running state: the header's initial sixteen
// entries, then every earlier line's loads, then this line's.  A load takes
// effect for the line it appears on, because the display list deposits it in that
// line's HBLANK — before pixel 0.

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#include "gc4_format.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using ClutColor::PaletteLoad;
using ClutColor::Rgb444;
using ClutColor::nibble_to_8;
using ClutColor::rgb444_b;
using ClutColor::rgb444_g;
using ClutColor::rgb444_r;

int main(int argc, char **argv)
{
    bool quiet = false;
    int  arg   = 1;
    while (arg < argc && std::strncmp(argv[arg], "--", 2) == 0)
    {
        if (std::strcmp(argv[arg], "--quiet") == 0)
        {
            quiet = true;
            arg++;
        }
        else
        {
            std::fprintf(stderr, "%s: unknown option %s\n", argv[0], argv[arg]);
            return 1;
        }
    }
    if (argc - arg < 1 || argc - arg > 2)
    {
        std::fprintf(stderr, "usage: %s [--quiet] <in.gc4> [out.png]\n", argv[0]);
        return 1;
    }

    const char *const in_path  = argv[arg];
    const char *const out_path = (argc - arg == 2) ? argv[arg + 1] : nullptr;

    std::vector<uint8_t> data;
    if (!Gc4::read_bytes(in_path, data))
    {
        std::fprintf(stderr, "%s: cannot read %s\n", argv[0], in_path);
        return 1;
    }

    Gc4::Picture picture;
    std::string  error;
    if (!Gc4::parse_picture(data, picture, error))
    {
        std::fprintf(stderr, "%s: %s\n", in_path, error.c_str());
        return 2;
    }
    if (!Gc4::validate_picture(picture, error))
    {
        std::fprintf(stderr, "%s: %s\n", in_path, error.c_str());
        return 2;
    }

    const int height = picture.height;
    const int width  = Gc4::OUT_WIDTH;

    std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3, 0);
    Rgb444               clut[Gc4::PALETTE_SIZE];
    for (int i = 0; i < Gc4::PALETTE_SIZE; i++)
    {
        clut[i] = picture.initial[i];
    }

    for (int y = 0; y < height; y++)
    {
        const Gc4::Line &line = picture.lines[static_cast<size_t>(y)];
        for (const PaletteLoad &l : line.loads)
        {
            clut[l.index & 0xF] = l.colour;
        }

        for (int x = 0; x < Gc4::PIXEL_WIDTH; x++)
        {
            // Four bits per group, MSB first out of a big-endian 16-bit word.
            const uint16_t word  = line.words[static_cast<size_t>(x / 4)];
            const int      shift = 12 - 4 * (x % 4);
            const int      index = (word >> shift) & 0xF;
            const Rgb444   c     = clut[index];

            // Half rate: the group is held two pixel clocks.
            for (int half = 0; half < 2; half++)
            {
                const size_t at = (static_cast<size_t>(y) * width + x * 2 + half) * 3;
                rgb[at + 0]     = nibble_to_8(rgb444_r(c));
                rgb[at + 1]     = nibble_to_8(rgb444_g(c));
                rgb[at + 2]     = nibble_to_8(rgb444_b(c));
            }
        }
    }

    if (out_path != nullptr)
    {
        if (stbi_write_png(out_path, width, height, 3, rgb.data(), width * 3) == 0)
        {
            std::fprintf(stderr, "%s: cannot write %s\n", argv[0], out_path);
            return 1;
        }
    }

    if (!quiet)
    {
        std::printf("%s: valid, 4bpp half rate %dx%d (shown %dx%d), K=%d\n", in_path,
                    Gc4::PIXEL_WIDTH, height, width, height, picture.k);
    }
    return 0;
}
