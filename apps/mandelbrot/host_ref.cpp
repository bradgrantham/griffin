// Host-side oracle for the Mandelbrot explorer.
//
//     host_ref check
//         Compare mandel_ref.h's fixed-point picture of the home view with a
//         double-precision one.  They cannot agree everywhere -- the escape
//         boundary is chaotic and Q4.28 is not a double -- so this reports the
//         fraction of pixels that differ and fails above a small bound.  It is
//         the check that the fixed-point arithmetic draws the Mandelbrot set
//         and not merely something self-consistent.
//
//     host_ref render OUT.ppm [ACTION]...
//         Write the 640x480 screen the app must show after the given actions.
//
//     host_ref compare SHOT.bmp [ACTION]...
//         Compare an emulator --screenshot with that screen, pixel for pixel.
//         Exit status 0 only if every pixel matches.
//
//     host_ref compare-progress SHOT.bmp [ACTION]...
//         The same for a screenshot taken WHILE the app is still refining:
//         a playfield pixel may show its own colour or that of the 2x2, 4x4
//         or 8x8 block anchor it was filled from, whichever pass last reached
//         it.  Anything else is wrong.  Much cheaper to run, since only the
//         first pass has to have finished; the cursor is still exact.
//
// ACTIONS, applied in order to the app's starting state (home view, cursor at
// the centre of the screen):
//
//     move DX DY      mouse motion in PS/2 counts, +Y up, as in a --mouse-in
//                     script; the cursor clamps at the screen edges as the
//                     app's does
//     left            left click: zoom in about the cursor
//     right           right click: zoom out about the cursor
//     key p           the p key: next palette
//     phase N         the palette animation stopped at phase N (0..14)
//
// The expected screen is the app run with `nocycle` (palette phase 0) after
// its render has finished.  Everything comes from the headers the app itself
// is built from: mandel_ref.h for the picture, griffin_cursor.h for the arrow.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../../super-engine/descriptor.h"
#include "../lib/griffin_cursor.h"

#include "mandel_ref.h"

namespace se = SuperEngine;

namespace
{

struct Rgb
{
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;

    bool operator==(const Rgb &) const = default;
};

// R4G4B4 to 8 bits per channel by nibble replication, as the emulator's DAC
// model (and super-engine's rgb444_to_rgb888) does.
Rgb expand(uint16_t rgb444)
{
    return Rgb{static_cast<uint8_t>(((rgb444 >> 8) & 0xF) * 17),
               static_cast<uint8_t>(((rgb444 >> 4) & 0xF) * 17),
               static_cast<uint8_t>((rgb444 & 0xF) * 17)};
}

struct Scene
{
    Mandel::View view;
    int          cursor_x = se::H_ACTIVE / 2;
    int          cursor_y = se::V_ACTIVE / 2;
    unsigned     palette  = 0;
    unsigned     phase    = 0;
};

int clamp(int v, int lo, int hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

bool apply_actions(Scene &scene, int argc, char **argv)
{
    for (int i = 0; i < argc; i++)
    {
        if (strcmp(argv[i], "move") == 0 && i + 2 < argc)
        {
            scene.cursor_x = clamp(scene.cursor_x + atoi(argv[i + 1]), 0, se::H_ACTIVE - 1);
            scene.cursor_y = clamp(scene.cursor_y - atoi(argv[i + 2]), 0, se::V_ACTIVE - 1);
            i += 2;
        }
        else if (strcmp(argv[i], "left") == 0)
        {
            (void)Mandel::zoom_in(scene.view, static_cast<unsigned>(scene.cursor_x) / 2u,
                                  static_cast<unsigned>(scene.cursor_y) / 2u);
        }
        else if (strcmp(argv[i], "right") == 0)
        {
            (void)Mandel::zoom_out(scene.view, static_cast<unsigned>(scene.cursor_x) / 2u,
                                   static_cast<unsigned>(scene.cursor_y) / 2u);
        }
        else if (strcmp(argv[i], "key") == 0 && i + 1 < argc && strcmp(argv[i + 1], "p") == 0)
        {
            scene.palette = (scene.palette + 1) % Mandel::PALETTE_COUNT;
            i += 1;
        }
        else if (strcmp(argv[i], "phase") == 0 && i + 1 < argc)
        {
            scene.phase = static_cast<unsigned>(atoi(argv[i + 1])) % Mandel::CYCLE_COLOURS;
            i += 1;
        }
        else
        {
            fprintf(stderr, "host_ref: bad action '%s'\n", argv[i]);
            return false;
        }
    }
    return true;
}

// The screen as four candidates per pixel: [0] the finished picture, [1..3]
// what the pixel shows while it still carries its 2x2, 4x4 or 8x8 anchor's
// colour from an earlier refinement pass.  The cursor is the same in all four.
struct Candidates
{
    std::vector<Rgb> screen[4];
};

Candidates expected_screens(const Scene &scene)
{
    std::vector<uint8_t> index(Mandel::WIDTH * Mandel::HEIGHT);
    for (unsigned py = 0; py < Mandel::HEIGHT; py++)
    {
        for (unsigned px = 0; px < Mandel::WIDTH; px++)
        {
            index[py * Mandel::WIDTH + px] =
                static_cast<uint8_t>(Mandel::pixel_index(scene.view, px, py));
        }
    }

    Candidates out;
    for (unsigned level = 0; level < 4; level++)
    {
        const unsigned mask = ~((1u << level) - 1u);     // anchor of the 2^level block
        std::vector<Rgb> &screen = out.screen[level];
        screen.resize(se::H_ACTIVE * se::V_ACTIVE);
        for (unsigned y = 0; y < se::V_ACTIVE; y++)
        {
            for (unsigned x = 0; x < se::H_ACTIVE; x++)
            {
                const unsigned px = (x / 2) & mask;
                const unsigned py = (y / 2) & mask;
                Rgb c = expand(Mandel::palette_colour(scene.palette, index[py * Mandel::WIDTH + px],
                                                      scene.phase));
                const int ax = static_cast<int>(x) - scene.cursor_x;
                const int ay = static_cast<int>(y) - scene.cursor_y;
                if (ax >= 0 && ay >= 0)
                {
                    const unsigned a = GriffinCursor::art_pixel(GriffinCursor::ARROW_ART,
                                                                static_cast<unsigned>(ax),
                                                                static_cast<unsigned>(ay));
                    if (a == 1)
                    {
                        c = expand(0x000);
                    }
                    else if (a == 2)
                    {
                        c = expand(0xFFF);
                    }
                }
                screen[y * se::H_ACTIVE + x] = c;
            }
        }
    }
    return out;
}



bool write_ppm(const char *path, const std::vector<Rgb> &screen)
{
    FILE *f = fopen(path, "wb");
    if (f == nullptr)
    {
        fprintf(stderr, "host_ref: cannot write %s\n", path);
        return false;
    }
    fprintf(f, "P6\n%u %u\n255\n", se::H_ACTIVE, se::V_ACTIVE);
    for (const Rgb &c : screen)
    {
        const uint8_t rgb[3] = {c.r, c.g, c.b};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return true;
}

uint32_t le32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

unsigned mask_shift(uint32_t mask)
{
    unsigned s = 0;
    while (mask != 0 && (mask & 1u) == 0)
    {
        mask >>= 1;
        s++;
    }
    return s;
}

// An uncompressed 24-bit or 32-bit (optionally BI_BITFIELDS) BMP, which covers
// what SDL_SaveBMP writes.
bool read_bmp(const char *path, std::vector<Rgb> &screen)
{
    FILE *f = fopen(path, "rb");
    if (f == nullptr)
    {
        fprintf(stderr, "host_ref: cannot read %s\n", path);
        return false;
    }
    std::vector<uint8_t> data;
    uint8_t              buffer[65536];
    size_t               got = 0;
    while ((got = fread(buffer, 1, sizeof(buffer), f)) > 0)
    {
        data.insert(data.end(), buffer, buffer + got);
    }
    fclose(f);

    if (data.size() < 70 || data[0] != 'B' || data[1] != 'M')
    {
        fprintf(stderr, "host_ref: %s is not a BMP\n", path);
        return false;
    }
    const uint32_t offset      = le32(&data[10]);
    const int32_t  width       = static_cast<int32_t>(le32(&data[18]));
    const int32_t  height_raw  = static_cast<int32_t>(le32(&data[22]));
    const unsigned bits        = data[28];
    const uint32_t compression = le32(&data[30]);
    const bool     top_down    = height_raw < 0;
    const int32_t  height      = top_down ? -height_raw : height_raw;

    if (width != static_cast<int32_t>(se::H_ACTIVE) || height != static_cast<int32_t>(se::V_ACTIVE) ||
        (bits != 24 && bits != 32) || (compression != 0 && compression != 3))
    {
        fprintf(stderr, "host_ref: %s is %dx%d %u-bit compression %u; need %ux%u 24/32-bit\n",
                path, width, height, bits, compression, se::H_ACTIVE, se::V_ACTIVE);
        return false;
    }

    uint32_t rmask = 0x00FF0000u;
    uint32_t gmask = 0x0000FF00u;
    uint32_t bmask = 0x000000FFu;
    if (compression == 3)
    {
        rmask = le32(&data[54]);
        gmask = le32(&data[58]);
        bmask = le32(&data[62]);
    }
    const unsigned bytes  = bits / 8;
    const size_t   stride = (static_cast<size_t>(width) * bytes + 3u) & ~static_cast<size_t>(3);
    if (data.size() < offset + stride * static_cast<size_t>(height))
    {
        fprintf(stderr, "host_ref: %s is truncated\n", path);
        return false;
    }

    screen.resize(se::H_ACTIVE * se::V_ACTIVE);
    for (int32_t y = 0; y < height; y++)
    {
        const int32_t  row = top_down ? y : (height - 1 - y);
        const uint8_t *p   = &data[offset + stride * static_cast<size_t>(row)];
        for (int32_t x = 0; x < width; x++)
        {
            uint32_t v = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                         (static_cast<uint32_t>(p[2]) << 16);
            if (bytes == 4)
            {
                v |= static_cast<uint32_t>(p[3]) << 24;
            }
            screen[static_cast<size_t>(y) * se::H_ACTIVE + static_cast<size_t>(x)] =
                Rgb{static_cast<uint8_t>((v & rmask) >> mask_shift(rmask)),
                    static_cast<uint8_t>((v & gmask) >> mask_shift(gmask)),
                    static_cast<uint8_t>((v & bmask) >> mask_shift(bmask))};
            p += bytes;
        }
    }
    return true;
}

// Double-precision escape with the same escape rule (|z|^2 > 4, tested before
// the update) and no closed-form shortcut; returns the count and R/4 as the
// fixed-point state would hold them, so the same colouring can be applied.
Mandel::State iterate_double(double cx, double cy, unsigned max_iter)
{
    double   x = 0.0;
    double   y = 0.0;
    unsigned n = 0;
    while (n < max_iter)
    {
        if (std::fabs(x) >= 2.0 || std::fabs(y) >= 2.0 || x * x + y * y > 4.0)
        {
            break;
        }
        const double nx = x * x - y * y + cx;
        y = 2.0 * x * y + cy;
        x = nx;
        n++;
    }
    Mandel::State s{};
    s.n     = static_cast<uint16_t>(n);
    s.norm4 = static_cast<uint32_t>((x * x + y * y) / 4.0 * Mandel::ONE);
    return s;
}

int check()
{
    // Not just the home view: a deep one too, where a pixel is only 48 units
    // of the fixed-point format wide and the arithmetic has least to spare.
    Scene deep;
    deep.cursor_x = 2 * 160;        // a seahorse-valley point: about -0.75 + 0.1i
    deep.cursor_y = 2 * 111;
    for (unsigned i = 0; i < Mandel::MAX_LEVEL; i++)
    {
        (void)Mandel::zoom_in(deep.view, static_cast<unsigned>(deep.cursor_x) / 2u,
                              static_cast<unsigned>(deep.cursor_y) / 2u);
    }

    const Mandel::View views[2] = {Mandel::View{}, deep.view};
    const char *const  names[2] = {"home view", "deepest view"};
    // Permille of pixels allowed to differ from double precision.
    const unsigned     bound[2] = {10, 150};
    bool               ok       = true;

    for (unsigned v = 0; v < 2; v++)
    {
        const unsigned max_iter = Mandel::max_iterations(views[v]);
        unsigned       differ   = 0;
        unsigned       interior = 0;
        for (unsigned py = 0; py < Mandel::HEIGHT; py++)
        {
            for (unsigned px = 0; px < Mandel::WIDTH; px++)
            {
                const int32_t  cx   = Mandel::pixel_cx(views[v], px);
                const int32_t  cy   = Mandel::pixel_cy(views[v], py);
                const Mandel::State d =
                    iterate_double(static_cast<double>(cx) / Mandel::ONE,
                                   static_cast<double>(cy) / Mandel::ONE, max_iter);
                const unsigned want = Mandel::colour_index(d.n, max_iter, d.norm4, px, py);
                const unsigned got = Mandel::pixel_index(views[v], px, py);
                differ   += (want != got) ? 1u : 0u;
                interior += (got == 0) ? 1u : 0u;
            }
        }
        const unsigned total    = Mandel::WIDTH * Mandel::HEIGHT;
        const unsigned permille = (differ * 1000u) / total;
        const bool     pass     = permille <= bound[v];
        printf("host_ref check: %-12s level %2u, max %3u iterations: %u of %u pixels differ from "
               "double precision (%u permille, bound %u), %u in the set: %s\n",
               names[v], views[v].level, max_iter, differ, total, permille, bound[v], interior,
               pass ? "PASS" : "FAIL");
        ok = ok && pass;
    }
    return ok ? 0 : 1;
}

}   // namespace

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "check") == 0)
    {
        return check();
    }
    const bool progress = argc >= 3 && strcmp(argv[1], "compare-progress") == 0;
    if (argc >= 3 && (strcmp(argv[1], "render") == 0 || strcmp(argv[1], "compare") == 0 || progress))
    {
        Scene scene;
        if (!apply_actions(scene, argc - 3, argv + 3))
        {
            return 2;
        }
        const Candidates        all  = expected_screens(scene);
        const std::vector<Rgb> &want = all.screen[0];
        if (strcmp(argv[1], "render") == 0)
        {
            return write_ppm(argv[2], want) ? 0 : 2;
        }

        std::vector<Rgb> got;
        if (!read_bmp(argv[2], got))
        {
            return 2;
        }
        const unsigned levels  = progress ? 4u : 1u;
        unsigned       differ  = 0;
        unsigned       first_x = 0;
        unsigned       first_y = 0;
        unsigned       settled = 0;     // pixels already at their final colour
        for (unsigned i = 0; i < want.size(); i++)
        {
            bool ok = false;
            for (unsigned level = 0; level < levels && !ok; level++)
            {
                ok = all.screen[level][i] == got[i];
            }
            settled += (want[i] == got[i]) ? 1u : 0u;
            if (!ok)
            {
                if (differ == 0)
                {
                    first_x = i % se::H_ACTIVE;
                    first_y = i / se::H_ACTIVE;
                }
                differ++;
            }
        }
        printf("host_ref %s: level %u, cursor (%d,%d), %s palette phase %u: %u of %zu "
               "pixels differ", progress ? "compare-progress" : "compare", scene.view.level,
               scene.cursor_x, scene.cursor_y, Mandel::PALETTE_NAMES[scene.palette], scene.phase,
               differ, want.size());
        if (progress)
        {
            printf(" (%u already final)", settled);
        }
        if (differ != 0)
        {
            const Rgb &w = want[first_y * se::H_ACTIVE + first_x];
            const Rgb &g = got[first_y * se::H_ACTIVE + first_x];
            printf("; first at (%u,%u): want %02X%02X%02X got %02X%02X%02X", first_x, first_y,
                   w.r, w.g, w.b, g.r, g.g, g.b);
        }
        printf(": %s\n", (differ == 0) ? "PASS" : "FAIL");
        return (differ == 0) ? 0 : 1;
    }

    fprintf(stderr, "usage: host_ref check\n"
                    "       host_ref render OUT.ppm [move DX DY | left | right | key p]...\n"
                    "       host_ref compare SHOT.bmp [move DX DY | left | right | key p]...\n"
                    "       host_ref compare-progress SHOT.bmp [ACTION]...   (mid-render shot)\n");
    return 2;
}
