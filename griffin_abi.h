/* Firmware<->application ABI types that are not register definitions.
 *
 * griffin.yml (via griffin.generated.h) owns the hardware; this header owns the
 * handful of structures and carve-outs the TRAP #15 syscalls exchange with an
 * application.  Shared verbatim by the firmware (C++) and by applications
 * (C or C++), so it stays plain C with a fixed, ABI-stable layout.
 */

#ifndef GRIFFIN_ABI_H
#define GRIFFIN_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Direct video access (SYS_VIDEO_DIRECT_START / SYS_VIDEO_DIRECT_END)
 *
 * SYS_VIDEO_DIRECT_START hands the ENGINE display-list hardware to the
 * application: the firmware stops re-arming ENGINE_DESC from its level-6 vsync
 * ISR, masks the vsync IRQ in GLUE CONFIG, and fills in a
 * GriffinVideoDirectInfo describing the descriptor storage the app may use.
 * SYS_VIDEO_DIRECT_END gives it back (and the loader calls it for any app that
 * exits or dies without releasing).
 *
 * OWNERSHIP RULES while direct mode is held:
 *
 *  - Only DESCRIPTOR TABLES have to live in the descriptor page: ENGINE's
 *    pointer is 15 bits of word address inside the hard-wired 0x3F0000 page,
 *    so every descriptor must sit in 0x3F0000..0x3FFFFF and be 8-byte aligned.
 *    The app's descriptors go in [desc_table_base, desc_table_base +
 *    desc_table_bytes).
 *  - VIDCMD words, pixel data and audio samples are ordinary DMA sources
 *    addressed by the full 22-bit descriptor source field, so they may live
 *    ANYWHERE in RAM -- normally in the app's own region or heap, not in this
 *    carve.
 *  - The carve is the app's only while it runs.  The firmware image viewer
 *    builds lists that span the whole 64 KB window between app runs, so an app
 *    must NOT expect anything it left here to survive to its next invocation.
 *  - Firmware guarantees its console display list, that list's VIDCMD payload,
 *    and the supervisor stack all stay BELOW desc_table_base, so the console
 *    text page comes back intact when direct mode ends.
 *
 * TWO PACING MODELS, and which one an app needs is decided by one fact: the
 * vblank pacers at the head of a list count HBLANK edges FROM THE ARM, so a
 * list armed N lines after the vsync edge puts the whole picture N lines low.
 *
 *  - POLLING (the default after START).  The app watches GLUE_VSYNC_STATUS
 *    bit 0, write-1-to-clears GLUE_VSYNC_CLEAR and writes ENGINE_DESC itself.
 *    Right for an app that finishes its frame's work and then SPINS on vsync,
 *    because the spin arms within a few instructions of the edge.
 *  - INTERRUPT.  An app that computes straight through vblank cannot poll
 *    often enough (one scanline is 445 SYSCLK), so it installs its own handler
 *    in the level-6 autovector (RAM vector 30, address 0x78) and then calls
 *    SYS_VIDEO_VSYNC_IRQ(1).  The handler must write-1-to-clear
 *    GLUE_VSYNC_CLEAR (the IRQ is level-triggered off the latch; leaving it
 *    set livelocks), should write ENGINE_DESC, and ends in rte.  The firmware
 *    saved the vector at START and restores it at END, so an app that dies
 *    cannot leave its handler installed.  Install the vector BEFORE enabling.
 *
 * The enable is a syscall, not a register write, because GLUE CONFIG is a
 * write-only register shadowed by the firmware, so an app cannot safely
 * read-modify-write it.
 * ------------------------------------------------------------------------ */

/* The app's descriptor-table carve: the descriptor page above the firmware's
 * 8 KB console list, 56 KB = 7,168 descriptors (raised from the upper 32 KB on
 * 2026-10-03).  Take the base and size from SYS_VIDEO_DIRECT_START's info at
 * run time; these are for static sizing only. */
#define GRIFFIN_APP_DESC_BASE   0x3F2000u
#define GRIFFIN_APP_DESC_BYTES  0xE000u

typedef struct GriffinVideoDirectInfo
{
    uint32_t desc_table_base;   /* first byte of the app's descriptor carve */
    uint32_t desc_table_bytes;  /* size of the carve in bytes */
} GriffinVideoDirectInfo;

/* ---------------------------------------------------------------------------
 * PS/2 mouse (SYS_MOUSE_READ)
 *
 * The firmware's level-2 ISR owns the PORTS mouse channel for the whole time an
 * application runs, so an app must NOT read the PORTS mouse registers itself:
 * it would race the ISR for every byte.  SYS_MOUSE_READ decodes whatever
 * packets have arrived and copies out this snapshot.
 *
 * x, y and wheel are ACCUMULATED since boot, not per-call deltas: an app
 * remembers the last values it saw and subtracts.  That makes a missed poll
 * harmless and lets several readers coexist.  Y is UP positive (the PS/2
 * sense), the opposite of raster line numbering.  One count is whatever the
 * device calls one count; nothing here scales or accelerates.
 * ------------------------------------------------------------------------ */

#define GRIFFIN_MOUSE_BUTTON_LEFT   0x01u
#define GRIFFIN_MOUSE_BUTTON_RIGHT  0x02u
#define GRIFFIN_MOUSE_BUTTON_MIDDLE 0x04u

typedef struct GriffinMouseReport
{
    int32_t  x;         /* accumulated X since init, right positive */
    int32_t  y;         /* accumulated Y since init, up positive (PS/2 sense) */
    int32_t  wheel;     /* accumulated wheel detents (IntelliMouse only) */
    uint32_t packets;   /* decoded packets, i.e. a change counter */
    uint32_t resyncs;   /* bytes discarded hunting for a packet boundary */
    uint8_t  buttons;   /* GRIFFIN_MOUSE_BUTTON_* bitmask, live state */
    uint8_t  packet_bytes;  /* 3 standard, 4 IntelliMouse (wheel) */
    uint8_t  present;   /* 1 once the reset/enable sequence succeeded */
} GriffinMouseReport;

#ifdef __cplusplus
}
#endif

#endif /* GRIFFIN_ABI_H */
