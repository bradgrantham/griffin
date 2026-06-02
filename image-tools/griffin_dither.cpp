// griffin_dither.cpp — Convert any image to Griffin 1bpp + per-line R3G3B2 palette
//
// Build: g++ -std=c++23 -O3 -march=native -o griffin_dither griffin_dither.cpp
// Usage: griffin_dither <input_image> <output_bitmap> <output_palette>
//
// Output format:
//   bitmap:  640*480/8 = 38400 bytes, MSB-first packed scanlines
//   palette: 480 lines × 2 bytes = 960 bytes
//            byte 0 = color A (R3G3B2), byte 1 = color B (R3G3B2)
//            pixel=1 → color A, pixel=0 → color B
//
// Algorithm:
//   For each scanline (top to bottom):
//     1. Trial-dither against every unordered {A,B} R3G3B2 pair
//        using Floyd-Steinberg (rightward error only during trial)
//     2. Pick the pair with lowest sum-of-squared-error
//     3. Re-dither that line with the winner, propagating error
//        downward into the next scanline's source data

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_resize2.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <numeric>
#include <thread>
#include <vector>

// ---------- R3G3B2 helpers ----------

struct Color3f {
    float r, g, b;
    Color3f operator-(const Color3f& o) const { return {r - o.r, g - o.g, b - o.b}; }
    Color3f operator+(const Color3f& o) const { return {r + o.r, g + o.g, b + o.b}; }
    Color3f operator*(float s) const { return {r * s, g * s, b * s}; }
};

static inline float sq(float x) { return x * x; }

static float color_dist_sq(const Color3f& a, const Color3f& b) {
    return sq(a.r - b.r) + sq(a.g - b.g) + sq(a.b - b.b);
}

// Expand R3G3B2 byte to 0-255 per channel
static Color3f r3g3b2_to_float(uint8_t c) {
    // R: bits 7-5 (3 bits), G: bits 4-2 (3 bits), B: bits 1-0 (2 bits)
    unsigned r3 = (c >> 5) & 7;
    unsigned g3 = (c >> 2) & 7;
    unsigned b2 = c & 3;
    return {
        r3 * (255.0f / 7.0f),
        g3 * (255.0f / 7.0f),
        b2 * (255.0f / 3.0f),
    };
}

// Precompute all 256 palette entries
static std::array<Color3f, 256> build_palette_lut() {
    std::array<Color3f, 256> lut;
    for (int i = 0; i < 256; ++i)
        lut[i] = r3g3b2_to_float(static_cast<uint8_t>(i));
    return lut;
}

static const auto PALETTE_LUT = build_palette_lut();

// ---------- Candidate pair table (unordered, A <= B) ----------

struct PalettePair {
    uint8_t a, b;   // R3G3B2 indices, a <= b
};

static std::vector<PalettePair> build_pair_table() {
    std::vector<PalettePair> pairs;
    pairs.reserve(256 * 257 / 2);
    for (int a = 0; a < 256; ++a)
        for (int b = a; b < 256; ++b)
            pairs.push_back({static_cast<uint8_t>(a), static_cast<uint8_t>(b)});
    return pairs;
}

// ---------- Per-line trial dither ----------

constexpr int WIDTH = 640;
constexpr int HEIGHT = 480;

struct TrialResult {
    double error;
    // We don't store the bitmap here — we'll re-dither the winner
};

// Trial F-S dither of one scanline with a given {A,B} pair.
// Only propagates error rightward (not downward). Returns total squared error.
static TrialResult trial_dither_line(const Color3f* src, int width,
                                     const Color3f& colA, const Color3f& colB) {
    // Work on a mutable copy so rightward error diffusion is local
    std::vector<Color3f> row(src, src + width);
    double total_err = 0.0;

    for (int x = 0; x < width; ++x) {
        const Color3f& px = row[x];
        // Pick closer of A or B
        float dA = color_dist_sq(px, colA);
        float dB = color_dist_sq(px, colB);
        const Color3f& chosen = (dA <= dB) ? colA : colB;

        Color3f err = px - chosen;
        total_err += sq(err.r) + sq(err.g) + sq(err.b);

        // F-S rightward weights: 7/16 to (x+1)
        if (x + 1 < width)
            row[x + 1] = row[x + 1] + err * (7.0f / 16.0f);
        // We skip down-left, down, down-right since this is a single-line trial
    }
    return {total_err};
}

// Full F-S dither of one scanline with chosen {A,B}, writing bitmap bits
// and propagating error into next_row (which is the mutable source for the
// next scanline).
static void commit_dither_line(Color3f* row, int width,
                               const Color3f& colA, const Color3f& colB,
                               Color3f* next_row,  // may be nullptr for last line
                               uint8_t* bits_out) {
    // bits_out: WIDTH/8 bytes, MSB-first
    std::memset(bits_out, 0, width / 8);

    for (int x = 0; x < width; ++x) {
        const Color3f& px = row[x];
        float dA = color_dist_sq(px, colA);
        float dB = color_dist_sq(px, colB);
        bool pick_a = (dA <= dB);
        const Color3f& chosen = pick_a ? colA : colB;

        if (pick_a) {
            // pixel=1 → color A
            bits_out[x / 8] |= (0x80 >> (x % 8));
        }

        Color3f err = px - chosen;

        // F-S distribution
        auto distribute = [&](Color3f* target, float weight) {
            target->r += err.r * weight;
            target->g += err.g * weight;
            target->b += err.b * weight;
        };

        if (x + 1 < width)
            distribute(&row[x + 1], 7.0f / 16.0f);

        if (next_row) {
            if (x > 0)
                distribute(&next_row[x - 1], 3.0f / 16.0f);
            distribute(&next_row[x], 5.0f / 16.0f);
            if (x + 1 < width)
                distribute(&next_row[x + 1], 1.0f / 16.0f);
        }
    }
}

// ---------- Main ----------

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "Usage: %s <input_image> <output_bitmap> <output_palette>\n", argv[0]);
        return 1;
    }

    // Load image
    int w, h, channels;
    uint8_t* img = stbi_load(argv[1], &w, &h, &channels, 3);  // force RGB
    if (!img) {
        std::fprintf(stderr, "Failed to load %s\n", argv[1]);
        return 1;
    }
    std::fprintf(stderr, "Loaded %s: %dx%d (%d channels)\n", argv[1], w, h, channels);

    // Resize to 640x480
    std::vector<uint8_t> resized(WIDTH * HEIGHT * 3);
    stbir_resize_uint8_linear(img, w, h, 0,
                              resized.data(), WIDTH, HEIGHT, 0,
                              STBIR_RGB);
    stbi_image_free(img);

    // Convert to float working buffer (we'll modify this in-place as error propagates)
    std::vector<Color3f> fbuf(WIDTH * HEIGHT);
    for (int i = 0; i < WIDTH * HEIGHT; ++i) {
        fbuf[i] = {
            static_cast<float>(resized[i * 3 + 0]),
            static_cast<float>(resized[i * 3 + 1]),
            static_cast<float>(resized[i * 3 + 2]),
        };
    }
    resized.clear();

    // Build pair table
    auto pairs = build_pair_table();
    const int npairs = static_cast<int>(pairs.size());
    std::fprintf(stderr, "Palette pairs to test per line: %d\n", npairs);

    // Output buffers
    std::vector<uint8_t> bitmap(WIDTH / 8 * HEIGHT, 0);
    std::vector<uint8_t> palette_blob(HEIGHT * 2, 0);

    // Process each scanline
    for (int y = 0; y < HEIGHT; ++y) {
        Color3f* row = &fbuf[y * WIDTH];

        // Evaluate all pairs in parallel via std::thread
        std::vector<double> errors(npairs);
        const int nthreads = static_cast<int>(std::thread::hardware_concurrency());
        const int chunk = (npairs + nthreads - 1) / nthreads;

        auto worker = [&](int begin, int end) {
            for (int idx = begin; idx < end; ++idx) {
                const auto& p = pairs[idx];
                TrialResult result = trial_dither_line(row, WIDTH,
                                                PALETTE_LUT[p.a], PALETTE_LUT[p.b]);
                errors[idx] = result.error;
            }
        };

        std::vector<std::thread> threads;
        for (int t = 0; t < nthreads; ++t) {
            int begin = t * chunk;
            int end = std::min(begin + chunk, npairs);
            if (begin < end)
                threads.emplace_back(worker, begin, end);
        }
        for (auto& t : threads) t.join();

        // Find best pair
        int best = 0;
        double best_err = errors[0];
        for (int i = 1; i < npairs; ++i) {
            if (errors[i] < best_err) {
                best_err = errors[i];
                best = i;
            }
        }

        const auto& winner = pairs[best];
        const Color3f& colA = PALETTE_LUT[winner.a];
        const Color3f& colB = PALETTE_LUT[winner.b];

        // Commit: dither this line for real, propagating error downward
        Color3f* next_row = (y + 1 < HEIGHT) ? &fbuf[(y + 1) * WIDTH] : nullptr;
        commit_dither_line(row, WIDTH, colA, colB, next_row,
                           &bitmap[y * (WIDTH / 8)]);

        // Store palette entry: color A first, color B second (MSB first / big-endian)
        palette_blob[y * 2 + 0] = winner.a;
        palette_blob[y * 2 + 1] = winner.b;

        if (y % 48 == 0)
            std::fprintf(stderr, "  line %3d/%d  best pair: 0x%02X,0x%02X  err=%.0f\n",
                         y, HEIGHT, winner.a, winner.b, best_err);
    }

    // Write outputs
    {
        std::ofstream f(argv[2], std::ios::binary);
        f.write(reinterpret_cast<const char*>(bitmap.data()), bitmap.size());
        std::fprintf(stderr, "Wrote bitmap: %s (%zu bytes)\n", argv[2], bitmap.size());
    }
    {
        std::ofstream f(argv[3], std::ios::binary);
        f.write(reinterpret_cast<const char*>(palette_blob.data()), palette_blob.size());
        std::fprintf(stderr, "Wrote palette: %s (%zu bytes)\n", argv[3], palette_blob.size());
    }

    std::fprintf(stderr, "Done.\n");
    return 0;
}
