// ports_full_audio.v — PORTS fit experiment: the full configuration plus the
// audio.v divider and /HF IRQ.  Stretch goal; expected to overflow the
// ATF1508 — the point is to record how far over it lands.

`define PORTS_KEYBOARD
`define PORTS_MOUSE
`define PORTS_JOYSTICK
`define PORTS_PADDLE
`define PORTS_AUDIO

`include "ports.v"
