// clut_decode.cpp — the truth of the CLUT experiment
//
// Build: see the Makefile in this directory.
// Usage: clut_decode [--quiet] <in.clut> [out.png]
//
// Validates the stream against DESIGN.md §3 (exiting 2 with a message naming
// the offending line) and renders it the way the hardware would: a 16-entry
// CLUT that persists from line to line, this line's loads applied in its
// horizontal blanking, indices consumed MSB-first out of big-endian 16-bit
// words, half-rate modes pixel-doubled to 640, and COMPOSITOR refinement spans
// painted over the pixel stream last.
//
// It shares clut_format.h with the encoder and nothing else: this file's
// renderer is an independent reading of the format, which is what makes
// "decode == the encoder's prediction" in `make check` worth running.

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#include "clut_format.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace Clut;

namespace
{

// Consumes `bits` at a time, MSB first, from a line's words.
class BitStream
{
public:
    BitStream(const std::vector<uint16_t> &words) : words_(words) {}

    int take(int bits)
    {
        int value = 0;
        for (int i = 0; i < bits; i++)
        {
            const int word = static_cast<int>(words_[index_ >> 4]);
            const int bit  = (word >> (15 - (index_ & 15))) & 1;
            value          = (value << 1) | bit;
            index_++;
        }
        return value;
    }

private:
    const std::vector<uint16_t> &words_;
    size_t                       index_ = 0;   // in bits
};

void render(const Stream &stream, std::vector<Rgb444> &frame)
{
    const ModeInfo &mi = stream.info();
    frame.assign(static_cast<size_t>(OUT_WIDTH) * HEIGHT, 0);

    Rgb444 clut[CLUT_SIZE] = { 0 };

    for (int y = 0; y < HEIGHT; y++)
    {
        const Line &line = stream.lines[static_cast<size_t>(y)];
        for (const Load &l : line.loads)
        {
            clut[l.index] = l.colour;
        }

        Rgb444    pixels[OUT_WIDTH];
        BitStream bits(line.words);

        if (mi.mode == Mode::ATTR)
        {
            int out = 0;
            for (int cell = 0; cell < mi.pixel_width / 8; cell++)
            {
                const int fg      = bits.take(4);
                const int bg      = bits.take(4);
                const int pattern = bits.take(8);
                for (int i = 0; i < 8; i++)
                {
                    const bool set = ((pattern >> (7 - i)) & 1) != 0;
                    pixels[out]    = clut[set ? fg : bg];
                    out++;
                }
            }
        }
        else
        {
            for (int x = 0; x < mi.pixel_width; x++)
            {
                pixels[x] = clut[bits.take(mi.bits_per_pixel)];
            }
        }

        Rgb444 *row = &frame[static_cast<size_t>(y) * OUT_WIDTH];
        if (mi.half_rate)
        {
            for (int x = 0; x < mi.pixel_width; x++)
            {
                row[x * 2 + 0] = pixels[x];
                row[x * 2 + 1] = pixels[x];
            }
        }
        else
        {
            for (int x = 0; x < OUT_WIDTH; x++)
            {
                row[x] = pixels[x];
            }
        }

        for (const Span &sp : line.spans)
        {
            for (int x = sp.x; x < sp.x + sp.length; x++)
            {
                row[x] = sp.colour;
            }
        }
    }
}

}  // namespace

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

    const int positional = argc - arg;
    if (positional < 1 || positional > 2)
    {
        std::fprintf(stderr, "Usage: %s [--quiet] <in.clut> [out.png]\n", argv[0]);
        return 1;
    }
    const char *const in_path  = argv[arg];
    const char *const png_path = (positional == 2) ? argv[arg + 1] : nullptr;

    std::vector<uint8_t> data;
    if (!read_bytes(in_path, data))
    {
        std::fprintf(stderr, "%s: cannot read %s\n", argv[0], in_path);
        return 1;
    }

    Stream      stream;
    std::string error;
    std::string warning;
    if (!parse_stream(data, stream, error))
    {
        std::fprintf(stderr, "%s: %s: INVALID: %s\n", argv[0], in_path, error.c_str());
        return 2;
    }
    if (!validate_stream(stream, error, warning))
    {
        std::fprintf(stderr, "%s: %s: INVALID: %s\n", argv[0], in_path, error.c_str());
        return 2;
    }
    if (!warning.empty() && !quiet)
    {
        std::fprintf(stderr, "%s: %s: FLAGGED: %s\n", argv[0], in_path, warning.c_str());
    }

    std::vector<Rgb444> frame;
    render(stream, frame);

    if (png_path != nullptr)
    {
        std::vector<uint8_t> rgb(static_cast<size_t>(OUT_WIDTH) * HEIGHT * 3);
        for (size_t i = 0; i < frame.size(); i++)
        {
            rgb[i * 3 + 0] = nibble_to_8(rgb444_r(frame[i]));
            rgb[i * 3 + 1] = nibble_to_8(rgb444_g(frame[i]));
            rgb[i * 3 + 2] = nibble_to_8(rgb444_b(frame[i]));
        }
        if (stbi_write_png(png_path, OUT_WIDTH, HEIGHT, 3, rgb.data(), OUT_WIDTH * 3) == 0)
        {
            std::fprintf(stderr, "%s: cannot write %s\n", argv[0], png_path);
            return 1;
        }
    }

    if (!quiet)
    {
        // Averages, because what the sweep wants to know about K and N is not
        // what the encoder was allowed but what it actually spent.
        int64_t loads = 0;
        int64_t spans = 0;
        for (const Line &line : stream.lines)
        {
            loads += static_cast<int64_t>(line.loads.size());
            spans += static_cast<int64_t>(line.spans.size());
        }
        std::printf("%s: valid, mode %s K=%d N=%d, %d words/line, loads/line %.2f, spans/line %.2f",
                    in_path, stream.info().name, stream.k, stream.n,
                    stream.info().words_per_line, static_cast<double>(loads) / HEIGHT,
                    static_cast<double>(spans) / HEIGHT);
        if (png_path != nullptr)
        {
            std::printf(", wrote %s", png_path);
        }
        std::printf("\n");
    }
    return 0;
}
