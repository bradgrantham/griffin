// PS/2 mouse for applications.
//
// The report structure, the button bits and the reason an app must not read
// the PORTS mouse registers directly are all in griffin_abi.h at the repo
// root -- read it first.  This header is only the trap wrapper.
//
// Typical use, once per vblank by whoever owns vsync:
//
//     GriffinMouseReport now;
//     griffin_mouse_read(&now);
//     int dx = now.x - last.x;          // counts, right positive
//     int dy = last.y - now.y;          // flipped: raster Y grows downward
//     bool left_pressed = (now.buttons & ~last.buttons) & GRIFFIN_MOUSE_BUTTON_LEFT;
//     last = now;
//
// THE CURSOR CONVENTION, which the emulator's host mouse relies on to keep
// its pointer and the guest cursor together: a cursor moves one pixel per
// count, is clamped to the 640x480 screen, and starts at the screen centre
// plus whatever the report has accumulated since boot:
//
//     cursor_x = clamp(320 + now.x, 0, 639);
//     cursor_y = clamp(240 - now.y, 0, 479);
//
// An app that scales or accelerates counts is free to, and is then driven
// from the emulator with --mouse-grab.

#ifndef GRIFFIN_MOUSE_H
#define GRIFFIN_MOUSE_H

#include "../../griffin_abi.h"
#include "griffin_syscall.h"

// Decode whatever mouse packets have arrived and snapshot the accumulated
// state.  Returns 0, or -1 with errno set (ENOSYS on a firmware that predates
// the call).  report->present is 0 when no mouse answered at boot.
static inline int griffin_mouse_read(GriffinMouseReport *report)
{
    return static_cast<int>(griffin_sys_ret(
        griffin_syscall(Griffin::SYS_MOUSE_READ, reinterpret_cast<long>(report), 0, 0)));
}

#endif /* GRIFFIN_MOUSE_H */
