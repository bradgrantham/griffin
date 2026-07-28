// ports_full_slow_debounce.v — PORTS fit experiment: the full configuration
// with the PS/2 CLK debounce moved onto the slow GLUE sample tick (2-sample
// window instead of 4 free-running samples), to measure the -4 FF saving.

`define PORTS_KEYBOARD
`define PORTS_MOUSE
`define PORTS_JOYSTICK
`define PORTS_PADDLE
`define PORTS_SLOW_DEBOUNCE

`include "ports.v"
