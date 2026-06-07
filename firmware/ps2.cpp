#include "ps2.h"
#include "../griffin.generated.refs.h"

using namespace Griffin::reg;

struct ps2_state_t
{
    uint8_t  rx_queue[Griffin::PS2_RX_QUEUE_SIZE];
    uint32_t rx_head;
    uint32_t rx_tail;

    uint8_t  err_flags;
    uint16_t err_data;

    // TX completion handshake.  tx_busy is set by ps2_send_byte before it
    // triggers the frame and cleared by the ISR on TX_DONE; tx_ack holds
    // the sampled device ACK (0 = acknowledged).
    volatile bool tx_busy;
    volatile bool tx_ack_failed;
};

[[gnu::section("monitor_data")]] volatile ps2_state_t ps2;

uint16_t ps2_get_err_data(void)
{
    return ps2.err_data;
}

uint8_t ps2_get_err_flags(void)
{
    auto t = ps2.err_flags;
    ps2.err_flags = 0;
    return t;
}

// Called by crt0.s, before interrupts
extern "C" {

void ps2_init()
{
    ps2.rx_head = 0;
    ps2.rx_tail = 0;
    ps2.err_flags = 0;
    ps2.err_data = 0;
    ps2.tx_busy = 0;
    ps2.tx_ack_failed = 0;
    GLUE_PS2_CTRL = 0;
    // Clear any latched RX/TX flags from power-up.
    GLUE_PS2_CLEAR = Griffin::GLUE_PS2_CLEAR_RX_READY_MASK
                   | Griffin::GLUE_PS2_CLEAR_TX_DONE_MASK;
}

/* ---- IRQ mask helpers ---------------------------------------------- */
/* On bare 68000, move-from-SR is unprivileged.  On 68010+ this is
 * privileged and only works in supervisor mode (which is fine for bare
 * metal but worth noting). */
static inline uint16_t irq_save(void)
{
    uint16_t sr;
    __asm__ volatile (
        "move.w %%sr,%0\n\t"
        "ori.w  #0x0700,%%sr"
        : "=d"(sr) :: "memory"
    );
    return sr;
}

static inline void irq_restore(uint16_t sr)
{
    __asm__ volatile ("move.w %0,%%sr" :: "d"(sr) : "memory");
}

/* ---- Odd-parity helper --------------------------------------------- */
/* Returns the odd-parity bit for x: 0 if x already has an odd number of
 * 1s, 1 otherwise (so data+parity is always odd).  Used both to generate
 * the TX parity bit and to validate a received frame's parity.
 * XOR-fold beats a loop on 68000. */
static inline uint8_t odd_parity_bit(uint8_t x)
{
    x ^= x >> 4;
    x ^= x >> 2;
    x ^= x >> 1;
    return (x & 1u) ^ 1u;
}

/* ====================================================================
 * ps2_isr — PS/2 frame IRQ (GLUE level 4)
 *
 * GLUE assembles whole frames now, so this fires at most once per byte
 * (RX_READY) and once per TX completion (TX_DONE), not per bit.  RX:
 * read the byte from PS2_RX_DATA, validate parity (from RX_PARITY) and
 * framing (RX_FRAME_ERR), enqueue.  TX: latch the ACK and clear tx_busy.
 * Both flags are acknowledged write-1-to-clear via PS2_CLEAR.
 * ==================================================================== */
void ps2_isr(void)
{
    uint8_t status = GLUE_PS2_STATUS;

    /* ---- TX completion ------------------------------------------- */
    if (status & Griffin::GLUE_PS2_STATUS_TX_DONE_MASK)
    {
        ps2.tx_ack_failed = (status & Griffin::GLUE_PS2_STATUS_TX_ACK_MASK) ? 1 : 0;
        ps2.tx_busy = 0;
        GLUE_PS2_CLEAR = Griffin::GLUE_PS2_CLEAR_TX_DONE_MASK;
    }

    /* ---- RX byte ------------------------------------------------- */
    if (status & Griffin::GLUE_PS2_STATUS_RX_READY_MASK)
    {
        uint8_t byte = GLUE_PS2_RX_DATA;
        uint8_t rx_parity =
            (status >> Griffin::GLUE_PS2_STATUS_RX_PARITY_SHIFT) & 1u;

        /* Ack first so a fast follow-on frame can re-arm immediately. */
        GLUE_PS2_CLEAR = Griffin::GLUE_PS2_CLEAR_RX_READY_MASK;

        if (status & Griffin::GLUE_PS2_STATUS_RX_FRAME_ERR_MASK)
        {
            ps2.err_data = byte;
            ps2.err_flags |= PS2_ERROR_FRAMING;
            return;
        }

        /* Odd parity: the received parity bit must match what we'd
         * generate for this data byte. */
        if (rx_parity != odd_parity_bit(byte))
        {
            ps2.err_data = byte;
            ps2.err_flags |= PS2_ERROR_PARITY;
            return;
        }

        /* Enqueue.  ISR is sole writer of tail; mainline is sole writer
         * of head, so no masking of the indices is needed. */
        uint32_t next = (ps2.rx_tail + 1u) & (Griffin::PS2_RX_QUEUE_SIZE - 1u);
        if (next == ps2.rx_head)
        {
            ps2.err_flags |= PS2_ERROR_OVERRUN;
            return;
        }
        ps2.rx_queue[ps2.rx_tail] = byte;
        ps2.rx_tail = next;
    }
}

/* ====================================================================
 * ps2_send_byte — transmit one byte host->device.
 *
 *   1. Pull CLK low (inhibit) for >=100 us.
 *   2. Write the byte to PS2_TX_DATA, with the firmware-computed odd
 *      parity carried in address bit 1 (PS2_TX_DATA_PARITY).  The write
 *      itself starts the frame: GLUE presents the start bit, releases
 *      CLK, and shifts the rest out on the device clock.
 *   3. Release the CLK inhibit and wait for the TX_DONE IRQ.
 * ==================================================================== */
void ps2_send_byte(uint8_t b)
{
    /* Wait for any previous send to finish. */
    while (ps2.tx_busy)
        ;

    ps2.tx_busy = 1;

    /* Pull CLK low (request-to-send / inhibit).  Mask IRQs only across
     * the register touch; the inhibit hold is a one-sided minimum so a
     * stray IRQ stretching it is harmless. */
    uint16_t saved_sr = irq_save();
    GLUE_PS2_CTRL = Griffin::GLUE_PS2_CTRL_CLK_MASK;
    irq_restore(saved_sr);

    /* Hold >=100 us.  At SYSCLK=14 MHz with ROM wait states this loop
     * runs ~16 cycles/iter; 250 iters ≈ 285 us.  Inline asm keeps the
     * compiler from retiming or deleting the delay. */
    __asm__ volatile (
        "    move.w  #250,%%d0   \n"
        "1:  dbra    %%d0,1b     \n"
        ::: "d0", "cc"
    );

    /* Trigger TX.  Parity in address bit 1; the write starts the frame
     * and the engine releases CLK (overriding PS2_CTRL.CLK while busy). */
    uint8_t parity = odd_parity_bit(b);
    *(&GLUE_PS2_TX_DATA + (parity ? Griffin::PS2_TX_DATA_PARITY : 0)) = b;

    /* Drop our CLK inhibit so CLK isn't re-driven low after the frame
     * (the engine ignores PS2_CTRL.CLK only while tx_active). */
    GLUE_PS2_CTRL = 0;

    /* Wait for the ISR to see TX_DONE. */
    while (ps2.tx_busy)
        ;
}

bool ps2_received_ready()
{
    return ps2.rx_head != ps2.rx_tail;
}

uint8_t ps2_getchar()
{
    uint8_t ch = ps2.rx_queue[ps2.rx_head];
    ps2.rx_head = (ps2.rx_head + 1) & (Griffin::PS2_RX_QUEUE_SIZE - 1);
    return ch;
}

};
