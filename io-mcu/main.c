/*
 * Griffin IO MCU Test Firmware — AT89S52
 *
 * Jig test: print "IO MCU\n" on UART at 115200, then toggle P1.4.
 *
 * Built with SDCC: sdcc -mmcs51 main.c
 */

#include <8052.h>
#include <stdint.h>

#define TOGGLE_PIN P1_4

extern const char build_date[];
extern const char build_provenance[];

static void uart_init(void) {
    /* Timer 1 Mode 2 (8-bit auto-reload) for baud rate generation.
     * 11.0592 MHz, SMOD=1: baud = 2 * fosc / (32 * 12 * (256 - TH1))
     * TH1 = 256 - (2 * 11059200) / (32 * 12 * 115200)
     *      = 256 - 22118400 / 44236800 = 256 - 0.5  ... that's wrong.
     *
     * Standard formula with SMOD=1:
     *   baud = (2^SMOD / 32) * (fosc / (12 * (256 - TH1)))
     *   115200 = (2/32) * (11059200 / (12 * (256 - TH1)))
     *   115200 = 11059200 / (192 * (256 - TH1))
     *   256 - TH1 = 11059200 / (192 * 115200) = 0.5  ... need SMOD=1
     *
     * With SMOD=0:
     *   115200 = (1/32) * (11059200 / (12 * (256 - TH1)))
     *   256 - TH1 = 11059200 / (384 * 115200) = 0.25  ... no good
     *
     * Actually for 115200 at 11.0592 MHz, use Timer 2 baud rate generator.
     * T2 baud = fosc / (32 * (65536 - RCAP2))
     * 115200 = 11059200 / (32 * (65536 - RCAP2))
     * 65536 - RCAP2 = 11059200 / (32 * 115200) = 3
     * RCAP2 = 65533 = 0xFFFD
     */
    SCON = 0x50;        /* Mode 1 (8-bit UART), REN=1 (receive enable) */

    /* Use Timer 2 as baud rate generator */
    T2CON = 0x30;       /* RCLK=1, TCLK=1: Timer 2 drives both RX and TX baud */
    RCAP2H = 0xFF;      /* Reload high byte */
    RCAP2L = 0xFD;      /* Reload low byte: 0xFFFD → divide by 3 → 115200 */
    TH2 = 0xFF;         /* Initial value */
    TL2 = 0xFD;
    TR2 = 1;            /* Start Timer 2 */
}

static void uart_putchar(uint8_t c) {
    SBUF = c;
    while (!TI);        /* Wait for transmit complete */
    TI = 0;             /* Clear transmit interrupt flag */
}

static void uart_puthex(uint8_t val) {
    uint8_t hi = val >> 4;
    uint8_t lo = val & 0x0F;
    uart_putchar(hi < 10 ? '0' + hi : 'A' - 10 + hi);
    uart_putchar(lo < 10 ? '0' + lo : 'A' - 10 + lo);
}

static void uart_puts(const char *s) {
    while (*s)
        uart_putchar(*s++);
}

static uint8_t uart_getchar(void) {
    while (!RI);        /* Wait for receive complete */
    RI = 0;             /* Clear receive interrupt flag */
    return SBUF;
}

void main(void) {
    uint8_t c;

    EA = 0;             /* Disable interrupts */

    uart_init();
    uart_puts("IO MCU Build: ");
    uart_puts(build_date);
    uart_puts(", GIT ");
    uart_puts(build_provenance);
    uart_putchar('\n');

    for (;;) {
        c = uart_getchar();
        uart_puts("character: ");
        if (c >= 0x20 && c < 0x7F)
            uart_putchar(c);
        else {
            uart_puts("0x");
            uart_puthex(c);
        }
        uart_putchar('\n');
    }
}
