// Griffin-specific application services (apps/lib/syscalls.cpp).
//
// These are the raw TRAP #15 stubs for the syscalls that have no newlib
// libgloss home: input readiness, directory iteration, and the two clocks.
// Ordinary applications should prefer the POSIX veneer built on top of them
// (poll(), opendir()/readdir()/closedir(), gettimeofday(), usleep()) and reach
// for these only when they want the Griffin call with no translation.
//
// The services that are about hardware the app takes over rather than asks the
// firmware for live in their own headers, pulled in below: griffin_video.h
// (direct display ownership and vblank pacing, apps/lib/griffin_video.cpp) and
// griffin_input.h (joystick and paddle sampling, apps/lib/griffin_input.cpp).
// Those objects are not in every app's link line -- see the comment at the top
// of griffin_video.cpp, or include apps/lib/lib.mk from the app Makefile.

#ifndef GRIFFIN_APP_H
#define GRIFFIN_APP_H

#include <stdint.h>

#include "../../griffin_dirent.h"
#include "griffin_input.h"
#include "griffin_video.h"

#ifdef __cplusplus
extern "C" {
#endif

// 1 if a read() from the console would return immediately (a whole line, or
// EOF, is ready), 0 if it would block, -1/errno on failure.  Polling this also
// pumps the firmware's line editor, so echo and editing stay live.
int griffin_inputready(void);

// Fill *out with entry `index` of directory `path`.  Returns 0 when an entry
// was filled in, 1 when index is past the end, -1/errno on failure.  Iteration
// is stateless: each call reopens the directory and skips forward.
int griffin_readdir(const char *path, int index, GriffinDirEnt *out);

// Milliseconds since boot; wraps every 49.7 days.  Resolution is the 100 Hz
// firmware tick, i.e. 10 ms.
uint32_t griffin_getticks(void);

// Seconds since the Unix epoch (boot epoch + uptime).  Both of these return
// the trap's d0 register verbatim: bit 31 is data, not an error indication.
uint32_t griffin_gettime(void);

#ifdef __cplusplus
}
#endif

#endif /* GRIFFIN_APP_H */
