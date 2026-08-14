// sprite2spans.cpp — PNG sprite sheet -> C++ header of per-row solid spans
//
// Build: c++ -std=c++23 -O2 -I. -o sprite2spans sprite2spans.cpp
// Usage: sprite2spans -o <out.h> [--key RRGGBB] [--max-spans N] [--prefix P]
//                     <sheet.png,W,H,frame0,frame1,...> ...
//
// Each sheet argument is a comma-separated spec: the PNG, the frame width and
// height, then one name per frame.  Frames are read left to right along the
// top row of the sheet, then wrap to the next row of frames.
//
// A pixel is transparent if its alpha is below 128 or if it equals the key
// colour (default magenta FF00FF).  Every run of identical opaque pixels in a
// row becomes one span {x, width, rgb444}.
//
// The header stores rgb444 and nothing else about how a span is drawn.  Whether
// a span goes out as a single RUN_COLOR word (only possible when the colour is
// one of the eight saturated corners, i.e. every channel is 0x0 or 0xF) or as a
// SET of cmp_held_fg followed by a RUN is the APP's decision at draw time, and
// it costs a different number of VIDCMD slots either way.
//
// BUDGET.  Overlay art is delivered as VIDCMD records inside the same 640-slot
// line as everything else, so a row with too many spans cannot be drawn.  The
// tool warns above --max-spans (default 6) and reports the worst row per frame.

#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{

struct Span
{
    int x     = 0;
    int width = 0;
    int rgb444 = 0;
};

struct Frame
{
    std::string       name;
    std::string       symbol;   // upper-case C identifier
    int               width  = 0;
    int               height = 0;
    std::vector<std::vector<Span>> rows;
    int               max_row_spans = 0;
    int               total_spans   = 0;
};

int quantize_8_to_4(int v)
{
    return (v * 15 + 127) / 255;
}

std::string to_symbol(const std::string &s)
{
    std::string out;
    for (char c : s)
    {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
        {
            out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }
        else
        {
            out.push_back('_');
        }
    }
    if (out.empty() || (out[0] >= '0' && out[0] <= '9'))
    {
        out.insert(out.begin(), '_');
    }
    return out;
}

std::vector<std::string> split(const std::string &s, char sep)
{
    std::vector<std::string> out;
    std::string              cur;
    for (char c : s)
    {
        if (c == sep)
        {
            out.push_back(cur);
            cur.clear();
        }
        else
        {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

bool load_sheet(const std::string &spec, uint32_t key, const std::string &prefix,
                std::vector<Frame> &frames)
{
    const std::vector<std::string> parts = split(spec, ',');
    if (parts.size() < 4)
    {
        std::fprintf(stderr, "bad sheet spec '%s': want file.png,W,H,name...\n", spec.c_str());
        return false;
    }

    const std::string path = parts[0];
    const int frame_w = std::atoi(parts[1].c_str());
    const int frame_h = std::atoi(parts[2].c_str());
    if (frame_w <= 0 || frame_h <= 0)
    {
        std::fprintf(stderr, "bad frame size in '%s'\n", spec.c_str());
        return false;
    }

    int width  = 0;
    int height = 0;
    int comps  = 0;
    unsigned char *image = stbi_load(path.c_str(), &width, &height, &comps, 4);
    if (image == nullptr)
    {
        std::fprintf(stderr, "cannot read %s\n", path.c_str());
        return false;
    }

    const int per_row = width / frame_w;
    if (per_row <= 0)
    {
        std::fprintf(stderr, "%s is %d wide, narrower than one %d frame\n", path.c_str(),
                     width, frame_w);
        stbi_image_free(image);
        return false;
    }

    bool ok = true;
    for (size_t i = 3; i < parts.size(); i++)
    {
        const int index = static_cast<int>(i - 3);
        const int fx    = (index % per_row) * frame_w;
        const int fy    = (index / per_row) * frame_h;
        if (fx + frame_w > width || fy + frame_h > height)
        {
            std::fprintf(stderr, "%s: frame '%s' (index %d) falls outside the %dx%d sheet\n",
                         path.c_str(), parts[i].c_str(), index, width, height);
            ok = false;
            break;
        }

        Frame f;
        f.name   = parts[i];
        f.symbol = to_symbol(prefix + parts[i]);
        f.width  = frame_w;
        f.height = frame_h;
        f.rows.resize(frame_h);

        for (int y = 0; y < frame_h; y++)
        {
            std::vector<Span> &row = f.rows[y];
            int run_start = -1;
            int run_col   = 0;

            for (int x = 0; x <= frame_w; x++)
            {
                bool opaque = false;
                int  col    = 0;
                if (x < frame_w)
                {
                    const size_t o = (static_cast<size_t>(fy + y) * width + (fx + x)) * 4;
                    const uint32_t rgb = (static_cast<uint32_t>(image[o + 0]) << 16) |
                                         (static_cast<uint32_t>(image[o + 1]) << 8) |
                                         static_cast<uint32_t>(image[o + 2]);
                    opaque = image[o + 3] >= 128 && rgb != key;
                    col    = (quantize_8_to_4(image[o + 0]) << 8) |
                             (quantize_8_to_4(image[o + 1]) << 4) |
                             quantize_8_to_4(image[o + 2]);
                }

                if (run_start >= 0 && (!opaque || col != run_col))
                {
                    row.push_back({run_start, x - run_start, run_col});
                    run_start = -1;
                }
                if (opaque && run_start < 0)
                {
                    run_start = x;
                    run_col   = col;
                }
            }

            f.max_row_spans = std::max(f.max_row_spans, static_cast<int>(row.size()));
            f.total_spans += static_cast<int>(row.size());
        }
        frames.push_back(std::move(f));
    }

    stbi_image_free(image);
    return ok;
}

}  // namespace

int main(int argc, char **argv)
{
    std::string              out_path;
    std::string              prefix;
    uint32_t                 key       = 0xFF00FF;
    int                      max_spans = 6;
    std::vector<std::string> sheets;

    for (int i = 1; i < argc; i++)
    {
        const std::string a = argv[i];
        if (a == "-o" && i + 1 < argc)
        {
            out_path = argv[++i];
        }
        else if (a == "--key" && i + 1 < argc)
        {
            key = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 16));
        }
        else if (a == "--max-spans" && i + 1 < argc)
        {
            max_spans = std::atoi(argv[++i]);
        }
        else if (a == "--prefix" && i + 1 < argc)
        {
            prefix = argv[++i];
        }
        else if (!a.empty() && a[0] == '-')
        {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 1;
        }
        else
        {
            sheets.push_back(a);
        }
    }

    if (out_path.empty() || sheets.empty())
    {
        std::fprintf(stderr,
                     "Usage: %s -o <out.h> [--key RRGGBB] [--max-spans N] [--prefix P] "
                     "<sheet.png,W,H,frame...> ...\n",
                     argv[0]);
        return 1;
    }

    std::vector<Frame> frames;
    for (const std::string &s : sheets)
    {
        if (!load_sheet(s, key, prefix, frames))
        {
            return 1;
        }
    }

    int over_budget = 0;
    for (const Frame &f : frames)
    {
        if (f.max_row_spans > max_spans)
        {
            std::fprintf(stderr,
                         "warning: frame %s has a row with %d spans, over the %d-span "
                         "VIDCMD budget\n",
                         f.name.c_str(), f.max_row_spans, max_spans);
            over_budget++;
        }
    }

    std::FILE *out = std::fopen(out_path.c_str(), "w");
    if (out == nullptr)
    {
        std::fprintf(stderr, "cannot write %s\n", out_path.c_str());
        return 1;
    }

    std::fprintf(out,
                 "// Generated by sprite2spans — do not edit.\n"
                 "//\n"
                 "// Each frame is stored as per-row lists of solid horizontal spans in\n"
                 "// 12-bit R4G4B4.  A span is drawn by the app as either one RUN_COLOR word\n"
                 "// (possible only when every channel is 0x0 or 0xF) or a SET of\n"
                 "// cmp_held_fg followed by a RUN of held_fg; the header does not choose.\n"
                 "//\n"
                 "// Span budget checked at generation time: %d per row.\n"
                 "\n"
                 "#pragma once\n"
                 "\n"
                 "#include <cstdint>\n"
                 "\n"
                 "struct SpriteSpan\n"
                 "{\n"
                 "    uint16_t x;        // pixels from the frame's left edge\n"
                 "    uint16_t width;    // pixels, always >= 1\n"
                 "    uint16_t rgb444;   // 0x0RGB\n"
                 "};\n"
                 "\n"
                 "struct SpriteRow\n"
                 "{\n"
                 "    uint16_t first;    // index into the frame's span array\n"
                 "    uint16_t count;    // spans in this row, 0 for a fully transparent row\n"
                 "};\n"
                 "\n"
                 "struct SpriteFrame\n"
                 "{\n"
                 "    const char       *name;\n"
                 "    uint16_t          width;\n"
                 "    uint16_t          height;\n"
                 "    uint16_t          max_row_spans;\n"
                 "    const SpriteRow  *rows;    // `height` entries\n"
                 "    const SpriteSpan *spans;\n"
                 "};\n"
                 "\n"
                 "inline constexpr int SPRITE_SPAN_BUDGET = %d;\n"
                 "\n",
                 max_spans, max_spans);

    for (const Frame &f : frames)
    {
        std::fprintf(out, "// %s — %dx%d, %d spans, worst row %d\n", f.name.c_str(),
                     f.width, f.height, f.total_spans, f.max_row_spans);
        std::fprintf(out, "inline constexpr SpriteSpan %s_SPANS[] = {\n", f.symbol.c_str());
        if (f.total_spans == 0)
        {
            // A zero-length array is ill-formed; a fully transparent frame still
            // needs one element for the pointer to be valid.
            std::fprintf(out, "    { 0, 0, 0x000 },\n");
        }
        for (int y = 0; y < f.height; y++)
        {
            if (f.rows[y].empty())
            {
                continue;
            }
            std::fprintf(out, "   ");
            for (const Span &s : f.rows[y])
            {
                std::fprintf(out, " { %3d, %3d, 0x%03X },", s.x, s.width, s.rgb444);
            }
            std::fprintf(out, "   // row %d\n", y);
        }
        std::fprintf(out, "};\n");

        std::fprintf(out, "inline constexpr SpriteRow %s_ROWS[] = {\n", f.symbol.c_str());
        int first = 0;
        for (int y = 0; y < f.height; y++)
        {
            const int n = static_cast<int>(f.rows[y].size());
            std::fprintf(out, "    { %3d, %2d },\n", first, n);
            first += n;
        }
        std::fprintf(out, "};\n");

        std::fprintf(out,
                     "inline constexpr SpriteFrame %s = { \"%s\", %d, %d, %d, %s_ROWS, "
                     "%s_SPANS };\n\n",
                     f.symbol.c_str(), f.name.c_str(), f.width, f.height, f.max_row_spans,
                     f.symbol.c_str(), f.symbol.c_str());
    }

    std::fprintf(out, "inline constexpr const SpriteFrame *SPRITE_FRAMES[] = {\n");
    for (const Frame &f : frames)
    {
        std::fprintf(out, "    &%s,\n", f.symbol.c_str());
    }
    std::fprintf(out, "};\n");
    std::fprintf(out, "inline constexpr int SPRITE_FRAME_COUNT = %zu;\n", frames.size());
    std::fclose(out);

    std::printf("%s: %zu frames", out_path.c_str(), frames.size());
    for (const Frame &f : frames)
    {
        std::printf(", %s %dx%d/%d spans (worst row %d)", f.name.c_str(), f.width, f.height,
                    f.total_spans, f.max_row_spans);
    }
    std::printf("\n");
    if (over_budget > 0)
    {
        std::printf("%d frame(s) exceed the %d-span row budget\n", over_budget, max_spans);
    }
    return 0;
}
