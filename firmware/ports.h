#ifndef PORTS_H
#define PORTS_H

#ifdef __cplusplus
extern "C" {
#endif

/* ====================================================================
 * PORTS CPLD (0xFC0000) — PS/2 mouse, joysticks, paddles, audio FIFO pop.
 *
 * PORTS wire-ORs all of its interrupt sources onto a single ~PORTS_IRQ
 * pin at autovector level 2, so the vector belongs to PORTS rather than
 * to any one of the facilities behind it.  ports_isr fans the interrupt
 * out to them.
 * ==================================================================== */

/* Level-2 autovector.  Installed by crt0.s. */
void ports_isr(void) __attribute__((interrupt_handler, section(".ramtext")));

/* Register-only initialization; no state of its own.  Called from crt0.s
 * before interrupts are enabled. */
void ports_init(void);

#ifdef __cplusplus
}
#endif

#endif
