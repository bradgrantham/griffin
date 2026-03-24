/*
 * Griffin IO MCU Firmware — AT89S52
 *
 * Handles keyboard (PS/2), mouse (PS/2), and serial (UART) IO for the
 * Griffin 68000 computer.  Communicates with the 68000 CPU via a
 * memory-mapped register interface at 0xF80000, using a polled
 * IO_SELECT / DTACK handshake.
 *
 * Built with SDCC: sdcc -mmcs51 main.c
 */

#include <8052.h>
#include <stdint.h>

/* ----------------------------------------------------------------
 * Port / pin assignments (directly match board netlist)
 * ---------------------------------------------------------------- */

/* P0.0-P0.4: Address A1-A5 from 68000 (active during IO_SELECT) */
/* P0.5:      ~IO_IRQ output to GLUE CPLD */
#define IO_IRQ_PIN    P0_5

/* P1.5: ~IO_SELECT input from GLUE (active low) */
#define IO_SELECT_PIN P1_5

/* P1.6: ~IO_DTACK output to GLUE (active low) */
#define IO_DTACK_PIN  P1_6

/* P1.7: ~IO_IACK input (active low, dual-purpose with ISP SCK) */
#define IO_IACK_PIN   P1_7

/* P2.0-P2.7: Data bus D0-D7 (directly connected to 68000 data bus) */

/* P3.0: UART TX, P3.1: UART RX (hardware UART) */
/* P3.2 (INT0): MOUSE_CLK */
/* P3.3 (INT1): KBD_CLK */
/* P3.4: MOUSE_DATA */
#define MOUSE_DATA_PIN P3_4
/* P3.5: KBD_DATA */
#define KBD_DATA_PIN   P3_5
/* P3.6: R/~W from 68000 (active high = read) */
#define RW_PIN         P3_6

/* ----------------------------------------------------------------
 * Register offsets (address bits A1-A5 from P0, masked to low 5 bits)
 * The 68000 accesses odd byte addresses: offset = (addr - 0xF80000).
 * A0 selects UDS/LDS and is not routed to the MCU.
 * A1-A5 appear on P0.0-P0.4.
 * Offset 0x01 → A1=1,A2-A5=0 → P0 & 0x1F = 0x01, but since A0 is
 * not present, the MCU sees (offset >> 1) on P0.  So:
 *   offset 0x01 → P0 = 0x00  (A5:A1 = 00000)
 *   offset 0x03 → P0 = 0x01  (A5:A1 = 00001)
 *   offset 0x05 → P0 = 0x02  (A5:A1 = 00010)
 *   etc.
 * ---------------------------------------------------------------- */
#define REG_STATUS_CMD   0x00   /* read: STATUS, write: CMD */
#define REG_RXDATA_TX    0x01   /* read: RX_DATA, write: TX_DATA */
#define REG_IRQ          0x02   /* read: IRQ_STATUS, write: IRQ_ENABLE */
#define REG_VERSION_CFG  0x03   /* read: VERSION, write: CONFIG */
#define REG_KBD_CMD      0x04   /* write: PS/2 keyboard command */
#define REG_MOUSE_CMD    0x05   /* write: PS/2 mouse command */

#define FIRMWARE_VERSION 0x01

/* ----------------------------------------------------------------
 * Event queue
 * ---------------------------------------------------------------- */
#define QUEUE_SIZE 64
#define QUEUE_MASK (QUEUE_SIZE - 1)

static volatile uint8_t queue_buf[QUEUE_SIZE];
static volatile uint8_t queue_head;   /* next byte to read */
static volatile uint8_t queue_tail;   /* next byte to write */
static volatile uint8_t queue_overflow;

/* Event type bytes */
#define EVT_EMPTY      0x00
#define EVT_SERIAL_RX  0x01
#define EVT_KEYBOARD   0x02
#define EVT_MOUSE      0x03

/* Read state machine: tracks how many payload bytes remain for current event */
static uint8_t rx_payload_remaining;

static uint8_t queue_count(void) {
    return (queue_tail - queue_head) & QUEUE_MASK;
}

static uint8_t queue_is_empty(void) {
    return queue_head == queue_tail;
}

static void queue_put(uint8_t byte) {
    uint8_t next = (queue_tail + 1) & QUEUE_MASK;
    if (next == queue_head) {
        /* Queue full — drop newest, set overflow flag */
        queue_overflow = 1;
        return;
    }
    queue_buf[queue_tail] = byte;
    queue_tail = next;
}

static uint8_t queue_get(void) {
    uint8_t byte;
    if (queue_head == queue_tail)
        return EVT_EMPTY;
    byte = queue_buf[queue_head];
    queue_head = (queue_head + 1) & QUEUE_MASK;
    return byte;
}

/* Enqueue a serial RX event: type + 1 payload byte */
static void enqueue_serial_rx(uint8_t data) {
    if (queue_count() + 2 > QUEUE_SIZE - 1) {
        queue_overflow = 1;
        return;
    }
    queue_put(EVT_SERIAL_RX);
    queue_put(data);
}

/* ----------------------------------------------------------------
 * STATUS register
 * ---------------------------------------------------------------- */
#define STATUS_RX_NOTEMPTY  0x01
#define STATUS_TX_READY     0x02
#define STATUS_KBD_PRESENT  0x04
#define STATUS_MOUSE_PRESENT 0x08
#define STATUS_OVERFLOW     0x10

static uint8_t read_status(void) {
    uint8_t s = 0;
    if (!queue_is_empty())
        s |= STATUS_RX_NOTEMPTY;
    /* For Phase 1, TX is always ready (UART not yet initialized) */
    s |= STATUS_TX_READY;
    /* Keyboard/mouse presence detection is Phase 3/4 */
    if (queue_overflow)
        s |= STATUS_OVERFLOW;
    return s;
}

/* ----------------------------------------------------------------
 * RX_DATA read — type-then-payload with 0x00 sentinel
 *
 * The MCU tracks rx_payload_remaining.  When 0, the next read
 * returns the type byte of the next event (or 0x00 if empty).
 * After a type byte, subsequent reads return payload bytes.
 * ---------------------------------------------------------------- */
static uint8_t read_rx_data(void) {
    uint8_t type;
    if (rx_payload_remaining > 0) {
        rx_payload_remaining--;
        return queue_get();
    }
    /* No payload remaining — return next event type */
    if (queue_is_empty())
        return EVT_EMPTY;

    type = queue_get();
    switch (type) {
        case EVT_SERIAL_RX:  rx_payload_remaining = 1; break;
        case EVT_KEYBOARD:   rx_payload_remaining = 1; break;
        case EVT_MOUSE:      rx_payload_remaining = 3; break;
        default:             rx_payload_remaining = 0; break;
    }
    return type;
}

/* ----------------------------------------------------------------
 * IO_IRQ management
 * ---------------------------------------------------------------- */
static void update_io_irq(void) {
    /* Assert IO_IRQ (active low) when queue has data */
    IO_IRQ_PIN = queue_is_empty() ? 1 : 0;
}

/* ----------------------------------------------------------------
 * Bus cycle handler
 *
 * Called from main loop when IO_SELECT is detected.
 * Interrupts are already disabled by the caller.
 *
 * Protocol:
 *   1. Read address from P0[4:0]
 *   2. Read R/~W from P3.6
 *   3. For reads: drive data on P2; for writes: read data from P2
 *   4. Assert DTACK (P1.6 low)
 *   5. Wait for IO_SELECT to deassert (P1.5 high)
 *   6. Release DTACK (P1.6 high), release P2 (write 0xFF)
 * ---------------------------------------------------------------- */
static void handle_bus_cycle(void) {
    uint8_t reg_addr = P0 & 0x1F;
    uint8_t is_read = RW_PIN;
    uint8_t data;

    if (is_read) {
        /* 68000 is reading from us */
        switch (reg_addr) {
            case REG_STATUS_CMD:  data = read_status(); break;
            case REG_RXDATA_TX:   data = read_rx_data(); break;
            case REG_IRQ:         data = 0; break;  /* TODO: IRQ_STATUS */
            case REG_VERSION_CFG: data = FIRMWARE_VERSION; break;
            default:              data = 0xFF; break;
        }
        P2 = data;
        IO_DTACK_PIN = 0;          /* assert DTACK */
        while (!IO_SELECT_PIN);    /* wait for AS deassert */
        IO_DTACK_PIN = 1;          /* release DTACK */
        P2 = 0xFF;                 /* release data bus */
    } else {
        /* 68000 is writing to us */
        data = P2;
        IO_DTACK_PIN = 0;          /* assert DTACK */
        while (!IO_SELECT_PIN);    /* wait for AS deassert */
        IO_DTACK_PIN = 1;          /* release DTACK */

        switch (reg_addr) {
            case REG_STATUS_CMD:
                /* CMD register — clear overflow on any write */
                queue_overflow = 0;
                break;
            case REG_RXDATA_TX:
                /* TX_DATA — will be UART TX in Phase 2 */
                break;
            case REG_IRQ:
                /* IRQ_ENABLE — TODO */
                break;
            case REG_VERSION_CFG:
                /* CONFIG — TODO: baud rate */
                break;
            case REG_KBD_CMD:
                /* PS/2 keyboard command — TODO Phase 3 */
                break;
            case REG_MOUSE_CMD:
                /* PS/2 mouse command — TODO Phase 4 */
                break;
            default:
                break;
        }
    }
}

/* ----------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------- */
void main(void) {
    /* Initialize port directions:
     * P0: bits 0-4 input (address), bit 5 output (IO_IRQ)
     * P1: bit 5 input (IO_SELECT), bit 6 output (DTACK), bit 7 input (IACK)
     * P2: all input initially (data bus idle)
     * P3: bits 0 output (TX), 1 input (RX), 2-5 input (PS/2), 6 input (R/W)
     */
    P0 = 0xFF;             /* float P0 inputs, IO_IRQ deasserted (high) */
    IO_DTACK_PIN = 1;      /* DTACK deasserted (high) */
    P2 = 0xFF;             /* float data bus */

    /* Initialize queue */
    queue_head = 0;
    queue_tail = 0;
    queue_overflow = 0;
    rx_payload_remaining = 0;

    /* Disable all interrupts for Phase 1 */
    EA = 0;

    /* Main polling loop */
    for (;;) {
        /* Check IO_SELECT (active low) */
        if (!IO_SELECT_PIN) {
            EA = 0;            /* ensure interrupts disabled during bus cycle */
            handle_bus_cycle();
            update_io_irq();
            /* Re-enable interrupts after bus cycle (when ISRs are active) */
            /* EA = 1; */      /* uncomment in Phase 2+ */
        }

        /* TODO Phase 2+: PS/2 polling, UART handling, etc. */
    }
}
