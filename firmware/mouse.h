#ifndef MOUSE_H
#define MOUSE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "../griffin_abi.h"

/* ====================================================================
 * PS/2 mouse packet decoder, sitting on top of the PORTS mouse channel in
 * ps2.cpp (as keymap.cpp sits on top of the GLUE keyboard channel).
 *
 * This layer only decodes; it never prints.  Whoever wants the numbers
 * calls mouse_get_report.
 * ==================================================================== */

/* The report IS the syscall ABI structure (griffin_abi.h): SYS_MOUSE_READ hands
 * an application's pointer straight to mouse_get_report, so there is one
 * layout and no copy between two that could drift. */
#define MOUSE_BUTTON_LEFT   GRIFFIN_MOUSE_BUTTON_LEFT
#define MOUSE_BUTTON_RIGHT  GRIFFIN_MOUSE_BUTTON_RIGHT
#define MOUSE_BUTTON_MIDDLE GRIFFIN_MOUSE_BUTTON_MIDDLE

typedef GriffinMouseReport mouse_report_t;

/* Reset the mouse and enable data reporting.  Returns true if the device
 * answered.
 *
 * ORDERING: this sends bytes, and the send routine spins waiting for a
 * TX_DONE that only ports_isr can deliver.  It therefore MUST be called
 * from mainline with interrupts already enabled — from main(), not from
 * crt0.s next to ports_init(), and never from an ISR. */
bool mouse_init(void);

/* Drain the mouse channel's byte queue and assemble packets.  Mainline
 * only; call after mouse_init. */
void mouse_poll(void);

/* Snapshot the decoded state.  Mainline only; call after mouse_init. */
void mouse_get_report(mouse_report_t *out);

#ifdef __cplusplus
}
#endif

#endif
