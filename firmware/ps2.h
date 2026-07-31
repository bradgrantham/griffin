#ifndef PS2_H
#define PS2_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "../griffin.generated.h"

/* ====================================================================
 * PS/2 frame engines — one driver, two channels.
 *
 * GLUE hosts the keyboard frame engine (autovector level 4) and PORTS
 * hosts a register-identical mouse frame engine (autovector level 2).
 * griffin.yml deliberately gives the mouse the same register offsets and
 * bit assignments the keyboard has, so a channel is described entirely
 * by its CPLD base address.  ps2.cpp static_asserts that equivalence
 * against the generated header.
 *
 * The state struct is opaque here; only ps2.cpp needs its layout.
 * ==================================================================== */
typedef struct ps2_channel ps2_channel_t;

extern volatile ps2_channel_t ps2_keyboard;   /* GLUE,  IRQ level 4 */
extern volatile ps2_channel_t ps2_mouse;      /* PORTS, IRQ level 2 */

/* Per-channel entry points.  The ISR body lives in .ramtext so it runs
 * without ROM wait states; both vectors funnel into it. */
void ps2_channel_isr(volatile ps2_channel_t *channel) __attribute__((section(".ramtext")));
bool ps2_channel_send_byte(volatile ps2_channel_t *channel, uint8_t b);
bool ps2_channel_received_ready(volatile ps2_channel_t *channel);
uint8_t ps2_channel_getchar(volatile ps2_channel_t *channel);
uint8_t ps2_channel_get_err_flags(volatile ps2_channel_t *channel);
uint16_t ps2_channel_get_err_data(volatile ps2_channel_t *channel);

/* Initializes both channels' state and quiesces both engines.  Called
 * from crt0.s before interrupts are enabled. */
void ps2_init(void);

/* Keyboard-channel shorthands (the original API; crt0.s and the keymap
 * layer use these). */
void ps2_isr(void) __attribute__((interrupt_handler, section(".ramtext")));
void ps2_send_byte(uint8_t b);
bool ps2_received_ready();
uint8_t ps2_getchar();
uint16_t ps2_get_err_data(void);

#define PS2_ERROR_FRAMING    0x01u
#define PS2_ERROR_PARITY     0x02u
#define PS2_ERROR_OVERRUN    0x04u
#define PS2_ERROR_TX_TIMEOUT 0x08u

uint8_t ps2_get_err_flags(void);

#ifdef __cplusplus
}
#endif

#endif
