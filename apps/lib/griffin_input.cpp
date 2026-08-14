// Joystick and paddle sampling.  See griffin_input.h for the ownership rule
// (the vsync owner calls this once per vblank) and griffin_video.cpp's header
// comment for what an app adds to its link line.

#include <cstdint>

#include "../../griffin.generated.h"
#include "../../griffin.generated.refs.h"
#include "griffin_input.h"

using namespace Griffin::reg;

// The masks in griffin_input.h are hand-written so the header stays C-clean;
// these keep them honest against griffin.yml.
static_assert(GRIFFIN_JOY_UP == static_cast<int>(Griffin::PORTS_JOYSTICK_PORT_1_UP_MASK), "JOY_UP");
static_assert(GRIFFIN_JOY_DOWN == static_cast<int>(Griffin::PORTS_JOYSTICK_PORT_1_DOWN_MASK), "JOY_DOWN");
static_assert(GRIFFIN_JOY_LEFT == static_cast<int>(Griffin::PORTS_JOYSTICK_PORT_1_LEFT_MASK), "JOY_LEFT");
static_assert(GRIFFIN_JOY_RIGHT == static_cast<int>(Griffin::PORTS_JOYSTICK_PORT_1_RIGHT_MASK), "JOY_RIGHT");
static_assert(GRIFFIN_JOY_FIRE == static_cast<int>(Griffin::PORTS_JOYSTICK_PORT_1_FIRE_MASK), "JOY_FIRE");
static_assert(GRIFFIN_JOY_PIN9 == static_cast<int>(Griffin::PORTS_JOYSTICK_PORT_1_PIN9_MASK), "JOY_PIN9");
static_assert(GRIFFIN_JOY_PIN5 == static_cast<int>(Griffin::PORTS_JOYSTICK_PORT_1_PIN5_MASK), "JOY_PIN5");

// Port 2 has the same bit order and no paddles; one set of masks serves both.
static_assert(Griffin::PORTS_JOYSTICK_PORT_1_UP_MASK == Griffin::PORTS_JOYSTICK_PORT_2_UP_MASK &&
                  Griffin::PORTS_JOYSTICK_PORT_1_DOWN_MASK == Griffin::PORTS_JOYSTICK_PORT_2_DOWN_MASK &&
                  Griffin::PORTS_JOYSTICK_PORT_1_LEFT_MASK == Griffin::PORTS_JOYSTICK_PORT_2_LEFT_MASK &&
                  Griffin::PORTS_JOYSTICK_PORT_1_RIGHT_MASK == Griffin::PORTS_JOYSTICK_PORT_2_RIGHT_MASK &&
                  Griffin::PORTS_JOYSTICK_PORT_1_FIRE_MASK == Griffin::PORTS_JOYSTICK_PORT_2_FIRE_MASK,
              "the two joystick ports must share a bit order for one mask set to serve both");

namespace
{

constexpr uint8_t PADDLE_DUMP    = static_cast<uint8_t>(Griffin::PORTS_PADDLE_CONTROL_DUMP_MASK);
constexpr uint8_t PADDLE_MEASURE = static_cast<uint8_t>(Griffin::PORTS_PADDLE_CONTROL_DEFAULT);

static_assert((PADDLE_MEASURE & PADDLE_DUMP) == 0, "the measure state must have DUMP clear");

}   // namespace

extern "C"
{

void griffin_input_read(struct GriffinInput *in)
{
    if (in == nullptr)
    {
        return;
    }

    // The order below is the protocol, not a preference:
    //
    // 1. Joysticks FIRST.  On port 1, PIN9 and PIN5 double as the raw paddle
    //    pot comparator levels, and they read low while a ramp is in progress.
    //    At vblank the ramp that started at the last vblank has finished, so
    //    both pins are back high and the byte is pure switch state.  Reading
    //    them after the dump pulse below would sample mid-ramp instead.
    in->joy1 = PORTS_JOYSTICK_PORT_1;
    in->joy2 = PORTS_JOYSTICK_PORT_2;

    // 2. Then the counters, which hold the finished measurement of the frame
    //    that just ended -- one line count per 31.469 kHz HSYNC.
    in->paddle_a = PORTS_PADDLE_A_COUNT;
    in->paddle_b = PORTS_PADDLE_B_COUNT;

    // 3. Then dump and release, which discharges the caps, zeroes both
    //    counters, and starts the ramp whose result the NEXT call collects.
    //    Both writes are volatile so neither the compiler nor the peripheral
    //    sees only the final state; the pulse is what the hardware acts on.
    PORTS_PADDLE_CONTROL = PADDLE_DUMP;
    PORTS_PADDLE_CONTROL = PADDLE_MEASURE;
}

}   // extern "C"
