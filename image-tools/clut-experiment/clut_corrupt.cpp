// clut_corrupt.cpp — deliberately invalid .clut files for the §6 validator gate
//
// Build: see the Makefile in this directory.
// Usage: clut_corrupt --kind kexceed|overlap|short <in.clut> <out.clut>
//
// A validator nobody has watched reject anything is not a validator, so
// check.sh builds one file of each kind and requires clut_decode to refuse it.
//
//   kexceed  line 100 loads K+1 entries, all indices distinct (so the only rule
//            it breaks is the reload budget).  Needs a source stream with K < 16.
//   overlap  line 100's second span is moved on top of its first.  Needs a
//            source stream with at least two spans on that line.
//   short    the file is truncated inside the last line's pixel words.

#include "clut_format.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace Clut;

namespace
{

constexpr int VICTIM_LINE = 100;

}  // namespace

int main(int argc, char **argv)
{
    const char *kind = nullptr;
    int         arg  = 1;
    while (arg < argc && std::strncmp(argv[arg], "--", 2) == 0)
    {
        if (std::strcmp(argv[arg], "--kind") == 0 && arg + 1 < argc)
        {
            kind = argv[arg + 1];
            arg += 2;
        }
        else
        {
            std::fprintf(stderr, "%s: unknown option %s\n", argv[0], argv[arg]);
            return 1;
        }
    }
    if (kind == nullptr || argc - arg != 2)
    {
        std::fprintf(stderr, "Usage: %s --kind kexceed|overlap|short <in.clut> <out.clut>\n",
                     argv[0]);
        return 1;
    }
    const char *const in_path  = argv[arg];
    const char *const out_path = argv[arg + 1];

    std::vector<uint8_t> data;
    if (!read_bytes(in_path, data))
    {
        std::fprintf(stderr, "%s: cannot read %s\n", argv[0], in_path);
        return 1;
    }

    if (std::strcmp(kind, "short") == 0)
    {
        if (data.size() < 64)
        {
            std::fprintf(stderr, "%s: %s is too small to truncate\n", argv[0], in_path);
            return 1;
        }
        data.resize(data.size() - 37);
        if (!write_bytes(out_path, data))
        {
            std::fprintf(stderr, "%s: cannot write %s\n", argv[0], out_path);
            return 1;
        }
        std::printf("%s: truncated by 37 bytes\n", out_path);
        return 0;
    }

    Stream      stream;
    std::string error;
    if (!parse_stream(data, stream, error))
    {
        std::fprintf(stderr, "%s: %s is already unparseable: %s\n", argv[0], in_path,
                     error.c_str());
        return 1;
    }

    Line &line = stream.lines[static_cast<size_t>(VICTIM_LINE)];

    if (std::strcmp(kind, "kexceed") == 0)
    {
        if (stream.k >= CLUT_SIZE)
        {
            std::fprintf(stderr, "%s: %s has K=%d; a load count above K needs K < %d so the "
                                 "indices can stay distinct\n",
                         argv[0], in_path, stream.k, CLUT_SIZE);
            return 1;
        }
        line.loads.clear();
        for (int i = 0; i <= stream.k; i++)
        {
            line.loads.push_back(Load{ static_cast<uint8_t>(i), rgb444(i, i, i) });
        }
        std::printf("%s: line %d loads %d entries against K=%d\n", out_path, VICTIM_LINE,
                    static_cast<int>(line.loads.size()), stream.k);
    }
    else if (std::strcmp(kind, "overlap") == 0)
    {
        if (line.spans.size() < 2)
        {
            std::fprintf(stderr, "%s: %s has %zu spans on line %d, need 2\n", argv[0], in_path,
                         line.spans.size(), VICTIM_LINE);
            return 1;
        }
        line.spans[1].x      = line.spans[0].x;
        line.spans[1].length = line.spans[0].length;
        std::printf("%s: line %d spans 0 and 1 both at x=%d\n", out_path, VICTIM_LINE,
                    line.spans[0].x);
    }
    else
    {
        std::fprintf(stderr, "%s: unknown kind %s\n", argv[0], kind);
        return 1;
    }

    if (!write_stream(out_path, stream))
    {
        std::fprintf(stderr, "%s: cannot write %s\n", argv[0], out_path);
        return 1;
    }
    return 0;
}
