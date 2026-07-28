// ports_mouse_joystick_paddle_audio.v — PORTS fit experiment: the revised
// architecture.  The PS/2 keyboard stays in GLUE (it already fits there), so
// PORTS carries the PS/2 mouse, both joystick ports, both paddle counters,
// and the audio FIFO pop timed off VIDEO's HSYNC line strobe.

`define PORTS_MOUSE
`define PORTS_JOYSTICK
`define PORTS_PADDLE
`define PORTS_LINE_STROBE
`define PORTS_AUDIO_POP

`include "ports.v"
