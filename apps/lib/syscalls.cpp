// Application side of the Griffin syscall ABI.
//
// These are the newlib libgloss functions the C library bottoms out on.  Each
// shared-resource call (console + FatFs) is a thin TRAP #15 stub into the
// firmware, which runs the real implementation (firmware/syscalls.c) and
// returns the result or -errno.  The call numbers come from griffin.yml via the
// generated header, so the app and firmware can never disagree.  Purely local
// services (the app's own heap, pid/signals) are handled here without a trap.

#include <cerrno>
#include <cstddef>
#include <sys/types.h>
#include <sys/stat.h>

#include "../../griffin.generated.h"

static_assert(Griffin::SYS_TRAP == 15, "the 'trap #15' literal below must match SYS_TRAP");

// Generic syscall: number in d0, up to three args in d1/d2/d3, return in d0.
// d0-d3 are clobbered (the firmware dispatch may use a0/a1 too).
static long griffin_syscall(long num, long a1, long a2, long a3)
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
static long sys_ret(long r)
{
    if (r < 0)
    {
        errno = static_cast<int>(-r);
        return -1;
    }
    return r;
}

extern "C"
{

ssize_t write(int fd, const void *buf, size_t len)
{
    return static_cast<ssize_t>(sys_ret(griffin_syscall(
        Griffin::SYS_WRITE, fd, reinterpret_cast<long>(buf), static_cast<long>(len))));
}

ssize_t read(int fd, void *buf, size_t len)
{
    return static_cast<ssize_t>(sys_ret(griffin_syscall(
        Griffin::SYS_READ, fd, reinterpret_cast<long>(buf), static_cast<long>(len))));
}

int open(const char *path, int flags, ...)
{
    // The optional mode arg is unused by the FatFs-backed firmware open.
    return static_cast<int>(sys_ret(griffin_syscall(
        Griffin::SYS_OPEN, reinterpret_cast<long>(path), flags, 0)));
}

int close(int fd)
{
    return static_cast<int>(sys_ret(griffin_syscall(Griffin::SYS_CLOSE, fd, 0, 0)));
}

off_t lseek(int fd, off_t off, int whence)
{
    return static_cast<off_t>(sys_ret(griffin_syscall(
        Griffin::SYS_LSEEK, fd, static_cast<long>(off), whence)));
}

int fstat(int fd, struct stat *st)
{
    return static_cast<int>(sys_ret(griffin_syscall(
        Griffin::SYS_FSTAT, fd, reinterpret_cast<long>(st), 0)));
}

int isatty(int fd)
{
    return static_cast<int>(sys_ret(griffin_syscall(Griffin::SYS_ISATTY, fd, 0, 0)));
}

int stat(const char *path, struct stat *st)
{
    return static_cast<int>(sys_ret(griffin_syscall(
        Griffin::SYS_STAT, reinterpret_cast<long>(path), reinterpret_cast<long>(st), 0)));
}

[[noreturn]] void _exit(int code)
{
    griffin_syscall(Griffin::SYS_EXIT, code, 0, 0);
    for (;;)
    {
    }
}

// ---- app-local services (no firmware trap) ----

// The app owns its heap: it grows up from _app_heap_start toward the top of
// the app region (_app_stack_top), both placed by app.ld.
void *sbrk(ptrdiff_t incr)
{
    extern char _app_heap_start;
    extern char _app_stack_top;
    static char *heap_end = nullptr;
    if (heap_end == nullptr)
    {
        heap_end = &_app_heap_start;
    }
    if (heap_end + incr > &_app_stack_top)
    {
        errno = ENOMEM;
        return reinterpret_cast<void *>(-1);
    }
    char *prev = heap_end;
    heap_end += incr;
    return prev;
}

int getpid(void)
{
    return 1;
}

int kill(int, int)
{
    errno = EINVAL;
    return -1;
}

}  // extern "C"
