// microham_encode.cpp — 640x480 PNG -> Griffin micro-HAM plane + per-line palette
//
// Build: c++ -std=c++23 -O3 -I. -o microham_encode microham_encode.cpp
// Usage: microham_encode [--dither none|ordered|fs] [--dither-amp N] \
//            <input.png> <out.ham> <out.pal> [preview.png]
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
//
// ---------------------------------------------------------------------------
// WHY ORDERED IS THE DEFAULT, WITH THE NUMBERS THAT ARGUE AGAINST IT
// ---------------------------------------------------------------------------
//
// Two colours per line plus saturating chroma codes is not many colours, and a
// squared-error objective spends them all on flat fills: undithered micro-HAM
// bands visibly on anything smooth — sky, skin, a wall in shadow.  A palette
// code is one pixel wide, so alternating fg/bg at full pixel rate costs nothing
// at all; the format wants to dither and only the objective was stopping it.
//
// Measured over four photographs, the three smurf backdrops and a synthetic ramp
// (per-pixel squared RGB444 error, then squared 8-bit error of 4x4 box means —
// the second is the one that tracks what the eye integrates; both are printed on
// every run):
//
//   image                per-pixel                 4x4-box
//                     none ordered     fs      none  ordered     fs
//   gradient          4.69    6.57  12.48   1267.40   580.03  230.53
//   maya2             7.17    8.13  15.73    776.06   643.64  199.16
//   millie2           8.89   10.54  17.93   1172.47   809.63  187.17
//   bcdlghibli        7.46    9.42  17.74   1088.32   656.91  210.57
//   adobe2           13.95   15.19  25.48   1163.18   991.40  300.46
//   backdrop1-meadow  0.53    0.76   2.04    121.40    91.97   25.04
//   backdrop2-forest  1.76    2.14   5.69    428.00   324.66   60.46
//   backdrop3-clear.  1.07    1.39   3.89    256.67   197.76   54.65
//
// The per-pixel column behaves exactly as dithering should make it behave: worse
// everywhere, and worst for the mode that dithers hardest.  The box column is
// the one to read, and by it fs wins every single image by a factor of three to
// five.  fs is nonetheless NOT the default, because the box metric measures only
// the half of the picture dithering is good at.  It has no opinion at all about
// high-frequency noise — a 4x4 mean is blind to how violently the pixels inside
// it disagree — and viewed at 1:1, which is the only size this hardware has, fs
// is violent:
//
//   * Hard edges shred.  The fence posts in backdrop1 stop being brown posts and
//     become vertical stacks of red-and-black checker: the greedy walk commits a
//     pixel, owes the difference, and pays it back into the next pixel, which on
//     a two-pixel-wide post is the post itself.
//   * Flat-ish areas grow horizontal dashes, error being carried rightward along
//     the line faster than it is carried down.
//   * Small features (the sun's rim, single-pixel highlights) come apart.
//
// Ordered dithering perturbs the TARGET and leaves the exact per-line shortest
// path to decide, so an edge stays exactly as sharp as it is without dithering —
// the DP still sees the edge — while smooth areas break into blue noise.  It is
// also gated off wherever the source is busy (microham.h, ordered_gate_line), so
// the fence posts and tree trunks stay solid.  On the cartoon backdrops this
// repo actually ships, ordered is plainly the better picture; on photographs fs
// renders tone better and ordered still posterizes in deep shadow, so
// `--dither fs` is the right call for photographic source and is why the mode
// exists.  Both beat the legacy 1bpp R3G3B2 pipeline (griffin_dither.cpp,
// maya2-332.png et al), which dithers tone well but streaks horribly and fringes
// every edge with complementary colour.
//
// One honest regression to know about: on a line that is bimodal rather than
// smooth — a tree trunk crossing grass — ordered can choose a different palette
// pair than the undithered encoder, and a third colour that neither slot covers
// (the trunk's shadowed side) then comes out as a brown-and-dark-red mixture
// instead of being silently flattened into the grass.  Closer in average colour,
// and the box metric agrees, but noisier to look at.
//
// The ordered perturbation runs mostly ALONG THE PALETTE AXIS, which needs no
// amplitude — see ordered_dither_line in microham.h, and note that a fixed
// isotropic perturbation on its own is nearly useless here: it moved the box
// error of maya2 from 776 only to 745, because the banding in this format is the
// gap between two palette entries, not the gap between two RGB444 levels.
// --dither-amp sets only the isotropic term, whose principled value is one
// RGB444 step (17); measurements across 8..34 differ by less than the noise.

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image.h"
#include "stb/stb_image_write.h"

#include "microham.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace MicroHam;

namespace
{

const char *dither_name(Dither mode)
{
    switch (mode)
    {
        case Dither::None:           return "none";
        case Dither::Ordered:        return "ordered";
        case Dither::FloydSteinberg: return "fs";
    }
    return "?";
}

bool parse_dither(const char *text, Dither &out)
{
    if (std::strcmp(text, "none") == 0)
    {
        out = Dither::None;
        return true;
    }
    if (std::strcmp(text, "ordered") == 0)
    {
        out = Dither::Ordered;
        return true;
    }
    if (std::strcmp(text, "fs") == 0)
    {
        out = Dither::FloydSteinberg;
        return true;
    }
    return false;
}

}  // namespace

int main(int argc, char **argv)
{
    DitherParams dither;
    int          arg = 1;

    while (arg < argc && argv[arg][0] == '-' && argv[arg][1] == '-')
    {
        if (std::strcmp(argv[arg], "--dither") == 0 && arg + 1 < argc)
        {
            if (!parse_dither(argv[arg + 1], dither.mode))
            {
                std::fprintf(stderr, "%s: unknown dither mode %s\n", argv[0], argv[arg + 1]);
                return 1;
            }
            arg += 2;
        }
        else if (std::strcmp(argv[arg], "--dither-amp") == 0 && arg + 1 < argc)
        {
            dither.amplitude = std::atoi(argv[arg + 1]);
            arg += 2;
        }
        else
        {
            std::fprintf(stderr, "%s: unknown option %s\n", argv[0], argv[arg]);
            return 1;
        }
    }

    const int positional = argc - arg;
    if (positional < 3 || positional > 4)
    {
        std::fprintf(stderr,
                     "Usage: %s [--dither none|ordered|fs] [--dither-amp N] "
                     "<input.png> <out.ham> <out.pal> [preview.png]\n",
                     argv[0]);
        return 1;
    }

    const char *const in_path      = argv[arg + 0];
    const char *const ham_path     = argv[arg + 1];
    const char *const pal_path     = argv[arg + 2];
    const char *const preview_path = (positional == 4) ? argv[arg + 3] : nullptr;

    int width  = 0;
    int height = 0;
    int comps  = 0;
    unsigned char *image = stbi_load(in_path, &width, &height, &comps, 3);
    if (image == nullptr)
    {
        std::fprintf(stderr, "%s: cannot read %s\n", argv[0], in_path);
        return 1;
    }
    if (width != WIDTH || height != HEIGHT)
    {
        std::fprintf(stderr, "%s: %s is %dx%d, expected %dx%d\n", argv[0], in_path, width, height,
                     WIDTH, HEIGHT);
        stbi_image_free(image);
        return 1;
    }

    const std::vector<uint8_t> source(image, image + static_cast<size_t>(WIDTH) * HEIGHT * 3);
    stbi_image_free(image);

    const EncodedFrame         frame = encode_frame(source, dither);
    const std::vector<uint8_t> plane = plane_bytes(frame);
    const std::vector<uint8_t> pal   = palette_bytes(frame);
    const FrameMetrics         m     = measure_frame(frame.decoded, source);

    if (frame.model_mismatches != 0)
    {
        std::fprintf(stderr, "%s: INTERNAL: %zu pixels where the dither model disagreed "
                             "with the decoder\n",
                     argv[0], frame.model_mismatches);
        return 1;
    }

    if (!write_file(ham_path, plane))
    {
        std::fprintf(stderr, "%s: cannot write %s\n", argv[0], ham_path);
        return 1;
    }
    if (!write_file(pal_path, pal))
    {
        std::fprintf(stderr, "%s: cannot write %s\n", argv[0], pal_path);
        return 1;
    }
    if (preview_path != nullptr)
    {
        std::vector<uint8_t> preview(static_cast<size_t>(WIDTH) * HEIGHT * 3);
        for (size_t i = 0; i < frame.decoded.size(); i++)
        {
            const Rgb444 c = frame.decoded[i];
            preview[i * 3 + 0] = nibble_to_8(rgb444_r(c));
            preview[i * 3 + 1] = nibble_to_8(rgb444_g(c));
            preview[i * 3 + 2] = nibble_to_8(rgb444_b(c));
        }
        if (stbi_write_png(preview_path, WIDTH, HEIGHT, 3, preview.data(), WIDTH * 3) == 0)
        {
            std::fprintf(stderr, "%s: cannot write %s\n", argv[0], preview_path);
            return 1;
        }
    }

    std::printf("%s: %s %zu bytes, %s %zu bytes, dither %s", in_path, ham_path, plane.size(),
                pal_path, pal.size(), dither_name(dither.mode));
    if (dither.mode == Dither::Ordered)
    {
        std::printf(" amp %d", dither.amplitude);
    }
    std::printf(", per-pixel sq RGB444 err %.2f (max %d), 4x4-box sq 8-bit err %.2f\n",
                m.per_pixel_mse, m.max_pixel_error, m.box4_mse);
    return 0;
}
