// microham.h — micro-HAM (2 bits/pixel) encoder, decoder, and file formats.
//
// SEMANTICS.  This file mirrors, and must never contradict, the suite model in
// super-engine/render.cpp (PixelUnit::next_pixel, lines 137-186) and the RTL it
// models, cpld/pixel/pixel.v.  The suite is the truth; if the two ever disagree
// the fix belongs here.
//
//   stream is consumed MSB-first out of consecutive 16-bit words, exactly two
//   bits per pixel clock (author.cpp's BitPacker packs it the same way):
//
//     0 p        1 clock,  1 pixel   held <- p ? pal_fg : pal_bg
//     1 0  g r   2 clocks, 2 pixels  held.green <- {4{g}}, held.red  <- {4{r}}
//     1 1  g b   2 clocks, 2 pixels  held.green <- {4{g}}, held.blue <- {4{b}}
//
//   A 4-bit chroma code straddles two pixel clocks and the decoder cannot see
//   the chroma pair until the second, so the FIRST pixel of the code shows the
//   OLD held colour and the SECOND shows both channels updated together.
//   Chroma writes are whole-nibble replications of one bit: a channel touched
//   by a chroma code becomes 0x0 or 0xF, never anything between.  Only pal_fg /
//   pal_bg carry arbitrary 4-bit levels into held.
//
//   held reloads from pal_fg at the start of every visible line
//   (PixelUnit::begin_line, render.cpp:110).
//
// The encoder is therefore an exact dynamic program over a small reachable
// state set rather than a greedy walk: with two bits per pixel there is no rate
// constraint at all (every pixel costs exactly two bits whichever code is
// used), so the optimal line encoding for a given (pal_fg, pal_bg) is just a
// shortest path.

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

namespace MicroHam
{

inline constexpr int WIDTH          = 640;
inline constexpr int HEIGHT         = 480;
inline constexpr int WORDS_PER_LINE = 80;   // 640 pixels * 2 bits / 16

// 12-bit R4G4B4, the same packing descriptor.h uses: r << 8 | g << 4 | b.
using Rgb444 = uint16_t;

constexpr Rgb444 rgb444(int r, int g, int b)
{
    return static_cast<Rgb444>(((r & 0xF) << 8) | ((g & 0xF) << 4) | (b & 0xF));
}

constexpr int rgb444_r(Rgb444 c) { return (c >> 8) & 0xF; }
constexpr int rgb444_g(Rgb444 c) { return (c >> 4) & 0xF; }
constexpr int rgb444_b(Rgb444 c) { return c & 0xF; }

// Nibble replication, matching descriptor.h's rgb444_channel_to_8.
constexpr uint8_t nibble_to_8(int nibble)
{
    return static_cast<uint8_t>((nibble & 0xF) * 17);
}

constexpr int quantize_8_to_4(int v)
{
    return (v * 15 + 127) / 255;
}

constexpr int colour_error(Rgb444 a, Rgb444 b)
{
    const int dr = rgb444_r(a) - rgb444_r(b);
    const int dg = rgb444_g(a) - rgb444_g(b);
    const int db = rgb444_b(a) - rgb444_b(b);
    return dr * dr + dg * dg + db * db;
}

// ---------------------------------------------------------------------------
// Bit packing / unpacking — MSB first into consecutive 16-bit words
// ---------------------------------------------------------------------------

class BitPacker
{
public:
    explicit BitPacker(std::span<uint16_t> words) : words_(words) {}

    void put(uint32_t value, int count)
    {
        for (int i = 0; i < count; i++)
        {
            acc_ = static_cast<uint16_t>((acc_ << 1) | ((value >> (count - 1 - i)) & 1u));
            held_++;
            if (held_ == 16)
            {
                words_[index_] = acc_;
                index_++;
                acc_  = 0;
                held_ = 0;
            }
        }
    }

    int words_written() const { return index_; }
    int bits_held() const { return held_; }

private:
    std::span<uint16_t> words_;
    int      index_ = 0;
    uint16_t acc_   = 0;
    int      held_  = 0;
};

// ---------------------------------------------------------------------------
// Decoder — a transcription of PixelUnit::next_pixel in micro-HAM mode
// ---------------------------------------------------------------------------

// Decodes one line of WORDS_PER_LINE words into WIDTH colours.  `out` must hold
// WIDTH entries.  Mirrors render.cpp:137-186 exactly, including the one-pixel
// chroma latency and the line-start reload of held from pal_fg.
inline void decode_line(std::span<const uint16_t> words, Rgb444 pal_fg, Rgb444 pal_bg,
                        std::span<Rgb444> out)
{
    uint64_t bitbuf     = 0;
    int      bits_avail = 0;
    int      next_word  = 0;

    auto refill = [&](int need)
    {
        while (bits_avail < need)
        {
            const uint16_t w = (next_word < static_cast<int>(words.size()))
                                   ? words[next_word]
                                   : 0;
            next_word++;
            bitbuf = (bitbuf << 16) | w;
            bits_avail += 16;
        }
    };
    auto peek = [&](int n) -> uint32_t
    {
        return static_cast<uint32_t>((bitbuf >> (bits_avail - n)) & ((1u << n) - 1u));
    };
    auto take = [&](int n) -> uint32_t
    {
        const uint32_t v = peek(n);
        bits_avail -= n;
        bitbuf &= (bits_avail == 0) ? 0ull : ((1ull << bits_avail) - 1ull);
        return v;
    };

    Rgb444 held    = pal_fg;   // held reloads from pal_fg at every line start
    bool   pending = false;
    int    type    = 0;
    int    chroma_g = 0;
    int    chroma_v = 0;

    for (int x = 0; x < WIDTH; x++)
    {
        if (pending)
        {
            pending = false;
            (void)take(2);
            if (type == 0)
            {
                held = rgb444(chroma_v, chroma_g, rgb444_b(held));   // 10_g_r
            }
            else
            {
                held = rgb444(rgb444_r(held), chroma_g, chroma_v);   // 11_g_b
            }
            out[x] = held;
            continue;
        }

        refill(1);
        if (peek(1) == 0)
        {
            refill(2);
            const uint32_t code = take(2);
            held   = ((code & 1u) != 0) ? pal_fg : pal_bg;
            out[x] = held;
            continue;
        }

        refill(4);
        const uint32_t code = peek(4);
        (void)take(2);
        type     = static_cast<int>((code >> 2) & 1u);
        chroma_g = (((code >> 1) & 1u) != 0) ? 0xF : 0x0;
        chroma_v = (((code >> 0) & 1u) != 0) ? 0xF : 0x0;
        pending  = true;
        out[x]   = held;   // the OLD held colour, per pixel.v
    }
}

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------

// The reachable held-colour set for a given palette pair.  A chroma code always
// saturates the two channels it writes, so once the line has left the palette
// colours behind, at most one channel can still carry a palette level:
//
//   red   in {0, F, pal_fg.r, pal_bg.r}
//   green in {0, F}                       (or exactly pal_fg / pal_bg)
//   blue  in {0, F, pal_fg.b, pal_bg.b}
//
// That is at most 2 + 4*2*4 = 34 states, so an exact shortest path per line is
// cheap.  The set is closed under both chroma forms by construction.
struct StateSet
{
    std::vector<Rgb444>       colours;
    std::array<int16_t, 4096> index{};   // colour -> state index, -1 if absent

    int size() const { return static_cast<int>(colours.size()); }
    int find(Rgb444 c) const { return index[c]; }
};

inline StateSet build_states(Rgb444 pal_fg, Rgb444 pal_bg)
{
    StateSet s;
    s.index.fill(-1);

    auto add = [&](Rgb444 c)
    {
        if (s.index[c] < 0)
        {
            s.index[c] = static_cast<int16_t>(s.colours.size());
            s.colours.push_back(c);
        }
    };

    add(pal_fg);
    add(pal_bg);

    const std::array<int, 4> r_levels = {0x0, 0xF, rgb444_r(pal_fg), rgb444_r(pal_bg)};
    const std::array<int, 4> b_levels = {0x0, 0xF, rgb444_b(pal_fg), rgb444_b(pal_bg)};

    for (int r : r_levels)
    {
        for (int g : {0x0, 0xF})
        {
            for (int b : b_levels)
            {
                add(rgb444(r, g, b));
            }
        }
    }
    return s;
}

// One encoded line: the packed words plus the colours the hardware will show.
struct EncodedLine
{
    std::array<uint16_t, WORDS_PER_LINE> words{};
    std::array<Rgb444, WIDTH>            decoded{};
    int64_t                              cost = 0;   // sum of squared RGB444 error
};

// Scratch buffers, hoisted so a whole frame's worth of candidate palettes can
// be evaluated without touching the allocator.
struct EncoderScratch
{
    std::vector<int64_t> dp;       // (WIDTH + 1) * states
    std::vector<uint8_t> choice;   // same shape: 0 = bg, 1 = fg, 8|code = chroma
    std::vector<int>     err;      // states, per pixel scratch
};

inline constexpr int64_t COST_INF = static_cast<int64_t>(1) << 40;

// Exact optimal encoding of one line for a fixed palette pair.
inline void encode_line(std::span<const Rgb444> target, Rgb444 pal_fg, Rgb444 pal_bg,
                        EncoderScratch &scratch, EncodedLine &out)
{
    const StateSet states = build_states(pal_fg, pal_bg);
    const int      n      = states.size();

    scratch.dp.assign(static_cast<size_t>(WIDTH + 1) * n, COST_INF);
    scratch.choice.assign(static_cast<size_t>(WIDTH + 1) * n, 0);
    scratch.err.assign(n, 0);

    const int fg_index = states.find(pal_fg);
    const int bg_index = states.find(pal_bg);

    // Successor table for the eight chroma codes, per state.
    std::vector<int> chroma_next(static_cast<size_t>(n) * 8, 0);
    for (int s = 0; s < n; s++)
    {
        const Rgb444 c = states.colours[s];
        for (int code = 0; code < 8; code++)
        {
            const int type = (code >> 2) & 1;
            const int g    = (((code >> 1) & 1) != 0) ? 0xF : 0x0;
            const int v    = (((code >> 0) & 1) != 0) ? 0xF : 0x0;
            const Rgb444 nc = (type == 0) ? rgb444(v, g, rgb444_b(c))
                                          : rgb444(rgb444_r(c), g, v);
            chroma_next[static_cast<size_t>(s) * 8 + code] = states.find(nc);
        }
    }

    for (int s = 0; s < n; s++)
    {
        scratch.dp[static_cast<size_t>(WIDTH) * n + s] = 0;
    }

    for (int x = WIDTH - 1; x >= 0; x--)
    {
        const Rgb444   t  = target[x];
        int64_t *const dp = &scratch.dp[static_cast<size_t>(x) * n];
        uint8_t *const ch = &scratch.choice[static_cast<size_t>(x) * n];
        const int64_t *const dp_next = &scratch.dp[static_cast<size_t>(x + 1) * n];

        // The palette options do not depend on the incoming held colour.
        const int64_t cost_fg = colour_error(pal_fg, t) + dp_next[fg_index];
        const int64_t cost_bg = colour_error(pal_bg, t) + dp_next[bg_index];
        const int64_t pal_best  = std::min(cost_fg, cost_bg);
        const uint8_t pal_code  = (cost_fg <= cost_bg) ? 1 : 0;

        const bool           chroma_ok = (x + 2 <= WIDTH);
        const Rgb444         t2        = chroma_ok ? target[x + 1] : 0;
        const int64_t *const dp_next2  = chroma_ok
                                             ? &scratch.dp[static_cast<size_t>(x + 2) * n]
                                             : nullptr;

        if (chroma_ok)
        {
            for (int s = 0; s < n; s++)
            {
                scratch.err[s] = colour_error(states.colours[s], t2);
            }
        }

        for (int s = 0; s < n; s++)
        {
            int64_t best   = pal_best;
            uint8_t best_c = pal_code;

            if (chroma_ok)
            {
                const int64_t here = colour_error(states.colours[s], t);
                const int *const nxt = &chroma_next[static_cast<size_t>(s) * 8];
                for (int code = 0; code < 8; code++)
                {
                    const int     ns = nxt[code];
                    const int64_t c  = here + scratch.err[ns] + dp_next2[ns];
                    if (c < best)
                    {
                        best   = c;
                        best_c = static_cast<uint8_t>(8 | code);
                    }
                }
            }

            dp[s] = best;
            ch[s] = best_c;
        }
    }

    // Traceback.  held starts the line at pal_fg.
    BitPacker packer{std::span<uint16_t>(out.words)};
    int       s = fg_index;
    int       x = 0;
    out.cost = scratch.dp[static_cast<size_t>(0) * n + fg_index];

    while (x < WIDTH)
    {
        const uint8_t c = scratch.choice[static_cast<size_t>(x) * n + s];
        if ((c & 8) == 0)
        {
            packer.put(c & 1u, 2);
            s = (c & 1u) != 0 ? fg_index : bg_index;
            x += 1;
        }
        else
        {
            const int code = c & 7;
            packer.put(static_cast<uint32_t>(0b1000 | code), 4);
            s = chroma_next[static_cast<size_t>(s) * 8 + code];
            x += 2;
        }
    }

    decode_line(std::span<const uint16_t>(out.words), pal_fg, pal_bg,
                std::span<Rgb444>(out.decoded));
}

// ---------------------------------------------------------------------------
// Palette selection
// ---------------------------------------------------------------------------

// k-means in RGB444 space over one line, seeded by luma-sorted percentiles so
// the result is deterministic and does not need an RNG.
inline std::vector<Rgb444> line_candidates(std::span<const Rgb444> line, int k)
{
    std::vector<Rgb444> sorted(line.begin(), line.end());
    auto luma = [](Rgb444 c)
    {
        return 299 * rgb444_r(c) + 587 * rgb444_g(c) + 114 * rgb444_b(c);
    };
    std::sort(sorted.begin(), sorted.end(),
              [&](Rgb444 a, Rgb444 b) { return luma(a) < luma(b); });

    std::vector<std::array<int, 3>> centre(k);
    for (int i = 0; i < k; i++)
    {
        const Rgb444 c = sorted[(static_cast<size_t>(i) * 2 + 1) * sorted.size() /
                                (static_cast<size_t>(k) * 2)];
        centre[i] = {rgb444_r(c), rgb444_g(c), rgb444_b(c)};
    }

    for (int iter = 0; iter < 8; iter++)
    {
        std::vector<std::array<int64_t, 4>> acc(k, {0, 0, 0, 0});
        for (Rgb444 c : line)
        {
            int best = 0;
            int best_d = 1 << 30;
            for (int i = 0; i < k; i++)
            {
                const int dr = rgb444_r(c) - centre[i][0];
                const int dg = rgb444_g(c) - centre[i][1];
                const int db = rgb444_b(c) - centre[i][2];
                const int d  = dr * dr + dg * dg + db * db;
                if (d < best_d)
                {
                    best_d = d;
                    best   = i;
                }
            }
            acc[best][0] += rgb444_r(c);
            acc[best][1] += rgb444_g(c);
            acc[best][2] += rgb444_b(c);
            acc[best][3] += 1;
        }
        for (int i = 0; i < k; i++)
        {
            if (acc[i][3] > 0)
            {
                centre[i][0] = static_cast<int>((acc[i][0] + acc[i][3] / 2) / acc[i][3]);
                centre[i][1] = static_cast<int>((acc[i][1] + acc[i][3] / 2) / acc[i][3]);
                centre[i][2] = static_cast<int>((acc[i][2] + acc[i][3] / 2) / acc[i][3]);
            }
        }
    }

    std::vector<Rgb444> out;
    for (int i = 0; i < k; i++)
    {
        const Rgb444 c = rgb444(centre[i][0], centre[i][1], centre[i][2]);
        if (std::find(out.begin(), out.end(), c) == out.end())
        {
            out.push_back(c);
        }
    }

    // The line mean is a useful extra: k-means can leave a gap in the middle of
    // a smooth gradient, and one of the two palette slots usually wants to sit
    // there.
    int64_t sr = 0;
    int64_t sg = 0;
    int64_t sb = 0;
    for (Rgb444 c : line)
    {
        sr += rgb444_r(c);
        sg += rgb444_g(c);
        sb += rgb444_b(c);
    }
    const auto   count = static_cast<int64_t>(line.size());
    const Rgb444 mean  = rgb444(static_cast<int>(sr / count), static_cast<int>(sg / count),
                                static_cast<int>(sb / count));
    if (std::find(out.begin(), out.end(), mean) == out.end())
    {
        out.push_back(mean);
    }
    return out;
}

struct LineResult
{
    Rgb444      pal_fg = 0;
    Rgb444      pal_bg = 0;
    EncodedLine encoded;
};

// A cheap greedy walk used only to RANK candidate palette pairs — the winner is
// then re-encoded exactly by encode_line().  Ranking with the exact DP would be
// correct too, just ~30x slower per line for no visible gain.
inline int64_t approx_cost(std::span<const Rgb444> target, Rgb444 pal_fg, Rgb444 pal_bg)
{
    Rgb444  held = pal_fg;
    int64_t cost = 0;
    int     x    = 0;

    while (x < WIDTH)
    {
        const Rgb444 t = target[x];
        const int    e_fg = colour_error(pal_fg, t);
        const int    e_bg = colour_error(pal_bg, t);
        const int    pal_here = std::min(e_fg, e_bg);
        const Rgb444 pal_next = (e_fg <= e_bg) ? pal_fg : pal_bg;

        if (x + 2 > WIDTH)
        {
            cost += pal_here;
            break;
        }

        const Rgb444 t2 = target[x + 1];
        const int64_t pal_two = pal_here + std::min(colour_error(pal_fg, t2),
                                                    colour_error(pal_bg, t2));

        const int here_held = colour_error(held, t);
        int64_t   best      = COST_INF;
        Rgb444    best_next = held;
        for (int code = 0; code < 8; code++)
        {
            const int    type = (code >> 2) & 1;
            const int    g    = (((code >> 1) & 1) != 0) ? 0xF : 0x0;
            const int    v    = (((code >> 0) & 1) != 0) ? 0xF : 0x0;
            const Rgb444 nc   = (type == 0) ? rgb444(v, g, rgb444_b(held))
                                            : rgb444(rgb444_r(held), g, v);
            const int64_t c = here_held + colour_error(nc, t2);
            if (c < best)
            {
                best      = c;
                best_next = nc;
            }
        }

        if (best < pal_two)
        {
            cost += best;
            held = best_next;
            x += 2;
        }
        else
        {
            cost += pal_here;
            held = pal_next;
            x += 1;
        }
    }
    return cost;
}

// Chooses the palette pair by running the exact encoder over every ordered pair
// of candidates (plus the previous line's winner, for temporal coherence) and
// keeping the cheapest.  Ordered, not unordered: held starts the line at
// pal_fg, so the two slots are not interchangeable.
inline LineResult encode_line_auto(std::span<const Rgb444> target, EncoderScratch &scratch,
                                   int k, int finalists, Rgb444 prev_fg, Rgb444 prev_bg)
{
    std::vector<Rgb444> cand = line_candidates(target, k);
    if (cand.size() < 2)
    {
        // Degenerate line (a single colour): any distinct second slot will do.
        cand.push_back(static_cast<Rgb444>(cand.front() ^ 0x001));
    }

    struct Pair
    {
        Rgb444  fg   = 0;
        Rgb444  bg   = 0;
        int64_t rank = 0;
    };
    std::vector<Pair> pairs;
    for (Rgb444 fg : cand)
    {
        for (Rgb444 bg : cand)
        {
            if (fg != bg)
            {
                pairs.push_back({fg, bg, approx_cost(target, fg, bg)});
            }
        }
    }
    if (prev_fg != prev_bg)
    {
        pairs.push_back({prev_fg, prev_bg, approx_cost(target, prev_fg, prev_bg)});
    }

    std::sort(pairs.begin(), pairs.end(),
              [](const Pair &a, const Pair &b) { return a.rank < b.rank; });
    if (static_cast<int>(pairs.size()) > finalists)
    {
        pairs.resize(finalists);
    }

    LineResult  best;
    EncodedLine trial;
    bool        have_best = false;

    for (const Pair &p : pairs)
    {
        encode_line(target, p.fg, p.bg, scratch, trial);
        if (!have_best || trial.cost < best.encoded.cost)
        {
            have_best    = true;
            best.pal_fg  = p.fg;
            best.pal_bg  = p.bg;
            best.encoded = trial;
        }
    }
    return best;
}

// Palette-search effort.  Both the encoder and the round-trip harness read
// these, so the harness always validates exactly what the encoder emits.
inline constexpr int DEFAULT_CANDIDATES = 6;   // k-means clusters per line
inline constexpr int DEFAULT_FINALISTS  = 6;   // pairs given the exact DP

// ---------------------------------------------------------------------------
// Whole-frame encoding
// ---------------------------------------------------------------------------

struct EncodedFrame
{
    std::vector<uint16_t> plane;     // HEIGHT * WORDS_PER_LINE, in host order
    std::vector<Rgb444>   pal_fg;    // HEIGHT
    std::vector<Rgb444>   pal_bg;    // HEIGHT
    std::vector<Rgb444>   decoded;   // WIDTH * HEIGHT, this file's own decode
};

inline EncodedFrame encode_frame(std::span<const Rgb444> target)
{
    EncodedFrame f;
    f.plane.resize(static_cast<size_t>(HEIGHT) * WORDS_PER_LINE);
    f.pal_fg.resize(HEIGHT);
    f.pal_bg.resize(HEIGHT);
    f.decoded.resize(static_cast<size_t>(HEIGHT) * WIDTH);

    EncoderScratch scratch;
    Rgb444         prev_fg = 0;
    Rgb444         prev_bg = 0;

    for (int y = 0; y < HEIGHT; y++)
    {
        const std::span<const Rgb444> line = target.subspan(static_cast<size_t>(y) * WIDTH,
                                                            WIDTH);
        const LineResult r = encode_line_auto(line, scratch, DEFAULT_CANDIDATES,
                                              DEFAULT_FINALISTS, prev_fg, prev_bg);
        prev_fg = r.pal_fg;
        prev_bg = r.pal_bg;

        f.pal_fg[y] = r.pal_fg;
        f.pal_bg[y] = r.pal_bg;
        std::copy(r.encoded.words.begin(), r.encoded.words.end(),
                  f.plane.begin() + static_cast<size_t>(y) * WORDS_PER_LINE);
        std::copy(r.encoded.decoded.begin(), r.encoded.decoded.end(),
                  f.decoded.begin() + static_cast<size_t>(y) * WIDTH);
    }
    return f;
}

// 8-bit RGB triples (stbi's 3-component layout) to the quantized RGB444 frame
// the encoder works in.
inline std::vector<Rgb444> quantize_frame(const unsigned char *rgb8)
{
    std::vector<Rgb444> t(static_cast<size_t>(WIDTH) * HEIGHT);
    for (size_t i = 0; i < t.size(); i++)
    {
        t[i] = rgb444(quantize_8_to_4(rgb8[i * 3 + 0]),
                      quantize_8_to_4(rgb8[i * 3 + 1]),
                      quantize_8_to_4(rgb8[i * 3 + 2]));
    }
    return t;
}

// ---------------------------------------------------------------------------
// File formats
// ---------------------------------------------------------------------------
//
// .ham — headerless, HEIGHT lines of WORDS_PER_LINE big-endian 16-bit words, in
//        scanline order.  76800 bytes for 640x480.  This is the byte order of a
//        68000 memory image, so the file can be loaded straight into RAM and
//        DMA'd verbatim by ENGINE; it matches author.cpp's BitPacker writing
//        bus words at ascending byte addresses.
//
// .pal — headerless, HEIGHT pairs of big-endian 16-bit words: pal_fg then
//        pal_bg, one pair per scanline, values are 12-bit R4G4B4 in the low
//        bits.  1920 bytes for 480 lines.  The app turns each pair into two
//        VIDCMD SETs (SET_PIX_PAL_FG = 2, SET_PIX_PAL_BG = 3) issued in that
//        line's horizontal blanking, where a SET costs no active slot.

inline void put_be16(std::vector<uint8_t> &v, uint16_t w)
{
    v.push_back(static_cast<uint8_t>(w >> 8));
    v.push_back(static_cast<uint8_t>(w & 0xFF));
}

inline std::vector<uint8_t> plane_bytes(const EncodedFrame &f)
{
    std::vector<uint8_t> out;
    out.reserve(f.plane.size() * 2);
    for (uint16_t w : f.plane)
    {
        put_be16(out, w);
    }
    return out;
}

inline std::vector<uint8_t> palette_bytes(const EncodedFrame &f)
{
    std::vector<uint8_t> out;
    out.reserve(f.pal_fg.size() * 4);
    for (size_t y = 0; y < f.pal_fg.size(); y++)
    {
        put_be16(out, f.pal_fg[y]);
        put_be16(out, f.pal_bg[y]);
    }
    return out;
}

inline bool write_file(const char *path, const std::vector<uint8_t> &data)
{
    std::FILE *f = std::fopen(path, "wb");
    if (f == nullptr)
    {
        return false;
    }
    const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    std::fclose(f);
    return ok;
}

}  // namespace MicroHam
