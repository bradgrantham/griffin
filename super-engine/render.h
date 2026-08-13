// render.h — event consumers: PIXEL, COMPOSITOR and AUDIO models.
//
// Host-only in that it builds std::vector results, but dependency-free
// otherwise (no SDL, no POSIX, no file I/O) so this can be included straight
// into emulator/emulator.cpp later.  Each facility is independent: PIXEL turns
// the pixel-bit stream into 12-bit RGB, COMPOSITOR plays the VIDCMD stream
// against that and also forwards register writes into PIXEL, and AUDIO knows
// nothing about either.  The only thing they share is the raster clock.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "descriptor.h"
#include "interpret.h"

namespace SuperEngine
{

struct FrameImage
{
    // H_ACTIVE * V_ACTIVE R4G4B4 values, row major.
    std::vector<Rgb444> pixels;
};

struct RenderParams
{
    // Raster frame index of the first image to keep.  Frame 0 is the one during
    // which the CPU arms the engine, so it is always simulated but never kept.
    uint32_t first_frame   = 1;
    uint32_t frame_count   = 2;
    bool     audio_enabled = false;

    // Cross-chip skew, in pixel clocks, between a SET's slot and the pixel at
    // which its effect becomes visible.  PROVISIONAL — see descriptor.h.
    uint32_t skew_pix = SKEW_PIX_TARGET;
    uint32_t skew_cmp = SKEW_CMP_TARGET;
};

struct RenderResult
{
    std::vector<FrameImage>  frames;
    std::vector<uint8_t>     audio;      // interleaved L,R unsigned 8-bit
    std::vector<std::string> violations;

    uint32_t pixels_fifo_high = 0;
    uint32_t pixels_fifo_low  = PIXELS_FIFO_WORDS + 1;
    uint32_t pixels_underruns = 0;
    uint32_t pixels_overflows = 0;

    // Occupancy the instant after the line's first word is popped.  Plain low
    // water is uninformative for PIXELS — the FIFO empties at the tail of every
    // line by construction — whereas this is exactly the margin by which the
    // HBLANK deposits beat the drain, and it is what shrinks as lines get
    // heavier.
    uint32_t pixels_line_start_min = PIXELS_FIFO_WORDS + 1;
    uint32_t pixels_line_start_max = 0;

    uint32_t vidcmd_fifo_high         = 0;
    uint32_t vidcmd_fifo_low          = VIDCMD_FIFO_WORDS + 1;
    uint32_t vidcmd_underruns         = 0;
    uint32_t vidcmd_overflows         = 0;
    uint32_t vidcmd_straddles         = 0;   // a record crossed the h=640 boundary
    uint32_t vidcmd_pacing_violations = 0;
    uint32_t vidcmd_color_runs        = 0;   // RUN_COLOR records played (src 11)
    uint32_t vidcmd_pending_overflow  = 0;   // skew queue ran out (model limit)

    uint32_t audio_fifo_high       = 0;
    uint32_t audio_fifo_low        = AUDIO_FIFO_PAIRS + 1;
    uint32_t audio_underruns       = 0;
    uint32_t audio_overflows       = 0;
    uint32_t audio_pairs_deposited = 0;
    uint32_t audio_pairs_consumed  = 0;
};

// `events` must be in nondecreasing cycle order, which interpret() guarantees.
RenderResult render(const std::vector<DepositEvent> &events, const RenderParams &params);

// R4G4B4 -> 8 bits per channel by nibble replication (x17).  Same "replicate
// the field across the byte" rule emulator.cpp:1065 uses for R3G3B2, on a field
// that happens to divide the byte evenly.
void rgb444_to_rgb888(Rgb444 c, uint8_t &r, uint8_t &g, uint8_t &b);

}  // namespace SuperEngine
