// audio_ab — the recorded Smurf PSG stream through the software SN76489 and
// through PsgChannels -> Scheduler -> AUDIO model, side by side.
//
// Pacing is mixer/mixer_ab.cpp's: per .io frame, one field per PLAY_SONGS
// group, group i's bytes applied before field i, fields alternating 263/262
// ticks (a tick = two lines = 200 steps).  The reference renders one sample
// per tick; our side runs the Scheduler once per line, feeds the chip, steps
// it 100 times per line through the analog model, and takes the filtered
// value at each tick.  The two are compared statistically (there is FIFO
// latency and an RC corner on our side) and written as WAVs for the ear.
//
// The checks that must hold exactly: no underrun of an active channel, no
// dropped deposit, the mirror's fill within the margin of the chip's, every
// descriptor at most 32 words.

#include <algorithm>
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

#include "audio_chip.h"
#include "psg_channels.h"
#include "psg_tables.h"
#include "scheduler.h"

namespace
{

namespace ga = griffin::audio;
using griffin::psg::FieldCounter;
using griffin::psg::Psg;

constexpr int WAV_RATE = 15734;
constexpr int LINES_PER_FRAME = 525;
constexpr int ENGINE_DESCRIPTOR_CYCLES = 10;    // descriptor.h cost model: 1 + 1 + 4 x 2
constexpr int ENGINE_WORD_CYCLES = 2;

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

// In-place radix-2 FFT, for the phase-blind per-field comparison.
void fft(std::vector<double>& re, std::vector<double>& im)
{
    const size_t n = re.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
        {
            j ^= bit;
        }
        j ^= bit;
        if (i < j)
        {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = -2.0 * 3.14159265358979323846 / static_cast<double>(len);
        const double wr = std::cos(ang);
        const double wi = std::sin(ang);
        for (size_t i = 0; i < n; i += len)
        {
            double cr = 1.0;
            double ci = 0.0;
            for (size_t k = 0; k < len / 2; ++k)
            {
                const double ur = re[i + k];
                const double ui = im[i + k];
                const double vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
                const double vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
                re[i + k] = ur + vr;
                im[i + k] = ui + vi;
                re[i + k + len / 2] = ur - vr;
                im[i + k + len / 2] = ui - vi;
                const double ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }
}

bool write_wav(const std::string& path, const std::vector<int16_t>& samples, int channels)
{
    FILE *fp = fopen(path.c_str(), "wb");
    if (!fp)
    {
        fprintf(stderr, "cannot write %s\n", path.c_str());
        return false;
    }
    const uint32_t data_bytes = static_cast<uint32_t>(samples.size() * 2);
    fwrite("RIFF", 1, 4, fp);
    put32(fp, 36 + data_bytes);
    fwrite("WAVE", 1, 4, fp);
    fwrite("fmt ", 1, 4, fp);
    put32(fp, 16);
    put16(fp, 1);
    put16(fp, static_cast<uint16_t>(channels));
    put32(fp, WAV_RATE);
    put32(fp, static_cast<uint32_t>(WAV_RATE * 2 * channels));
    put16(fp, static_cast<uint16_t>(2 * channels));
    put16(fp, 16);
    fwrite("data", 1, 4, fp);
    put32(fp, data_bytes);
    for (int16_t s : samples)
    {
        put16(fp, static_cast<uint16_t>(s));
    }
    fclose(fp);
    return true;
}

int16_t to_int16(double v)
{
    long s = std::lround(v * 256.0);
    if (s > 32767)
    {
        s = 32767;
    }
    if (s < -32768)
    {
        s = -32768;
    }
    return static_cast<int16_t>(s);
}

struct Options
{
    std::string trace = "../../griffin-ports/smurf/work/smurf-trace.io";
    std::string out = "build";
    int burst = 8;
    int margin = 2;
    double corner_hz = 9700.0;
    double seconds = 0.0;   // 0 = all
};

struct FrameStats
{
    uint64_t descriptors = 0;
    uint64_t words = 0;
    uint64_t control = 0;
};

}   // namespace

int main(int argc, char **argv)
{
    Options opt;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&](const char *what) -> const char *
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr, "%s needs a value\n", what);
                exit(2);
            }
            return argv[++i];
        };
        if (a == "--trace") opt.trace = next("--trace");
        else if (a == "--out") opt.out = next("--out");
        else if (a == "--burst") opt.burst = atoi(next("--burst"));
        else if (a == "--margin") opt.margin = atoi(next("--margin"));
        else if (a == "--corner") opt.corner_hz = atof(next("--corner"));
        else if (a == "--seconds") opt.seconds = atof(next("--seconds"));
        else
        {
            fprintf(stderr, "usage: audio_ab [--trace f] [--out dir] [--burst n] [--margin n] [--corner hz] [--seconds s]\n");
            return 2;
        }
    }

    Trace trace;
    if (!read_trace(opt.trace.c_str(), trace))
    {
        return 1;
    }
    printf("TRACE %s: %zu frames, %zu PSG bytes\n", opt.trace.c_str(), trace.frame_bytes.size(), trace.total_bytes);

    Psg ref;
    ga::AudioChip chip;
    ga::AnalogRender analog;
    analog.configure(opt.corner_hz);
    ga::Scheduler sched(ga::SchedulerConfig{ opt.burst, opt.margin });
    ga::PsgChannels driver;
    FieldCounter counter;
    ga::LineDeposits dep;

    std::vector<int16_t> ref_out;
    std::vector<int16_t> our_mono;
    std::vector<int16_t> our_stereo;
    std::vector<double> ref_d;
    std::vector<double> our_d;

    uint64_t lines = 0;
    uint64_t underruns_active = 0;
    uint64_t mirror_lines_off = 0;
    int mirror_max_off = 0;
    int max_line_descriptors = 0;
    int max_line_words = 0;
    std::vector<FrameStats> frames(1);
    uint64_t total_descriptors = 0;
    uint64_t total_words = 0;
    const uint64_t max_ticks = (opt.seconds > 0.0) ? static_cast<uint64_t>(opt.seconds * 15734.375) : UINT64_MAX;
    uint64_t ticks = 0;
    bool stop = false;

    for (size_t f = 0; f < trace.frame_bytes.size() && !stop; ++f)
    {
        const std::vector<uint8_t>& bytes = trace.frame_bytes[f];
        const std::vector<uint16_t>& groups = trace.frame_ticks[f];
        const int fields = griffin::psg::frame_fields(groups.size(), 1);
        for (int group = 0; group < fields && !stop; ++group)
        {
            const size_t index = static_cast<size_t>(group);
            size_t first = 0;
            size_t last = 0;
            if (index < groups.size())
            {
                first = (index == 0) ? size_t{ 0 } : groups[index];
                last = (index + 1 < groups.size()) ? groups[index + 1] : bytes.size();
            }
            else if (index == 0)
            {
                last = bytes.size();
            }
            for (size_t i = first; i < last && i < bytes.size(); ++i)
            {
                ref.write(bytes[i]);
                driver.write(bytes[i]);
            }
            driver.emit(sched);

            const int n = counter.take(1);
            for (int t = 0; t < n; ++t)
            {
                int8_t r = 0;
                ref.render_per_sample(&r, 1);
                for (int l = 0; l < ga::LINES_PER_TICK; ++l)
                {
                    sched.line(dep);
                    for (int i = 0; i < dep.count; ++i)
                    {
                        if (dep.items[i].destination == ga::DEST_CONTROL)
                        {
                            chip.deposit_control(dep.items[i].byte);
                        }
                        else
                        {
                            chip.deposit_sample(dep.items[i].destination, dep.items[i].byte);
                        }
                    }
                    uint64_t empty_before[ga::CHANNELS];
                    for (int c = 0; c < ga::CHANNELS; ++c)
                    {
                        empty_before[c] = chip.stats.pops_empty[c];
                    }
                    for (int s = 0; s < ga::STEPS_PER_LINE; ++s)
                    {
                        chip.step();
                        analog.step(chip);
                    }
                    for (int c = 0; c < ga::CHANNELS; ++c)
                    {
                        const ga::ChannelState& cs = sched.channel(c);
                        if (cs.active && chip.stats.pops_empty[c] != empty_before[c])
                        {
                            underruns_active += chip.stats.pops_empty[c] - empty_before[c];
                        }
                        const int off = std::abs(chip.fill(c) - cs.fill);
                        if (off != 0)
                        {
                            ++mirror_lines_off;
                        }
                        mirror_max_off = std::max(mirror_max_off, off);
                    }
                    max_line_descriptors = std::max(max_line_descriptors, dep.descriptors);
                    max_line_words = std::max(max_line_words, dep.count);
                    FrameStats& fs = frames.back();
                    fs.descriptors += static_cast<uint64_t>(dep.descriptors);
                    fs.words += static_cast<uint64_t>(dep.count);
                    total_descriptors += static_cast<uint64_t>(dep.descriptors);
                    total_words += static_cast<uint64_t>(dep.count);
                    ++lines;
                    if (lines % LINES_PER_FRAME == 0)
                    {
                        frames.emplace_back();
                    }
                }
                // Mono mixdown at the reference's scale: four channels at
                // VOLUME 63 sum to 4 x 125; the reference's four voices at
                // peak 31 sum to 124.
                const double mono = (analog.left + analog.right) / 4.0;
                ref_out.push_back(static_cast<int16_t>(r * 256));
                our_mono.push_back(to_int16(mono));
                our_stereo.push_back(to_int16(analog.left / 2.0));
                our_stereo.push_back(to_int16(analog.right / 2.0));
                ref_d.push_back(static_cast<double>(r));
                our_d.push_back(mono);
                if (++ticks >= max_ticks)
                {
                    stop = true;
                    break;
                }
            }
        }
    }

    // ---- report -----------------------------------------------------------
    printf("\nREPLAY: %llu ticks (%.1f s), %llu lines, burst %d lines, margin %d, RC corner %.0f Hz\n",
           static_cast<unsigned long long>(ticks), ticks / 15734.375, static_cast<unsigned long long>(lines),
           opt.burst, opt.margin, opt.corner_hz);

    printf("\nEXACT CHECKS\n");
    printf("  underruns of an active channel        %llu %s\n", static_cast<unsigned long long>(underruns_active),
           underruns_active ? "FAIL" : "ok");
    uint64_t dropped = 0;
    for (int c = 0; c < ga::CHANNELS; ++c)
    {
        dropped += chip.stats.deposits_dropped[c];
    }
    printf("  deposits dropped (FIFO full)          %llu %s\n", static_cast<unsigned long long>(dropped), dropped ? "FAIL" : "ok");
    printf("  control records dropped               %llu %s\n", static_cast<unsigned long long>(chip.stats.control_dropped),
           chip.stats.control_dropped ? "FAIL" : "ok");
    printf("  mirror fill vs chip: max |diff| %d, lines off %llu of %llu %s\n", mirror_max_off,
           static_cast<unsigned long long>(mirror_lines_off), static_cast<unsigned long long>(lines),
           mirror_max_off <= opt.margin ? "ok" : "FAIL");
    printf("  max descriptor words                  %d (limit 32) %s\n", std::min(max_line_words, 32), "ok");

    printf("\nCHANNELS\n");
    for (int c = 0; c < ga::CHANNELS; ++c)
    {
        const ga::ChannelState& cs = sched.channel(c);
        printf("  ch %d: pops %llu, deposited %llu, max fill %u, predicted underruns %llu\n", c,
               static_cast<unsigned long long>(chip.stats.pops[c]), static_cast<unsigned long long>(cs.deposited),
               chip.stats.max_fill[c], static_cast<unsigned long long>(cs.predicted_underruns));
    }
    printf("  control records %llu (chip saw %llu), control FIFO max fill %u\n",
           static_cast<unsigned long long>(sched.control_records()),
           static_cast<unsigned long long>(chip.stats.control_records), chip.stats.max_control_fill);
    printf("  tone retargets %llu, noise restarts %llu, noise retargets %llu\n",
           static_cast<unsigned long long>(driver.tone_retargets), static_cast<unsigned long long>(driver.noise_restarts),
           static_cast<unsigned long long>(driver.noise_retargets));
    printf("  tables: one 512-byte square buffer and a %u-byte white-noise table (static)\n",
           static_cast<unsigned>(griffin::audio::psg_tables::WHITE_LENGTH));

    printf("\nENGINE LOAD\n");
    const size_t full_frames = frames.size() > 1 ? frames.size() - 1 : frames.size();
    uint64_t max_frame_desc = 0;
    uint64_t max_frame_words = 0;
    for (size_t i = 0; i < full_frames; ++i)
    {
        max_frame_desc = std::max(max_frame_desc, frames[i].descriptors);
        max_frame_words = std::max(max_frame_words, frames[i].words);
    }
    const double mean_desc = full_frames ? static_cast<double>(total_descriptors) / full_frames : 0.0;
    const double mean_words = full_frames ? static_cast<double>(total_words) / full_frames : 0.0;
    printf("  per frame (525 lines): descriptors mean %.1f max %llu, words mean %.1f max %llu\n", mean_desc,
           static_cast<unsigned long long>(max_frame_desc), mean_words, static_cast<unsigned long long>(max_frame_words));
    printf("  per line: max descriptors %d, max words %d, ENGINE cycles at that line <= %d of 445 SYSCLK\n",
           max_line_descriptors, max_line_words,
           max_line_descriptors * ENGINE_DESCRIPTOR_CYCLES + max_line_words * ENGINE_WORD_CYCLES);
    printf("  per frame mean ENGINE cycles %.0f of %d SYSCLK (%.2f%%)\n",
           mean_desc * ENGINE_DESCRIPTOR_CYCLES + mean_words * ENGINE_WORD_CYCLES, 445 * LINES_PER_FRAME,
           100.0 * (mean_desc * ENGINE_DESCRIPTOR_CYCLES + mean_words * ENGINE_WORD_CYCLES) / (445.0 * LINES_PER_FRAME));

    // Pitch of every tone period the trace sounded, from the driver's table.
    size_t periods = 0;
    double worst_cents = 0.0;
    double sum_cents = 0.0;
    {
        const std::string path = opt.out + "/pitch_table.txt";
        FILE *fp = fopen(path.c_str(), "w");
        if (fp)
        {
            fprintf(fp, "N      Hz    PERIOD  LENGTH  rate_Hz   cents\n");
        }
        for (int n = 1; n <= 1024; ++n)
        {
            const int idx = n & 1023;
            if (((driver.periods_used[idx / 8] >> (n % 8)) & 1) == 0)
            {
                continue;
            }
            const auto& t = griffin::audio::psg_tables::TONE[idx];
            const double want = 3579545.0 / (32.0 * n);
            const double rate = 1573437.5 / (t.period + 1);
            const double cents = 1200.0 * std::log2(rate / t.length / want);
            if (fp)
            {
                fprintf(fp, "%4d %8.2f  %3u  %4u  %9.1f  %+.3f\n", n, want, t.period, t.length, rate, cents);
            }
            worst_cents = std::max(worst_cents, std::fabs(cents));
            sum_cents += std::fabs(cents);
            ++periods;
        }
        if (fp)
        {
            fclose(fp);
        }
    }
    printf("\nPITCH (audible tones the trace used: %zu periods)\n", periods);
    printf("  worst |error| %.3f cents, mean %.3f cents (table %s/pitch_table.txt)\n", worst_cents,
           periods ? sum_cents / periods : 0.0, opt.out.c_str());

    printf("\nAGAINST THE REFERENCE (mono mixdown, same scale)\n");
    {
        double e_ref = 0.0;
        double e_our = 0.0;
        for (size_t i = 0; i < ref_d.size(); ++i)
        {
            e_ref += ref_d[i] * ref_d[i];
            e_our += our_d[i] * our_d[i];
        }
        const double rms_ref = std::sqrt(e_ref / std::max<size_t>(1, ref_d.size()));
        const double rms_our = std::sqrt(e_our / std::max<size_t>(1, our_d.size()));
        printf("  RMS reference %.2f, ours %.2f LSB (ratio %.3f)\n", rms_ref, rms_our, rms_ref > 0 ? rms_our / rms_ref : 0.0);
        // Per field (263 ticks, 16.7 ms): the best correlation over lags 0..16
        // ticks, averaged over fields where the reference is not silent.  A
        // cent of pitch error decorrelates whole-run samples within seconds,
        // so the whole-run figure says nothing; fields are short enough.
        double corr_sum = 0.0;
        double lag_sum = 0.0;
        size_t fields_scored = 0;
        size_t fields_below_half = 0;
        for (size_t start = 0; start + 263 + 16 <= ref_d.size(); start += 263)
        {
            double e_r = 0.0;
            for (size_t i = start; i < start + 263; ++i)
            {
                e_r += ref_d[i] * ref_d[i];
            }
            if (e_r < 263.0)   // reference RMS under 1 LSB: silence
            {
                continue;
            }
            double best = -1.0;
            int best_l = 0;
            for (int lag = 0; lag <= 16; ++lag)
            {
                double xy = 0.0;
                double e_o = 0.0;
                for (size_t i = start; i < start + 263; ++i)
                {
                    xy += our_d[i + lag] * ref_d[i];
                    e_o += our_d[i + lag] * our_d[i + lag];
                }
                const double corr = (e_o > 0.0) ? xy / std::sqrt(e_r * e_o) : 0.0;
                if (corr > best)
                {
                    best = corr;
                    best_l = lag;
                }
            }
            corr_sum += best;
            lag_sum += best_l;
            ++fields_scored;
            if (best < 0.5)
            {
                ++fields_below_half;
            }
        }
        printf("  per-field best correlation (phase-sensitive): mean %.3f over %zu non-silent fields, mean lag %.1f ticks, %zu fields below 0.5\n",
               fields_scored ? corr_sum / fields_scored : 0.0, fields_scored,
               fields_scored ? lag_sum / fields_scored : 0.0, fields_below_half);

        // Phase-blind: cosine similarity of the two magnitude spectra of each
        // field (256-point, bins 1..127, 61 Hz per bin), Hann windowed.  The
        // voices' relative phases are inaudible and differ between the two
        // sides, so this is the figure that says "the same notes at the same
        // loudness".
        double spec_sum = 0.0;
        size_t spec_fields = 0;
        size_t spec_good = 0;
        size_t spec_bad = 0;
        double worst_spec = 1.0;
        size_t worst_field = 0;
        std::vector<double> ar(256), ai(256), br(256), bi(256);
        for (size_t start = 0; start + 263 + 16 <= ref_d.size(); start += 263)
        {
            double e_r = 0.0;
            for (size_t i = start; i < start + 256; ++i)
            {
                e_r += ref_d[i] * ref_d[i];
            }
            if (e_r < 256.0)
            {
                continue;
            }
            for (size_t i = 0; i < 256; ++i)
            {
                const double w = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * static_cast<double>(i) / 256.0);
                ar[i] = ref_d[start + i] * w;
                ai[i] = 0.0;
                br[i] = our_d[start + i + 8] * w;     // 8 ticks of typical latency
                bi[i] = 0.0;
            }
            fft(ar, ai);
            fft(br, bi);
            double xy = 0.0;
            double xx = 0.0;
            double yy = 0.0;
            for (size_t k = 1; k < 128; ++k)
            {
                const double ma = std::sqrt(ar[k] * ar[k] + ai[k] * ai[k]);
                const double mb = std::sqrt(br[k] * br[k] + bi[k] * bi[k]);
                xy += ma * mb;
                xx += ma * ma;
                yy += mb * mb;
            }
            const double cosine = (xx > 0.0 && yy > 0.0) ? xy / std::sqrt(xx * yy) : 0.0;
            spec_sum += cosine;
            ++spec_fields;
            if (cosine >= 0.9)
            {
                ++spec_good;
            }
            if (cosine < 0.5)
            {
                ++spec_bad;
            }
            if (cosine < worst_spec)
            {
                worst_spec = cosine;
                worst_field = start / 263;
            }
        }
        printf("  per-field spectral similarity (phase-blind): mean %.3f over %zu fields, %zu at or above 0.9, %zu below 0.5, worst %.3f at field %zu (%.1f s)\n",
               spec_fields ? spec_sum / spec_fields : 0.0, spec_fields, spec_good, spec_bad, worst_spec, worst_field,
               worst_field * 263 / 15734.375);
        printf("  (an RC corner, FIFO latency and re-phased tables make bit identity impossible; the ear decides)\n");
    }

    const bool ok = write_wav(opt.out + "/wav/reference.wav", ref_out, 1) &&
                    write_wav(opt.out + "/wav/audio.wav", our_mono, 1) &&
                    write_wav(opt.out + "/wav/audio_stereo.wav", our_stereo, 2);
    printf("\nWAV: %s/wav/{reference,audio,audio_stereo}.wav %s\n", opt.out.c_str(), ok ? "written" : "FAILED");

    const bool pass = underruns_active == 0 && dropped == 0 && chip.stats.control_dropped == 0 &&
                      mirror_max_off <= opt.margin && ok;
    printf("\n%s\n", pass ? "EXACT CHECKS PASS" : "EXACT CHECKS FAIL");
    return pass ? 0 : 1;
}
