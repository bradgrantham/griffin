// frame_scheduler.h — the 68000's side of AUDIO in REGIONS instead of lines,
// with deposits handed over BY REFERENCE.
//
// scheduler.h keeps the four sample FIFOs fed one scanline at a time and hands
// the caller a byte per deposit.  That contract costs a 14 MHz 68000 about a
// million SYSCLK a field (525 lines x 4 channels of mirror bookkeeping plus a
// byte store per sample; measured, griffin-ports/smurf/app/DESIGN-NATIVE.md
// "What does NOT work").  This class keeps the same laws and the same policy
// and pays for them once per REGION of R lines, and it never touches a sample:
// it hands out RUNS, each a pointer into a static ring plus a count, which an
// ENGINE descriptor can name directly.
//
// ---------------------------------------------------------------------------
// THE POP LAW AS A WINDOW, AND WHY THE ACCUMULATOR IS SEEDED FROM COUNT
// ---------------------------------------------------------------------------
//
// audio_chip.h: at every STEP, a channel whose COUNT is 0 POPS and reloads
// COUNT <= PERIOD; otherwise COUNT <= COUNT - 1.  So with COUNT = c at the
// first step of a window of S steps, the pops fall at step offsets
//
//     c, c + D, c + 2D, ...        D = PERIOD + 1
//
// and over the window there are
//
//     pops = floor((S - 1 - c) / D) + 1        (0 if c > S - 1)
//
// of them, leaving COUNT = c + pops*D - S.  Substituting the COMPLEMENT
//
//     r = PERIOD - c = D - 1 - c               (r in [0, D) while c <= PERIOD)
//
// turns both halves into one division and one remainder of the SAME dividend:
//
//     pops = floor((S + r) / D)                r' = (S + r) mod D
//
// (check: floor((S - 1 - c)/D) + 1 = floor((S - 1 - c + D)/D) = floor((S + r)/D),
// and c' = D - 1 - r' follows from c + pops*D - S with pops*D = S + r - r'.)
// Both hold for r outside [0, D) as well, which is what makes the PERIOD case
// below work; the c > S - 1 case is covered because then S + r < D and the
// floor is 0.
//
// So for a FIXED window length S the whole law is two numbers per PERIOD:
//
//     q[PERIOD]   = S / D      rem[PERIOD] = S % D
//     t = r + rem;  pops = q;  if (t >= D) { t -= D; ++pops; }  r' = t
//
// which is exact because rem < D and r < D put t below 2D.  There is no
// division, no modulo, no multiply and nothing wider than an int in it.  The
// tables are built once, in reset(), for the two window lengths a field uses
// (R lines and the short last region) — 50*R and 50*R_last steps.
//
// SEEDING.  r is the complement of the chip's COUNT, so it IS seeded from
// COUNT: at reset the chip has COUNT 0 and PERIOD 0, i.e. r = 0, and after
// that r carries forward as r' from region to region.  Nothing reads the chip
// (it cannot be read), exactly as scheduler.h's mirror does not.
//
// A PERIOD WRITE DOES NOT RESET COUNT.  audio_chip.h: COUNT is reloaded only
// at a pop, so a PERIOD record changes D while COUNT keeps counting down.  In
// complement form that is a RESEED of r at the same c:
//
//     r_new = PERIOD_new - c = r_old + (PERIOD_new - PERIOD_old)
//
// which is negative exactly when COUNT is larger than the new PERIOD — the
// case scheduler.h's line_pops() spells out as "count >= STEPS_PER_LINE", a
// channel that will not pop for up to 255 steps after a PERIOD drop.  The
// window form handles it without a special case: r is normalised into [0, D)
// by adding D k times and subtracting k from the window's pop count, which is
// exact by the identity above.  With S >= 300 steps (R >= 6 lines) and COUNT
// at most 255 the normalised count can never go negative, so no pop is ever
// owed across a region boundary.  The loop runs only on a PERIOD DECREASE and
// at most 255/D times; T9 proves the result against line_pops() summed over
// the region for all 256 periods and every accumulator state.
//
// ---------------------------------------------------------------------------
// POLICY — scheduler.h's, with burst_lines := R and the stagger dropped
// ---------------------------------------------------------------------------
//
//   * Every active channel is topped up ONCE PER REGION, at the region's first
//     line, to `pops over this region + margin` (a queued faster PERIOD is
//     assumed for the prediction so a sweep upward cannot drain the FIFO),
//     capped by the room in the FIFO.
//   * A PERIOD record is issued at the first REGION BOUNDARY by which every
//     sample that was in the FIFO when the change was asked for has been
//     popped (`pending_pops_left`), so the new rate applies to the new table.
//     scheduler.h resolves that to a line; this resolves it to a region, so a
//     PERIOD lands up to R-1 lines LATER than the per-line scheduler would
//     land it — later, never earlier, which is the safe direction: the rule is
//     "not before the old samples have drained".
//   * VOLUME changes go out at the next region.
//   * Control records are two bytes {register, value} to DEST_CONTROL, and
//     they are emitted BEFORE the region's sample runs so a PERIOD is in force
//     when the samples it belongs to are popped.
//   * The deposits of a whole region land at its FIRST line, up to R-1 lines
//     EARLY.  Earlier is free: the FIFO holds 256 bytes and `need` is capped
//     by the room, so nothing is ever dropped.
//
// ---------------------------------------------------------------------------
// DEPOSITS BY REFERENCE: THE RINGS
// ---------------------------------------------------------------------------
//
// ENGINE moves WORDS, and a sample FIFO takes the low byte of each (the high
// byte is ignored, audio_chip.h).  So a descriptor's source has to be an array
// of uint16_t whose low bytes are the samples, and that is what a RING is: the
// caller's byte table, expanded once into static word storage, with the
// pattern repeated until
//
//     ring_words >= length + FRAME_MAX_RUN_WORDS
//
// so that a run of at most FRAME_MAX_RUN_WORDS words starting anywhere in
// [0, length) is CONTIGUOUS — no wrap, one descriptor per 32 words and no
// more.  A table too long to duplicate (the 32,767-sample white-noise
// sequence) is stored once and a run that crosses the end is SPLIT into two
// runs, which is two descriptors.
//
// Rings are keyed by (table pointer, length), which is exactly the identity
// psg_channels.h hands over: square_table(L) is a distinct pointer per L and
// the noise sequences are one pointer each with the length the repeat implies.
// They are built on first use out of one static pool and never freed; a
// stream's `repeat` is NOT applied at run time, so...
//
// THE ONE PLACE A WORD IS WRITTEN: repeat > 1.  psg_channels.h uses `repeat`
// for a noise shift rate below the chip's 6.15 kHz floor, where each sequence
// bit is deposited `repeat` times.  Expanding that into a ring would cost the
// whole sequence times the repeat, and psg_tables.h produces 57 distinct
// repeats over the PSG's reload range: 7.45 MB of word rings, which is not a
// design, it is a wish.  Those streams are STAGED instead — the scheduler
// writes the region's words into a window the caller supplies (the region's
// own payload) and the run points at that window.  It is bounded: the fastest
// (PERIOD+1) x repeat psg_tables.h picks for repeat >= 2 is 268 steps, so a
// 64-line region asks for at most 26 words.  Everything else — every tone, and
// noise at repeat 1 — is pure reference.
//
// ---------------------------------------------------------------------------
// DRIVING IT FROM PsgChannels WITHOUT EDITING PsgChannels
// ---------------------------------------------------------------------------
//
// PsgChannels::emit() takes a `Scheduler&` and reads `scheduler.channel(c)`
// for the phase it carries across a retarget.  Rather than change that
// signature, a FrameScheduler OWNS a Scheduler as an INBOX: emit() writes its
// requests there (set_stream / set_period / set_volume / stop), the inbox's
// line() is NEVER called, and adopt_requests() then
//
//   * compares the inbox's ChannelState against the values this class last
//     WROTE into it, so every set_stream/set_period/set_volume is detected
//     exactly (set_stream is the only thing that moves those fields, and the
//     last write is known), and
//   * publishes this scheduler's advanced position and repeat_count back into
//     the inbox with set_stream(), so the next emit()'s carry_phase() sees the
//     phase the streams are really at.
//
// Use:
//     psg.write(byte) ...                       // the game's PSG bytes
//     psg.emit(frame_scheduler.inbox());        // once per game tick
//     frame_scheduler.adopt_requests();         // once, right after emit()
//     for each region: frame_scheduler.region(out, window, window_words);
//
// Bare metal capable: <cstdint>/<cstddef> only, no allocation, no floating
// point, no I/O, and no division, modulo, multiply or 64-bit arithmetic in
// anything region() calls.

#pragma once

#include <cstddef>
#include <cstdint>

#include "audio_chip.h"
#include "scheduler.h"

namespace griffin::audio
{

// A run is at most this many words, so a ring of length + this words never
// makes a run wrap.  It is the FIFO's depth because `need` is capped by the
// FIFO's room, which is the only bound a run has.
inline constexpr unsigned FRAME_MAX_RUN_WORDS = FIFO_DEPTH;

// Runs one channel can be given in one region: MAX_RUN_WORDS split at a
// descriptor's 32, plus one for a ring wrap.
inline constexpr int FRAME_MAX_RUNS_PER_CHANNEL =
    static_cast<int>((FRAME_MAX_RUN_WORDS + MAX_DESCRIPTOR_WORDS - 1) / MAX_DESCRIPTOR_WORDS) + 1;

// Control bytes one region can carry: a PERIOD and a VOLUME record per channel.
inline constexpr int FRAME_MAX_CONTROL_BYTES = 4 * CHANNELS;

// A table this long or shorter is duplicated so runs never wrap; a longer one
// is stored once and a run that crosses the end is split.
inline constexpr uint32_t FRAME_RING_PATTERN_MAX = 512;

// The ring pool.  Sized for what psg_tables.h can ask for: the 130 distinct
// square lengths duplicated past FRAME_MAX_RUN_WORDS (48,038 words), the
// 32,767-word white-noise sequence, the periodic sequence duplicated (270),
// and room over it.  Bytes are reported by ring_pool_used().
inline constexpr unsigned FRAME_RING_POOL_WORDS = 86016;
inline constexpr unsigned FRAME_RING_MAX_RINGS  = 192;

// One run of samples: `count` words at `source`, into sample FIFO `channel`.
// `staged` marks a run the scheduler wrote into the caller's window rather
// than one that points into a ring.
struct SampleRun
{
    const uint16_t *source = nullptr;
    uint16_t        count = 0;
    uint8_t         channel = 0;
    uint8_t         staged = 0;
};

// What one region deposits.  Control bytes are values, so the caller copies
// those few; sample runs are pointers, so the caller copies nothing.
struct RegionDeposits
{
    uint8_t   control[FRAME_MAX_CONTROL_BYTES];
    int       control_bytes = 0;
    SampleRun runs[CHANNELS][FRAME_MAX_RUNS_PER_CHANNEL];
    uint8_t   runs_of[CHANNELS] = {};
    int       run_count = 0;        // runs over all channels
    int       words = 0;            // sample words over all runs
    int       staged_words = 0;     // of `words`, the ones written to the window

    void clear()
    {
        control_bytes = 0;
        run_count = 0;
        words = 0;
        staged_words = 0;
        for (int c = 0; c < CHANNELS; ++c)
        {
            runs_of[c] = 0;
        }
    }
};

struct FrameSchedulerConfig
{
    int region_lines = 8;           // R
    int field_lines = 525;          // V_TOTAL
    int margin = 2;                 // scheduler.h's MARGIN
};

struct FrameChannelState
{
    // The stream as the inbox asked for it.
    const uint8_t *table = nullptr;
    uint32_t length = 0;
    uint32_t position = 0;
    uint32_t repeat = 1;
    uint32_t repeat_count = 0;
    bool     active = false;

    // Where its words are.  `ring` null means the stream is staged into the
    // caller's window each region (repeat > 1, or no room left in the pool).
    // `ring_pos` is the read index, congruent to `position` modulo `length`
    // because a ring is a whole number of copies of the table; it is what the
    // hot path advances, and `position` is refreshed from it (one 16-bit
    // remainder per active channel) only when the phase is published to the
    // inbox for PsgChannels' carry_phase.
    const uint16_t *ring = nullptr;
    uint32_t        ring_words = 0;
    uint32_t        ring_pos = 0;
    bool            ring_nowrap = false;

    // The chip mirror.  `acc` is PERIOD - COUNT (see the header).
    uint8_t period = 0;
    int     acc = 0;
    int     fill = 0;

    bool    period_pending = false;
    uint8_t pending_period = 0;
    int     pending_pops_left = 0;

    bool    volume_dirty = false;
    uint8_t volume = 0;

    // Reports.
    uint32_t deposited = 0;
    uint32_t predicted_underruns = 0;
    int      max_fill = 0;
};

class FrameScheduler
{
public:
    explicit FrameScheduler(FrameSchedulerConfig config = FrameSchedulerConfig{})
        : config_(config)
    {
        reset();
    }

    void reset();

    // The Scheduler PsgChannels::emit() writes its requests into.  Its line()
    // is never called; adopt_requests() reads it and publishes the phase back.
    Scheduler& inbox() { return inbox_; }
    const Scheduler& inbox() const { return inbox_; }

    // Take everything emit() asked for and give the inbox this scheduler's
    // phase.  Call once per game tick, right after PsgChannels::emit().
    void adopt_requests();

    // The same requests without an inbox, for callers that drive this class
    // directly (the tests do).  set_stream()'s arguments are scheduler.h's.
    void set_stream(int channel, const uint8_t *table, uint32_t length, uint32_t position,
                    uint32_t repeat = 1, uint32_t repeat_count = 0);
    void set_period(int channel, uint8_t period);
    void set_volume(int channel, uint8_t volume);
    void stop(int channel);

    // ONE REGION.  `window`/`window_words` is caller storage the scheduler may
    // stage a repeat > 1 stream's words into; pass nullptr/0 if no stream
    // needs it (the run count then falls short and staged_short() counts it).
    void region(RegionDeposits& out, uint16_t *window = nullptr, unsigned window_words = 0);

    // Regions that passed with none of this scheduler's deposits: the mirror
    // pops, nothing is queued, the streams do not advance.  Scheduler's
    // idle_line() over the same lines (T7's semantics).
    void idle_regions(int regions);

    // Samples the CHIP was given that this scheduler did not ask for.  A
    // display list is the raster's pacer and it is re-walked every field, so a
    // field whose pass never ran REPLAYS the runs the last one left in the
    // slots; the chip takes them again and the mirror has to be told, or the
    // next top-up is computed against a fill that is too low and the FIFO
    // overflows (measured: griffin-ports/smurf/app, "Tier 1 audio result").
    // The FIFO is 256 deep and a write to a full one is dropped, so the mirror
    // saturates exactly as the part does.
    void note_replay(int channel, int words);

    int regions_per_field() const { return regions_; }
    int region_index() const { return region_; }
    int lines_of_region(int region) const
    {
        return (region + 1 == regions_) ? last_region_lines_ : config_.region_lines;
    }
    const FrameChannelState& channel(int channel) const { return channels_[channel]; }
    const FrameSchedulerConfig& config() const { return config_; }
    uint32_t control_records() const { return control_records_; }
    uint32_t staged_short() const { return staged_short_; }
    uint32_t runs_dropped() const { return runs_dropped_; }
    uint32_t rings_built() const { return rings_built_; }
    uint32_t ring_pool_used() const { return ring_pool_used_; }
    uint32_t ring_pool_failures() const { return ring_pool_failures_; }

    // The smallest PERIOD a region of this many lines can keep fed out of a
    // FIFO_DEPTH FIFO: 50*R/(PERIOD+1) + margin must fit the FIFO.  A faster
    // channel has its run capped and shows up in predicted_underruns.
    int min_safe_period() const { return min_safe_period_; }

    // The closed form itself, so a test can hold it against line_pops()
    // summed over the region (T9) and a report can quote it.  `region` picks
    // the window: any index but the last region's is a full R lines.
    struct RegionPops
    {
        int pops;
        int acc;
    };

    RegionPops region_pops(int region, int period, int acc) const
    {
        const Pops p = window_pops_of(window_[(region + 1 == regions_) ? 1 : 0], period, acc);
        return RegionPops{ p.pops, p.acc };
    }

private:
    // One entry per PERIOD, PACKED: the quotient in the high 24 bits and the
    // remainder in the low 8.  One indexed long load per window instead of two,
    // which on a 68000 is the difference between 18 and 36 cycles.
    struct Window
    {
        uint32_t qr[256];
        int      steps = 0;
    };

    struct Pops
    {
        int pops;
        int acc;
    };

    // The law of frame_scheduler.h's header, inline because it is the hot path:
    // one load, one add, one compare.
    static Pops window_pops_of(const Window& w, int period, int acc)
    {
        const int d = period + 1;
        int owed = 0;
        while (acc < 0)
        {
            acc += d;
            ++owed;
        }
        const uint32_t qr = w.qr[period];
        int t = acc + static_cast<int>(qr & 0xFF);
        int pops = static_cast<int>(qr >> 8);
        if (t >= d)
        {
            t -= d;
            ++pops;
        }
        pops -= owed;
        if (pops < 0)
        {
            pops = 0;
        }
        return Pops{ pops, t };
    }

    void build_windows();
    void bind_ring(FrameChannelState& s);
    void emit_run(RegionDeposits& out, int channel, FrameChannelState& s, int need,
                  uint16_t *window, unsigned window_words);
    void advance_region(int lines_index);

    FrameSchedulerConfig config_;
    FrameChannelState channels_[CHANNELS];
    Window window_[2];              // [0] a full region, [1] the last one
    int  regions_ = 1;
    int  last_region_lines_ = 0;
    int  region_ = 0;
    int  min_safe_period_ = 0;
    uint32_t control_records_ = 0;
    uint32_t staged_short_ = 0;
    uint32_t runs_dropped_ = 0;
    uint32_t rings_built_ = 0;
    uint32_t ring_pool_used_ = 0;
    uint32_t ring_pool_failures_ = 0;

    // What was last written into the inbox, so a request is an exact edge.
    struct InboxShadow
    {
        const uint8_t *table = nullptr;
        uint32_t length = 0;
        uint32_t position = 0;
        uint32_t repeat = 1;
        uint32_t repeat_count = 0;
        bool     active = false;
        uint8_t  period = 0;
        bool     period_pending = false;
        uint8_t  pending_period = 0;
        uint8_t  volume = 0;
        bool     volume_seen = false;
    };
    Scheduler   inbox_;
    InboxShadow shadow_[CHANNELS];
};

// The static ring pool: its base, so a caller that has to turn a run's source
// POINTER into a machine address can do it with one subtraction (on the 68000
// the two are the same number; on the host the pool has to be mirrored into the
// simulated RAM), and its numbers for reports.
const uint16_t *frame_ring_pool_base();
uint32_t frame_ring_pool_words();
uint32_t frame_ring_pool_capacity();

}   // namespace griffin::audio
