// microham_encode.cpp — 640x480 PNG -> Griffin micro-HAM plane + per-line palette
//
// Build: c++ -std=c++23 -O3 -I. -o microham_encode microham_encode.cpp
// Usage: microham_encode <input.png> <out.ham> <out.pal> [preview.png]
//
// Outputs (see microham.h for the authoritative format comments):
//   .ham   480 lines x 80 big-endian 16-bit words = 76800 bytes, no header.
//          A 68000 memory image: load it and DMA it verbatim.
//   .pal   480 x {pal_fg, pal_bg} big-endian 16-bit words = 1920 bytes.
//          The app issues each pair as two VIDCMD SETs in that line's HBLANK.
//   preview.png (optional) the decoded frame, so quality is eyeballable.
//
// The encoder is exact for a given palette pair (a shortest path over the
// reachable held-colour set) and heuristic only in choosing the pair.

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image.h"
#include "stb/stb_image_write.h"

#include "microham.h"

#include <cstdio>
#include <vector>

using namespace MicroHam;

int main(int argc, char **argv)
{
    if (argc < 4 || argc > 5)
    {
        std::fprintf(stderr, "Usage: %s <input.png> <out.ham> <out.pal> [preview.png]\n",
                     argv[0]);
        return 1;
    }

    int width  = 0;
    int height = 0;
    int comps  = 0;
    unsigned char *image = stbi_load(argv[1], &width, &height, &comps, 3);
    if (image == nullptr)
    {
        std::fprintf(stderr, "%s: cannot read %s\n", argv[0], argv[1]);
        return 1;
    }
    if (width != WIDTH || height != HEIGHT)
    {
        std::fprintf(stderr, "%s: %s is %dx%d, expected %dx%d\n", argv[0], argv[1],
                     width, height, WIDTH, HEIGHT);
        stbi_image_free(image);
        return 1;
    }

    const std::vector<Rgb444> target = quantize_frame(image);
    stbi_image_free(image);

    const EncodedFrame         frame = encode_frame(target);
    const std::vector<uint8_t> plane = plane_bytes(frame);
    const std::vector<uint8_t> pal   = palette_bytes(frame);

    std::vector<uint8_t> preview(static_cast<size_t>(WIDTH) * HEIGHT * 3);
    int64_t              total_error = 0;
    int                  max_error   = 0;

    for (size_t i = 0; i < target.size(); i++)
    {
        const Rgb444 c = frame.decoded[i];
        preview[i * 3 + 0] = nibble_to_8(rgb444_r(c));
        preview[i * 3 + 1] = nibble_to_8(rgb444_g(c));
        preview[i * 3 + 2] = nibble_to_8(rgb444_b(c));

        const int e = colour_error(c, target[i]);
        total_error += e;
        max_error = std::max(max_error, e);
    }

    if (!write_file(argv[2], plane))
    {
        std::fprintf(stderr, "%s: cannot write %s\n", argv[0], argv[2]);
        return 1;
    }
    if (!write_file(argv[3], pal))
    {
        std::fprintf(stderr, "%s: cannot write %s\n", argv[0], argv[3]);
        return 1;
    }
    if (argc == 5)
    {
        if (stbi_write_png(argv[4], WIDTH, HEIGHT, 3, preview.data(), WIDTH * 3) == 0)
        {
            std::fprintf(stderr, "%s: cannot write %s\n", argv[0], argv[4]);
            return 1;
        }
    }

    const double mean = static_cast<double>(total_error) /
                        (static_cast<double>(WIDTH) * HEIGHT);
    std::printf("%s: %s %zu bytes, %s %zu bytes, "
                "mean sq RGB444 error %.2f, max %d\n",
                argv[1], argv[2], plane.size(), argv[3], pal.size(), mean, max_error);
    return 0;
}
