#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include <cstring>

#include "griffin.h"

static constexpr uint32_t SYSCLK = 12'000'000;

/**
 * Bitbang one character at 9600 baud (8N1) through a byte-wide
 * memory-mapped I/O location on a 68000.
 *
 * Protocol: idle=high(0x01), start bit=low(0x00), 8 data bits LSB first,
 *           stop bit=high(0x01).
 *
 * @param ch       Character to transmit
 */
extern "C" {

void debug_serial_putchar(uint8_t ch)
{
    using namespace Griffin;
    volatile uint8_t* debug_out = (uint8_t*) GLUE_DEBUG_OUT;

    static constexpr uint32_t CYCLES_PER_BIT = SYSCLK / 9600;
    static constexpr uint32_t DBRA_CYCLES = 10;
    static constexpr uint32_t DELAY_LOOP_COUNT = (CYCLES_PER_BIT - 20) / DBRA_CYCLES; // subtract overhead
    // We build a 10-bit frame: start bit (0) + 8 data bits + stop bit (1)
    // Shift into a 16-bit word so we can rotate through all bits.
    // Bit 0 = start bit = 0
    // Bits 1-8 = data LSB first
    // Bit 9 = stop bit = 1
    uint16_t frame = (static_cast<uint16_t>(ch) << 1) | (1u << 9);
    // bit 0 is already 0 (start bit)

    asm volatile (
        // a0 = I/O address
        // d0 = frame data (10 bits to send, LSB first)
        // d1 = bit counter (10 bits: start + 8 data + stop)
        // d2 = delay loop counter

        "    move.w   #9, %%d1          \n"  // 10 bits (0..9)

        ".Lbit_loop%=:                  \n"
        // Extract LSB into carry, shift frame right
        "    lsr.w    #1, %[frame]          \n"  // LSB -> X/C flag
        "    bcs.s    .Lsend_one%=      \n"  // if carry set, send 1

        // Send a 0 bit
        "    move.b   #0x00, (%[io])    \n"  // clear output
        "    bra.s    .Ldelay%=         \n"

        ".Lsend_one%=:                  \n"
        "    move.b   #0x01, (%[io])    \n"  // set output

        ".Ldelay%=:                     \n"
        "    move.w   %[dly], %%d2      \n"  // load delay count
        ".Ldelay_loop%=:               \n"
        "    dbra     %%d2, .Ldelay_loop%= \n" // 10 cycles/iteration

        "    dbra     %%d1, .Lbit_loop%=  \n" // next bit

        // Ensure line returns to idle high after stop bit
        "    move.b   #0x01, (%[io])    \n"

        : [frame] "+d" (frame)          // input/output: frame gets destroyed
        : [io]  "a" (debug_out),
          [dly]  "i" (DELAY_LOOP_COUNT)
        : "d1", "d2", "cc", "memory"
    );
}

void debug_printf(const char *fmt, ...)
{
    va_list args;
    char dummy[512];

    va_start(args, fmt);
    vsprintf(dummy, fmt, args);
    va_end(args);

    for(const char* s = dummy; *s; s++)
    {
        debug_serial_putchar(*s);
    }
}

void initialize_debug_out(void)
{
    using namespace Griffin;
    volatile uint8_t* debug_out = (uint8_t*) GLUE_DEBUG_OUT;
    *debug_out = 0x01;
}

};


int main()
{
    initialize_debug_out();
    printf("Hello There!\n");
    volatile uint16_t* p = (uint16_t*)0x400;
    *p = 0xAA55;
    *p = 0x55AA;
    printf("*p = %04X\n", *p);
}
