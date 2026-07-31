#include "ports.h"
#include "ps2.h"
#include "../griffin.generated.h"
#include "../griffin.generated.refs.h"

using namespace Griffin::reg;

extern "C" {

/* ====================================================================
 * ports_init — quiesce the PORTS peripherals.  Registers only; PORTS has
 * no driver state of its own (the PS/2 mouse channel's state belongs to
 * ps2.cpp and is initialized by ps2_init).
 *
 * Called from crt0.s before interrupts are enabled, so it must not wait
 * on anything that an ISR would have to complete.
 * ==================================================================== */
void ports_init(void)
{
    /* Paddle counters to their reset default (DUMP clear = measuring).
     * The per-frame dump/measure cycle belongs to whoever owns vsync. */
    PORTS_PADDLE_CONTROL = static_cast<uint8_t>(Griffin::PORTS_PADDLE_CONTROL_DEFAULT);

    /* Audio: pops disabled (ENABLE = 0), and write-1-clear any half-full
     * IRQ latched before we took over.  A stale HF_IRQ would hold
     * ~PORTS_IRQ asserted from the first instant interrupts are enabled. */
    PORTS_AUDIO_CONTROL = static_cast<uint8_t>(Griffin::PORTS_AUDIO_CONTROL_CLEAR_HF_IRQ_MASK);
}

/* ====================================================================
 * ports_isr — level-2 autovector.
 *
 * PORTS wire-ORs the PS/2 mouse frame engine (RX_READY / TX_DONE) and the
 * latched audio FIFO half-full flag onto one interrupt pin, so every entry
 * has to service both: the line stays asserted until every source is
 * cleared, and a level-triggered autovector that returns with a source
 * still latched is an interrupt storm.
 *
 * This is why the vector lives here and not in ps2.cpp — PS/2 owns only
 * one of the two sources behind it.
 * ==================================================================== */
void ports_isr(void)
{
    /* ---- PS/2 mouse frame engine --------------------------------- */
    ps2_channel_isr(&ps2_mouse);

    /* ---- Audio FIFO half-full ------------------------------------ */
    /* Serviced even though playback is not implemented yet: HF_IRQ is a
     * latch, so leaving it set holds level 2 asserted forever. */
    uint8_t audio_status = PORTS_AUDIO_STATUS;
    if (audio_status & Griffin::PORTS_AUDIO_STATUS_HF_IRQ_MASK)
    {
        /* STUB: refill the 7202 FIFOs here once audio playback exists.
         * Roughly 512 stereo sample pairs of headroom per service; write
         * them to AUDIO_FIFO.  Until then the DACs simply run dry. */

        /* Ack.  AUDIO_CONTROL is a plain write register, not write-1-clear:
         * only CLEAR_HF_IRQ is self-clearing, so ENABLE has to be written
         * back at its current value (reported by AUDIO_STATUS.ENABLE) or
         * the ack would silently stop the FIFO pop. */
        uint8_t ack = static_cast<uint8_t>(Griffin::PORTS_AUDIO_CONTROL_CLEAR_HF_IRQ_MASK);
        if (audio_status & Griffin::PORTS_AUDIO_STATUS_ENABLE_MASK)
        {
            ack = static_cast<uint8_t>(ack | Griffin::PORTS_AUDIO_CONTROL_ENABLE_MASK);
        }
        PORTS_AUDIO_CONTROL = ack;
    }
}

};
