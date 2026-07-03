#ifndef KEYMAP_H
#define KEYMAP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

// PS/2 (scan-code set 2) -> ASCII translation, exposed as a ring that the
// console layer drains.  This facility is PS/2-only by design: it does not
// know about the DUART.  Merging the keyboard with the serial console is the
// consumer's job (read() in syscalls.c), per the project's rule that
// facilities don't cross-communicate.  keymap depends only on ps2.cpp, the
// layer directly beneath it.

// Cooked path: translate any pending scancodes and report whether a cooked
// ASCII byte is waiting.  keyboard_ready() pumps internally, so polling it is
// what advances the decode state machine; keyboard_getchar() is a
// non-blocking pop whose result is only defined when keyboard_ready() is true.
bool    keyboard_ready(void);
uint8_t keyboard_getchar(void);

// Raw-mode switch (input routing only; graphics control is deferred).  In raw
// mode the cooked path is not fed and the console reads raw scancodes via
// keyboard_raw_*().  Both transitions flush the ring and reset modifier /
// decode state so no half-decoded E0/F0 or stuck modifier survives.
void griffin_enter_raw(void);
void griffin_leave_raw(void);
bool griffin_is_raw(void);

// Raw path: pass raw PS/2 scancodes straight through (meaningful while
// griffin_is_raw()).  keyboard_raw_getchar() is a non-blocking pop whose
// result is only defined when keyboard_raw_ready() is true.
bool    keyboard_raw_ready(void);
uint8_t keyboard_raw_getchar(void);

#ifdef __cplusplus
}
#endif

#endif /* KEYMAP_H */
