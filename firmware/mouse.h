#ifndef MOUSE_H
#define MOUSE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ====================================================================
 * PS/2 mouse packet decoder, sitting on top of the PORTS mouse channel in
 * ps2.cpp (as keymap.cpp sits on top of the GLUE keyboard channel).
 *
 * This layer only decodes; it never prints.  Whoever wants the numbers
 * calls mouse_get_report.
 * ==================================================================== */

#define MOUSE_BUTTON_LEFT   0x01u
#define MOUSE_BUTTON_RIGHT  0x02u
#define MOUSE_BUTTON_MIDDLE 0x04u

typedef struct mouse_report
{
    int32_t  x;         /* accumulated X since init, right positive */
    int32_t  y;         /* accumulated Y since init, up positive (PS/2 sense) */
    int32_t  wheel;     /* accumulated wheel detents (IntelliMouse only) */
    uint32_t packets;   /* decoded packets, i.e. a change counter */
    uint32_t resyncs;   /* bytes discarded hunting for a packet boundary */
    uint8_t  buttons;   /* MOUSE_BUTTON_* bitmask, live state */
    uint8_t  packet_bytes;  /* 3 standard, 4 IntelliMouse (wheel) */
    uint8_t  present;   /* 1 once the reset/enable sequence succeeded */
} mouse_report_t;

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
