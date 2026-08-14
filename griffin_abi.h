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
 *    builds lists that span the whole 40 KB window between app runs, so an app
 *    must NOT expect anything it left here to survive to its next invocation.
 *  - Firmware guarantees its console display list, that list's VIDCMD payload,
 *    and the supervisor stack all stay BELOW desc_table_base, so the console
 *    text page comes back intact when direct mode ends.
 *
 * The supported model is POLLING: the app watches GLUE_VSYNC_STATUS bit 0 and
 * write-1-to-clears GLUE_VSYNC_CLEAR itself.  The vsync IRQ enable stays
 * syscall-owned because GLUE CONFIG is a write-only register shadowed by the
 * firmware, so an app cannot safely read-modify-write it.
 * ------------------------------------------------------------------------ */

/* Upper 32 KB of the descriptor page: the app's descriptor-table carve. */
#define GRIFFIN_APP_DESC_BASE   0x3F8000u
#define GRIFFIN_APP_DESC_BYTES  0x8000u

typedef struct GriffinVideoDirectInfo
{
    uint32_t desc_table_base;   /* first byte of the app's descriptor carve */
    uint32_t desc_table_bytes;  /* size of the carve in bytes */
} GriffinVideoDirectInfo;

#ifdef __cplusplus
}
#endif

#endif /* GRIFFIN_ABI_H */
