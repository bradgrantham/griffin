// make_smurf_assets.cpp — generate the smurf demo's source art as PNGs
//
// Build: c++ -std=c++23 -O2 -I. -o make_smurf_assets make_smurf_assets.cpp
// Usage: make_smurf_assets <output_directory>
//
// The art is generated rather than drawn so the asset pipeline can be rebuilt
// from source with no binary inputs.  Everything is built from filled
// rectangles, ellipses and vertical gradients, which has a useful side effect
// for the sprite sheets: an ellipse or rectangle contributes exactly one span
// per row, so the sprites stay inside the overlay's per-row VIDCMD budget by
// construction rather than by luck.
//
// Backdrops are 640x480 and lean on vertical gradients, which is what micro-HAM
// is good at (one palette pair per line, chroma steps for everything else).
// Sprite sheets are keyed on magenta 0xFF00FF, which sprite2spans treats as
// transparent.

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace
{

struct Rgb
{
    int r = 0;
    int g = 0;
    int b = 0;
};

constexpr Rgb KEY = {0xFF, 0x00, 0xFF};

class Canvas
{
public:
    Canvas(int width, int height, Rgb fill)
        : width_(width), height_(height),
          pixels_(static_cast<size_t>(width) * height * 4)
    {
        for (int y = 0; y < height_; y++)
        {
            for (int x = 0; x < width_; x++)
            {
                set(x, y, fill);
            }
        }
    }

    int width() const { return width_; }
    int height() const { return height_; }
    const uint8_t *data() const { return pixels_.data(); }

    void set(int x, int y, Rgb c)
    {
        if (x < 0 || y < 0 || x >= width_ || y >= height_)
        {
            return;
        }
        const size_t o = (static_cast<size_t>(y) * width_ + x) * 4;
        pixels_[o + 0] = static_cast<uint8_t>(std::clamp(c.r, 0, 255));
        pixels_[o + 1] = static_cast<uint8_t>(std::clamp(c.g, 0, 255));
        pixels_[o + 2] = static_cast<uint8_t>(std::clamp(c.b, 0, 255));
        pixels_[o + 3] = 255;
    }

    void rect(int x, int y, int w, int h, Rgb c)
    {
        for (int yy = y; yy < y + h; yy++)
        {
            for (int xx = x; xx < x + w; xx++)
            {
                set(xx, yy, c);
            }
        }
    }

    // One span per row by construction.
    void ellipse(double cx, double cy, double rx, double ry, Rgb c)
    {
        const int y0 = static_cast<int>(std::floor(cy - ry));
        const int y1 = static_cast<int>(std::ceil(cy + ry));
        for (int y = y0; y <= y1; y++)
        {
            const double dy = (y + 0.5 - cy) / ry;
            if (dy * dy > 1.0)
            {
                continue;
            }
            const double half = rx * std::sqrt(1.0 - dy * dy);
            const int    x0   = static_cast<int>(std::round(cx - half));
            const int    x1   = static_cast<int>(std::round(cx + half));
            for (int x = x0; x < x1; x++)
            {
                set(x, y, c);
            }
        }
    }

    // Vertical gradient across the full width, rows [y0, y1).
    void vgradient(int y0, int y1, Rgb top, Rgb bottom)
    {
        const int span = std::max(1, y1 - y0 - 1);
        for (int y = y0; y < y1; y++)
        {
            const int t = std::clamp(y - y0, 0, span);
            const Rgb c = {top.r + (bottom.r - top.r) * t / span,
                           top.g + (bottom.g - top.g) * t / span,
                           top.b + (bottom.b - top.b) * t / span};
            for (int x = 0; x < width_; x++)
            {
                set(x, y, c);
            }
        }
    }

    // A gentle left-to-right tint, laid over whatever is already there.  This is
    // the part of a backdrop that micro-HAM has to solve with chroma codes
    // rather than with the line's palette pair, so it is deliberately present.
    void htint(int y0, int y1, int amount)
    {
        for (int y = y0; y < y1; y++)
        {
            for (int x = 0; x < width_; x++)
            {
                const size_t o = (static_cast<size_t>(y) * width_ + x) * 4;
                const int    d = amount * (x - width_ / 2) / (width_ / 2);
                pixels_[o + 0] = static_cast<uint8_t>(std::clamp(pixels_[o + 0] + d, 0, 255));
                pixels_[o + 1] = static_cast<uint8_t>(std::clamp(pixels_[o + 1] + d / 2, 0, 255));
                pixels_[o + 2] = static_cast<uint8_t>(std::clamp(pixels_[o + 2] - d / 2, 0, 255));
            }
        }
    }

    bool write(const std::string &path) const
    {
        return stbi_write_png(path.c_str(), width_, height_, 4, pixels_.data(),
                              width_ * 4) != 0;
    }

private:
    int                  width_;
    int                  height_;
    std::vector<uint8_t> pixels_;
};

// ---------------------------------------------------------------------------
// Backdrops
// ---------------------------------------------------------------------------

// A rolling hill: a wide, shallow ellipse whose top edge is the skyline.
void hill(Canvas &c, double cx, double top_y, double rx, double ry, Rgb col)
{
    c.ellipse(cx, top_y + ry, rx, ry, col);
}

void fence_post(Canvas &c, int x, int y_top, int y_bottom, Rgb wood, Rgb shade)
{
    c.rect(x, y_top, 8, y_bottom - y_top, wood);
    c.rect(x + 6, y_top, 2, y_bottom - y_top, shade);
    c.rect(x, y_top, 8, 3, shade);
}

// Screen 1: meadow.  The fence runs along the ground with a deliberate gap
// around x=360..440 where the fence hazard sprite will stand.
void backdrop_meadow(const std::string &dir)
{
    Canvas c(640, 480, {0, 0, 0});

    c.vgradient(0, 300, {58, 122, 224}, {186, 224, 250});
    c.htint(0, 300, 14);

    c.ellipse(520, 78, 46, 46, {255, 246, 196});
    c.ellipse(520, 78, 34, 34, {255, 252, 232});

    // Two clouds, flattened ellipse clusters.
    c.ellipse(140, 90, 54, 20, {248, 250, 255});
    c.ellipse(180, 82, 38, 24, {248, 250, 255});
    c.ellipse(102, 96, 30, 14, {248, 250, 255});
    c.ellipse(360, 140, 44, 15, {240, 246, 255});
    c.ellipse(392, 134, 30, 18, {240, 246, 255});

    hill(c, 120, 236, 300, 110, {96, 150, 108});
    hill(c, 470, 220, 280, 130, {74, 132, 92});
    hill(c, 300, 262, 420, 120, {112, 172, 104});

    c.vgradient(300, 480, {126, 190, 96}, {40, 92, 44});
    c.htint(300, 480, 10);

    // Grass tufts get darker with depth, which keeps the lower half from being
    // a flat ramp the encoder can solve with the palette alone.
    for (int i = 0; i < 90; i++)
    {
        const int x = (i * 137) % 640;
        const int y = 306 + (i * 53) % 168;
        const int s = 3 + (i % 4);
        const Rgb col = {40 + (i % 5) * 8, 110 - (y - 300) / 4, 40 + (i % 3) * 6};
        c.ellipse(x, y, s, s / 2 + 1, col);
    }

    const Rgb wood  = {150, 106, 62};
    const Rgb shade = {112, 74, 40};
    for (int x = 24; x < 640; x += 56)
    {
        if (x >= 352 && x <= 448)
        {
            continue;   // the gap the fence hazard stands in
        }
        fence_post(c, x, 336, 402, wood, shade);
    }
    for (int rail = 0; rail < 2; rail++)
    {
        const int y = 348 + rail * 30;
        for (int x = 0; x < 640; x++)
        {
            if (x >= 352 && x <= 448)
            {
                continue;
            }
            c.rect(x, y, 1, 6, rail == 0 ? wood : shade);
        }
    }

    c.write(dir + "/backdrop1-meadow.png");
}

// Screen 2: forest.  Trunks and canopy, so the encoder meets hard vertical
// colour edges as well as gradients.
void backdrop_forest(const std::string &dir)
{
    Canvas c(640, 480, {0, 0, 0});

    c.vgradient(0, 210, {28, 74, 46}, {132, 186, 128});
    c.htint(0, 210, 12);
    c.vgradient(210, 480, {96, 132, 74}, {44, 62, 32});

    // Canopy: overlapping ellipses, darker at the top of the screen.
    for (int i = 0; i < 26; i++)
    {
        const int x  = (i * 91) % 700 - 30;
        const int y  = 10 + (i * 37) % 120;
        const int rx = 50 + (i % 4) * 18;
        const int ry = 26 + (i % 3) * 10;
        const Rgb col = {20 + (i % 4) * 10, 70 + (i % 5) * 16, 34 + (i % 3) * 10};
        c.ellipse(x, y, rx, ry, col);
    }

    const int trunk_x[5] = {52, 188, 322, 458, 588};
    for (int i = 0; i < 5; i++)
    {
        const int x = trunk_x[i];
        const int w = 26 + (i % 3) * 8;
        c.rect(x, 60, w, 420, {104, 72, 44});
        c.rect(x, 60, w / 3, 420, {132, 96, 60});
        c.rect(x + w - w / 4, 60, w / 4, 420, {72, 48, 28});
        // Root flare.
        c.ellipse(x + w / 2.0, 452, w * 1.1, 18, {88, 60, 36});
    }

    // Ferns on the floor.
    for (int i = 0; i < 60; i++)
    {
        const int x = (i * 173) % 640;
        const int y = 300 + (i * 61) % 172;
        c.ellipse(x, y, 10 + (i % 4) * 3, 3, {52 + (i % 4) * 10, 104 + (i % 5) * 12, 44});
    }

    c.write(dir + "/backdrop2-forest.png");
}

// Screen 3: the clearing, with the mushroom house — the finale screen.
void backdrop_clearing(const std::string &dir)
{
    Canvas c(640, 480, {0, 0, 0});

    c.vgradient(0, 320, {96, 60, 140}, {255, 186, 118});
    c.htint(0, 320, 16);

    c.ellipse(150, 292, 62, 62, {255, 236, 176});
    c.vgradient(320, 480, {118, 156, 78}, {36, 70, 38});
    c.htint(320, 480, 8);

    // Tree line on the horizon.
    for (int i = 0; i < 22; i++)
    {
        const int x = i * 32 + (i % 3) * 6;
        c.ellipse(x, 306, 22, 16, {46, 74, 52});
    }

    // Mushroom house: stem, door, window, cap, spots.
    c.ellipse(400, 396, 74, 62, {238, 224, 186});
    c.rect(326, 340, 148, 56, {238, 224, 186});
    c.ellipse(400, 402, 62, 54, {248, 238, 208});

    c.ellipse(400, 410, 24, 40, {96, 62, 38});
    c.rect(376, 410, 48, 48, {96, 62, 38});
    c.ellipse(400, 388, 8, 8, {212, 176, 96});

    c.ellipse(348, 372, 13, 13, {124, 180, 220});
    c.ellipse(452, 372, 13, 13, {124, 180, 220});

    // The cap sits high enough to leave the windows and door showing.
    c.ellipse(400, 294, 154, 62, {206, 44, 44});
    c.ellipse(400, 288, 140, 52, {228, 62, 54});
    c.ellipse(360, 274, 22, 11, {252, 244, 232});
    c.ellipse(436, 280, 26, 12, {252, 244, 232});
    c.ellipse(400, 252, 18, 9, {252, 244, 232});
    c.ellipse(322, 300, 16, 8, {252, 244, 232});
    c.ellipse(472, 298, 18, 9, {252, 244, 232});

    // Toadstools scattered around for scale.
    for (int i = 0; i < 8; i++)
    {
        const int x = 60 + i * 74;
        if (x > 300 && x < 500)
        {
            continue;
        }
        const int y = 420 + (i % 3) * 14;
        c.rect(x - 3, y - 10, 6, 12, {240, 232, 208});
        c.ellipse(x, y - 12, 12, 7, {196, 56, 52});
    }

    c.write(dir + "/backdrop3-clearing.png");
}

// ---------------------------------------------------------------------------
// Sprites
// ---------------------------------------------------------------------------

constexpr Rgb SMURF_BLUE  = {102, 170, 238};
constexpr Rgb SMURF_DARK  = {51, 102, 170};
constexpr Rgb WHITE       = {255, 255, 255};
constexpr Rgb BLACK       = {0, 0, 0};
constexpr Rgb DRESS       = {255, 255, 255};
constexpr Rgb HAIR        = {255, 221, 68};
constexpr Rgb BAT_BODY    = {68, 34, 85};
constexpr Rgb BAT_WING    = {102, 51, 119};
constexpr Rgb WOOD        = {153, 102, 51};
constexpr Rgb WOOD_DARK   = {102, 68, 34};

// One smurf pose inside a 24x32 cell at (ox, oy).
//   legs   -2..2, horizontal offset of the two legs (walk cycle)
//   arms   0 = down, 1 = raised (jump), 2 = flung out (death)
//   squash rows removed from the body's height (duck)
//   xeyes  draw crossed-out eyes
//   female add hair and a dress instead of the hat and shorts
void draw_smurf(Canvas &c, int ox, int oy, int legs, int arms, int squash, bool xeyes,
                bool female)
{
    const int top = oy + squash;

    if (female)
    {
        // The side hair starts BELOW the eye rows on purpose: hair beside a head
        // that already has two eyes in it would make a 7-span row, one over the
        // overlay's per-row VIDCMD budget.
        c.ellipse(ox + 12, top + 6, 11, 5, HAIR);
        c.rect(ox + 1, top + 14, 4, 8, HAIR);
        c.rect(ox + 19, top + 14, 4, 8, HAIR);
    }
    else
    {
        c.ellipse(ox + 12, top + 7, 9, 6, WHITE);
        c.ellipse(ox + 12, top + 3, 5, 4, WHITE);
        c.rect(ox + 3, top + 7, 18, 2, WHITE);
    }

    // Head.
    c.ellipse(ox + 12, top + 13, 8, 6, SMURF_BLUE);
    c.rect(ox + 8, top + 11, 2, 3, BLACK);
    c.rect(ox + 14, top + 11, 2, 3, BLACK);
    if (xeyes)
    {
        c.rect(ox + 7, top + 10, 4, 1, BLACK);
        c.rect(ox + 13, top + 10, 4, 1, BLACK);
        c.rect(ox + 7, top + 14, 4, 1, BLACK);
        c.rect(ox + 13, top + 14, 4, 1, BLACK);
    }

    // Torso.
    const int body_top = top + 18;
    const int body_h   = 7 - squash / 2;
    c.ellipse(ox + 12, body_top + body_h / 2.0, 7, body_h / 2.0 + 2, SMURF_BLUE);

    // Arms.
    if (arms == 1)
    {
        c.rect(ox + 2, body_top - 6, 4, 8, SMURF_BLUE);
        c.rect(ox + 18, body_top - 6, 4, 8, SMURF_BLUE);
    }
    else if (arms == 2)
    {
        c.rect(ox + 0, body_top + 1, 6, 3, SMURF_BLUE);
        c.rect(ox + 18, body_top + 1, 6, 3, SMURF_BLUE);
    }
    else
    {
        c.rect(ox + 3, body_top, 3, 7, SMURF_BLUE);
        c.rect(ox + 18, body_top, 3, 7, SMURF_BLUE);
    }

    // Shorts or dress, then legs and shoes.
    const int hip = body_top + body_h;
    if (female)
    {
        c.ellipse(ox + 12, hip + 2, 10, 5, DRESS);
        c.rect(ox + 5, hip + 2, 14, 3, DRESS);
    }
    else
    {
        c.rect(ox + 5, hip - 1, 14, 4, WHITE);
    }

    const int leg_top = hip + (female ? 4 : 2);
    const int leg_h   = std::max(1, oy + 30 - leg_top);
    c.rect(ox + 6 - legs, leg_top, 5, leg_h, female ? SMURF_BLUE : WHITE);
    c.rect(ox + 13 + legs, leg_top, 5, leg_h, female ? SMURF_BLUE : WHITE);
    c.rect(ox + 5 - legs, oy + 30, 7, 2, BLACK);
    c.rect(ox + 12 + legs, oy + 30, 7, 2, BLACK);
}

void sprite_smurf(const std::string &dir)
{
    Canvas c(24 * 5, 32, KEY);
    draw_smurf(c, 0, 0, 2, 0, 0, false, false);    // walk0
    draw_smurf(c, 24, 0, -2, 0, 0, false, false);  // walk1
    draw_smurf(c, 48, 0, 0, 1, 0, false, false);   // jump
    draw_smurf(c, 72, 0, 0, 0, 6, false, false);   // duck
    draw_smurf(c, 96, 0, 3, 2, 0, true, false);    // death
    c.write(dir + "/smurf.png");
}

void sprite_smurfette(const std::string &dir)
{
    Canvas c(24 * 2, 32, KEY);
    draw_smurf(c, 0, 0, 2, 0, 0, false, true);
    draw_smurf(c, 24, 0, -2, 0, 0, false, true);
    c.write(dir + "/smurfette.png");
}

// A bat in a 24x16 cell: body plus two wings, one frame up and one down.
void draw_bat(Canvas &c, int ox, int oy, bool wings_up)
{
    c.ellipse(ox + 12, oy + 9, 4, 5, BAT_BODY);
    c.rect(ox + 9, oy + 2, 2, 3, BAT_BODY);   // ears
    c.rect(ox + 13, oy + 2, 2, 3, BAT_BODY);
    c.rect(ox + 10, oy + 7, 1, 1, WHITE);
    c.rect(ox + 13, oy + 7, 1, 1, WHITE);

    if (wings_up)
    {
        c.ellipse(ox + 5, oy + 4, 6, 3, BAT_WING);
        c.ellipse(ox + 19, oy + 4, 6, 3, BAT_WING);
        c.rect(ox + 1, oy + 3, 8, 2, BAT_WING);
        c.rect(ox + 15, oy + 3, 8, 2, BAT_WING);
    }
    else
    {
        c.ellipse(ox + 5, oy + 11, 6, 3, BAT_WING);
        c.ellipse(ox + 19, oy + 11, 6, 3, BAT_WING);
        c.rect(ox + 1, oy + 10, 8, 2, BAT_WING);
        c.rect(ox + 15, oy + 10, 8, 2, BAT_WING);
    }
}

void sprite_bat(const std::string &dir)
{
    Canvas c(24 * 2, 16, KEY);
    draw_bat(c, 0, 0, true);
    draw_bat(c, 24, 0, false);
    c.write(dir + "/bat.png");
}

// The fence hazard: 32x40, two posts and three rails, sized to fill the gap the
// meadow backdrop leaves open.
void sprite_fence(const std::string &dir)
{
    Canvas c(32, 40, KEY);
    c.rect(3, 4, 7, 36, WOOD);
    c.rect(22, 4, 7, 36, WOOD);
    c.rect(8, 4, 2, 36, WOOD_DARK);
    c.rect(27, 4, 2, 36, WOOD_DARK);
    c.rect(0, 10, 32, 5, WOOD);
    c.rect(0, 14, 32, 1, WOOD_DARK);
    c.rect(0, 24, 32, 5, WOOD);
    c.rect(0, 28, 32, 1, WOOD_DARK);
    c.rect(3, 0, 7, 4, WOOD_DARK);
    c.rect(22, 0, 7, 4, WOOD_DARK);
    c.write(dir + "/fence.png");
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "Usage: %s <output_directory>\n", argv[0]);
        return 1;
    }
    const std::string dir = argv[1];

    backdrop_meadow(dir);
    backdrop_forest(dir);
    backdrop_clearing(dir);
    sprite_smurf(dir);
    sprite_smurfette(dir);
    sprite_bat(dir);
    sprite_fence(dir);

    std::printf("%s: 3 backdrops (640x480), smurf/smurfette/bat/fence sheets\n", dir.c_str());
    return 0;
}
