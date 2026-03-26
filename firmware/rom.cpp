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

// IO_MCU event ring buffer (written by ISR in crt0.s, read by main)
constexpr size_t IO_EVT_QUEUE_SIZE = 256;  // must match crt0.s
extern volatile uint8_t io_evt_queue[IO_EVT_QUEUE_SIZE];
extern volatile uint32_t io_evt_head;
extern volatile uint32_t io_evt_tail;
extern volatile uint8_t io_evt_overflow;

// Timer tick handler — called from ISR context at IPL 5
void timer_tick()
{
    // TODO: increment tick counter, feed watchdog, etc.
}

}; // extern "C"

static volatile uint8_t &io_mcu_tx = *reinterpret_cast<volatile uint8_t *>(Griffin::IO_MCU_TX_DATA);

static void io_mcu_putchar(uint8_t ch)
{
    io_mcu_tx = ch;
}

// Pop one byte from the event ring buffer.  Returns false if empty.
// No interrupt masking needed — head is only modified here (single consumer).
static bool evt_pop(uint8_t *out)
{
    uint32_t h = io_evt_head;
    if(h == io_evt_tail)
    {
        return false;
    }
    *out = io_evt_queue[h];
    io_evt_head = (h + 1) & (IO_EVT_QUEUE_SIZE - 1);
    return true;
}

int main()
{
    debug_printf("Firmware Build: %s, GIT %s\n", build_date, build_provenance);
    debug_printf("IO_MCU console ready\n");

    uint8_t evt;
    for (;;)
    {
        if(!evt_pop(&evt))
        {
            continue;
        }

        switch(evt)
        {
            case Griffin::IO_MCU_EVT_UART_RX:
            {
                uint8_t ch;
                if(evt_pop(&ch))
                {
                    debug_serial_putchar(ch);
                    io_mcu_putchar(ch);
                }
                break;
            }

            case Griffin::IO_MCU_EVT_KBD:
            {
                uint8_t scancode;
                if(evt_pop(&scancode))
                {
                    debug_printf("[KBD: 0x%02X]\n", scancode);
                }
                break;
            }

            case Griffin::IO_MCU_EVT_MOUSE:
            {
                uint8_t data;
                if(evt_pop(&data))
                {
                    debug_printf("[MOUSE: 0x%02X]\n", data);
                }
                break;
            }

            case Griffin::IO_MCU_EVT_TIMER:
                // Timer tick already handled in ISR; nothing to do here
                break;

            default:
                debug_printf("[IO EVT: 0x%02X]\n", evt);
                break;
        }
    }
}
