#include <string>
#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include <cstring>

#include "../griffin.generated.h"

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
extern "C" void debug_serial_putchar(const char s);

asm(
    ".global debug_serial_putchar     \n"
    "debug_serial_putchar:            \n"
    "    move.l  %d2, -(%sp) \n"
    "    move.b  11(%sp), %d0 \n"
    "    lea     .Lret_stub(%pc), %a5 \n"
    "    jmp     early_putchar \n"
    ".Lret_stub:                   \n"
    "    move.l  (%sp)+, %d2\n"
    "    rts                  \n"
);

extern "C" void panic(const char *s);

asm(
    ".global panic\n"
    "panic:\n"
    "    move.l 4(%sp), %a1\n"
    "    jmp monitor_panic\n"
);

extern "C" {

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

extern uint32_t memory_size;
extern const char *build_date;
extern const char *build_provenance;

};

int main()
{
    printf("Firmware Build: %s, GIT %s\n", build_date, build_provenance);
    // panic("Panic!\n");
}
