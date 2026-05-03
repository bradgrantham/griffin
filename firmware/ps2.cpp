#include "ps2.h"

static volatile uint8_t &glue_ps2_clear = *reinterpret_cast<volatile uint8_t *>(Griffin::GLUE_PS2_CLEAR);
static volatile uint8_t &glue_ps2_ctrl = *reinterpret_cast<volatile uint8_t *>(Griffin::GLUE_PS2_CTRL);
static volatile uint8_t &glue_ps2_status = *reinterpret_cast<volatile uint8_t *>(Griffin::GLUE_PS2_STATUS);
static volatile uint8_t &glue_debug_out = *reinterpret_cast<volatile uint8_t *>(Griffin::GLUE_DEBUG_OUT);

struct ps2_state_t
{
    uint8_t  rx_queue[Griffin::PS2_RX_QUEUE_SIZE];
    uint32_t rx_head;
    uint32_t rx_tail;

    uint8_t  rx_clocks;
    uint16_t rx_data;

    uint8_t  err_flags;
    uint16_t err_data;

    bool  kbd_sending;
    bool  kbd_next_clk_is_ack;
    uint8_t  kbd_tx_bits;
    uint16_t kbd_tx_data;
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
    ps2.rx_clocks = 0;
    ps2.rx_data = 0;
    ps2.err_flags = 0;
    ps2.err_data = 0;
    ps2.kbd_sending = 0;
    ps2.kbd_next_clk_is_ack = 0;
    ps2.kbd_tx_bits = 0;
    ps2.kbd_tx_data = 0;
    glue_ps2_ctrl = 0;
    glue_ps2_clear = Griffin::GLUE_PS2_CLEAR_BIT_READY_MASK;
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
/* Returns 1 if (XOR of input bits) is even (so total with parity bit
 * becomes odd), else 0.  XOR-fold beats a loop on 68000. */
static inline uint8_t odd_parity_bit(uint8_t x) {
    x ^= x >> 4;
    x ^= x >> 2;
    x ^= x >> 1;
    return (x & 1u) ^ 1u;
}

/* ====================================================================
 * ps2_isr — PS/2 bit-level IRQ (GLUE level 4)
 *
 * Fires once per falling edge of PS2_CLK.  Accumulates 11-bit frames
 * (start, 8 data LSB first, odd parity, stop), validates them, and
 * enqueues the data byte.  TX shares this ISR: when kbd_sending is set,
 * each falling edge shifts the next bit of kbd_tx_data onto DATA.
 * ==================================================================== */
void ps2_isr(void)
{
    /* Read status (captures the bit that was on DATA at the falling
     * edge) then ack BIT_READY so the next edge can latch cleanly. */
    uint8_t status = glue_ps2_status;
    glue_ps2_clear = Griffin::GLUE_PS2_CLEAR_BIT_READY_MASK;

    /* TX branch */
    if (ps2.kbd_sending)
    {
        /* Decrement remaining-bit counter; when it hits 0, frame done. */
        ps2.kbd_tx_bits = ps2.kbd_tx_bits - 1;
        if (ps2.kbd_tx_bits == 0)
        {
            /* All 11 bits placed.  Release DATA so the device can pull
             * it low on the next clock to line-ACK. */
            glue_ps2_ctrl = 0;
            ps2.kbd_next_clk_is_ack = 1;
            ps2.kbd_sending = 0;
            return;
        }

        /* Place bit 0 of kbd_tx_data onto DATA.
         * Open-drain: bit=0 -> drive=1 (pull low),
         *             bit=1 -> drive=0 (release, pull-up -> high). */
        uint16_t txd = ps2.kbd_tx_data;
        glue_ps2_ctrl = (txd & 1u) ? 0u : Griffin::GLUE_PS2_CTRL_DATA_MASK;
        ps2.kbd_tx_data = txd >> 1;
        return;
    }

    /* ACK-discard: edge following last TX bit is the device's line-ACK.
     * Consume it without shifting into the RX data accumulator. */
    if (ps2.kbd_next_clk_is_ack)
    {
        ps2.kbd_next_clk_is_ack = 0;
        return;
    }

    /* ---- RX path -------------------------------------------------- */
    uint16_t bit = (status >> Griffin::GLUE_PS2_STATUS_DATA_IN_SHIFT) & 1u;

    /* Shift data accumulator right, insert new bit at position 10.
     * Final layout: bit 0 = start, bits 1..8 = data LSB first,
     *               bit 9 = parity, bit 10 = stop. */
    ps2.rx_data = (ps2.rx_data >> 1) | (bit << 10);

    ps2.rx_clocks = ps2.rx_clocks + 1;
    if (ps2.rx_clocks < 11) {
        return;
    }

    /* Start bit must be 0, stop bit must be 1. */
    if ((ps2.rx_data & 0x0001u) || !(ps2.rx_data & 0x0400u)) {
        ps2.err_data = ps2.rx_data;
        ps2.err_flags |= PS2_ERROR_FRAMING;          /* framing */
        return;
    }

    /* Odd parity: XOR of bits 1..9 must be 1. */
    uint16_t pbits = (ps2.rx_data >> 1) & 0x01FFu;
    /* Reuse the same fold trick on 9 bits. */
    pbits ^= pbits >> 8;
    pbits ^= pbits >> 4;
    pbits ^= pbits >> 2;
    pbits ^= pbits >> 1;
    if (!(pbits & 1u)) {
        ps2.err_data = ps2.rx_data;
        ps2.err_flags |= PS2_ERROR_PARITY;          /* parity */
        return;
    }

    /* Extract data byte: bits 1..8. */
    uint8_t byte = (uint8_t)(ps2.rx_data >> 1);

    /* Enqueue.  ISR is sole writer of tail; mainline is sole writer of
     * head, so no mask needed for the queue indices themselves. */
    uint32_t next = (ps2.rx_tail + 1u) & (Griffin::PS2_RX_QUEUE_SIZE - 1u);
    if (next == ps2.rx_head) {
        ps2.err_flags |= PS2_ERROR_OVERRUN;          /* overrun */
        return;
    }
    ps2.rx_queue[ps2.rx_tail] = byte;
    ps2.rx_tail = next;

    /* Reset for next */
    ps2.rx_clocks = 0;
    ps2.rx_data = 0;
}

/* ====================================================================
 * ps2_send_byte — transmit one byte host->keyboard.
 *
 *   1. Pull CLK low (inhibit) for >=100 us.
 *   2. Pull DATA low (start bit = 0), release CLK.
 *   3. Let the device clock 10 more bits out of kbd_tx_data — handled
 *      by the TX branch of ps2_isr.
 *   4. Release DATA; device acks by pulling DATA low for one clock.
 * ==================================================================== */
void ps2_send_byte(uint8_t b)
{
    // Wait for previous send to finish if there is one
    while(ps2.kbd_sending || ps2.kbd_next_clk_is_ack);

    /* Build 11-bit frame:
     *   bit 0    = start (0)
     *   bits 1..8 = data LSB first
     *   bit 9    = odd parity
     *   bit 10   = stop (1) */
    uint16_t parity = odd_parity_bit(b);
    uint16_t frame  = ((uint16_t)b << 1)
                    | (parity << 9)
                    | 0x0400u;          /* stop bit */

    /* Mask IRQs while we touch the CPLD and TX state. */
    uint16_t saved_sr = irq_save();

    /* Pull CLK low (request-to-send / inhibit). */
    glue_ps2_ctrl = Griffin::GLUE_PS2_CTRL_CLK_MASK;

    /* Hold >=100 us.  At SYSCLK=14 MHz with ROM wait states this loop
     * runs ~16 cycles/iter; 250 iters ≈ 285 us.
     *
     * NOTE: this is the one place where C is genuinely worse than asm —
     * the compiler is free to retime this loop or, with optimization,
     * delete it entirely.  Inline asm keeps the timing predictable. */
    __asm__ volatile (
        "    move.w  #250,%%d0   \n"
        "1:  dbra    %%d0,1b     \n"
        ::: "d0", "cc"
    );

    /* Place start bit (= 0) on DATA and release CLK.
     * Setting CTRL = DATA only: CLK released, DATA pulled low. */
    glue_ps2_ctrl = Griffin::GLUE_PS2_CTRL_DATA_MASK;

    /* Pre-shift the frame so bit 0 of kbd_tx_data is the first
     * post-start bit (data0).  The ISR will place that on the first
     * falling edge and shift until kbd_tx_bits reaches 0. */
    ps2.kbd_tx_data         = frame >> 1;
    ps2.kbd_tx_bits         = 11;
    ps2.kbd_next_clk_is_ack = 0;
    ps2.kbd_sending         = 1;

    /* Clear any BIT_READY latched by our own CLK-falling edge. */
    glue_ps2_clear = Griffin::GLUE_PS2_CLEAR_BIT_READY_MASK;

    irq_restore(saved_sr);
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
