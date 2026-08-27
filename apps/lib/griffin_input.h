// Joystick and paddle sampling for Griffin applications.
//
// These are NOT syscalls: PORTS is a plain memory-mapped peripheral and apps
// run in supervisor mode, so an app reads it directly.  What the firmware does
// own is the frame timebase, and the paddles are measured per frame, so:
//
// WHOEVER OWNS VSYNC OWNS griffin_input_read().  The paddle counters ramp for
// one frame and are dumped (discharged) to start the next measurement; that
// dump pulse has to happen exactly once per vblank, which means the routine
// belongs to the loop that is already pacing on vblank.  Call it once per
// observed vblank -- see griffin_video.h -- and never twice in a frame, or the
// second call restarts a ramp mid-frame and both counts go wrong.
//
// In ordinary console (non-direct) mode nothing runs the paddle cycle at all,
// so the paddle counts there are meaningless: no dump ever fires, and the
// counters read whatever they saturated at.  Joystick reads are valid
// anywhere and any time -- they are just switch levels.

#ifndef GRIFFIN_INPUT_H
#define GRIFFIN_INPUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One frame's worth of port state.
struct GriffinInput
{
    uint8_t joy1;       // JOYSTICK_PORT_1 switches, active low (0 = closed)
    uint8_t joy2;       // JOYSTICK_PORT_2 switches, active low (0 = closed)
    uint8_t paddle_a;   // port 1 pin 9 pot, 0..255 PADDLE_TICK counts (15.734 kHz)
    uint8_t paddle_b;   // port 1 pin 5 pot, 0..255 PADDLE_TICK counts (15.734 kHz)
};

// Joystick bit masks, mirroring PORTS_JOYSTICK_PORT_n_*_MASK in
// griffin.generated.h.  They are spelled out here so this header stays usable
// from plain C (the generated header is C++); griffin_input.cpp static_asserts
// each one against the generated value, so a griffin.yml change that moved a
// bit would break the build rather than silently swap up for down.
enum
{
    GRIFFIN_JOY_UP    = 0x01,
    GRIFFIN_JOY_DOWN  = 0x02,
    GRIFFIN_JOY_LEFT  = 0x04,   // port 1: also paddle B fire
    GRIFFIN_JOY_RIGHT = 0x08,   // port 1: also paddle A fire
    GRIFFIN_JOY_FIRE  = 0x10,
    GRIFFIN_JOY_PIN9  = 0x20,   // port 1: SMS button 2 / paddle A pot level
    GRIFFIN_JOY_PIN5  = 0x40    // port 1: paddle B pot level
};

// Sample both joysticks and both paddles, in the one order the hardware
// permits.  Only valid at vblank, and only for the vsync owner; see above.
void griffin_input_read(struct GriffinInput *in);

// Switch tests, active low in the register and pressed-as-true here.
static inline int griffin_joy_pressed(uint8_t joy, uint8_t mask)
{
    return (joy & mask) == 0 ? 1 : 0;
}

static inline int griffin_joy_up(uint8_t joy)
{
    return griffin_joy_pressed(joy, GRIFFIN_JOY_UP);
}

static inline int griffin_joy_down(uint8_t joy)
{
    return griffin_joy_pressed(joy, GRIFFIN_JOY_DOWN);
}

static inline int griffin_joy_left(uint8_t joy)
{
    return griffin_joy_pressed(joy, GRIFFIN_JOY_LEFT);
}

static inline int griffin_joy_right(uint8_t joy)
{
    return griffin_joy_pressed(joy, GRIFFIN_JOY_RIGHT);
}

static inline int griffin_joy_fire(uint8_t joy)
{
    return griffin_joy_pressed(joy, GRIFFIN_JOY_FIRE);
}

#ifdef __cplusplus
}
#endif

#endif /* GRIFFIN_INPUT_H */
