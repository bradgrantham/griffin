// Early-boot console ring buffer.
//
// Captures every byte that flows through the formatted-output paths
// (debug_printf and printf/_write) from the moment crt0 finishes the bss
// init until textport_console_enable() replays and freezes the ring.
// After freeze, push() is a no-op so we don't pay for buffering forever.
//
// Overflow policy: drop oldest.  Bytes pushed while full advance the head
// past the tail (overwriting), and `early_log_dropped` is incremented so
// the replay can prefix a "[N bytes lost]" notice if anything was lost.

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EARLY_LOG_CAPACITY 1024U

// Append one byte.  No-op once early_log_freeze() has been called.
void early_log_push(uint8_t c);

// Walk head→tail, calling emit() for each byte.  Does not modify the ring;
// callers typically follow up with early_log_freeze().
void early_log_replay(int (*emit)(int));

// Disable further pushes; subsequent early_log_push() calls do nothing.
void early_log_freeze(void);

// Bytes dropped to overflow since boot.  Cleared by early_log_reset().
uint32_t early_log_dropped_count(void);

#ifdef __cplusplus
} // extern "C"
#endif
