#include <string>
#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include <cstring>

#include "../griffin.generated.h"


/**
 * Send one character via the GLUE hardware UART TX (115200 baud 8N1).
 * Polls UART_STATUS until not busy, then writes UART_TX_DATA.
 */
extern "C" void debug_serial_putchar(const char s);

asm(
    ".global debug_serial_putchar     \n"
    "debug_serial_putchar:            \n"
    "    move.b  7(%sp), %d0 \n"
    "    lea     .Lret_stub(%pc), %a5 \n"
    "    jmp     uart_putchar \n"
    ".Lret_stub:                   \n"
    "    rts                  \n"
);

/**
 * Bitbang one character at 9600 baud (8N1) through DEBUG_OUT.
 * Kept as a fallback for debugging the GLUE UART itself.
 */
extern "C" void debug_serial_putchar_bitbang(const char s);

asm(
    ".global debug_serial_putchar_bitbang \n"
    "debug_serial_putchar_bitbang:        \n"
    "    move.l  %d2, -(%sp) \n"
    "    move.b  11(%sp), %d0 \n"
    "    lea     .Lret_stub_bb(%pc), %a5 \n"
    "    jmp     early_putchar \n"
    ".Lret_stub_bb:                   \n"
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

static volatile uint8_t &uart_status = *reinterpret_cast<volatile uint8_t *>(Griffin::GLUE_UART_STATUS);
static volatile uint8_t &uart_rx_data = *reinterpret_cast<volatile uint8_t *>(Griffin::GLUE_UART_RX_DATA);

int main()
{
    debug_printf("Firmware Build: %s, GIT %s\n", build_date, build_provenance);
    debug_printf("Waiting for UART RX...\n");

    for (;;) {
        if (uart_status & Griffin::GLUE_UART_STATUS_RECEIVED_MASK) {
            uint8_t ch = uart_rx_data;
            debug_printf("received: %c (%d)\n", isprint(ch) ? ch : '.', ch);
        }
    }
}
