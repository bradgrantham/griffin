// Internal apps/lib helper: the raw TRAP #15 entry sequence.
//
// Not part of the application-facing API -- apps call the griffin_*() wrappers
// in griffin_app.h (and the POSIX veneer in syscalls.cpp) instead.  This header
// exists so every apps/lib translation unit that needs to trap into the
// firmware shares ONE copy of the register-pinned asm and ONE copy of the
// -errno translation; a second hand-written copy is exactly the kind of thing
// that silently diverges from the ABI.
//
// C++ only (it is compiled into the library, never included by app C sources).

#ifndef GRIFFIN_SYSCALL_H
#define GRIFFIN_SYSCALL_H

#include <cerrno>
#include <cstdint>

#include "../../griffin.generated.h"

static_assert(Griffin::SYS_TRAP == 15, "the 'trap #15' literal below must match SYS_TRAP");

// Generic syscall: number in d0, up to three args in d1/d2/d3, return in d0.
// d0-d3 are clobbered (the firmware dispatch may use a0/a1 too).
static inline long griffin_syscall(long num, long a1, long a2, long a3)
{
    register long d0 asm("d0") = num;
    register long d1 asm("d1") = a1;
    register long d2 asm("d2") = a2;
    register long d3 asm("d3") = a3;
    asm volatile("trap #15"
                 : "+d"(d0)
                 : "d"(d1), "d"(d2), "d"(d3)
                 : "memory", "a0", "a1", "cc");
    return d0;
}

// Translate the Linux-style return (>=0 result, <0 is -errno) into errno/-1.
static inline long griffin_sys_ret(long r)
{
    if (r < 0)
    {
        errno = static_cast<int>(-r);
        return -1;
    }
    return r;
}

// Same trap, no error translation.  The clock calls return unsigned 32-bit
// counts in which bit 31 is data (49.7-day tick wrap, post-2038 dates), so
// running them through griffin_sys_ret() would turn a perfectly good value
// into -1.
static inline uint32_t griffin_syscall_raw(long num, long a1, long a2, long a3)
{
    return static_cast<uint32_t>(griffin_syscall(num, a1, a2, a3));
}

#endif /* GRIFFIN_SYSCALL_H */
