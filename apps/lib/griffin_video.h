// Direct video access for Griffin applications: the two ownership syscalls
// plus the vblank pacing an app needs once it owns the display.
//
// The ownership contract lives in griffin_abi.h at the repo root -- read it
// before building a display list.  The short version:
//
//   griffin_video_direct_start() stops the firmware's level-6 vsync ISR from
//   re-arming ENGINE_DESC, masks the vsync IRQ, and reports the descriptor
//   carve the app may build tables in.  From that moment the app owns ENGINE,
//   and the vsync latch in GLUE is the app's to poll and to clear.
//   griffin_video_direct_end() hands it all back and restores the console.
//
// PACING CONTRACT (poll mode, the only supported one):
//
//   Nothing acknowledges the vsync latch on the app's behalf, so the frame
//   loop is: do the frame's work, griffin_vsync_wait(), repeat.  The wait both
//   waits for the flag and write-1-to-clears it, so the next wait observes the
//   NEXT vblank rather than returning instantly.  Missing a frame is not an
//   error; the latch is a single sticky bit, so a late loop simply sees the
//   flag already set and continues on the following vblank boundary.
//
//   The vsync IRQ enable stays syscall-owned: GLUE CONFIG is write-only and
//   shadowed by the firmware, so an app cannot safely read-modify-write it.
//
// UNDEFINED OUTSIDE DIRECT MODE: in ordinary console mode the firmware's
// level-6 ISR is live and clears the same latch.  An app that polls it then is
// racing the ISR -- it will usually see nothing and spin forever, and when it
// does win the race it steals the firmware's acknowledge.  Call these only
// between a successful griffin_video_direct_start() and its matching
// griffin_video_direct_end().

#ifndef GRIFFIN_VIDEO_H
#define GRIFFIN_VIDEO_H

#include <stdint.h>

#include "../../griffin_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

// Take direct control of the video ENGINE (SYS_VIDEO_DIRECT_START).  On
// success *info describes the descriptor-table carve the app may use; returns
// 0, or -1/errno on failure (e.g. direct mode already held).  info must not be
// null: it is the app's only source for the carve bounds.
int griffin_video_direct_start(GriffinVideoDirectInfo *info);

// Return the ENGINE to the firmware console (SYS_VIDEO_DIRECT_END).  Returns 0
// or -1/errno.  Calling it when direct mode is not held is harmless; the
// loader also calls it for any app that exits or dies while still holding it.
int griffin_video_direct_end(void);

// Block until the next vblank, then acknowledge it.  Direct mode only -- see
// the pacing contract above.  This spins on the GLUE latch; there is nothing
// else for the CPU to do while it waits and no interrupt is enabled.
void griffin_vsync_wait(void);

// Nonblocking probe: 1 if a vblank has been latched since the last
// acknowledge, 0 if not.  Does NOT acknowledge -- call griffin_vsync_ack()
// when the frame's work is done, or the very next probe still reads 1.
int griffin_vsync_pending(void);

// Acknowledge the latched vblank (write-1-to-clear).  Safe to call when
// nothing is pending: writing the bit clears a flag that is already clear.
void griffin_vsync_ack(void);

#ifdef __cplusplus
}
#endif

#endif /* GRIFFIN_VIDEO_H */
