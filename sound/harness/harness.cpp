// harness.cpp — the Z80 sound processor on a z80emu core, in lockstep with the
// C++ model (mixer/mixer.h, the spec).
//
// Per tick k: the boundary-k records are pushed into the FIFO model; the Z80
// runs until the AUDIO_TICK period (mhz * 1e6 / 15734.375 T, fraction carried)
// has elapsed since the previous NMI; the NMI is injected; the words the Z80
// popped in the gap before it are fed to Mixer (exactly those, so "same
// records, same boundaries" holds by construction), Mixer::tick() gives the
// expected pair; when the handler's RETN completes, the DAC pair the Z80
// latched (bit 7 un-inverted) is compared bit for bit.  T-states inside the
// handler (accept to RETN), the main loop's T per record (from a header's FIFO
// read to the next FIFO read, NMI time excluded), FIFO underflows and overruns
// are measured.  The model is the spec: a mismatch is reported, never tuned.
//
// Sources: --synthetic nop (oracle a, a generated NOP ROM), --synthetic square
// (b), --trace FILE (c, the Smurf PSG trace through psg_driver, with --pcm
// adding a silent SAMPLES stream), --synthetic records (d, every record type,
// uploads, the overrun fallback).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

extern "C"
{
#include "z80emu/z80emu.h"
#include "z80emu/instructions.h"
}
#include "z80user.h"

#include "../audio_records.h"
#include "mixer.h"
#include "psg_driver.h"
#include "tests/trace_io.h"

namespace ar = griffin::audio;
namespace mx = griffin::mixer;
namespace mp = griffin::mixer::psg;

namespace
{

constexpr double SAMPLE_HZ = 25175000.0 / 1600.0;   // 15734.375
constexpr int WAV_RATE = 15734;
constexpr size_t FIFO_DEPTH = 256;                  // one 7200 pair

// Ports, decoded on A4..A2 as the 74HC138 does.
enum class Port { FifoHi = 7, FifoLo = 6, DacL = 5, DacR = 4, Debug = 3 };

Port decode_port(uint8_t port)
{
    return static_cast<Port>((port >> 2) & 7);
}

// Record types for the drain statistics.
enum RecType { R_SET, R_WAIT, R_SAMPLES, R_LOAD, R_JUMP, R_VOICE, R_RESET, R_OTHER, R_COUNT };
const char *const REC_NAMES[R_COUNT] = { "SET", "WAIT", "SAMPLES", "LOAD", "JUMP", "VOICE", "RESET", "other" };

RecType classify(uint8_t hi)
{
    if (hi & 0x80)
    {
        return R_SET;
    }
    if ((hi & 0xC0) == 0)
    {
        return R_WAIT;
    }
    switch (hi)
    {
        case ar::REC_SAMPLES: return R_SAMPLES;
        case ar::REC_LOAD: return R_LOAD;
        case ar::REC_JUMP: return R_JUMP;
        case ar::REC_VOICE: return R_VOICE;
        case ar::REC_RESET: return R_RESET;
        default: return R_OTHER;
    }
}

struct TStat
{
    uint64_t count = 0;
    uint64_t sum = 0;
    uint64_t max = 0;
    void add(uint64_t t)
    {
        ++count;
        sum += t;
        max = std::max(max, t);
    }
    double mean() const { return count ? static_cast<double>(sum) / static_cast<double>(count) : 0.0; }
};

// What the Z80 popped in one gap, as the model must see it.
struct GapAction
{
    enum Kind { Audcmd, Sample, Reset, Voice } kind;
    uint16_t word = 0;
    int voice = 0;
    uint16_t base = 0;
    uint16_t length = 0;
};

// The record grammar as the drain reads it: which words are headers, which are
// payload, and what the model gets.
struct Parser
{
    enum State { Header, Samples, LoadDest, LoadWords, LoadSum, JumpEntry, VoiceBase, VoiceLen } state = Header;
    int left = 0;
    bool repeat = false;
    int voice = 0;
    uint16_t voice_base = 0;

    bool at_header() const { return state == Header; }

    void word(uint16_t w, std::vector<GapAction>& out)
    {
        switch (state)
        {
            case Header:
            {
                const uint8_t hi = ar::high_byte(w);
                const uint8_t lo = ar::low_byte(w);
                if (ar::is_set(w) || ar::is_wait(w))
                {
                    out.push_back({ GapAction::Audcmd, w });
                    break;
                }
                switch (hi)
                {
                    case ar::REC_SAMPLES:
                        left = lo & 0x7F;
                        repeat = (lo & ar::SAMPLES_REPEAT) != 0;
                        if (left)
                        {
                            state = Samples;
                        }
                        break;
                    case ar::REC_LOAD:
                        left = lo ? lo : 256;
                        state = LoadDest;
                        break;
                    case ar::REC_JUMP:
                        state = JumpEntry;
                        break;
                    case ar::REC_VOICE:
                        voice = lo & 0x0F;
                        state = VoiceBase;
                        break;
                    case ar::REC_RESET:
                        out.push_back({ GapAction::Reset });
                        break;
                    default:
                        break;
                }
                break;
            }
            case Samples:
                out.push_back({ GapAction::Sample, w });
                if (repeat)
                {
                    out.push_back({ GapAction::Sample, w });
                }
                if (--left == 0)
                {
                    state = Header;
                }
                break;
            case LoadDest:
                state = LoadWords;
                break;      // payload words carry one byte each; the count is in `left`
            case LoadWords:
                if (--left == 0)
                {
                    state = LoadSum;
                }
                break;
            case LoadSum:
            case JumpEntry:
                state = Header;
                break;
            case VoiceBase:
                voice_base = w;
                state = VoiceLen;
                break;
            case VoiceLen:
            {
                GapAction a{ GapAction::Voice };
                a.voice = voice;
                a.base = voice_base;
                a.length = w;
                out.push_back(a);
                state = Header;
                break;
            }
        }
    }
};

struct NmiRecord
{
    uint64_t t_entry = 0;
    uint64_t t_exit = 0;
    uint64_t drain_t = 0;       // main-loop T spent on records in the gap before it
    uint8_t dac_l = 0x80;
    uint8_t dac_r = 0x80;
    bool dac_l_written = false;
    bool dac_r_written = false;
    bool nested = false;        // another NMI arrived inside it
    bool race = false;          // it landed while the drain had a record in flight
    bool counted = false;       // a tick (after init)
    int8_t exp_l = 0;
    int8_t exp_r = 0;
    bool compare = true;
    bool raw_expect = false;    // expected values are raw DAC bytes (uploaded tick)
};

struct Board;
Board *g_board = nullptr;

struct Board
{
    HarnessCtx ctx{};
    Z80_STATE cpu{};
    std::map<std::string, uint16_t> syms;

    // FIFO
    std::deque<uint16_t> fifo;
    std::optional<uint8_t> pending_lo;
    uint64_t fifo_overflows = 0;
    uint64_t fifo_underflows = 0;   // low byte read with nothing pending
    uint64_t fifo_pops = 0;
    uint64_t empty_reads = 0;
    std::vector<GapAction> gap_actions;     // committed: the drain finished the record
    std::vector<GapAction> pending_actions; // popped, record still in flight
    uint16_t sym_poll = 0;
    uint16_t sym_main_loop = 0;
    uint64_t race_ticks = 0;                // NMIs that landed with a record in flight
    uint32_t gap_pops = 0;
    uint32_t max_gap_pops = 0;
    Parser parser;

    // Drain timing
    TStat rec_stats[R_COUNT];
    bool have_header = false;
    RecType header_type = R_OTHER;
    uint64_t header_t = 0;
    uint64_t header_nmi_total = 0;
    uint64_t gap_drain_t = 0;
    uint64_t nmi_total_t = 0;

    // NMI tracking
    int nmi_depth = 0;
    uint64_t nmi_injections = 0;
    uint64_t nested_injections = 0;
    std::vector<NmiRecord> nmi_stack;
    std::vector<NmiRecord> done;    // finished NMIs in order
    NmiRecord *current = nullptr;

    // Debug port
    struct DebugWrite { uint64_t t; uint8_t value; };
    std::vector<DebugWrite> debug;

    bool init_done = false;
    uint16_t sym_main = 0;

    void load_rom(const std::string& path)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f)
        {
            fprintf(stderr, "harness: cannot open %s\n", path.c_str());
            exit(1);
        }
        f.read(reinterpret_cast<char *>(ctx.mem), 0x4000);
    }

    void load_syms(const std::string& path)
    {
        std::ifstream f(path);
        std::string name;
        std::string hex;
        while (f >> name >> hex)
        {
            syms[name] = static_cast<uint16_t>(strtoul(hex.c_str(), nullptr, 16));
        }
    }

    uint16_t sym(const std::string& name) const
    {
        auto it = syms.find(name);
        if (it == syms.end())
        {
            fprintf(stderr, "harness: no symbol %s\n", name.c_str());
            exit(1);
        }
        return it->second;
    }

    void push(uint16_t word)
    {
        if (!ar::is_depositable(word))
        {
            fprintf(stderr, "harness: protocol: word %04X has the 0xFF high byte (the empty sentinel)\n", word);
            exit(1);
        }
        if (fifo.size() >= FIFO_DEPTH)
        {
            ++fifo_overflows;
            return;
        }
        fifo.push_back(word);
    }

    // The port hooks.
    uint8_t in(uint8_t port, uint64_t t)
    {
        switch (decode_port(port))
        {
            case Port::FifoHi:
            {
                if (pending_lo)
                {
                    fprintf(stderr, "harness: high byte read with a low byte pending at T=%llu\n",
                            static_cast<unsigned long long>(t));
                    ++fifo_underflows;
                }
                if (fifo.empty())
                {
                    ++empty_reads;
                    return ar::EMPTY;
                }
                const uint16_t w = fifo.front();
                fifo.pop_front();
                ++fifo_pops;
                ++gap_pops;
                pending_lo = ar::low_byte(w);
                if (parser.at_header())
                {
                    have_header = true;
                    header_type = classify(ar::high_byte(w));
                    header_t = t;
                    header_nmi_total = nmi_total_t;
                }
                parser.word(w, pending_actions);
                // SAMPLES words reach the ring pair by pair: commit them at once.
                if (!pending_actions.empty() && pending_actions.back().kind == GapAction::Sample)
                {
                    for (const GapAction& a : pending_actions)
                    {
                        gap_actions.push_back(a);
                    }
                    pending_actions.clear();
                }
                return ar::high_byte(w);
            }
            case Port::FifoLo:
            {
                if (!pending_lo)
                {
                    fprintf(stderr, "harness: low byte read with nothing pending at T=%llu\n",
                            static_cast<unsigned long long>(t));
                    ++fifo_underflows;
                    return 0xFF;
                }
                const uint8_t v = *pending_lo;
                pending_lo.reset();
                return v;
            }
            default:
                return 0xFF;
        }
    }

    void commit_pending()
    {
        for (const GapAction& a : pending_actions)
        {
            gap_actions.push_back(a);
        }
        pending_actions.clear();
    }

    void close_record(uint64_t t)
    {
        if (have_header)
        {
            const uint64_t nmi_inside = nmi_total_t - header_nmi_total;
            const uint64_t dt = (t - header_t) - nmi_inside;
            rec_stats[header_type].add(dt);
            gap_drain_t += dt;
            have_header = false;
        }
    }

    void out(uint8_t port, uint8_t value, uint64_t t)
    {
        switch (decode_port(port))
        {
            case Port::DacL:
                if (current)
                {
                    current->dac_l = value;
                    current->dac_l_written = true;
                }
                break;
            case Port::DacR:
                if (current)
                {
                    current->dac_r = value;
                    current->dac_r_written = true;
                }
                break;
            case Port::Debug:
                debug.push_back({ t, value });
                break;
            default:
                break;
        }
    }

    int trace_insns = 0;

    void insn(int pc, int opcode, int instruction, int next_pc, uint64_t t_end)
    {
        if (trace_insns > 0)
        {
            --trace_insns;
            printf("    T=%-8llu pc %04X op %02X -> %04X  sp %04X hl %04X de %04X bc %04X\n",
                   static_cast<unsigned long long>(t_end), pc, opcode, next_pc, cpu.registers.word[Z80_SP],
                   cpu.registers.word[Z80_HL], cpu.registers.word[Z80_DE], cpu.registers.word[Z80_BC]);
        }
        if (!init_done && next_pc == sym_main)
        {
            init_done = true;
        }
        if (next_pc == sym_poll || next_pc == sym_main_loop)
        {
            close_record(t_end);
            commit_pending();       // records with no window effect (JUMP, LOAD, reserved SETs)
        }
        if (pc == 0x0038)
        {
            fprintf(stderr, "harness: the Z80 trapped at 0x0038 (RST 38H: it executed ROM padding) at T=%llu\n",
                    static_cast<unsigned long long>(t_end));
            exit(1);
        }
        if (instruction == RETI_RETN && nmi_depth > 0)
        {
            --nmi_depth;
            NmiRecord r = nmi_stack.back();
            nmi_stack.pop_back();
            r.t_exit = t_end;
            nmi_total_t += r.t_exit - r.t_entry;
            done.push_back(r);
            current = nmi_depth ? &nmi_stack.back() : nullptr;
        }
    }

    // One instruction (or one NOP while halted, as Z80EmulateOrHalted does).
    void step()
    {
        int cycles;
        if (cpu.status == Z80_STATUS_HALT)
        {
            cycles = 4;
            cpu.r = (cpu.r & 0x80) | ((cpu.r + 1) & 0x7f);
        }
        else
        {
            cycles = Z80Emulate(&cpu, 1, &ctx);
        }
        ctx.clock += static_cast<uint64_t>(cycles);
    }

    void inject_nmi()
    {
        NmiRecord r;
        r.t_entry = ctx.clock;
        r.drain_t = gap_drain_t;
        gap_drain_t = 0;
        if (have_header)
        {
            r.race = true;
            ++race_ticks;
        }
        if (nmi_depth > 0)
        {
            ++nested_injections;
            for (NmiRecord& outer : nmi_stack)
            {
                outer.nested = true;
            }
        }
        ++nmi_depth;
        ++nmi_injections;
        nmi_stack.push_back(r);
        current = &nmi_stack.back();
        const int cycles = Z80NonMaskableInterrupt(&cpu, &ctx);
        ctx.clock += static_cast<uint64_t>(cycles);
    }
};

uint8_t harness_in_impl(uint8_t port, uint64_t t) { return g_board->in(port, t); }

}   // namespace

extern "C" uint8_t harness_in(HarnessCtx *, uint8_t port, uint64_t t)
{
    return harness_in_impl(port, t);
}

int hz_watch = -1;

extern "C" void harness_watch(HarnessCtx *, uint16_t address, uint8_t byte)
{
    printf("    WATCH write %04X = %02X at pc %04X sp %04X T=%llu\n", address, byte, g_board->cpu.pc,
           g_board->cpu.registers.word[Z80_SP], static_cast<unsigned long long>(g_board->ctx.clock));
}

// A record's effect: the drain wrote the SET window or the WAIT counter.  The
// model gets the record at THIS boundary, which is what the Z80's next tick
// sees, rather than at the drain's return to its poll loop.
extern "C" void harness_effect(HarnessCtx *, uint16_t)
{
    g_board->commit_pending();
}

extern "C" void harness_out(HarnessCtx *, uint8_t port, uint8_t value, uint64_t t)
{
    g_board->out(port, value, t);
}

extern "C" void harness_insn(HarnessCtx *, int pc, int opcode, int instruction, int next_pc, uint64_t t_end)
{
    g_board->insn(pc, opcode, instruction, next_pc, t_end);
}

namespace
{

// ---------------------------------------------------------------------------
// Sources: what gets deposited at boundary k.
// ---------------------------------------------------------------------------

struct FieldCounterMirror     // frame_audio.h FieldCounter: 263/262 alternating
{
    uint8_t parity = 0;
    int take()
    {
        const int n = (parity & 1) ? 262 : 263;
        parity ^= 1;
        return n;
    }
};

struct Source
{
    virtual ~Source() = default;
    // Words for boundary k; returns false when the stream is finished.
    virtual bool deposit(uint32_t k, std::vector<uint16_t>& words) = 0;
};

// (b) square voices: SETs at tick 0, then changes with WAITs, both WAIT paths.
struct SquareSource : Source
{
    int voices;
    uint32_t ticks;
    uint32_t next_change = 0;
    uint32_t seed = 0x12345678;
    uint32_t rnd()
    {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return seed;
    }
    SquareSource(int v, uint32_t t) : voices(v), ticks(t) {}

    bool deposit(uint32_t k, std::vector<uint16_t>& w) override
    {
        if (k >= ticks)
        {
            return false;
        }
        if (k == 0)
        {
            w.push_back(ar::set_word(ar::LFSR_VOICE_SELECT, 0x0F));
            for (int v = 0; v < voices; ++v)
            {
                const uint16_t inc = static_cast<uint16_t>(0x0200 + 0x0173 * v);
                w.push_back(ar::set_word(ar::voice_addr(v, mx::V_INC_LO), static_cast<uint8_t>(inc)));
                w.push_back(ar::set_word(ar::voice_addr(v, mx::V_INC_HI), static_cast<uint8_t>(inc >> 8)));
                w.push_back(ar::set_word(ar::voice_addr(v, mx::V_AMP), static_cast<uint8_t>(20 + 3 * v)));
                w.push_back(ar::set_word(ar::voice_addr(v, mx::V_CTL), mx::ctl(mx::MODE_SQUARE, true, true, true)));
            }
            w.push_back(ar::wait_word(50));
            next_change = 50;
            return true;
        }
        if (k == next_change)
        {
            // Change one voice: INC, AMP (sometimes negative), PAN, sometimes a
            // RESET_PHASE, sometimes a direct PHASE SET; then a WAIT, sometimes
            // above 255 (wait_hi path).
            const int v = static_cast<int>(rnd() % static_cast<uint32_t>(voices));
            const uint16_t inc = static_cast<uint16_t>(rnd() & 0x3FFF);
            w.push_back(ar::set_word(ar::voice_addr(v, mx::V_INC_LO), static_cast<uint8_t>(inc)));
            w.push_back(ar::set_word(ar::voice_addr(v, mx::V_INC_HI), static_cast<uint8_t>(inc >> 8)));
            const int8_t amp = static_cast<int8_t>((rnd() % 121) - 60);
            w.push_back(ar::set_word(ar::voice_addr(v, mx::V_AMP), static_cast<uint8_t>(amp)));
            const uint32_t r = rnd();
            const bool pan_l = (r & 1) != 0;
            const bool pan_r = (r & 2) != 0;
            const bool reset = (r & 4) != 0;
            w.push_back(ar::set_word(ar::voice_addr(v, mx::V_CTL), mx::ctl(mx::MODE_SQUARE, pan_l, pan_r, reset)));
            if (r & 8)
            {
                w.push_back(ar::set_word(ar::voice_addr(v, mx::V_PHASE_HI), static_cast<uint8_t>(r >> 8)));
            }
            uint16_t n = static_cast<uint16_t>(1 + rnd() % 40);
            if ((rnd() & 7) == 0)
            {
                n = static_cast<uint16_t>(256 + rnd() % 100);
            }
            w.push_back(ar::wait_word(n));
            next_change = k + n;
        }
        return true;
    }
};

// (c) the Smurf trace through psg_driver, boundary by boundary as
// mixer_ab.cpp's Replay::run deposits it (silence_voices first, then per
// emission group the driver's SETs and one WAIT of the field's ticks).
struct TraceSource : Source
{
    Trace trace;
    mp::PsgDriver driver;
    FieldCounterMirror counter;
    int voices;
    size_t frame = 0;
    int group = 0;
    int fields = 0;
    uint32_t ticks_left = 0;
    uint32_t ticks_after = 0;   // trailing ticks after the last group
    bool finished = false;
    size_t groups_total = 0;
    size_t max_sets = 0;

    TraceSource(const std::string& path, int v) : voices(v)
    {
        if (!read_trace(path.c_str(), trace))
        {
            fprintf(stderr, "harness: cannot read trace %s\n", path.c_str());
            exit(1);
        }
    }

    bool deposit(uint32_t k, std::vector<uint16_t>& w) override
    {
        if (k == 0)
        {
            uint16_t words[2 * mx::MAX_VOICES];
            const size_t n = mp::silence_voices(mp::PSG_VOICES, voices, words);
            for (size_t i = 0; i < n; ++i)
            {
                w.push_back(words[i]);
            }
        }
        if (ticks_left > 0)
        {
            --ticks_left;
            return true;
        }
        if (finished)
        {
            if (ticks_after == 0)
            {
                return false;
            }
            --ticks_after;
            return true;
        }
        // Next group.
        while (frame < trace.frame_bytes.size() && group >= fields)
        {
            if (group > 0 || fields > 0)
            {
                ++frame;
                group = 0;
            }
            if (frame >= trace.frame_bytes.size())
            {
                break;
            }
            fields = std::max(static_cast<int>(trace.frame_ticks[frame].size()), 1);
            if (fields == 0)
            {
                break;
            }
            if (group < fields)
            {
                break;
            }
        }
        if (frame >= trace.frame_bytes.size())
        {
            finished = true;
            ticks_after = 300;
            return true;
        }
        const std::vector<uint8_t>& bytes = trace.frame_bytes[frame];
        const std::vector<uint16_t>& ticks = trace.frame_ticks[frame];
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
            driver.write(bytes[i]);
        }
        uint16_t sets[mp::MAX_SETS_PER_EMIT];
        const size_t n_sets = driver.emit(sets);
        for (size_t i = 0; i < n_sets; ++i)
        {
            w.push_back(sets[i]);
        }
        max_sets = std::max(max_sets, n_sets);
        const int n = counter.take();
        w.push_back(ar::wait_word(static_cast<uint16_t>(n)));
        ticks_left = static_cast<uint32_t>(n) - 1;
        ++groups_total;
        ++group;
        return true;
    }
};

// Silent PCM alongside any source, deposited the way the one-stream protocol
// requires: a WAIT blocks everything queued behind it, so the PCM for a
// stretch of ticks must precede the WAIT that covers it.  Every WAIT n the
// source emits becomes chunks of SAMPLES c + WAIT c (c <= 32), one chunk per
// boundary, as ENGINE line descriptors would deposit them; the ring never
// runs dry and every tick pays for the pop.
struct PcmChunker
{
    static constexpr uint16_t CHUNK = 32;
    uint32_t remaining = 0;     // ticks of the current WAIT still to cover
    uint32_t next_emit = 0;

    void chunk(std::vector<uint16_t>& w, uint16_t c)
    {
        w.push_back(ar::samples_header(c));
        for (int i = 0; i < c; ++i)
        {
            w.push_back(ar::sample_word(0, 0));
        }
        w.push_back(ar::wait_word(c));
    }

    // Rewrites `words` (this boundary's deposit) in place.
    void deposit(uint32_t k, std::vector<uint16_t>& words)
    {
        std::vector<uint16_t> out;
        if (remaining > 0 && k == next_emit)
        {
            const uint16_t c = static_cast<uint16_t>(std::min<uint32_t>(CHUNK, remaining));
            chunk(out, c);
            remaining -= c;
            next_emit = k + c;
        }
        for (uint16_t w : words)
        {
            if (ar::is_wait(w) && (w & ar::WAIT_MAX) != 0)
            {
                const uint32_t n = w & ar::WAIT_MAX;
                const uint16_t c = static_cast<uint16_t>(std::min<uint32_t>(CHUNK, n));
                chunk(out, c);
                remaining = n - c;
                next_emit = k + c;
            }
            else
            {
                out.push_back(w);
            }
        }
        words.swap(out);
    }
};

// (d) every record type on a scripted stream; see README.md "Oracle d".
struct RecordsSource : Source
{
    // Phases by first tick.
    enum Phase { P_SQUARE, P_PCM, P_WAVE, P_UPLOAD, P_BADJUMP, P_RESTORE, P_OVERRUN, P_RESET, P_END };
    static constexpr uint32_t PHASE_START[P_END + 1] = { 0, 300, 700, 1100, 1300, 1400, 1500, 1700, 1900 };
    uint16_t sym_tick_done;
    uint16_t sym_tick;
    uint16_t sym_main;
    int slow_loops;
    int8_t pcm_l = 0;
    int8_t pcm_r = 0;

    RecordsSource(uint16_t tick_done, uint16_t tick, uint16_t main, int slow)
        : sym_tick_done(tick_done), sym_tick(tick), sym_main(main), slow_loops(slow) {}

    static Phase phase_of(uint32_t k)
    {
        Phase p = P_SQUARE;
        for (int i = 0; i < P_END; ++i)
        {
            if (k >= PHASE_START[i])
            {
                p = static_cast<Phase>(i);
            }
        }
        return p;
    }

    static void load(std::vector<uint16_t>& w, uint16_t dest, const std::vector<uint8_t>& bytes, bool corrupt = false)
    {
        w.push_back(ar::load_header(static_cast<int>(bytes.size())));
        w.push_back(dest);
        for (uint8_t x : bytes)
        {
            w.push_back(ar::load_word(x));
        }
        const uint16_t sum = ar::load_sum(bytes.data(), static_cast<int>(bytes.size()));
        w.push_back(static_cast<uint16_t>(corrupt ? sum + 1 : sum));
    }

    bool deposit(uint32_t k, std::vector<uint16_t>& w) override
    {
        if (k >= PHASE_START[P_END])
        {
            return false;
        }
        const Phase p = phase_of(k);
        if (k == PHASE_START[P_SQUARE])
        {
            w.push_back(ar::set_word(ar::VOICE_COUNT, 4));
            w.push_back(ar::set_word(ar::LFSR_VOICE_SELECT, 3));
            // v0 square left, v1 square right, v2 silent, v3 white noise both
            const uint16_t incs[4] = { 0x0300, 0x0480, 0x0100, 0x0040 };
            const uint8_t amps[4] = { 30, 25, 0, 15 };
            const uint8_t ctls[4] = { mx::ctl(mx::MODE_SQUARE, true, false, true), mx::ctl(mx::MODE_SQUARE, false, true, true),
                                      mx::ctl(mx::MODE_SQUARE, false, false, true), mx::ctl(mx::MODE_NOISE_WHITE, true, true, true) };
            for (int v = 0; v < 4; ++v)
            {
                w.push_back(ar::set_word(ar::voice_addr(v, mx::V_INC_LO), static_cast<uint8_t>(incs[v])));
                w.push_back(ar::set_word(ar::voice_addr(v, mx::V_INC_HI), static_cast<uint8_t>(incs[v] >> 8)));
                w.push_back(ar::set_word(ar::voice_addr(v, mx::V_AMP), amps[v]));
                w.push_back(ar::set_word(ar::voice_addr(v, mx::V_CTL), ctls[v]));
            }
            w.push_back(ar::wait_word(100));
        }
        else if (k == 100)
        {
            w.push_back(ar::set_word(ar::voice_addr(3, mx::V_CTL), mx::ctl(mx::MODE_NOISE_PERIODIC, true, true, true)));
            w.push_back(ar::wait_word(100));
        }
        else if (k == 200)
        {
            w.push_back(ar::set_word(ar::voice_addr(3, mx::V_INC_HI), 0xFF));   // a shift every tick
            w.push_back(ar::set_word(ar::voice_addr(3, mx::V_INC_LO), 0xFF));
            w.push_back(ar::wait_word(100));
        }
        if (p == P_PCM)
        {
            // Live PCM (a ramp) 8 pairs at a time, repeat bit on every other
            // record; the voices keep sounding on top.
            if ((k - PHASE_START[P_PCM]) % 8 == 0)
            {
                const bool repeat = ((k - PHASE_START[P_PCM]) / 8) % 2 == 1;
                const int pairs = repeat ? 4 : 8;
                w.push_back(ar::samples_header(pairs, repeat));
                for (int i = 0; i < pairs; ++i)
                {
                    pcm_l = static_cast<int8_t>(pcm_l + 7);
                    pcm_r = static_cast<int8_t>(pcm_r - 5);
                    w.push_back(ar::sample_word(pcm_l, pcm_r));
                }
            }
        }
        if (k == PHASE_START[P_WAVE])
        {
            // Voices 0, 1, 3 quiet (PAN none), v2 a 64-byte wavetable at 0x8600.
            w.push_back(ar::set_word(ar::voice_addr(0, mx::V_CTL), mx::ctl(mx::MODE_SQUARE, false, false)));
            w.push_back(ar::set_word(ar::voice_addr(1, mx::V_CTL), mx::ctl(mx::MODE_SQUARE, false, false)));
            w.push_back(ar::set_word(ar::voice_addr(3, mx::V_CTL), mx::ctl(mx::MODE_NOISE_WHITE, false, false)));
            std::vector<uint8_t> table(64);
            for (int i = 0; i < 64; ++i)
            {
                table[static_cast<size_t>(i)] = static_cast<uint8_t>(static_cast<int8_t>((i * 4) - 128 + (i & 1 ? 100 : 0)));
            }
            load(w, 0x8600, table);
            w.push_back(ar::voice_header(2));
            w.push_back(0x8600);
            w.push_back(64);
            w.push_back(ar::set_word(ar::voice_addr(2, mx::V_INC_LO), 0x37));
            w.push_back(ar::set_word(ar::voice_addr(2, mx::V_INC_HI), 0x05));
            w.push_back(ar::set_word(ar::voice_addr(2, mx::V_CTL), mx::ctl(mx::MODE_WAVETABLE, true, true, true)));
            w.push_back(ar::wait_word(200));
        }
        if (k == PHASE_START[P_WAVE] + 200)
        {
            w.push_back(ar::set_word(ar::voice_addr(2, mx::V_CTL), mx::ctl(mx::MODE_WAVETABLE, true, false)));
            w.push_back(ar::wait_word(100));
        }
        if (k == PHASE_START[P_WAVE] + 300)
        {
            w.push_back(ar::set_word(ar::voice_addr(2, mx::V_CTL), mx::ctl(mx::MODE_WAVETABLE, false, true)));
            w.push_back(ar::wait_word(90));
        }
        if (k == PHASE_START[P_UPLOAD])
        {
            w.push_back(ar::reset_word());
            // An uploaded tick: OUT 0x12 / 0x34, JP tick_done.
            std::vector<uint8_t> code = { 0x3E, 0x12, 0xD3, 0xF7, 0x3E, 0x34, 0xD3, 0xF3,
                                          0xC3, static_cast<uint8_t>(sym_tick_done), static_cast<uint8_t>(sym_tick_done >> 8) };
            load(w, 0x8800, code);
            load(w, 0x8082, { 0x00, 0x88 });     // the tick vector, little endian
        }
        if (k == PHASE_START[P_BADJUMP])
        {
            // A program that restores the ROM synth: LD HL,tick; LD (0x8082),HL; JP main.
            std::vector<uint8_t> code = { 0x21, static_cast<uint8_t>(sym_tick), static_cast<uint8_t>(sym_tick >> 8),
                                          0x22, 0x82, 0x80,
                                          0xC3, static_cast<uint8_t>(sym_main), static_cast<uint8_t>(sym_main >> 8) };
            load(w, 0x8900, code, true);    // bad sum
            w.push_back(ar::jump_header());
            w.push_back(0x8900);                      // refused
        }
        if (k == PHASE_START[P_RESTORE])
        {
            std::vector<uint8_t> code = { 0x21, static_cast<uint8_t>(sym_tick), static_cast<uint8_t>(sym_tick >> 8),
                                          0x22, 0x82, 0x80,
                                          0xC3, static_cast<uint8_t>(sym_main), static_cast<uint8_t>(sym_main >> 8) };
            load(w, 0x8900, code);
            w.push_back(ar::jump_header());
            w.push_back(0x8900);                      // taken
        }
        if (k == PHASE_START[P_OVERRUN])
        {
            // A slow tick: slow_loops blocks of LD B,0 / DJNZ $ (256 x 13 T each,
            // immune to the A/F clobber of a nested NMI), OUT 0x55 to both
            // DACs, JP tick_done.
            std::vector<uint8_t> code;
            for (int i = 0; i < slow_loops; ++i)
            {
                code.insert(code.end(), { 0x06, 0x00, 0x10, 0xFE });
            }
            code.insert(code.end(), { 0x3E, 0x55, 0xD3, 0xF7, 0xD3, 0xF3,
                                      0xC3, static_cast<uint8_t>(sym_tick_done), static_cast<uint8_t>(sym_tick_done >> 8) });
            load(w, 0x8A00, code);
            load(w, 0x8082, { 0x00, 0x8A });
        }
        if (k == PHASE_START[P_RESET])
        {
            w.push_back(ar::reset_word());
        }
        return true;
    }
};

// ---------------------------------------------------------------------------
// WAV, as mixer_ab writes it: 16-bit mono, 8-bit samples scaled by 256.
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
        fprintf(stderr, "harness: cannot write %s\n", path.c_str());
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

int saturate(int v)
{
    return v > 127 ? 127 : (v < -128 ? -128 : v);
}

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------
struct Options
{
    std::string rom = "../rom/build/rom.bin";
    std::string syms;           // default: the ROM's path with .syms
    std::string trace;
    std::string synthetic;
    std::string out = "build";
    double mhz = 6.0;
    double period_override = 0.0;
    bool pcm = false;
    int voices = 4;
    uint32_t ticks = 10000;
    uint32_t stop = 0;          // stop a trace run after this many ticks (0 = whole trace)
    uint32_t dump_from = 0;     // -v -v -v per-tick lines from this tick on
    bool wav = true;
    int verbose = 0;
};

void usage()
{
    fprintf(stderr,
            "harness [--rom rom.bin] [--syms rom.syms] (--trace FILE | --synthetic nop|square|records)\n"
            "        [--mhz N] [--period T] [--pcm] [--voices N] [--ticks N] [--out DIR] [--no-wav] [-v]\n");
    exit(2);
}

Options parse(int argc, char **argv)
{
    Options o;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc)
            {
                usage();
            }
            return argv[++i];
        };
        if (a == "--rom") { o.rom = next(); }
        else if (a == "--syms") { o.syms = next(); }
        else if (a == "--trace") { o.trace = next(); }
        else if (a == "--synthetic") { o.synthetic = next(); }
        else if (a == "--mhz") { o.mhz = atof(next().c_str()); }
        else if (a == "--period") { o.period_override = atof(next().c_str()); }
        else if (a == "--pcm") { o.pcm = true; }
        else if (a == "--no-pcm") { o.pcm = false; }
        else if (a == "--voices") { o.voices = atoi(next().c_str()); }
        else if (a == "--ticks") { o.ticks = static_cast<uint32_t>(atol(next().c_str())); }
        else if (a == "--stop") { o.stop = static_cast<uint32_t>(atol(next().c_str())); }
        else if (a == "--dump-from") { o.dump_from = static_cast<uint32_t>(atol(next().c_str())); }
        else if (a == "--out") { o.out = next(); }
        else if (a == "--no-wav") { o.wav = false; }
        else if (a == "-v") { ++o.verbose; }
        else { usage(); }
    }
    if (o.trace.empty() && o.synthetic.empty())
    {
        usage();
    }
    if (o.syms.empty())
    {
        o.syms = o.rom.substr(0, o.rom.rfind('.')) + ".syms";
    }
    return o;
}

// ---------------------------------------------------------------------------
// Oracle (a): a generated NOP ROM whose handler costs exactly 50 T after the
// 11 T accept: EX AF,AF' 4, EXX 4, 5 x NOP 20, EXX 4, EX AF,AF' 4, RETN 14.
// The main program is JR $ (12 T), so an NMI is accepted at most 12 T late.
// ---------------------------------------------------------------------------
int run_nop(Board& b, const Options& o, double period)
{
    memset(b.ctx.mem, 0, sizeof b.ctx.mem);
    b.ctx.mem[0x0000] = 0x18;
    b.ctx.mem[0x0001] = 0xFE;
    const uint8_t handler[] = { 0x08, 0xD9, 0x00, 0x00, 0x00, 0x00, 0x00, 0xD9, 0x08, 0xED, 0x45 };
    memcpy(&b.ctx.mem[0x0066], handler, sizeof handler);
    Z80Reset(&b.cpu);
    b.init_done = true;
    constexpr uint64_t HANDLER_T = 11 + 50;

    double next_nmi = 0.0;
    uint64_t failures = 0;
    uint64_t max_late = 0;
    for (uint32_t k = 0; k < o.ticks; ++k)
    {
        while (static_cast<double>(b.ctx.clock) < next_nmi)
        {
            b.step();
        }
        const uint64_t late = b.ctx.clock - static_cast<uint64_t>(next_nmi);
        max_late = std::max(max_late, late);
        if (late > 12)
        {
            ++failures;
        }
        b.inject_nmi();
        while (b.nmi_depth > 0)
        {
            b.step();
        }
        const NmiRecord& r = b.done.back();
        if (r.t_exit - r.t_entry != HANDLER_T)
        {
            if (failures < 5)
            {
                printf("  tick %u: handler took %llu T, expected %llu\n", k,
                       static_cast<unsigned long long>(r.t_exit - r.t_entry), static_cast<unsigned long long>(HANDLER_T));
            }
            ++failures;
        }
        next_nmi += period;
    }
    const double elapsed = static_cast<double>(b.ctx.clock);
    printf("oracle a: %u NMIs, period %.4f T, handler %llu T each, max acceptance lateness %llu T, "
           "elapsed %.0f T (ideal %.0f), %llu failures\n",
           o.ticks, period, static_cast<unsigned long long>(HANDLER_T), static_cast<unsigned long long>(max_late),
           elapsed, period * o.ticks, static_cast<unsigned long long>(failures));
    return failures ? 1 : 0;
}

// ---------------------------------------------------------------------------
// The lockstep run
// ---------------------------------------------------------------------------
struct WaveVoice
{
    uint16_t base = 0;
    uint8_t mask = 0;
    bool active = false;
};

int run(Board& b, const Options& o, double period, Source& source)
{
    b.load_rom(o.rom);
    b.load_syms(o.syms);
    b.sym_main = b.sym("main");
    b.sym_poll = b.sym("poll");
    b.sym_main_loop = b.sym("main_loop");
    const uint16_t sym_tick = b.sym("tick");
    Z80Reset(&b.cpu);

    if (o.verbose >= 2)
    {
        b.trace_insns = 60000;
    }
    mx::Mixer model(o.voices);
    // The window is unreset SRAM: the Z80 sees the model's power-on garbage.
    for (int i = 0; i < mx::SET_WINDOW_BYTES; ++i)
    {
        b.ctx.mem[0x8000 + i] = model.ram(static_cast<uint8_t>(i));
    }
    WaveVoice wave[mx::MAX_VOICES];
    std::optional<std::vector<uint16_t>> pending_first;

    // Run init: NMIs happen from the first period as on hardware (the Z80 accepts
    // no NMI before its first instruction); those are idle ticks, uncounted.
    double next_nmi = period;
    uint64_t init_nmis = 0;
    while (!b.init_done)
    {
        while (static_cast<double>(b.ctx.clock) < next_nmi && !b.init_done)
        {
            b.step();
        }
        if (b.init_done)
        {
            break;
        }
        b.inject_nmi();
        ++init_nmis;
        next_nmi += period;
    }
    const uint64_t t_init = b.ctx.clock;
    if (o.verbose)
    {
        printf("init done at T=%llu, next NMI at %.0f\n", static_cast<unsigned long long>(t_init), next_nmi);
        fflush(stdout);
    }
    // Tick 0's NMI is the next one on the schedule.
    b.gap_actions.clear();
    b.gap_pops = 0;
    b.gap_drain_t = 0;
    b.have_header = false;

    std::vector<uint16_t> words;
    uint32_t k = 0;
    PcmChunker pcm;
    if (o.voices != 4 && o.synthetic != "records")
    {
        b.push(ar::set_word(ar::VOICE_COUNT, static_cast<uint8_t>(o.voices)));
    }
    bool more = source.deposit(0, words);
    if (o.pcm)
    {
        pcm.deposit(0, words);
    }
    for (uint16_t w : words)
    {
        b.push(w);
    }

    std::vector<int8_t> wav_left;
    uint64_t mismatches = 0;
    uint64_t race_mismatches = 0;   // at a tick whose NMI landed mid-record (not exact by construction)
    uint64_t compared = 0;
    uint64_t skipped = 0;
    size_t done_consumed = 0;
    std::vector<uint64_t> hist;     // 16-T bins
    TStat nmi_stat;
    TStat gate_stat;                // nmi T + gap drain T per tick
    uint64_t max_gap_pops = 0;
    bool fell_back_seen = false;
    uint64_t rom_overruns_at_end = 0;

    auto model_tick = [&](NmiRecord& r) {
        // Feed the model what the Z80 popped in the gap before this NMI.
        bool reset = false;
        for (const GapAction& a : b.gap_actions)
        {
            switch (a.kind)
            {
                case GapAction::Audcmd: model.push_audcmd(a.word); break;
                case GapAction::Sample: model.push_samples(a.word); break;
                case GapAction::Reset:
                    reset = true;
                    model.reset();
                    for (int v = 0; v < mx::MAX_VOICES; ++v)
                    {
                        model.push_audcmd(ar::set_word(ar::voice_addr(v, mx::V_AMP), 0));
                        model.push_audcmd(ar::set_word(ar::voice_addr(v, mx::V_CTL), 0));
                        wave[v].active = false;
                    }
                    break;
                case GapAction::Voice:
                {
                    WaveVoice& wv = wave[a.voice];
                    const int len = (a.length == 0 || a.length > 256) ? 256 : a.length;
                    wv.mask = static_cast<uint8_t>(len - 1);
                    wv.base = static_cast<uint16_t>(a.base & ~static_cast<uint16_t>(wv.mask));
                    wv.active = true;
                    // The ROM forces MODE 2, keeping PAN: the model gets the same CTL.
                    const uint8_t pan = static_cast<uint8_t>(model.ram(ar::voice_addr(a.voice, mx::V_CTL)) & 3);
                    model.push_audcmd(ar::set_word(ar::voice_addr(a.voice, mx::V_CTL),
                                                   static_cast<uint8_t>((mx::MODE_WAVETABLE << mx::CTL_MODE_SHIFT) | pan)));
                    break;
                }
            }
        }
        (void)reset;
        max_gap_pops = std::max<uint64_t>(max_gap_pops, b.gap_pops);
        b.gap_actions.clear();
        b.gap_pops = 0;
        const mx::StereoSample s = model.tick();
        int left = s.left;
        int right = s.right;
        // Wavetable voices: the model is silent for them; add what the ROM
        // adds, valid when no other audible voice follows a wave voice (the
        // records source keeps the other voices PAN none then).
        for (int v = 0; v < model.voices(); ++v)
        {
            const uint8_t ctl = model.ram(ar::voice_addr(v, mx::V_CTL));
            if (((ctl & mx::CTL_MODE_MASK) >> mx::CTL_MODE_SHIFT) != mx::MODE_WAVETABLE)
            {
                continue;
            }
            const uint16_t phase = model.phase(v);
            const uint8_t index = static_cast<uint8_t>((phase >> 8) & wave[v].mask);
            const int8_t sample = static_cast<int8_t>(b.ctx.mem[static_cast<uint16_t>(wave[v].base + index)]);
            if (ctl & mx::CTL_PAN_L)
            {
                left = saturate(left + sample);
            }
            if (ctl & mx::CTL_PAN_R)
            {
                right = saturate(right + sample);
            }
        }
        r.exp_l = static_cast<int8_t>(left);
        r.exp_r = static_cast<int8_t>(right);
    };

    // Expected-value policy for the records source's upload phases.
    auto records_policy = [&](uint32_t tick, NmiRecord& r) {
        if (o.synthetic != "records")
        {
            return;
        }
        const RecordsSource::Phase p = RecordsSource::phase_of(tick);
        if (p == RecordsSource::P_UPLOAD || p == RecordsSource::P_BADJUMP)
        {
            if (tick >= RecordsSource::PHASE_START[RecordsSource::P_UPLOAD] + 2)
            {
                r.raw_expect = true;
                r.exp_l = 0x12;
                r.exp_r = 0x34;
            }
            else
            {
                r.compare = false;
            }
        }
        else if (p == RecordsSource::P_RESTORE || p == RecordsSource::P_OVERRUN)
        {
            r.compare = false;      // the JUMPed program and the slow tick: checked by state below
        }
        else if (p == RecordsSource::P_RESET && tick < RecordsSource::PHASE_START[RecordsSource::P_RESET] + 3)
        {
            r.compare = false;
        }
    };

    while (more)
    {
        // Run the gap.
        while (static_cast<double>(b.ctx.clock) < next_nmi)
        {
            b.step();
        }
        b.inject_nmi();
        if (o.verbose && (k % 1000 == 0 || k < 5))
        {
            printf("tick %u at T=%llu fifo %zu depth %d\n", k, static_cast<unsigned long long>(b.ctx.clock), b.fifo.size(), b.nmi_depth);
            fflush(stdout);
        }
        NmiRecord& injected = b.nmi_stack.back();
        injected.counted = true;
        model_tick(injected);
        records_policy(k, injected);
        next_nmi += period;
        ++k;
        // Boundary k's records go in now, during tick k-1's handler and the gap after it.
        words.clear();
        more = source.deposit(k, words);
        if (o.stop && k >= o.stop)
        {
            more = false;
        }
        if (o.pcm)
        {
            pcm.deposit(k, words);
        }
        for (uint16_t w : words)
        {
            b.push(w);
        }
        if (!more)
        {
            // Let the last handler finish (bounded: an uploaded tick may never return).
            const uint64_t limit = b.ctx.clock + 100000;
            while (b.nmi_depth > 0 && b.ctx.clock < limit)
            {
                b.step();
            }
        }
        // Consume finished NMIs.
        for (; done_consumed < b.done.size(); ++done_consumed)
        {
            const NmiRecord& r = b.done[done_consumed];
            if (!r.counted)
            {
                continue;
            }
            const uint64_t t = r.t_exit - r.t_entry;
            nmi_stat.add(t);
            gate_stat.add(t + r.drain_t);
            const size_t bin = static_cast<size_t>(t / 16);
            if (hist.size() <= bin)
            {
                hist.resize(bin + 1, 0);
            }
            ++hist[bin];
            const int8_t got_l = static_cast<int8_t>(r.dac_l ^ 0x80);
            const int8_t got_r = static_cast<int8_t>(r.dac_r ^ 0x80);
            wav_left.push_back(got_l);
            if (!r.compare || r.nested)
            {
                ++skipped;
                continue;
            }
            ++compared;
            if (o.verbose >= 3 && done_consumed >= o.dump_from)
            {
                printf("  tick %zu z80 (%d,%d) model (%d,%d) phases model %04X %04X %04X %04X z80 %02X%02X %02X%02X %02X%02X %02X%02X gap_pops %u\n",
                       done_consumed, got_l, got_r, r.exp_l, r.exp_r, model.phase(0), model.phase(1), model.phase(2), model.phase(3),
                       b.ctx.mem[0x8005], b.ctx.mem[0x8004], b.ctx.mem[0x800D], b.ctx.mem[0x800C],
                       b.ctx.mem[0x8015], b.ctx.mem[0x8014], b.ctx.mem[0x801D], b.ctx.mem[0x801C], b.gap_pops);
            }
            bool ok;
            if (r.raw_expect)
            {
                ok = r.dac_l == static_cast<uint8_t>(r.exp_l) && r.dac_r == static_cast<uint8_t>(r.exp_r);
            }
            else
            {
                ok = r.dac_l_written && r.dac_r_written && got_l == r.exp_l && got_r == r.exp_r;
            }
            if (!ok && r.race)
            {
                ++race_mismatches;
            }
            else if (!ok)
            {
                if (mismatches < 10 || o.verbose)
                {
                    printf("  MISMATCH tick %zu: z80 (%d, %d)%s model (%d, %d)  [NMI %llu T]\n",
                           done_consumed, got_l, got_r, r.dac_l_written && r.dac_r_written ? "" : " (DAC not written)",
                           r.exp_l, r.exp_r, static_cast<unsigned long long>(t));
                }
                if (mismatches == 0 && o.verbose)
                {
                    for (int v = 0; v < model.voices(); ++v)
                    {
                        printf("    v%d model:", v);
                        for (int i = 0; i < 8; ++i)
                        {
                            printf(" %02X", model.ram(ar::voice_addr(v, static_cast<uint8_t>(i))));
                        }
                        printf("   z80:");
                        for (int i = 0; i < 8; ++i)
                        {
                            printf(" %02X", b.ctx.mem[0x8000 + v * 8 + i]);
                        }
                        printf("\n");
                    }
                    printf("    model wait %u lfsr %04X; z80 wait %02X%02X lfsr %02X%02X\n", model.wait_remaining(), model.lfsr(),
                           b.ctx.mem[0x8085], b.ctx.mem[0x8084], b.ctx.mem[0x8087], b.ctx.mem[0x8086]);
                }
                ++mismatches;
            }
        }
        // State checks for the records source's upload phases.
        if (o.synthetic == "records")
        {
            const uint16_t vector = static_cast<uint16_t>(b.ctx.mem[0x8082] | (b.ctx.mem[0x8083] << 8));
            if (k == RecordsSource::PHASE_START[RecordsSource::P_BADJUMP] + 50 && vector != 0x8800)
            {
                printf("  FAIL: a JUMP after a bad LOAD was taken (vector %04X)\n", vector);
                ++mismatches;
            }
            if (k == RecordsSource::PHASE_START[RecordsSource::P_OVERRUN] - 1 && vector != sym_tick)
            {
                printf("  FAIL: the JUMPed program did not restore the ROM synth (vector %04X)\n", vector);
                ++mismatches;
            }
            if (k == RecordsSource::PHASE_START[RecordsSource::P_RESET] - 1)
            {
                rom_overruns_at_end = static_cast<uint64_t>(b.ctx.mem[0x808B] | (b.ctx.mem[0x808C] << 8));
                fell_back_seen = vector == sym_tick;
                if (!fell_back_seen)
                {
                    printf("  FAIL: no fallback to the ROM synth after overruns (vector %04X)\n", vector);
                    ++mismatches;
                }
            }
        }
    }

    // Report.
    printf("ticks %llu compared %llu skipped %llu mismatches %llu (plus %llu at %llu race ticks: the NMI landed mid-record)\n",
           static_cast<unsigned long long>(nmi_stat.count), static_cast<unsigned long long>(compared),
           static_cast<unsigned long long>(skipped), static_cast<unsigned long long>(mismatches),
           static_cast<unsigned long long>(race_mismatches), static_cast<unsigned long long>(b.race_ticks));
    printf("period %.4f T (%.3f MHz); init %llu T with %llu idle NMIs\n", period, o.mhz,
           static_cast<unsigned long long>(t_init), static_cast<unsigned long long>(init_nmis));
    printf("NMI T: worst %llu mean %.1f; NMI + gap drain per tick: worst %llu mean %.1f\n",
           static_cast<unsigned long long>(nmi_stat.max), nmi_stat.mean(),
           static_cast<unsigned long long>(gate_stat.max), gate_stat.mean());
    printf("overruns: %llu nested NMI injections (ROM counter %u); FIFO: %llu pops, %llu overflows, %llu underflows, "
           "%llu empty reads, max pops per gap %llu\n",
           static_cast<unsigned long long>(b.nested_injections),
           b.ctx.mem[0x808B] | (b.ctx.mem[0x808C] << 8),
           static_cast<unsigned long long>(b.fifo_pops), static_cast<unsigned long long>(b.fifo_overflows),
           static_cast<unsigned long long>(b.fifo_underflows), static_cast<unsigned long long>(b.empty_reads),
           static_cast<unsigned long long>(max_gap_pops));
    printf("model: %u sets %u waits %u empty boundaries %u limited, %u samples popped %u empty, %u saturations, %u lfsr shifts\n",
           model.stats.sets, model.stats.waits, model.stats.empty_boundaries, model.stats.limited_boundaries,
           model.stats.samples_popped, model.stats.samples_empty, model.stats.saturations, model.stats.lfsr_shifts);
    printf("drain T per record (header read to the next FIFO read, NMI time excluded):\n");
    for (int i = 0; i < R_COUNT; ++i)
    {
        if (b.rec_stats[i].count)
        {
            printf("  %-8s n %8llu  mean %7.1f  max %6llu\n", REC_NAMES[i],
                   static_cast<unsigned long long>(b.rec_stats[i].count), b.rec_stats[i].mean(),
                   static_cast<unsigned long long>(b.rec_stats[i].max));
        }
    }
    printf("NMI T histogram (16-T bins):\n");
    for (size_t i = 0; i < hist.size(); ++i)
    {
        if (hist[i])
        {
            printf("  %5zu-%-5zu %llu\n", i * 16, i * 16 + 15, static_cast<unsigned long long>(hist[i]));
        }
    }
    if (!b.debug.empty())
    {
        printf("DEBUG port: %zu writes, first 0x%02X at T=%llu\n", b.debug.size(), b.debug[0].value,
               static_cast<unsigned long long>(b.debug[0].t));
    }
    if (o.synthetic == "records")
    {
        printf("records: overrun fallback %s, ROM overrun total %llu\n", fell_back_seen ? "seen" : "NOT seen",
               static_cast<unsigned long long>(rom_overruns_at_end));
    }
    if (o.wav)
    {
        const std::string dir = o.out + "/wav";
        (void)system(("mkdir -p " + dir).c_str());
        write_wav(dir + "/z80.wav", wav_left);
        printf("wrote %s/z80.wav (%zu samples, %.1f s)\n", dir.c_str(), wav_left.size(),
               static_cast<double>(wav_left.size()) / SAMPLE_HZ);
    }
    return (mismatches || b.fifo_underflows) ? 1 : 0;
}

}   // namespace

int main(int argc, char **argv)
{
    const Options o = parse(argc, argv);
    if (const char *w = getenv("HZ_WATCH"))
    {
        hz_watch = static_cast<int>(strtol(w, nullptr, 16));
    }
    static Board board;
    g_board = &board;
    const double period = o.period_override > 0 ? o.period_override : (o.mhz * 1e6) / SAMPLE_HZ;

    if (o.synthetic == "nop")
    {
        return run_nop(board, o, period);
    }
    if (!o.trace.empty())
    {
        TraceSource src(o.trace, o.voices);
        printf("trace %s: %zu frames, %zu bytes\n", o.trace.c_str(), src.trace.frame_bytes.size(), src.trace.total_bytes);
        return run(board, o, period, src);
    }
    if (o.synthetic == "square")
    {
        SquareSource src(o.voices, o.ticks);
        return run(board, o, period, src);
    }
    if (o.synthetic == "records")
    {
        board.load_syms(o.syms);
        // The slow tick must overrun the period: 3328 T per block, so 1.5 periods.
        const int loops = static_cast<int>(period * 1.5 / 3328.0) + 1;
        RecordsSource src(board.sym("tick_done"), board.sym("tick"), board.sym("main"), std::min(loops, 60));
        return run(board, o, period, src);
    }
    usage();
    return 2;
}
