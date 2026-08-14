// Direct video access: the ownership syscalls and the vblank poll helpers.
//
// This is the seed of the eventual libgriffin.a.  There is no library archive
// yet, so an app that calls anything here adds the object to its own link:
//
//     OBJECTS = $(LIB)/crt0.o $(LIB)/syscalls.o $(LIB)/griffin_video.o main.o
//
// and either picks up the build rules from apps/lib/lib.mk (`include
// ../lib/lib.mk`, which supplies GRIFFIN_LIB_OBJECTS and the $(LIB)/%.o
// pattern rules) or copies the two-line rule the way apps/hello/Makefile
// copies the one for syscalls.o.  The same goes for griffin_input.o.
//
// Apps run in SUPERVISOR mode, so the raw MMIO below is legal from an app: the
// vsync latch is a plain byte register that the firmware, once it has been
// asked to step aside, no longer touches.  See griffin_video.h for the poll
// contract and griffin_abi.h for the ownership rules.

#include <cstdint>

#include "../../griffin.generated.h"
#include "../../griffin.generated.refs.h"
#include "griffin_syscall.h"
#include "griffin_video.h"

using namespace Griffin::reg;

namespace
{

constexpr uint8_t VSYNC_PENDING = static_cast<uint8_t>(Griffin::GLUE_VSYNC_STATUS_VSYNC_PENDING_MASK);

// The read (STATUS) and the write-1-to-clear (CLEAR) are the same address seen
// from the two directions; nothing here would work if they ever diverged.
static_assert(Griffin::GLUE_VSYNC_STATUS == Griffin::GLUE_VSYNC_CLEAR,
              "vsync poll assumes STATUS and CLEAR are the same register");
static_assert(Griffin::GLUE_VSYNC_STATUS_VSYNC_PENDING_MASK ==
                  Griffin::GLUE_VSYNC_CLEAR_VSYNC_PENDING_MASK,
              "vsync poll assumes the STATUS and CLEAR pending bits line up");

}   // namespace

extern "C"
{

int griffin_video_direct_start(GriffinVideoDirectInfo *info)
{
    return static_cast<int>(griffin_sys_ret(griffin_syscall(
        Griffin::SYS_VIDEO_DIRECT_START, reinterpret_cast<long>(info), 0, 0)));
}

int griffin_video_direct_end(void)
{
    return static_cast<int>(griffin_sys_ret(
        griffin_syscall(Griffin::SYS_VIDEO_DIRECT_END, 0, 0, 0)));
}

void griffin_vsync_wait(void)
{
    while ((GLUE_VSYNC_STATUS & VSYNC_PENDING) == 0)
    {
    }
    GLUE_VSYNC_CLEAR = VSYNC_PENDING;
}

int griffin_vsync_pending(void)
{
    return (GLUE_VSYNC_STATUS & VSYNC_PENDING) != 0 ? 1 : 0;
}

void griffin_vsync_ack(void)
{
    GLUE_VSYNC_CLEAR = VSYNC_PENDING;
}

}   // extern "C"
