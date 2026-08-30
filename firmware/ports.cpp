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

    /* Audio: pops disabled and the FIFO pair held in reset (the register's
     * power-on state).  Whoever plays audio releases RESET and primes. */
    PORTS_AUDIO_CONTROL = static_cast<uint8_t>(Griffin::PORTS_AUDIO_CONTROL_DEFAULT);
}

/* ====================================================================
 * ports_isr — level-2 autovector.
 *
 * The PS/2 mouse frame engine (RX_READY / TX_DONE) is the only source on
 * this pin; audio raises no interrupt (its FIFO flags are polled).  The
 * vector stays here rather than in ps2.cpp so PORTS owns its own level.
 * ==================================================================== */
void ports_isr(void)
{
    /* ---- PS/2 mouse frame engine --------------------------------- */
    ps2_channel_isr(&ps2_mouse);

}

};
