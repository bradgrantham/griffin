// mixer_ab — the recorded Smurf PSG stream through the software SN76489 and
// through psg_driver -> MIXER, side by side (wave-table-design.md §3 step 1).
//
// The trace is read by the reference's own tests/trace_io.h and replayed as
// frame_audio.cpp replays it: per .io frame, one field per PLAY_SONGS group
// (frame_fields(ticks, 1)), group i's bytes applied before field i, fields
// alternating 263/262 from a FieldCounter.  The reference renders through
// Psg::render_per_sample (the per-sample path), one sample at a time so its
// state can be read on every tick; the MIXER side gets each group's SETs plus
// a WAIT of the field's length through its AUDCMD queue, then ticks.
//
// The oracle is README.md's expectation table (from §2), not the reference:
// the invariants below say whether every difference is the rounding §2
// predicts, and the statistics say how big it is.

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "frame_audio.h"
#include "sn76489.h"
#include "tests/trace_io.h"

#include "mixer.h"
#include "psg_driver.h"

#ifndef PSG_STATS
#error "mixer_ab reads Psg::stat_shifts: build it and sn76489.cpp with -DPSG_STATS"
#endif

namespace
{

namespace mx = griffin::mixer;
namespace mp = griffin::mixer::psg;
using griffin::psg::Config;
using griffin::psg::FieldCounter;
using griffin::psg::Psg;
using griffin::psg::Renderer;

constexpr int WAV_RATE = 15734;                         // as the reference's wav_test
constexpr double CLOCK_HZ = 3579545.0;
constexpr double SAMPLE_HZ = 25175000.0 / 1600.0;       // 15734.375 exactly

// Four PSG voices at the peak fit the 8-bit accumulator only at peak <= 31.
// Above it (make test-peak60) the MIXER saturates per add and the
// reference clamps once at the end, so the checks that assume no clipping
// (E10, E16) are reported, not asserted.
constexpr bool BUDGETED = 4 * mp::PEAK_PER_CHANNEL <= 127;

// ---------------------------------------------------------------------------
// WAV: the reference's format exactly (tests/wav_test.cpp): 16-bit mono PCM
// container, each 8-bit two's complement sample scaled by 256.
// ---------------------------------------------------------------------------

void put32(FILE *fp, uint32_t v)
{
    const uint8_t b[4] = { static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8),
                           static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24) };
    fwrite(b, 1, 4, fp);
}

void put16(FILE *fp, uint16_t v)
{
    const uint8_t b[2] = { static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8) };
    fwrite(b, 1, 2, fp);
}

bool write_wav(const std::string& path, const std::vector<int8_t>& samples)
{
    FILE *fp = fopen(path.c_str(), "wb");
    if (!fp)
    {
        fprintf(stderr, "mixer_ab: cannot write %s: %s\n", path.c_str(), strerror(errno));
        return false;
    }
    const uint32_t data_bytes = static_cast<uint32_t>(samples.size() * 2);
    fwrite("RIFF", 1, 4, fp);
    put32(fp, 36 + data_bytes);
    fwrite("WAVEfmt ", 1, 8, fp);
    put32(fp, 16);
    put16(fp, 1);
    put16(fp, 1);
    put32(fp, WAV_RATE);
    put32(fp, WAV_RATE * 2);
    put16(fp, 2);
    put16(fp, 16);
    fwrite("data", 1, 4, fp);
    put32(fp, data_bytes);
    for (int8_t s : samples)
    {
        put16(fp, static_cast<uint16_t>(static_cast<int16_t>(s * 256)));
    }
    fclose(fp);
    return true;
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

struct DiffStats
{
    uint64_t samples = 0;
    uint64_t differing = 0;
    int      max_abs = 0;
    double   sum_sq = 0.0;
    double   ref_sum_sq = 0.0;
    double   mix_sum_sq = 0.0;

    void add(int ref, int mix)
    {
        const int d = mix - ref;
        ++samples;
        if (d != 0)
        {
            ++differing;
        }
        max_abs = std::max(max_abs, std::abs(d));
        sum_sq += static_cast<double>(d) * d;
        ref_sum_sq += static_cast<double>(ref) * ref;
        mix_sum_sq += static_cast<double>(mix) * mix;
    }

    double rms() const { return samples ? std::sqrt(sum_sq / static_cast<double>(samples)) : 0.0; }
    double ref_rms() const { return samples ? std::sqrt(ref_sum_sq / static_cast<double>(samples)) : 0.0; }
    double mix_rms() const { return samples ? std::sqrt(mix_sum_sq / static_cast<double>(samples)) : 0.0; }
    double fraction() const { return samples ? static_cast<double>(differing) / static_cast<double>(samples) : 0.0; }

    void print(const char *label) const
    {
        printf("  %-34s %9" PRIu64 " samples  max|d| %3d  RMS d %6.2f  differing %9" PRIu64 " (%5.1f%%)"
               "  level ref %5.2f mix %5.2f\n",
               label, samples, max_abs, rms(), differing, 100.0 * fraction(), ref_rms(), mix_rms());
    }
};

// The sequence §2 defines, generated independently of both models: state
// after k shifts from LFSR_SEED, white or periodic.
struct LfsrReplica
{
    std::vector<uint16_t> states;
    bool white = false;

    void start(bool is_white)
    {
        white = is_white;
        states.assign(1, mx::LFSR_SEED);
    }

    uint16_t at(uint64_t k)
    {
        while (states.size() <= k)
        {
            const uint16_t s = states.back();
            const uint16_t fb = white ? static_cast<uint16_t>((s ^ (s >> 1)) & 1u)
                                      : static_cast<uint16_t>(s & 1u);
            states.push_back(static_cast<uint16_t>((s >> 1) | (fb << 14)));
        }
        return states[k];
    }
};

// The PSG registers in force for one field, for locating what was playing.
struct FieldState
{
    size_t   first_sample;
    size_t   frame;
    uint16_t period[3];
    uint8_t  atten[4];
    uint8_t  noise_control;
};

struct PitchRow
{
    int    period;
    double f_exact;
    uint32_t inc_true;
    uint16_t inc_stored;
    double f_mix;
    double cents_mix;
    uint32_t step_ref;
    double f_ref;
    double cents_ref;
    int    step_minus_2inc;
};

uint32_t reference_step(uint32_t period)
{
    // sn76489.cpp make_step_table(), recomputed here because the table is in
    // that file's anonymous namespace.
    constexpr uint64_t numerator = 65536ull * griffin::psg::CLOCK_HZ * griffin::psg::SAMPLE_RATE_DENOMINATOR;
    const uint64_t denominator = 16ull * griffin::psg::SAMPLE_RATE_NUMERATOR * period;
    return static_cast<uint32_t>((numerator + denominator / 2) / denominator);
}

// One replay of a byte stream through both models, with every counter the
// report reads.  Run on the recording, and on a synthetic stream that covers
// what the recording does not (white noise).
Config per_sample_config()
{
    Config c;
    c.renderer = Renderer::PerSample;
    c.peak_per_channel = mp::PEAK_PER_CHANNEL;     // equal loudness at any build's peak
    return c;
}

struct Replay
{
    Psg ref{ per_sample_config() };

    mx::Mixer mixer;                // 8 voices, SRAM garbage (LFSR select too, until the driver's first emit)
    mp::PsgDriver driver;
    FieldCounter counter;

    std::vector<int8_t> ref_out;
    std::vector<int8_t> mix_out;
    std::vector<uint8_t> noise_field;   // per sample: noise channel audible in this field
    std::vector<FieldState> field_states;

    DiffStats all;
    DiffStats agree;                    // every audible source's sign agrees
    DiffStats disagree;
    DiffStats noise_all;
    DiffStats noise_agree;
    DiffStats noise_disagree;
    DiffStats noise_clamped_stats;      // fields where rate 3 was clamped (E9)
    uint64_t voice_disagreements[4] = {};

    uint64_t tone_violations = 0;
    uint64_t noise_violations = 0;
    uint64_t lfsr_mismatches = 0;
    uint64_t output_mismatches = 0;     // E7h: output != the §2 sum with the start-of-tick LFSR
    uint64_t heard_checked = 0;         // E8h: ticks in epochs with INC == noise_step throughout
    uint64_t heard_mismatches = 0;
    bool     epoch_exact = true;
    uint64_t lr_mismatches = 0;
    uint64_t mix_saturated_ticks = 0;   // ticks in which the MIXER clipped an add
    uint64_t ref_clipped_ticks = 0;     // reference output at -128 or +127
    uint32_t tone_drift[3] = {};        // Σ(STEP - 2 INC) mod 2^17
    int64_t  noise_drift = 0;           // Σ(noise_step - INC) since the epoch's reseed
    uint64_t ref_epoch_base = 0;        // Psg::stat_shifts at the epoch's start
    uint64_t mix_epoch_base = 0;
    uint64_t epochs = 0;
    int64_t  max_shift_gap = 0;
    int64_t  rounding_drift = 0;        // the part of noise_drift accrued while not clamped
    int64_t  max_rounding_drift = 0;
    uint64_t first_sign_disagreement = UINT64_MAX;
    size_t   first_disagreement_frame = 0;
    LfsrReplica replica;

    size_t max_sets = 0;                // after the first emit
    size_t first_sets = 0;
    uint64_t total_sets = 0;
    uint64_t groups_total = 0;
    size_t max_frame_words = 0;
    size_t max_frame_index = 0;
    uint64_t frames_over_fifo = 0;
    uint64_t clamped_fields = 0;


    void run(const Trace& trace);
};

void Replay::run(const Trace& trace)
{
// SRAM garbage in voices 4..7 is silenced in the first boundary.
{
    uint16_t words[2 * mx::MAX_VOICES];
    const size_t n = mp::silence_voices(mp::PSG_VOICES, mixer.voices(), words);
    for (size_t i = 0; i < n; ++i)
    {
        mixer.push_audcmd(words[i]);
    }
}

replica.start(false);           // power-on noise control 0: periodic
for (size_t f = 0; f < trace.frame_bytes.size(); ++f)
{
    const std::vector<uint8_t>& bytes = trace.frame_bytes[f];
    const std::vector<uint16_t>& ticks = trace.frame_ticks[f];
    const int fields = griffin::psg::frame_fields(ticks.size(), 1);
    size_t frame_words = 0;
    for (int group = 0; group < fields; ++group)
    {
        bool reseed = false;
        const size_t index = static_cast<size_t>(group);
        size_t first = 0;
        size_t last = 0;
        if (index < ticks.size())
        {
            first = (index == 0) ? size_t{ 0 } : ticks[index];
            last = (index + 1 < ticks.size()) ? ticks[index + 1] : bytes.size();
        }
        else if (index == 0)
        {
            last = bytes.size();
        }
        for (size_t i = first; i < last && i < bytes.size(); ++i)
        {
            ref.write(bytes[i]);
            driver.write(bytes[i]);
            if ((bytes[i] & 0xF0) == 0xE0)
            {
                reseed = true;
            }
        }

        uint16_t sets[mp::MAX_SETS_PER_EMIT];
        const size_t n_sets = driver.emit(sets);
        for (size_t i = 0; i < n_sets; ++i)
        {
            mixer.push_audcmd(sets[i]);
        }
        const int n = counter.take(1);
        mixer.push_audcmd(mx::wait_word(static_cast<uint16_t>(n)));
        if (groups_total == 0)
        {
            first_sets = n_sets;
        }
        else
        {
            max_sets = std::max(max_sets, n_sets);
        }
        total_sets += n_sets;
        ++groups_total;
        frame_words += n_sets + 1;

        const bool noise_audible = ref.atten[3] != 15;
        field_states.push_back(FieldState{ ref_out.size(), f,
                                           { ref.period[0], ref.period[1], ref.period[2] },
                                           { ref.atten[0], ref.atten[1], ref.atten[2], ref.atten[3] },
                                           ref.noise_control });
        const bool clamped = driver.noise_clamped();
        if (clamped)
        {
            ++clamped_fields;
        }
        if (reseed)
        {
            noise_drift = 0;
            rounding_drift = 0;
            ref_epoch_base = ref.stat_shifts;
            mix_epoch_base = mixer.stats.lfsr_shifts;
            replica.start((ref.noise_control & 4) != 0);
            epoch_exact = true;
            ++epochs;
        }

        for (int s = 0; s < n; ++s)
        {
            // The LFSR every MIXER voice reads this tick is the one standing
            // before the tick (shift at the END of the tick, amended §2), taken
            // from the independent replica at the shift count reached so far.
            // The reference's register before its sample is the one it held
            // after the previous sample.
            const uint64_t mix_heard_shifts = mixer.stats.lfsr_shifts - mix_epoch_base;
            const uint16_t ref_prev_register = ref.shift_register;
            int8_t r = 0;
            ref.render_per_sample(&r, 1);
            const uint32_t saturations_before = mixer.stats.saturations;
            const mx::StereoSample m = mixer.tick();
            if (mixer.stats.saturations != saturations_before)
            {
                ++mix_saturated_ticks;
            }
            if (r == 127 || r == -128)
            {
                ++ref_clipped_ticks;
            }
            const uint16_t mix_heard = replica.at(mix_heard_shifts);
            if (m.left != m.right)
            {
                ++lr_mismatches;
            }
            ref_out.push_back(r);
            mix_out.push_back(m.left);
            noise_field.push_back(noise_audible ? 1 : 0);

            // E5: tone phase identity.
            for (int c = 0; c < 3; ++c)
            {
                tone_drift[c] = (tone_drift[c] + ref.step[c] - 2u * mixer.inc(c)) & 0x1FFFFu;
                const uint32_t lhs = (ref.phase[c] - 2u * mixer.phase(c) - tone_drift[c]) & 0x1FFFFu;
                if (lhs != 0)
                {
                    ++tone_violations;
                }
            }
            // E6: noise shift accounting.  E7: both registers on §2's sequence.
            const int64_t noise_d = static_cast<int64_t>(ref.noise_step) - static_cast<int64_t>(mixer.inc(3));
            noise_drift += noise_d;
            if (noise_d != 0)
            {
                epoch_exact = false;
            }
            // E8h: equal increments since the reseed means equal shift
            // instants, so the MIXER hears at tick t what the reference held
            // after tick t-1.
            if (epoch_exact)
            {
                ++heard_checked;
                if (mix_heard != ref_prev_register)
                {
                    ++heard_mismatches;
                }
            }
            if (!clamped)
            {
                rounding_drift += noise_d;
                max_rounding_drift = std::max(max_rounding_drift, std::llabs(rounding_drift));
            }
            const int64_t ref_shifts = static_cast<int64_t>(ref.stat_shifts - ref_epoch_base);
            const int64_t mix_shifts = static_cast<int64_t>(mixer.stats.lfsr_shifts - mix_epoch_base);
            const int64_t account = (ref_shifts - mix_shifts) * 65536
                                    + static_cast<int64_t>(ref.noise_phase)
                                    - static_cast<int64_t>(mixer.phase(3));
            if (account != noise_drift)
            {
                ++noise_violations;
            }
            const int64_t gap = std::llabs(ref_shifts - mix_shifts);
            max_shift_gap = std::max(max_shift_gap, gap);
            if (ref.shift_register != replica.at(static_cast<uint64_t>(ref_shifts))
                || mixer.lfsr() != replica.at(static_cast<uint64_t>(mix_shifts)))
            {
                ++lfsr_mismatches;
            }

            // E10/E11: classify by the audible sources' signs.
            bool signs_agree = true;
            for (int c = 0; c < 4; ++c)
            {
                if (ref.amp[c] == 0)
                {
                    continue;
                }
                const bool ref_pos = (c < 3) ? ((ref.phase[c] >> 16) & 1u) != 0
                                             : (ref.shift_register & 1u) != 0;
                const bool mix_pos = (c < 3) ? (mixer.phase(c) & 0x8000u) != 0
                                             : (mix_heard & 1u) != 0;
                if (ref_pos != mix_pos)
                {
                    signs_agree = false;
                    ++voice_disagreements[c];
                }
            }
            // E7h: the MIXER's output is §2's saturating sum of the four PSG
            // voices (4..7 are silenced) with the noise sign from mix_heard.
            {
                int expect = 0;
                for (int c = 0; c < 4; ++c)
                {
                    const int amp = mp::amp_for_attenuation(driver.atten[c]);
                    const bool pos = (c < 3) ? (mixer.phase(c) & 0x8000u) != 0 : (mix_heard & 1u) != 0;
                    expect = std::clamp(expect + (pos ? amp : -amp), -128, 127);
                }
                if (expect != m.left)
                {
                    ++output_mismatches;
                }
            }
            if (!signs_agree && first_sign_disagreement == UINT64_MAX)
            {
                first_sign_disagreement = ref_out.size() - 1;
                first_disagreement_frame = f;
            }
            all.add(r, m.left);
            (signs_agree ? agree : disagree).add(r, m.left);
            if (noise_audible)
            {
                noise_all.add(r, m.left);
                (signs_agree ? noise_agree : noise_disagree).add(r, m.left);
                if (clamped)
                {
                    noise_clamped_stats.add(r, m.left);
                }
            }
        }
    }
    if (frame_words > max_frame_words)
    {
        max_frame_words = frame_words;
        max_frame_index = f;
    }
    if (frame_words > mx::AUDCMD_FIFO_WORDS)
    {
        ++frames_over_fifo;
    }
}
}

int failures = 0;

void check(bool ok, const char *what)
{
    printf("  %-72s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
    {
        ++failures;
    }
}

}   // namespace

int main(int argc, char **argv)
{
    std::string trace_path = "../../griffin-ports/smurf/work/smurf-trace.io";
    std::string out_dir = "build";
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--out") == 0 && i + 1 < argc)
        {
            out_dir = argv[++i];
        }
        else if (strcmp(argv[i], "--trace") == 0 && i + 1 < argc)
        {
            trace_path = argv[++i];
        }
        else
        {
            fprintf(stderr, "usage: mixer_ab [--trace smurf-trace.io] [--out builddir]\n");
            return 2;
        }
    }

    // --- pitch, from the tables alone ---------------------------------------
    printf("PITCH (every period 1..1024; E1-E3)\n");
    std::vector<PitchRow> rows;
    bool table_matches = true;
    for (int n = 1; n <= mp::PERIODS; ++n)
    {
        PitchRow r{};
        r.period = n;
        r.f_exact = CLOCK_HZ / (32.0 * n);
        r.inc_true = mp::true_inc(static_cast<uint32_t>(n));
        r.inc_stored = mp::inc_for_register(static_cast<uint16_t>(n & 0x3FF));
        if (r.inc_stored != (r.inc_true & 0xFFFFu))
        {
            table_matches = false;
        }
        r.f_mix = r.inc_true * SAMPLE_HZ / 65536.0;
        r.cents_mix = 1200.0 * std::log2(r.f_mix / r.f_exact);
        r.step_ref = reference_step(static_cast<uint32_t>(n));
        r.f_ref = r.step_ref * SAMPLE_HZ / 131072.0;
        r.cents_ref = 1200.0 * std::log2(r.f_ref / r.f_exact);
        r.step_minus_2inc = static_cast<int>(r.step_ref) - 2 * static_cast<int>(r.inc_true);
        rows.push_back(r);
    }
    check(table_matches, "stored INC == true INC mod 65536 for every period");
    {
        const std::string path = out_dir + "/pitch_table.txt";
        FILE *fp = fopen(path.c_str(), "w");
        if (!fp)
        {
            fprintf(stderr, "mixer_ab: cannot write %s: %s\n", path.c_str(), strerror(errno));
            return 1;
        }
        fprintf(fp, "# period f_exact_Hz INC_true INC_stored f_mixer_Hz cents_mixer STEP_ref f_ref_Hz cents_ref STEP-2INC\n");
        fprintf(fp, "# periods 1..7: INC_true > 65535, stored mod 65536; the square is the same alias (psg_driver.h)\n");
        for (const PitchRow& r : rows)
        {
            fprintf(fp, "%4d %11.4f %6u %5u %11.4f %+8.4f %6u %11.4f %+8.4f %+d\n",
                    r.period, r.f_exact, r.inc_true, r.inc_stored, r.f_mix, r.cents_mix,
                    r.step_ref, r.f_ref, r.cents_ref, r.step_minus_2inc);
        }
        fclose(fp);
    }
    double max_c = 0.0;
    double sum_c = 0.0;
    double max_c_ref = 0.0;
    double sum_c_ref = 0.0;
    int nonzero_d = 0;
    for (const PitchRow& r : rows)
    {
        max_c = std::max(max_c, std::fabs(r.cents_mix));
        sum_c += std::fabs(r.cents_mix);
        max_c_ref = std::max(max_c_ref, std::fabs(r.cents_ref));
        sum_c_ref += std::fabs(r.cents_ref);
        if (r.step_minus_2inc != 0)
        {
            ++nonzero_d;
        }
    }
    printf("  MIXER vs exact SN76489:     max %.3f cents, mean %.3f cents\n", max_c, sum_c / rows.size());
    printf("  reference vs exact SN76489: max %.3f cents, mean %.3f cents (its 16.16 half-period step)\n",
           max_c_ref, sum_c_ref / rows.size());
    printf("  periods with STEP != 2*INC (tone phases drift apart): %d of %zu\n", nonzero_d, rows.size());
    {
        std::vector<PitchRow> worst = rows;
        std::sort(worst.begin(), worst.end(), [](const PitchRow& a, const PitchRow& b)
                  {
                      return std::fabs(a.cents_mix) > std::fabs(b.cents_mix);
                  });
        printf("  ten worst: period  f_exact   INC    cents\n");
        for (int i = 0; i < 10; ++i)
        {
            printf("             %6d %9.3f %6u %+7.3f\n", worst[i].period, worst[i].f_exact,
                   worst[i].inc_true, worst[i].cents_mix);
        }
    }
    check(max_c <= 1.902, "E1 max pitch error <= 1.90 cents");
    printf("  full table: %s/pitch_table.txt\n\n", out_dir.c_str());

    // --- amplitude ladder -------------------------------------------------------
    printf("AMPLITUDE LADDER at peak %d (E4)\n", mp::PEAK_PER_CHANNEL);
    printf("  atten   exact   AMP   err LSB   err dB\n");
    double worst_lsb = 0.0;
    for (int a = 0; a < 16; ++a)
    {
        const double exact = (a == 15) ? 0.0 : mp::PEAK_PER_CHANNEL * std::pow(10.0, -a / 10.0);
        const int amp = mp::amp_for_attenuation(static_cast<uint8_t>(a));
        const double err = amp - exact;
        const double db = (a == 15 || amp == 0) ? 0.0 : 20.0 * std::log10(amp / exact);
        worst_lsb = std::max(worst_lsb, std::fabs(err));
        printf("  %5d %7.3f %5d %+9.3f %+8.2f\n", a, exact, amp, err, db);
    }
    {
        int collapsed = 0;
        for (int a = 11; a < 15; ++a)
        {
            if (mp::amp_for_attenuation(static_cast<uint8_t>(a)) == mp::amp_for_attenuation(static_cast<uint8_t>(a - 1)))
            {
                ++collapsed;
            }
        }
        printf("  attenuations 10..14 -> AMP %d %d %d %d %d; steps lost in 11..14: %d\n",
               mp::amp_for_attenuation(10), mp::amp_for_attenuation(11), mp::amp_for_attenuation(12),
               mp::amp_for_attenuation(13), mp::amp_for_attenuation(14), collapsed);
    }
    check(worst_lsb <= 0.5, "E4 every AMP within 0.5 LSB of the exact ladder");
    printf("\n");

    // --- the trace ----------------------------------------------------------
    Trace trace;
    if (!read_trace(trace_path.c_str(), trace))
    {
        return 1;
    }
    printf("TRACE %s: %zu frames, %zu PSG bytes\n\n", trace_path.c_str(), trace.frame_bytes.size(),
           trace.total_bytes);

    Replay rp;
    rp.run(trace);

    // --- report ---------------------------------------------------------------
    printf("INVARIANTS (the differences are §2's rounding and nothing else)\n");
    printf("  tone phase identity violations      %" PRIu64 "\n", rp.tone_violations);
    printf("  noise shift accounting violations   %" PRIu64 "\n", rp.noise_violations);
    printf("  LFSR off §2's sequence              %" PRIu64 "  (%" PRIu64 " reseed epochs)\n",
           rp.lfsr_mismatches, rp.epochs);
    printf("  output != sum with start-of-tick LFSR %" PRIu64 "\n", rp.output_mismatches);
    printf("  heard != reference one tick earlier %" PRIu64 " of %" PRIu64 " ticks in exact epochs\n",
           rp.heard_mismatches, rp.heard_checked);
    printf("  L != R                              %" PRIu64 "\n", rp.lr_mismatches);
    printf("  largest shift-count gap in an epoch %" PRId64 "; from rounding alone (unclamped ticks) %.3f shifts\n",
           rp.max_shift_gap, static_cast<double>(rp.max_rounding_drift) / 65536.0);
    printf("  first sign disagreement: sample %" PRIu64 " (%.2f s, .io frame %zu)\n",
           rp.first_sign_disagreement, static_cast<double>(rp.first_sign_disagreement) / SAMPLE_HZ,
           rp.first_disagreement_frame);
    printf("  MIXER: %u ticks, %u SETs, %u WAITs, %u empty boundaries, %u AUDCMD overflows, "
           "%u saturations, %u wavetable ticks\n",
           rp.mixer.stats.ticks, rp.mixer.stats.sets, rp.mixer.stats.waits, rp.mixer.stats.empty_boundaries,
           rp.mixer.stats.audcmd_overflows, rp.mixer.stats.saturations, rp.mixer.stats.wavetable_ticks);
    check(rp.tone_violations == 0, "E5 ref.phase == 2 mix.phase + sum(STEP - 2 INC) mod 2^17, every tick");
    check(rp.noise_violations == 0, "E6 noise shift accounting identity, every tick");
    check(rp.lfsr_mismatches == 0, "E7 both LFSRs on the same sequence at their shift counts");
    check(rp.output_mismatches == 0, "E7h output == sum with the LFSR as it stood before the tick");
    check(rp.heard_checked > 0 && rp.heard_mismatches == 0,
          "E8h exact-increment epochs: MIXER hears the reference's previous tick");
    printf("  ticks with a MIXER saturating add %" PRIu64 "; reference output at full scale %" PRIu64 "\n",
           rp.mix_saturated_ticks, rp.ref_clipped_ticks);
    if (BUDGETED)
    {
        check(rp.lr_mismatches == 0 && rp.mixer.stats.saturations == 0, "E16 L == R and no saturating add");
    }
    else
    {
        check(rp.lr_mismatches == 0, "E16 L == R (saturation reported only: peak over budget)");
    }
    check(rp.ref_out.size() == 1285463, "E19 1,285,463 samples");
    printf("\n");

    // The recording never selects white noise (Smurf writes only periodic
    // controls), so a synthetic stream covers it: every noise control 4..7,
    // rate 3 at an ordinary and at a clamped tone 2 period, over a tone.
    {
        Trace synth;
        auto hold = [&synth](std::vector<uint8_t> bytes, int frames)
        {
            synth.frame_bytes.push_back(std::move(bytes));
            for (int i = 1; i < frames; ++i)
            {
                synth.frame_bytes.emplace_back();
            }
        };
        hold({ 0x94, 0x8E, 0x0F, 0xBF, 0xDF, 0xF0, 0xE4 }, 120);   // tone0 period 254 a4; white rate 0
        hold({ 0x94, 0xE5 }, 120);                                 // white rate 1
        hold({ 0x94, 0xE6 }, 120);                                 // white rate 2
        hold({ 0x94, 0xC4, 0x06, 0xE7 }, 120);                     // white rate 3, tone2 period 100 (silent)
        hold({ 0x94, 0xC5, 0x00, 0xE7 }, 60);                      // white rate 3, tone2 period 5: clamped
        hold({ 0x94, 0xC4, 0x06, 0xE3 }, 60);                      // periodic rate 3
        split_ticks(synth);
        Replay sp;
        sp.run(synth);
        printf("  synthetic white-noise stream: %zu samples, %" PRIu64 " reseed epochs, %u LFSR shifts\n",
               sp.ref_out.size(), sp.epochs, sp.mixer.stats.lfsr_shifts);
        printf("    violations: tone %" PRIu64 ", noise %" PRIu64 ", LFSR %" PRIu64
               "; rounding-only shift drift %.3f; %" PRIu64 " clamped fields\n",
               sp.tone_violations, sp.noise_violations, sp.lfsr_mismatches,
               static_cast<double>(sp.max_rounding_drift) / 65536.0, sp.clamped_fields);
        check(sp.tone_violations == 0 && sp.noise_violations == 0 && sp.lfsr_mismatches == 0
                  && sp.output_mismatches == 0 && sp.epochs == 6,
              "E5-E7, E7h on the synthetic white-noise stream");
        printf("    heard != reference one tick earlier: %" PRIu64 " of %" PRIu64 " exact-epoch ticks\n",
               sp.heard_mismatches, sp.heard_checked);
        check(sp.heard_checked > 0 && sp.heard_mismatches == 0, "E8h on the synthetic white-noise stream");
    }

    // E20: one LFSR value per tick, heard one tick after the carry.  Voice 1
    // (before the LFSR voice) plays noise into L only, voice 5 (after it) into
    // R only, the LFSR voice itself is silent with INC 0xFFFF from phase 0, so
    // it carries on every tick but the first: the value heard at tick t is the
    // replica at max(t-1, 0) shifts, in both channels.
    {
        mx::Mixer m;
        uint16_t words[2 * mx::MAX_VOICES];
        const size_t n = mp::silence_voices(0, m.voices(), words);
        for (size_t i = 0; i < n; ++i)
        {
            m.push_audcmd(words[i]);
        }
        constexpr int8_t AMP = 10;
        constexpr int lv = 3;
        m.push_audcmd(mx::set_word(mx::LFSR_VOICE_SELECT, lv));
        m.push_audcmd(mx::set_word(mx::voice_addr(lv, mx::V_INC_LO), 0xFF));
        m.push_audcmd(mx::set_word(mx::voice_addr(lv, mx::V_INC_HI), 0xFF));
        m.push_audcmd(mx::set_word(mx::voice_addr(lv, mx::V_CTL),
                                   mx::ctl(mx::MODE_NOISE_WHITE, false, false, true)));
        m.push_audcmd(mx::set_word(mx::voice_addr(1, mx::V_AMP), AMP));
        m.push_audcmd(mx::set_word(mx::voice_addr(1, mx::V_CTL), mx::ctl(mx::MODE_NOISE_WHITE, true, false)));
        m.push_audcmd(mx::set_word(mx::voice_addr(5, mx::V_AMP), AMP));
        m.push_audcmd(mx::set_word(mx::voice_addr(5, mx::V_CTL), mx::ctl(mx::MODE_NOISE_PERIODIC, false, true)));
        LfsrReplica expect;
        expect.start(true);
        constexpr int TICKS = 400;
        int bad = 0;
        for (int t = 0; t < TICKS; ++t)
        {
            const mx::StereoSample out = m.tick();
            const int8_t want = (expect.at(static_cast<uint64_t>(t > 0 ? t - 1 : 0)) & 1u) ? AMP : -AMP;
            if (out.left != want || out.right != want)
            {
                ++bad;
            }
        }
        printf("  E20 synthetic: voices 1 and 5 around LFSR voice %d, %d ticks, %d wrong, %u shifts\n",
               lv, TICKS, bad, m.stats.lfsr_shifts);
        check(bad == 0 && m.stats.lfsr_shifts == TICKS - 1,
              "E20 every voice reads one LFSR value per tick, heard the tick after the carry");
    }
    printf("\n");

    printf("SAMPLES (mixer - reference, LSB of the 8-bit output)\n");
    rp.all.print("whole trace");
    rp.agree.print("  every audible sign agrees (E10)");
    rp.disagree.print("  some audible sign disagrees (E11)");
    printf("  sign disagreements by source: tone0 %" PRIu64 ", tone1 %" PRIu64 ", tone2 %" PRIu64
           ", noise %" PRIu64 "\n",
           rp.voice_disagreements[0], rp.voice_disagreements[1], rp.voice_disagreements[2], rp.voice_disagreements[3]);
    rp.noise_all.print("noise-audible fields");
    rp.noise_agree.print("  every audible sign agrees");
    rp.noise_disagree.print("  some audible sign disagrees");
    rp.noise_clamped_stats.print("  noise rate clamped (E9)");
    printf("  fields with the noise rate clamped: %" PRIu64 "\n", rp.clamped_fields);
    if (BUDGETED)
    {
        check(rp.agree.max_abs <= 2, "E10 sign-agreeing samples differ by at most 2 LSB");
    }
    else
    {
        printf("  E10 reported only at peak %d: sign-agreeing max |d| %d\n", mp::PEAK_PER_CHANNEL,
               rp.agree.max_abs);
    }
    printf("\n");

    // E15: loudness.
    {
        constexpr size_t BLOCK = 128;
        double max_d = 0.0;
        double sum_d = 0.0;
        size_t blocks = 0;
        double noise_max_d = 0.0;
        double noise_sum_d = 0.0;
        size_t noise_blocks = 0;
        size_t worst_block = 0;
        double worst_ref = 0.0;
        double worst_mix = 0.0;
        for (size_t b = 0; b + BLOCK <= rp.ref_out.size(); b += BLOCK)
        {
            double sr = 0.0;
            double sm = 0.0;
            bool noisy = false;
            for (size_t i = b; i < b + BLOCK; ++i)
            {
                sr += static_cast<double>(rp.ref_out[i]) * rp.ref_out[i];
                sm += static_cast<double>(rp.mix_out[i]) * rp.mix_out[i];
                noisy = noisy || rp.noise_field[i];
            }
            const double d = std::fabs(std::sqrt(sr / BLOCK) - std::sqrt(sm / BLOCK));
            if (d > max_d)
            {
                worst_block = b;
                worst_ref = std::sqrt(sr / BLOCK);
                worst_mix = std::sqrt(sm / BLOCK);
            }
            max_d = std::max(max_d, d);
            sum_d += d;
            ++blocks;
            if (noisy)
            {
                noise_max_d = std::max(noise_max_d, d);
                noise_sum_d += d;
                ++noise_blocks;
            }
        }
        printf("LOUDNESS (|difference| of 128-sample RMS envelopes, E15)\n");
        printf("  whole trace:          mean %.2f  max %.2f LSB over %zu blocks\n",
               sum_d / blocks, max_d, blocks);
        printf("  worst block at sample %zu (%.2f s): RMS ref %.2f, mix %.2f\n", worst_block,
               static_cast<double>(worst_block) / SAMPLE_HZ, worst_ref, worst_mix);
        for (size_t i = 0; i < rp.field_states.size(); ++i)
        {
            const FieldState& fs = rp.field_states[i];
            const size_t end = (i + 1 < rp.field_states.size()) ? rp.field_states[i + 1].first_sample : rp.ref_out.size();
            if (end > worst_block && fs.first_sample < worst_block + 128)
            {
                printf("    field at %zu (.io frame %zu): periods %u %u %u, atten %u %u %u noise %u, control %u\n",
                       fs.first_sample, fs.frame, fs.period[0], fs.period[1], fs.period[2],
                       fs.atten[0], fs.atten[1], fs.atten[2], fs.atten[3], fs.noise_control);
            }
        }
        printf("  noise-audible blocks: mean %.2f  max %.2f LSB over %zu blocks\n\n",
               noise_blocks ? noise_sum_d / noise_blocks : 0.0, noise_max_d, noise_blocks);
    }

    printf("AUDCMD (E17, E18)\n");
    printf("  SETs per group: first emit %zu, max after it %zu, mean %.2f over %" PRIu64 " groups\n",
           rp.first_sets, rp.max_sets, static_cast<double>(rp.total_sets) / static_cast<double>(rp.groups_total),
           rp.groups_total);
    check(rp.first_sets == mp::MAX_SETS_PER_EMIT && rp.max_sets <= 13,
          "E17 first emit 17 SETs (select + 16), later ones <= 13");
    check(rp.mixer.lfsr_voice() == mp::NOISE_VOICE, "E22 the driver's first emit made voice 3 the LFSR voice");
    printf("  words if a whole .io frame is queued at once: max %zu (frame %zu); "
           "%" PRIu64 " frames over the %zu-word FIFO\n\n",
           rp.max_frame_words, rp.max_frame_index, rp.frames_over_fifo, mx::AUDCMD_FIFO_WORDS);

    const bool wav_ok = write_wav(out_dir + "/wav/reference.wav", rp.ref_out)
                        && write_wav(out_dir + "/wav/mixer.wav", rp.mix_out);
    printf("BOUNDARY LIMIT (provisional MAX_RECORDS_PER_BOUNDARY %u)\n", mx::MAX_RECORDS_PER_BOUNDARY);
    printf("  most records at one boundary %u (SETs %u); boundaries stopped by the limit %u\n",
           rp.mixer.stats.max_records_per_boundary, rp.mixer.stats.max_sets_per_boundary,
           rp.mixer.stats.limited_boundaries);
    // The slide itself: limit + 6 SETs of AMP to a silent voice, then WAIT 1.
    // Tick 0 takes exactly the limit; tick 1 the remaining 6 and the WAIT.
    {
        mx::Mixer m;
        const uint32_t extra = 6;
        for (uint32_t i = 0; i < mx::MAX_RECORDS_PER_BOUNDARY + extra; ++i)
        {
            m.push_audcmd(mx::set_word(mx::voice_addr(7, mx::V_RESERVED), static_cast<uint8_t>(i)));
        }
        m.push_audcmd(mx::wait_word(1));
        m.tick();
        const bool first = m.stats.sets == mx::MAX_RECORDS_PER_BOUNDARY && m.stats.limited_boundaries == 1;
        m.tick();
        const bool second = m.stats.sets == mx::MAX_RECORDS_PER_BOUNDARY + extra && m.stats.waits == 1
                            && m.audcmd_size() == 0;
        check(first && second, "E21 records past the limit slide to the next boundary");
    }
    check(rp.mixer.stats.max_records_per_boundary <= mx::MAX_RECORDS_PER_BOUNDARY
              && rp.mixer.stats.limited_boundaries == 0,
          "E21 no Smurf boundary reaches the provisional record limit");
    printf("\n");

    check(wav_ok, "WAVs written");
    printf("  %s/wav/reference.wav, %s/wav/mixer.wav: %zu samples (%.1f s)\n",
           out_dir.c_str(), out_dir.c_str(), rp.ref_out.size(), rp.ref_out.size() / SAMPLE_HZ);

    printf("\n%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
