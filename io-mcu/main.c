/*
 * Griffin IO MCU Firmware — AT89S52
 *
 * IO coprocessor for the Griffin 68000 computer.
 * Manages PS/2 keyboard, PS/2 mouse, UART, and systick timer.
 * Communicates with the 68000 via memory-mapped registers at 0xF80000.
 *
 * Built with SDCC: sdcc -mmcs51 --model-small --opt-code-size
 */

#include <8052.h>
#include <stdint.h>

extern const char build_date[];
extern const char build_provenance[];

/* ---- Pin assignments --------------------------------------------------- */

/* P0: address bus A1-A4 (directly from 68000 address lines), active low */
/* P0 directly from 68000 address lines and directly from active-low select */
#define nIO_IRQ_PIN     P0_5    /* Active low output to GLUE */

/* P1 */
#define TOGGLE_PIN      P1_4    /* Debug toggle */
#define nIO_SELECT_PIN  P1_5    /* Active low input from GLUE */
#define nIO_DTACK_PIN   P1_6   /* Active low output to GLUE */
/* P1.7 = ~IO_IACK / ISP SCK — not used in firmware */

/* P2: data bus D0-D7 (directly from 68000 data lines) */

/* P3 */
/* P3.0 = UART TXD (directly from 8051 UART) */
/* P3.1 = UART RXD (directly from 8051 UART) */
#define MOUSE_CLK_PIN   P3_2   /* INT0 — falling edge */
#define KBD_CLK_PIN     P3_3   /* INT1 — falling edge */
#define MOUSE_DATA_PIN  P3_4
#define KBD_DATA_PIN    P3_5
#define R_nW_PIN        P3_6   /* 68000 R/~W: 1=read, 0=write */

/* ---- Register map (address bits A1-A4 from P0) ------------------------- */

#define REG_RX_DATA     0x00   /* Read: dequeue event byte (0x00 = empty) */
#define REG_TX_DATA     0x00   /* Write: UART transmit byte */
#define REG_STATUS      0x01   /* Read: status flags */
#define REG_CONFIG      0x01   /* Write: configuration (timer rate, etc.) */

/* ---- Status register bits ---------------------------------------------- */

#define STATUS_QUEUE_NOTEMPTY  0x01  /* Event queue has data */
#define STATUS_TX_READY        0x02  /* UART TX buffer empty */
#define STATUS_KBD_PRESENT     0x04  /* Keyboard detected */
#define STATUS_MOUSE_PRESENT   0x08  /* Mouse detected */
#define STATUS_OVERFLOW        0x10  /* Queue overflow occurred (sticky) */

/* ---- Event types ------------------------------------------------------- */

#define EVT_EMPTY       0x00   /* Queue empty sentinel */
#define EVT_UART_RX     0x01   /* UART RX byte; 1 payload byte follows */
#define EVT_KBD         0x02   /* PS/2 keyboard scancode; 1 payload byte */
#define EVT_MOUSE       0x03   /* PS/2 mouse byte; 1 payload byte */
#define EVT_TIMER       0x04   /* Systick timer tick; no payload */

/* ---- Timer rate codes (written to CONFIG register) --------------------- */
/*
 * Timer 0 Mode 1 (16-bit), fosc/12 = 921600 Hz.
 * On overflow, ISR reloads the count and enqueues EVT_TIMER.
 *
 * Rate code  Hz     Count   Reload (65536 - count)
 * 0x00       off    —       —
 * 0x01       50     18432   0xB800
 * 0x02       60     15360   0xC400
 * 0x03       100    9216    0xDC00
 * 0x04       200    4608    0xEE00
 */

#define TIMER_RATE_OFF  0x00
#define TIMER_RATE_50   0x01
#define TIMER_RATE_60   0x02
#define TIMER_RATE_100  0x03
#define TIMER_RATE_200  0x04

/* ---- Event queue ------------------------------------------------------- */

#define QUEUE_SIZE 64

static volatile uint8_t queue_buf[QUEUE_SIZE];
static volatile uint8_t queue_head;  /* Next position to write */
static volatile uint8_t queue_tail;  /* Next position to read */
static volatile uint8_t status_flags;

static void queue_init(void)
{
    queue_head = 0;
    queue_tail = 0;
}

static uint8_t queue_empty(void)
{
    return queue_head == queue_tail;
}

static uint8_t queue_full(void)
{
    return ((queue_head + 1) & (QUEUE_SIZE - 1)) == queue_tail;
}

/* Enqueue a single byte. Called from ISRs with EA=0 or from contexts
 * where we know interrupts won't interfere. */
static void queue_put(uint8_t b)
{
    if (queue_full()) {
        status_flags |= STATUS_OVERFLOW;
        return;
    }
    queue_buf[queue_head] = b;
    queue_head = (queue_head + 1) & (QUEUE_SIZE - 1);
}

/* Dequeue a single byte. Returns EVT_EMPTY if queue is empty. */
static uint8_t queue_get(void)
{
    uint8_t b;
    if (queue_empty())
        return EVT_EMPTY;
    b = queue_buf[queue_tail];
    queue_tail = (queue_tail + 1) & (QUEUE_SIZE - 1);
    return b;
}

/* Enqueue a typed event with one payload byte. */
static void queue_put_event(uint8_t type, uint8_t payload)
{
    /* Need room for 2 bytes; check before either write so we don't
     * enqueue a type without its payload. */
    uint8_t free = (queue_tail - queue_head - 1) & (QUEUE_SIZE - 1);
    if (free < 2) {
        status_flags |= STATUS_OVERFLOW;
        return;
    }
    queue_buf[queue_head] = type;
    queue_head = (queue_head + 1) & (QUEUE_SIZE - 1);
    queue_buf[queue_head] = payload;
    queue_head = (queue_head + 1) & (QUEUE_SIZE - 1);
}

/* ---- Update IO_IRQ from queue state ------------------------------------ */

static void irq_update(void)
{
    /* IO_IRQ is active low: 0 = asserted (interrupt 68000) */
    nIO_IRQ_PIN = queue_empty();
}

/* ---- UART -------------------------------------------------------------- */

static void uart_init(void)
{
    /*
     * Timer 2 baud rate generator for 115200 at 11.0592 MHz.
     * T2 baud = fosc / (32 * (65536 - RCAP2))
     * 115200 = 11059200 / (32 * 3)  →  RCAP2 = 0xFFFD
     */
    SCON = 0x50;        /* Mode 1 (8-bit UART), REN=1 */
    T2CON = 0x30;       /* RCLK=1, TCLK=1: Timer 2 baud rate gen */
    RCAP2H = 0xFF;
    RCAP2L = 0xFD;      /* 0xFFFD → divide by 3 → 115200 */
    TH2 = 0xFF;
    TL2 = 0xFD;
    TR2 = 1;            /* Start Timer 2 */
}

static void uart_putchar(uint8_t c)
{
    SBUF = c;
    while (!TI);
    TI = 0;
}

static void uart_puts(const char *s)
{
    while (*s)
        uart_putchar(*s++);
}

/* Serial ISR: handles both RX and TX interrupts.
 * Uses register bank 3. */
void serial_isr(void) __interrupt(4) __using(3)
{
    if (RI) {
        RI = 0;
        queue_put_event(EVT_UART_RX, SBUF);
        irq_update();
    }
    if (TI) {
        TI = 0;
        /* TX complete — nothing to do, polled TX for now */
    }
}

/* ---- Systick timer (Timer 0, Mode 1, 16-bit) -------------------------- */

static uint8_t timer0_reload_hi;
static uint8_t timer0_reload_lo;
static uint8_t timer0_enabled;

static void timer0_start(uint8_t hi, uint8_t lo)
{
    TR0 = 0;
    TMOD = (TMOD & 0xF0) | 0x01;  /* Timer 0 Mode 1 (16-bit) */
    TH0 = hi;
    TL0 = lo;
    timer0_reload_hi = hi;
    timer0_reload_lo = lo;
    timer0_enabled = 1;
    TF0 = 0;
    ET0 = 1;            /* Enable Timer 0 interrupt */
    TR0 = 1;            /* Start Timer 0 */
}

static void timer0_stop(void)
{
    TR0 = 0;
    ET0 = 0;
    timer0_enabled = 0;
}

static void timer0_set_rate(uint8_t rate)
{
    switch (rate) {
    case TIMER_RATE_50:  timer0_start(0xB8, 0x00); break;
    case TIMER_RATE_60:  timer0_start(0xC4, 0x00); break;
    case TIMER_RATE_100: timer0_start(0xDC, 0x00); break;
    case TIMER_RATE_200: timer0_start(0xEE, 0x00); break;
    default:             timer0_stop();             break;
    }
}

/* Timer 0 overflow ISR.  No dedicated register bank needed — it only
 * touches timer0_reload vars and the queue (which is safe since bus
 * cycles run with EA=0, and other ISRs don't read the queue). */
void timer0_isr(void) __interrupt(1) __using(0)
{
    /* Reload for next tick */
    TH0 = timer0_reload_hi;
    TL0 = timer0_reload_lo;
    queue_put(EVT_TIMER);
    irq_update();
}

/* ---- PS/2 receive state machines --------------------------------------- */

/* PS/2 protocol: 11 bits on falling clock edges:
 *   start(0), d0, d1, d2, d3, d4, d5, d6, d7, parity(odd), stop(1)
 * We sample the data pin on each falling clock edge (via INT0/INT1). */

static volatile uint8_t kbd_bitcount;
static volatile uint8_t kbd_shift;
static volatile uint8_t kbd_parity;

static volatile uint8_t mouse_bitcount;
static volatile uint8_t mouse_shift;
static volatile uint8_t mouse_parity;

static void ps2_init(void)
{
    kbd_bitcount = 0;
    mouse_bitcount = 0;

    /* INT0 (P3.2 = MOUSE_CLK): falling edge triggered */
    IT0 = 1;
    EX0 = 1;

    /* INT1 (P3.3 = KBD_CLK): falling edge triggered */
    IT1 = 1;
    EX1 = 1;

    /* Set interrupt priorities: PS/2 high, serial/timer low */
    PX0 = 1;   /* INT0 high priority */
    PX1 = 1;   /* INT1 high priority */
    PS = 0;    /* Serial low priority */
    PT0 = 0;   /* Timer 0 low priority */
}

/* INT1 ISR: PS/2 keyboard clock — falling edge.
 * Uses register bank 2. */
void kbd_clk_isr(void) __interrupt(2) __using(2)
{
    uint8_t bit = KBD_DATA_PIN;

    if (kbd_bitcount == 0) {
        /* Start bit — should be 0 */
        if (bit == 0) {
            kbd_shift = 0;
            kbd_parity = 0;
            kbd_bitcount = 1;
        }
    } else if (kbd_bitcount <= 8) {
        /* Data bits, LSB first */
        kbd_shift >>= 1;
        if (bit)
            kbd_shift |= 0x80;
        kbd_parity += bit;
        kbd_bitcount++;
    } else if (kbd_bitcount == 9) {
        /* Parity bit (odd parity: data bits + parity bit should be odd) */
        kbd_parity += bit;
        kbd_bitcount++;
    } else {
        /* Stop bit — should be 1 */
        if (bit && (kbd_parity & 1)) {
            /* Valid frame: odd parity check passed and stop bit is 1 */
            queue_put_event(EVT_KBD, kbd_shift);
            irq_update();
        }
        /* else: framing/parity error — silently drop */
        kbd_bitcount = 0;
    }
}

/* INT0 ISR: PS/2 mouse clock — falling edge.
 * Uses register bank 1. */
void mouse_clk_isr(void) __interrupt(0) __using(1)
{
    uint8_t bit = MOUSE_DATA_PIN;

    if (mouse_bitcount == 0) {
        if (bit == 0) {
            mouse_shift = 0;
            mouse_parity = 0;
            mouse_bitcount = 1;
        }
    } else if (mouse_bitcount <= 8) {
        mouse_shift >>= 1;
        if (bit)
            mouse_shift |= 0x80;
        mouse_parity += bit;
        mouse_bitcount++;
    } else if (mouse_bitcount == 9) {
        mouse_parity += bit;
        mouse_bitcount++;
    } else {
        if (bit && (mouse_parity & 1)) {
            queue_put_event(EVT_MOUSE, mouse_shift);
            irq_update();
        }
        mouse_bitcount = 0;
    }
}

/* ---- Bus interface ----------------------------------------------------- */

static void bus_init(void)
{
    /* P0 lower bits as input (address), P0.5 as output (IO_IRQ) */
    P0 = 0x1F;         /* Write 1s to address bits for input mode */
    nIO_IRQ_PIN = 1;   /* Deassert IO_IRQ (active low: 1 = no interrupt) */

    /* P1.5 (IO_SELECT) as input, P1.6 (IO_DTACK) as output */
    nIO_SELECT_PIN = 1; /* Write 1 for input mode */
    nIO_DTACK_PIN = 1;  /* Deassert DTACK (active low: 1 = not acknowledged) */

    /* P2 data bus: write 0xFF for input mode (will be driven as needed) */
    P2 = 0xFF;

    /* P3.6 (R/W) as input */
    R_nW_PIN = 1;       /* Write 1 for input mode */
}

static void bus_process(void)
{
    uint8_t address;
    uint8_t data;

    /* Gather address from A1-A4 on P0 low nibble.
     * The 68000 address lines A1-A4 map to P0.0-P0.3.
     * (A0 is not connected — byte-wide peripheral on odd addresses.) */
    address = P0 & 0x0F;

    if (R_nW_PIN) {
        /* 68000 is reading: MCU provides data */
        switch (address) {
        case REG_RX_DATA:
            data = queue_get();
            break;
        case REG_STATUS:
            data = status_flags;
            /* Live bits — set from current state */
            if (!queue_empty())
                data |= STATUS_QUEUE_NOTEMPTY;
            if (TI || !(SCON & 0x02))
                data |= STATUS_TX_READY;
            break;
        default:
            data = 0xFF;
            break;
        }

        /* Drive data onto P2 */
        P2 = data;

        /* Assert DTACK (active low) */
        nIO_DTACK_PIN = 0;

        /* Wait for 68000 to deassert IO_SELECT (active low: wait for high) */
        while (!nIO_SELECT_PIN);

        /* Release bus and deassert DTACK */
        P2 = 0xFF;
        nIO_DTACK_PIN = 1;

        /* Update IRQ — queue_get may have emptied the queue */
        irq_update();
    } else {
        /* 68000 is writing: MCU reads data */

        /* Assert DTACK to tell 68000 we're ready to latch */
        nIO_DTACK_PIN = 0;

        /* Read data from P2 */
        data = P2;

        /* Wait for 68000 to deassert IO_SELECT */
        while (!nIO_SELECT_PIN);

        /* Deassert DTACK */
        nIO_DTACK_PIN = 1;

        /* Handle write */
        switch (address) {
        case REG_TX_DATA:
            uart_putchar(data);
            break;
        case REG_CONFIG:
            timer0_set_rate(data);
            break;
        default:
            break;
        }
    }
}

/* ---- Main -------------------------------------------------------------- */

void main(void)
{
    EA = 0;

    uart_init();
    uart_puts("IO MCU Build: ");
    uart_puts(build_date);
    uart_puts(", GIT ");
    uart_puts(build_provenance);
    uart_putchar('\n');

    queue_init();
    bus_init();
    ps2_init();

    /* Enable serial interrupt (UART RX) */
    ES = 1;

    EA = 1;

    for (;;) {
        if (!nIO_SELECT_PIN) {
            EA = 0;
            bus_process();
            EA = 1;
        }
    }
}
