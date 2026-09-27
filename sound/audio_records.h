// audio_records.h — the 16-bit records ENGINE deposits into the sound FIFO
// for the Z80 sound processor (griffin.yml AUDIO; wave-table-design.md §2).
//
// The FIFO is one ganged 16-bit pair; the Z80 reads the HIGH byte first (port
// 0xFF, which reads 0xFF when the FIFO is empty) and the low byte second, so
// every record type is decided by the high byte alone and every field is a
// byte the Z80 can use without shifting:
//
//   1aaa aaaa vvvv vvvv   SET      RAM[window + a] = v (a = 0x7F reserved: the
//                                  high byte 0xFF is the empty sentinel)
//   00tt tttt tttt tttt   WAIT     t ticks; a WAIT n consumed at the boundary
//                                  of tick k lets ticks k .. k+n-1 play
//   0100 0000 rnnn nnnn   SAMPLES  n {L, R} words follow (n <= 127), copied
//                                  into the PCM ring; r = play each pair twice
//   0100 0001 nnnn nnnn   LOAD     word: dest; n words each carrying ONE byte in
//                                  its low half (n = 0 means 256); word: the
//                                  16-bit sum of those payload words
//   0100 0010 ---- ----   JUMP     word: entry (refused if the last LOAD's sum
//                                  failed)
//   0100 0011 vvvv vvvv   VOICE    v; two words follow: table base, table
//                                  length (a power of two); voice v becomes a
//                                  wavetable voice
//   0100 0100 ---- ----   RESET    ROM synth: voices silent, ring empty
//
// THE SENTINEL RULE: the Z80 reads the high byte first and 0xFF there means
// "empty" (the 7200 pair ignores the read, the bus pull-ups give 0xFF), and
// nothing else is read until a high byte is not 0xFF.  So NO WORD IN THE
// STREAM MAY HAVE A HIGH BYTE OF 0xFF, payloads included: SET address 0x7F is
// reserved, a PCM left sample of -1 is sent as -2 (sample_word does it), LOAD
// carries one byte per word, and addresses are below 0xFF00 by construction.
//
// SET and WAIT keep mixer/mixer.h's encoding (WAIT lost bit 14 to the control
// records), so psg_driver and Mixer stay valid consumers and producers.
//
// The constants come from griffin.yml through griffin.generated.h; this header
// gives them shape.  Bare-metal: <cstdint> only, shared by the harness and, in
// package B, apps/smurf.

#pragma once

#include <cstdint>

#include "../griffin.generated.h"

namespace griffin::audio
{

inline constexpr uint16_t SET_FLAG      = static_cast<uint16_t>(Griffin::AUDIO_REC_SET_FLAG);   // 0x80 in the high byte
inline constexpr uint16_t CTRL_FLAG     = 0x40;                                                  // 0100 0000 in the high byte
inline constexpr uint8_t  REC_SAMPLES   = static_cast<uint8_t>(Griffin::AUDIO_REC_SAMPLES);
inline constexpr uint8_t  REC_LOAD      = static_cast<uint8_t>(Griffin::AUDIO_REC_LOAD);
inline constexpr uint8_t  REC_JUMP      = static_cast<uint8_t>(Griffin::AUDIO_REC_JUMP);
inline constexpr uint8_t  REC_VOICE     = static_cast<uint8_t>(Griffin::AUDIO_REC_VOICE);
inline constexpr uint8_t  REC_RESET     = static_cast<uint8_t>(Griffin::AUDIO_REC_RESET);
inline constexpr uint8_t  EMPTY         = static_cast<uint8_t>(Griffin::AUDIO_REC_EMPTY);        // the high byte of an empty read
inline constexpr uint8_t  SET_ADDR_RESERVED = static_cast<uint8_t>(Griffin::AUDIO_SET_ADDR_RESERVED);
inline constexpr int      SET_WINDOW    = static_cast<int>(Griffin::AUDIO_SET_WINDOW);
inline constexpr int      VOICE_BYTES   = static_cast<int>(Griffin::AUDIO_VOICE_BYTES);
inline constexpr int      SAMPLES_MAX   = static_cast<int>(Griffin::AUDIO_SAMPLES_MAX);
inline constexpr uint16_t WAIT_MAX      = static_cast<uint16_t>(Griffin::AUDIO_WAIT_MAX);
inline constexpr uint8_t  SAMPLES_REPEAT = 0x80;                                                 // the r bit of SAMPLES' low byte

static_assert(SET_FLAG == 0x80 && CTRL_FLAG == 0x40);
static_assert(REC_SAMPLES == 0x40 && REC_LOAD == 0x41 && REC_JUMP == 0x42 && REC_VOICE == 0x43 && REC_RESET == 0x44);
static_assert(EMPTY == 0xFF && SET_ADDR_RESERVED == 0x7F, "0xFF as a high byte can only mean empty");
static_assert(SET_WINDOW == 128 && VOICE_BYTES == 8 && SAMPLES_MAX == 127 && WAIT_MAX == 0x3FFF);
static_assert((static_cast<uint16_t>(SET_FLAG | SET_ADDR_RESERVED) & 0xFF) == EMPTY);
static_assert(REC_RESET < 0x80 && (REC_RESET & 0xC0) == CTRL_FLAG, "control records live in 0x40 .. 0x7F");

// Classification of a record by its high byte.
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

// Encoders.
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

// SAMPLES header: n pairs follow, each {L, R} as one word, L in the high byte.
constexpr uint16_t samples_header(int pairs, bool repeat = false)
{
    return ctrl_word(REC_SAMPLES, static_cast<uint8_t>((pairs & SAMPLES_MAX) | (repeat ? SAMPLES_REPEAT : 0)));
}

constexpr uint16_t sample_word(int8_t left, int8_t right)
{
    if (left == -1)
    {
        left = -2;      // the sentinel rule: no 0xFF high byte
    }
    return static_cast<uint16_t>((static_cast<uint8_t>(left) << 8) | static_cast<uint8_t>(right));
}

// True when a word may be deposited at all.
constexpr bool is_depositable(uint16_t word)
{
    return high_byte(word) != EMPTY;
}

// LOAD header: followed by the dest word, n payload words (one byte each,
// load_word), and the sum word.  n = 256 is sent as 0.
constexpr uint16_t load_header(int bytes)
{
    return ctrl_word(REC_LOAD, static_cast<uint8_t>(bytes & 0xFF));
}

constexpr uint16_t load_word(uint8_t byte)
{
    return byte;
}

// LOAD's sum: the 16-bit wrapping sum of the n payload words.
constexpr uint16_t load_sum(const uint8_t *bytes, int n)
{
    uint16_t sum = 0;
    for (int i = 0; i < n; ++i)
    {
        sum = static_cast<uint16_t>(sum + load_word(bytes[i]));
    }
    return sum;
}

// JUMP header: followed by the entry address word.
constexpr uint16_t jump_header()
{
    return ctrl_word(REC_JUMP, 0);
}

// VOICE header: followed by the table base word and the table length word.
constexpr uint16_t voice_header(int voice)
{
    return ctrl_word(REC_VOICE, static_cast<uint8_t>(voice));
}

constexpr uint16_t reset_word()
{
    return ctrl_word(REC_RESET, 0);
}

constexpr uint8_t voice_addr(int voice, uint8_t offset)
{
    return static_cast<uint8_t>((voice * VOICE_BYTES + offset) & (SET_WINDOW - 1));
}

// The SET window's control block (0x78 .. 0x7E; 0x7F reserved).  0x78 is
// mixer.h's LFSR_VOICE_SELECT; the rest are the ROM synth's.
inline constexpr uint8_t CONTROL_BASE       = 0x78;
inline constexpr uint8_t LFSR_VOICE_SELECT  = CONTROL_BASE + 0;   // bits 3:0: the voice whose carry shifts the LFSR
inline constexpr uint8_t VOICE_COUNT        = CONTROL_BASE + 1;   // voices played per tick, 1 .. 14 (sound/rom/rom.s)
static_assert(VOICE_COUNT < SET_ADDR_RESERVED);

}   // namespace griffin::audio
