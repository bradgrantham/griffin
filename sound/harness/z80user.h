/* z80user.h -- the Griffin sound-processor board as z80emu sees it.
 *
 * Flat 64 KB: 0x0000-0x3FFF ROM (writes ignored), 0x8000-0x9FFF RAM mirrored
 * through 0xFFFF, 0x4000-0x7FFF open bus (reads 0xFF).  Ports are decoded on
 * A4..A2 as the 74HC138 does: 7 FIFO high byte, 6 FIFO low byte, 5 DAC left,
 * 4 DAC right, 3 DEBUG.  The port and instruction hooks call into harness.cpp
 * with the exact T-state (clock + elapsed_cycles).
 *
 * This header is included by z80emu.c (C) and by harness.cpp (C++).
 */

#ifndef __Z80USER_INCLUDED__
#define __Z80USER_INCLUDED__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct HarnessCtx
{
    uint8_t  mem[65536];
    uint64_t clock;         /* T-states at the start of the current Z80Emulate call */
} HarnessCtx;

/* Implemented in harness.cpp. */
uint8_t harness_in(HarnessCtx *ctx, uint8_t port, uint64_t t);
void    harness_out(HarnessCtx *ctx, uint8_t port, uint8_t value, uint64_t t);
void    harness_insn(HarnessCtx *ctx, int pc, int opcode, int instruction, int next_pc, uint64_t t_end);

static inline uint8_t hz_read(HarnessCtx *ctx, uint32_t address32)
{
    uint16_t address = (uint16_t)address32;
    if (address < 0x4000)
    {
        return ctx->mem[address];
    }
    if (address >= 0x8000)
    {
        return ctx->mem[0x8000 | (address & 0x1FFF)];
    }
    return 0xFF;
}

extern int hz_watch;   /* -1 or an address whose writes are reported (debugging) */
void harness_watch(HarnessCtx *ctx, uint16_t address, uint8_t byte);
void harness_effect(HarnessCtx *ctx, uint16_t address);   /* a write into the SET window or the WAIT counter */

static inline void hz_write(HarnessCtx *ctx, uint32_t address32, uint8_t byte)
{
    uint16_t address = (uint16_t)address32;
    if (hz_watch >= 0 && (address & 0x1FFF) == (hz_watch & 0x1FFF) && address >= 0x8000)
    {
        harness_watch(ctx, address, byte);
    }
    if (address >= 0x8000)
    {
        uint16_t a = 0x8000 | (address & 0x1FFF);
        ctx->mem[a] = byte;
        if (a < 0x8080 || a == 0x8084 || a == 0x8085)
        {
            harness_effect(ctx, a);
        }
    }
}

#define Z80_READ_BYTE(address32, x) \
{ \
    (x) = hz_read((HarnessCtx *)(context), (address32)); \
}

#define Z80_FETCH_BYTE(address32, x)    Z80_READ_BYTE((address32), (x))

#define Z80_READ_WORD(address32, x) \
{ \
    uint32_t hz_a = (address32); \
    (x) = hz_read((HarnessCtx *)(context), hz_a) \
        | (hz_read((HarnessCtx *)(context), hz_a + 1) << 8); \
}

#define Z80_READ_WORD_INTERRUPT(address32, x)   Z80_READ_WORD((address32), (x))
#define Z80_FETCH_WORD(address32, x)            Z80_READ_WORD((address32), (x))

#define Z80_WRITE_BYTE(address32, x) \
{ \
    hz_write((HarnessCtx *)(context), (address32), (uint8_t)(x)); \
}

#define Z80_WRITE_WORD(address32, x) \
{ \
    uint32_t hz_a = (address32); \
    hz_write((HarnessCtx *)(context), hz_a, (uint8_t)(x)); \
    hz_write((HarnessCtx *)(context), hz_a + 1, (uint8_t)((x) >> 8)); \
}

#define Z80_WRITE_WORD_INTERRUPT(address32, x)  Z80_WRITE_WORD((address32), (x))

#define Z80_INPUT_BYTE(port16, x) \
{ \
    (x) = harness_in((HarnessCtx *)(context), (uint8_t)(port16), \
                     ((HarnessCtx *)(context))->clock + (elapsed_cycles)); \
}

#define Z80_OUTPUT_BYTE(port16, x) \
{ \
    harness_out((HarnessCtx *)(context), (uint8_t)(port16), (uint8_t)(x), \
                ((HarnessCtx *)(context))->clock + (elapsed_cycles)); \
}

#define Z80_TRACE_START(current_pc) {}

#define Z80_TRACE_INSN(current_pc, current_opcode, current_instruction, next_pc, cycles) \
{ \
    harness_insn((HarnessCtx *)(context), (current_pc), (current_opcode), (current_instruction), \
                 (next_pc), ((HarnessCtx *)(context))->clock + (cycles)); \
}

#define Z80_INSTRUCTION_BREAKPOINT(next_pc) {}

#define Z80_PROCESS_CYCLES(cycles) {}

#ifdef __cplusplus
}
#endif

#endif
