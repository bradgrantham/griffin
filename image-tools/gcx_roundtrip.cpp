// gcx_roundtrip.cpp — a .gcx file from disk, through the REAL suite model
//
// Build: see the Makefile in this directory.
// Usage: gcx_roundtrip [--arbitration4-only] <in.gcx> <preview.png>
//
// The encoder verifies its own output before writing it; this is the same check
// run on the bytes that reached the disk, against the encoder's --preview PNG.
// gcx_verify.h does the work: the file goes into a RAM image, its descriptor
// table is relocated by the function the 68000 runs, ENGINE's walker timestamps
// every deposit, and the frame is rendered clock by clock.  The rendered frame
// must equal the preview pixel for pixel, with every fault counter at zero and
// every line group inside its scanline.
//
// `make check` also feeds it the encoder's --fault files, which it must REJECT.

#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

#include "gcx_format.h"
#include "gcx_verify.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace se = SuperEngine;

using ClutColor::nibble_to_8;
using ClutColor::rgb444_b;
using ClutColor::rgb444_g;
using ClutColor::rgb444_r;

int main(int argc, char **argv)
{
    Gcx::VerifyOptions options;
    int                arg = 1;
    if (arg < argc && std::strcmp(argv[arg], "--arbitration4-only") == 0)
    {
        options.arbitrations = { se::ENGINE_ARBITRATION_CYCLES };
        arg++;
    }
    if (argc - arg != 2)
    {
        std::fprintf(stderr, "usage: %s [--arbitration4-only] <in.gcx> <preview.png>\n", argv[0]);
        return 1;
    }

    std::vector<uint8_t> data;
    if (!Gcx::read_bytes(argv[arg], data))
    {
        std::fprintf(stderr, "%s: cannot read %s\n", argv[0], argv[arg]);
        return 1;
    }

    int            pw      = 0;
    int            ph      = 0;
    int            comps   = 0;
    unsigned char *preview = stbi_load(argv[arg + 1], &pw, &ph, &comps, 3);
    if (preview == nullptr || pw != Gcx::OUT_WIDTH || ph != Gcx::HEIGHT)
    {
        std::fprintf(stderr, "%s: cannot read %s as a %dx%d preview\n", argv[0], argv[arg + 1],
                     Gcx::OUT_WIDTH, Gcx::HEIGHT);
        return 1;
    }

    Gcx::VerifyReport report = Gcx::verify_file(data, options);

    if (!report.frame.empty())
    {
        size_t mismatches = 0;
        size_t first      = 0;
        for (size_t i = 0; i < report.frame.size(); i++)
        {
            const se::Rgb444 c = report.frame[i];
            if (nibble_to_8(rgb444_r(c)) != preview[i * 3 + 0]
                || nibble_to_8(rgb444_g(c)) != preview[i * 3 + 1]
                || nibble_to_8(rgb444_b(c)) != preview[i * 3 + 2])
            {
                first = (mismatches == 0) ? i : first;
                mismatches++;
            }
        }
        if (mismatches != 0)
        {
            char buf[200];
            std::snprintf(buf, sizeof(buf),
                          "%zu of %zu pixels differ from the preview; first at (%zu,%zu): suite "
                          "0x%03X, preview %02X%02X%02X",
                          mismatches, report.frame.size(), first % Gcx::OUT_WIDTH,
                          first / Gcx::OUT_WIDTH, report.frame[first], preview[first * 3 + 0],
                          preview[first * 3 + 1], preview[first * 3 + 2]);
            report.problems.emplace_back(buf);
        }
    }
    stbi_image_free(preview);

    for (size_t i = 0; i < report.problems.size() && i < 12; i++)
    {
        std::printf("PROBLEM: %s\n", report.problems[i].c_str());
    }
    if (report.ok())
    {
        std::printf("%-32s suite==preview EXACT, %u descriptors, worst group %u of %u SYSCLK, "
                    "FIFO high %u\n",
                    argv[arg], report.descriptors, report.worst_group, Gcx::GROUP_SYSCLK_LIMIT,
                    report.fifo_high);
    }
    std::printf("%s\n", report.ok() ? "ROUND TRIP OK" : "ROUND TRIP FAILED");
    return report.ok() ? 0 : 1;
}
