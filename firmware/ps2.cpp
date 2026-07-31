#include "ps2.h"
#include "../griffin.generated.h"

/* ====================================================================
 * PS/2 frame engine driver, parameterized on a channel base address.
 *
 * There are two physically distinct engines: the keyboard in GLUE (0xF00000,
 * autovector level 4) and the mouse in PORTS (0xFC0000, autovector level 2).
 * griffin.yml gives them the same register offsets and the same bit
 * assignments on purpose, so the only thing that distinguishes a channel is
 * its CPLD base address.
 *
 * The offsets below are derived from GLUE's keyboard registers; the
 * static_asserts that follow prove PORTS' mouse registers agree.  If one of
 * them ever fires it means griffin.yml and this driver have diverged — fix
 * griffin.yml (or the design doc), do not special-case it here.
 * ==================================================================== */

namespace {

constexpr uint32_t PS2_OFF_TX_DATA = Griffin::GLUE_PS2_TX_DATA - Griffin::GLUE_BASE;
constexpr uint32_t PS2_OFF_STATUS  = Griffin::GLUE_PS2_STATUS  - Griffin::GLUE_BASE;
constexpr uint32_t PS2_OFF_CLEAR   = Griffin::GLUE_PS2_CLEAR   - Griffin::GLUE_BASE;
constexpr uint32_t PS2_OFF_CTRL    = Griffin::GLUE_PS2_CTRL    - Griffin::GLUE_BASE;
constexpr uint32_t PS2_OFF_RX_DATA = Griffin::GLUE_PS2_RX_DATA - Griffin::GLUE_BASE;

/* ---- Register offsets: PORTS mouse == GLUE keyboard ----------------- */
static_assert(Griffin::PORTS_PS2_MOUSE_TX_DATA - Griffin::PORTS_BASE == PS2_OFF_TX_DATA,
              "PORTS PS2_MOUSE_TX_DATA offset differs from GLUE PS2_TX_DATA");
static_assert(Griffin::PORTS_PS2_MOUSE_STATUS - Griffin::PORTS_BASE == PS2_OFF_STATUS,
              "PORTS PS2_MOUSE_STATUS offset differs from GLUE PS2_STATUS");
static_assert(Griffin::PORTS_PS2_MOUSE_CLEAR - Griffin::PORTS_BASE == PS2_OFF_CLEAR,
              "PORTS PS2_MOUSE_CLEAR offset differs from GLUE PS2_CLEAR");
static_assert(Griffin::PORTS_PS2_MOUSE_CTRL - Griffin::PORTS_BASE == PS2_OFF_CTRL,
              "PORTS PS2_MOUSE_CTRL offset differs from GLUE PS2_CTRL");
static_assert(Griffin::PORTS_PS2_MOUSE_RX_DATA - Griffin::PORTS_BASE == PS2_OFF_RX_DATA,
              "PORTS PS2_MOUSE_RX_DATA offset differs from GLUE PS2_RX_DATA");

/* The parity alias (address bit 1) has to land on a register slot that is
 * still inside the shared TX_DATA window on both channels. */
static_assert(Griffin::PS2_TX_DATA_PARITY == 0x02u,
              "PS2_TX_DATA_PARITY is no longer address bit 1");

/* ---- STATUS bits --------------------------------------------------- */
static_assert(Griffin::PORTS_PS2_MOUSE_STATUS_RX_READY_MASK == Griffin::GLUE_PS2_STATUS_RX_READY_MASK,
              "PORTS mouse STATUS.RX_READY differs from GLUE keyboard STATUS.RX_READY");
static_assert(Griffin::PORTS_PS2_MOUSE_STATUS_TX_DONE_MASK == Griffin::GLUE_PS2_STATUS_TX_DONE_MASK,
              "PORTS mouse STATUS.TX_DONE differs from GLUE keyboard STATUS.TX_DONE");
static_assert(Griffin::PORTS_PS2_MOUSE_STATUS_TX_ACK_MASK == Griffin::GLUE_PS2_STATUS_TX_ACK_MASK,
              "PORTS mouse STATUS.TX_ACK differs from GLUE keyboard STATUS.TX_ACK");
static_assert(Griffin::PORTS_PS2_MOUSE_STATUS_RX_PARITY_MASK == Griffin::GLUE_PS2_STATUS_RX_PARITY_MASK,
              "PORTS mouse STATUS.RX_PARITY differs from GLUE keyboard STATUS.RX_PARITY");
static_assert(Griffin::PORTS_PS2_MOUSE_STATUS_RX_PARITY_SHIFT == Griffin::GLUE_PS2_STATUS_RX_PARITY_SHIFT,
              "PORTS mouse STATUS.RX_PARITY sits at a different bit position");
static_assert(Griffin::PORTS_PS2_MOUSE_STATUS_RX_FRAME_ERR_MASK == Griffin::GLUE_PS2_STATUS_RX_FRAME_ERR_MASK,
              "PORTS mouse STATUS.RX_FRAME_ERR differs from GLUE keyboard STATUS.RX_FRAME_ERR");

/* ---- CLEAR bits ---------------------------------------------------- */
static_assert(Griffin::PORTS_PS2_MOUSE_CLEAR_RX_READY_MASK == Griffin::GLUE_PS2_CLEAR_RX_READY_MASK,
              "PORTS mouse CLEAR.RX_READY differs from GLUE keyboard CLEAR.RX_READY");
static_assert(Griffin::PORTS_PS2_MOUSE_CLEAR_TX_DONE_MASK == Griffin::GLUE_PS2_CLEAR_TX_DONE_MASK,
              "PORTS mouse CLEAR.TX_DONE differs from GLUE keyboard CLEAR.TX_DONE");

/* ---- CTRL bits ----------------------------------------------------- */
static_assert(Griffin::PORTS_PS2_MOUSE_CTRL_CLK_MASK == Griffin::GLUE_PS2_CTRL_CLK_MASK,
              "PORTS mouse CTRL.CLK differs from GLUE keyboard CTRL.CLK");
static_assert(Griffin::PORTS_PS2_MOUSE_CTRL_DATA_MASK == Griffin::GLUE_PS2_CTRL_DATA_MASK,
              "PORTS mouse CTRL.DATA differs from GLUE keyboard CTRL.DATA");

/* One channel register.  The base is a runtime value (that is the whole
 * point), so this cannot be a generated reference; form the address by
 * hand instead. */
inline volatile uint8_t &ps2_reg(uint32_t base, uint32_t offset)
{
    return *reinterpret_cast<volatile uint8_t *>(base + offset);
}

/* Bound on the TX handshake spin.  A PS/2 frame is about 1 ms at the device's
 * 10-16.7 kHz clock, so anything past ~100 ms means nothing is clocking the
 * line: unplugged device, held-low CLK, or (in the emulator) a CPLD that is
 * not modelled at all.  Without this bound a missing mouse would wedge the
 * boot inside mouse_init().  Each iteration is a volatile byte read plus a
 * decrement and a branch, roughly 16 SYSCLK cycles out of ROM. */
constexpr uint32_t PS2_SPINS_PER_SECOND = Griffin::SYSCLK_HZ / 16u;
constexpr uint32_t PS2_TX_TIMEOUT_SPINS = PS2_SPINS_PER_SECOND / 10u;

}  // namespace

struct ps2_channel
{
    /* Base address of the CPLD hosting this engine (GLUE_BASE for the
     * keyboard, PORTS_BASE for the mouse).  Set once by ps2_init. */
    uint32_t base;

    uint8_t  rx_queue[Griffin::PS2_RX_QUEUE_SIZE];
    uint32_t rx_head;
    uint32_t rx_tail;

    uint8_t  err_flags;
    uint16_t err_data;

    // TX completion handshake.  tx_busy is set by ps2_channel_send_byte
    // before it triggers the frame and cleared by the ISR on TX_DONE;
    // tx_ack_failed holds the sampled device ACK (0 = acknowledged).
    bool tx_busy;
    bool tx_ack_failed;
};

[[gnu::section("monitor_data")]] volatile ps2_channel_t ps2_keyboard;
[[gnu::section("monitor_data")]] volatile ps2_channel_t ps2_mouse;

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

extern "C" {

/* Bring one channel's state and engine to a known-quiet state. */
static void ps2_channel_init(volatile ps2_channel_t *channel, uint32_t base)
{
    channel->base = base;
    channel->rx_head = 0;
    channel->rx_tail = 0;
    channel->err_flags = 0;
    channel->err_data = 0;
    channel->tx_busy = 0;
    channel->tx_ack_failed = 0;
    ps2_reg(base, PS2_OFF_CTRL) = 0;
    // Clear any latched RX/TX flags from power-up.
    ps2_reg(base, PS2_OFF_CLEAR) =
        static_cast<uint8_t>(Griffin::GLUE_PS2_CLEAR_RX_READY_MASK
                           | Griffin::GLUE_PS2_CLEAR_TX_DONE_MASK);
}

// Called by crt0.s, before interrupts
void ps2_init()
{
    ps2_channel_init(&ps2_keyboard, Griffin::GLUE_BASE);
    ps2_channel_init(&ps2_mouse, Griffin::PORTS_BASE);
}

/* ====================================================================
 * ps2_channel_isr — PS/2 frame IRQ body, shared by both channels.
 *
 * The CPLD assembles whole frames, so this fires at most once per byte
 * (RX_READY) and once per TX completion (TX_DONE), not per bit.  RX:
 * read the byte from RX_DATA, validate parity (from RX_PARITY) and
 * framing (RX_FRAME_ERR), enqueue.  TX: latch the ACK and clear tx_busy.
 * Both flags are acknowledged write-1-to-clear via CLEAR.
 *
 * Called from ps2_isr (GLUE level 4) and from ports_isr (PORTS level 2).
 * ==================================================================== */
void ps2_channel_isr(volatile ps2_channel_t *channel)
{
    uint32_t base = channel->base;
    uint8_t status = ps2_reg(base, PS2_OFF_STATUS);

    /* ---- TX completion ------------------------------------------- */
    if (status & Griffin::GLUE_PS2_STATUS_TX_DONE_MASK)
    {
        channel->tx_ack_failed = (status & Griffin::GLUE_PS2_STATUS_TX_ACK_MASK) ? 1 : 0;
        channel->tx_busy = 0;
        ps2_reg(base, PS2_OFF_CLEAR) =
            static_cast<uint8_t>(Griffin::GLUE_PS2_CLEAR_TX_DONE_MASK);
    }

    /* ---- RX byte ------------------------------------------------- */
    if (status & Griffin::GLUE_PS2_STATUS_RX_READY_MASK)
    {
        uint8_t byte = ps2_reg(base, PS2_OFF_RX_DATA);
        uint8_t rx_parity =
            (status >> Griffin::GLUE_PS2_STATUS_RX_PARITY_SHIFT) & 1u;

        /* Ack first so a fast follow-on frame can re-arm immediately. */
        ps2_reg(base, PS2_OFF_CLEAR) =
            static_cast<uint8_t>(Griffin::GLUE_PS2_CLEAR_RX_READY_MASK);

        if (status & Griffin::GLUE_PS2_STATUS_RX_FRAME_ERR_MASK)
        {
            channel->err_data = byte;
            channel->err_flags |= PS2_ERROR_FRAMING;
            return;
        }

        /* Odd parity: the received parity bit must match what we'd
         * generate for this data byte. */
        if (rx_parity != odd_parity_bit(byte))
        {
            channel->err_data = byte;
            channel->err_flags |= PS2_ERROR_PARITY;
            return;
        }

        /* Enqueue.  ISR is sole writer of tail; mainline is sole writer
         * of head, so no masking of the indices is needed. */
        uint32_t next = (channel->rx_tail + 1u) & (Griffin::PS2_RX_QUEUE_SIZE - 1u);
        if (next == channel->rx_head)
        {
            channel->err_flags |= PS2_ERROR_OVERRUN;
            return;
        }
        channel->rx_queue[channel->rx_tail] = byte;
        channel->rx_tail = next;
    }
}

/* Spin until the channel's TX handshake goes idle, or give up.  Returns
 * false on timeout (see PS2_TX_TIMEOUT_SPINS). */
static bool ps2_wait_tx_idle(volatile ps2_channel_t *channel)
{
    uint32_t spins = PS2_TX_TIMEOUT_SPINS;
    while (channel->tx_busy)
    {
        spins--;
        if (spins == 0)
        {
            return false;
        }
    }
    return true;
}

/* ====================================================================
 * ps2_channel_send_byte — transmit one byte host->device.
 *
 *   1. Pull CLK low (inhibit) for >=100 us.
 *   2. Write the byte to TX_DATA, with the firmware-computed odd parity
 *      carried in address bit 1 (PS2_TX_DATA_PARITY).  The write itself
 *      starts the frame: the CPLD presents the start bit, releases CLK,
 *      and shifts the rest out on the device clock.
 *   3. Release the CLK inhibit and wait for the TX_DONE IRQ.
 *
 * Returns false if the TX_DONE handshake never arrived, which means no
 * device is clocking the line.  Requires interrupts to be enabled: only
 * the ISR clears tx_busy, so this must never be called from an ISR or
 * before crt0 unmasks IPL.
 * ==================================================================== */
bool ps2_channel_send_byte(volatile ps2_channel_t *channel, uint8_t b)
{
    /* Wait for any previous send to finish. */
    if (!ps2_wait_tx_idle(channel))
    {
        channel->err_flags |= PS2_ERROR_TX_TIMEOUT;
        return false;
    }

    uint32_t base = channel->base;

    channel->tx_busy = 1;

    /* Pull CLK low (request-to-send / inhibit).  Mask IRQs only across
     * the register touch; the inhibit hold is a one-sided minimum so a
     * stray IRQ stretching it is harmless. */
    uint16_t saved_sr = irq_save();
    ps2_reg(base, PS2_OFF_CTRL) = static_cast<uint8_t>(Griffin::GLUE_PS2_CTRL_CLK_MASK);
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
     * and the engine releases CLK (overriding CTRL.CLK while busy). */
    uint8_t parity = odd_parity_bit(b);
    ps2_reg(base, PS2_OFF_TX_DATA + (parity ? Griffin::PS2_TX_DATA_PARITY : 0u)) = b;

    /* Drop our CLK inhibit so CLK isn't re-driven low after the frame
     * (the engine ignores CTRL.CLK only while tx_active). */
    ps2_reg(base, PS2_OFF_CTRL) = 0;

    /* Wait for the ISR to see TX_DONE. */
    if (!ps2_wait_tx_idle(channel))
    {
        channel->tx_busy = 0;
        channel->err_flags |= PS2_ERROR_TX_TIMEOUT;
        return false;
    }
    return true;
}

bool ps2_channel_received_ready(volatile ps2_channel_t *channel)
{
    return channel->rx_head != channel->rx_tail;
}

uint8_t ps2_channel_getchar(volatile ps2_channel_t *channel)
{
    uint8_t ch = channel->rx_queue[channel->rx_head];
    channel->rx_head = (channel->rx_head + 1) & (Griffin::PS2_RX_QUEUE_SIZE - 1);
    return ch;
}

uint8_t ps2_channel_get_err_flags(volatile ps2_channel_t *channel)
{
    uint8_t t = channel->err_flags;
    channel->err_flags = 0;
    return t;
}

uint16_t ps2_channel_get_err_data(volatile ps2_channel_t *channel)
{
    return channel->err_data;
}

/* ---- Keyboard-channel shorthands ----------------------------------- */
/* The original single-channel API, kept so crt0.s, keymap.cpp and rom.cpp
 * do not have to know the channel exists. */

void ps2_isr(void)
{
    ps2_channel_isr(&ps2_keyboard);
}

void ps2_send_byte(uint8_t b)
{
    static_cast<void>(ps2_channel_send_byte(&ps2_keyboard, b));
}

bool ps2_received_ready()
{
    return ps2_channel_received_ready(&ps2_keyboard);
}

uint8_t ps2_getchar()
{
    return ps2_channel_getchar(&ps2_keyboard);
}

uint8_t ps2_get_err_flags(void)
{
    return ps2_channel_get_err_flags(&ps2_keyboard);
}

uint16_t ps2_get_err_data(void)
{
    return ps2_channel_get_err_data(&ps2_keyboard);
}

};
