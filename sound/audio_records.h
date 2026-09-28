// audio_records.h -- the AUDCMD record encodings of the Z80 sound processor,
// second form (sound/rom/rom2.s), shared by the harness and, in package B, by
// 68000 applications.  Bare metal: <cstdint> only, no allocation, no I/O.
//
// Two ganged 16-bit FIFO pairs, both written by ENGINE descriptors:
//   SAMPLES  (AUDIO_FIFO_W strobe)  {L[15:8], R[7:0]} two's complement pairs,
//                                   popped once per tick by the Z80
//   AUDCMD   (SPARE strobe)         the records below, read in the gap
// The Z80 reads a 74HC244 status port for both pairs' empty and half-full
// flags, so every 16-bit value is legal in either stream.
//
// AUDCMD records (the high byte is read first):
//   1aaa aaaa vvvv vvvv   SET        window[a] = v.  INC, AMP and PHASE are
//                                    patched into the live tick; MODE, PAN,
//                                    the LFSR select and the voice count take
//                                    effect at the next CONFIGURE.
//   00tt tttt tttt tttt   WAIT       t ticks; a WAIT n consumed at the
//                                    boundary of tick k lets ticks k..k+n-1
//                                    play; the next record is consumed at k+n.
//   0100 0000 cccc 00mp   CONFIGURE  c = voice count (1..14, 0 = 1),
//                                    p = PCM on, m = mono; then c words whose
//                                    low byte is that voice's wavetable page.
//                                    The tick is rebuilt from templates.
//   0100 0001 nnnn nnnn   LOAD       then: dest; n words of two bytes (high
//                                    first, 0 = 256); the 16-bit sum of them.
//   0100 0010 ---- ----   JUMP       then: entry.  Refused after a bad LOAD.
//   0100 0100 ---- ----   RESET      voices silent, WAIT expired, LFSR
//                                    seeded, the tick rebuilt.
//   0100 0101 ---- ----   DUMP       the window (128), lfsr (2), tick vector
//                                    (2) and status (1) bytes to the DEBUG port.
//
// RETIRED 2026-09-27: the Z80 sound processor gave way to the AUDIO chip
// (griffin.log).  The record constants below were griffin.yml's until then;
// they live here now so this record of the experiment still builds.

#pragma once

#include <cstdint>


namespace griffin::audio
{

inline constexpr uint16_t SET_FLAG       = 0x80;
inline constexpr uint16_t CTRL_FLAG      = 0x40;
inline constexpr uint8_t  REC_CONFIGURE  = 0x40;
inline constexpr uint8_t  REC_LOAD       = 0x41;
inline constexpr uint8_t  REC_JUMP       = 0x42;
inline constexpr uint8_t  REC_RESET      = 0x44;
inline constexpr uint8_t  REC_DUMP       = 0x45;
inline constexpr int      SET_WINDOW     = 128;
inline constexpr int      VOICE_BYTES    = 8;
inline constexpr uint16_t WAIT_MAX       = 0x3FFF;
inline constexpr int      MAX_VOICES     = 14;

inline constexpr uint8_t CFG_PCM   = 0x01;
inline constexpr uint8_t CFG_MONO  = 0x02;

// Status port bits (74HC244): the FIFO flags as the pins read them.
inline constexpr uint8_t ST_AUDCMD_NOT_EMPTY  = 0x01;
inline constexpr uint8_t ST_AUDCMD_HALF_FULL  = 0x02;
inline constexpr uint8_t ST_SAMPLES_NOT_EMPTY = 0x04;
inline constexpr uint8_t ST_SAMPLES_HALF_FULL = 0x08;

static_assert(SET_FLAG == 0x80 && CTRL_FLAG == 0x40);
static_assert(REC_LOAD == 0x41 && REC_JUMP == 0x42 && REC_RESET == 0x44 && REC_DUMP == 0x45);
static_assert(SET_WINDOW == 128 && VOICE_BYTES == 8 && WAIT_MAX == 0x3FFF);
static_assert((REC_DUMP & 0xC0) == CTRL_FLAG, "control records live in 0x40 .. 0x7F");

constexpr bool is_set(uint16_t word)
{
    return (word & 0x8000) != 0;
}

constexpr bool is_wait(uint16_t word)
{
    return (word & 0xC000) == 0;
}

constexpr bool is_ctrl(uint16_t word)
{
    return (word & 0xC000) == 0x4000;
}

constexpr uint8_t high_byte(uint16_t word)
{
    return static_cast<uint8_t>(word >> 8);
}

constexpr uint8_t low_byte(uint16_t word)
{
    return static_cast<uint8_t>(word & 0xFF);
}

constexpr uint16_t set_word(uint8_t addr, uint8_t value)
{
    return static_cast<uint16_t>(((SET_FLAG | (addr & 0x7F)) << 8) | value);
}

constexpr uint16_t wait_word(uint16_t ticks)
{
    return static_cast<uint16_t>(ticks & WAIT_MAX);
}

constexpr uint16_t ctrl_word(uint8_t type, uint8_t low)
{
    return static_cast<uint16_t>((type << 8) | low);
}

// A SAMPLES pair, deposited on the SAMPLES FIFO's own strobe.
constexpr uint16_t sample_word(int8_t left, int8_t right)
{
    return static_cast<uint16_t>((static_cast<uint8_t>(left) << 8) | static_cast<uint8_t>(right));
}

constexpr uint8_t configure_flags(int voices, bool pcm, bool mono)
{
    return static_cast<uint8_t>(((voices & 0x0F) << 4) | (pcm ? CFG_PCM : 0) | (mono ? CFG_MONO : 0));
}

constexpr uint16_t configure_header(int voices, bool pcm, bool mono)
{
    return ctrl_word(REC_CONFIGURE, configure_flags(voices, pcm, mono));
}

constexpr uint16_t configure_voice_word(uint8_t page)
{
    return page;
}

constexpr int configure_voices(uint8_t flags)
{
    const int v = (flags >> 4) & 0x0F;
    return v == 0 ? 1 : (v > MAX_VOICES ? MAX_VOICES : v);
}

constexpr uint16_t load_header(int words)
{
    return ctrl_word(REC_LOAD, static_cast<uint8_t>(words & 0xFF));
}

// LOAD payload: two bytes per word, the lower address in the high byte.
constexpr uint16_t load_word(uint8_t first, uint8_t second)
{
    return static_cast<uint16_t>((first << 8) | second);
}

constexpr uint16_t load_sum(const uint8_t *bytes, int n)
{
    uint16_t sum = 0;
    for (int i = 0; i + 1 < n; i += 2)
    {
        sum = static_cast<uint16_t>(sum + load_word(bytes[i], bytes[i + 1]));
    }
    if (n & 1)
    {
        sum = static_cast<uint16_t>(sum + load_word(bytes[n - 1], 0));
    }
    return sum;
}

constexpr uint16_t jump_header()
{
    return ctrl_word(REC_JUMP, 0);
}

constexpr uint16_t reset_word()
{
    return ctrl_word(REC_RESET, 0);
}

constexpr uint16_t dump_word()
{
    return ctrl_word(REC_DUMP, 0);
}

constexpr uint8_t voice_addr(int voice, uint8_t offset)
{
    return static_cast<uint8_t>((voice * VOICE_BYTES + offset) & (SET_WINDOW - 1));
}

// The control block of the SET window.
inline constexpr uint8_t CONTROL_BASE       = 0x78;
inline constexpr uint8_t LFSR_VOICE_SELECT  = CONTROL_BASE + 0;   // bits 3:0, effective at CONFIGURE
inline constexpr uint8_t VOICE_COUNT        = CONTROL_BASE + 1;   // stored; the count is CONFIGURE's

// DUMP writes this many bytes to the DEBUG port.
inline constexpr int DUMP_BYTES = SET_WINDOW + 2 + 2 + 1;

}   // namespace griffin::audio
