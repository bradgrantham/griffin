// Firmware side of the TRAP #15 syscall ABI.
//
// The asm entry point (_syscall_isr in crt0.s) marshals the trapping app's
// d0/d1/d2/d3 into a call here.  We dispatch on the call number (generated from
// griffin.yml into Griffin::SYS_*) to the newlib libgloss functions in
// syscalls.c, and return the result — or -errno on failure (Linux convention),
// which the app-side stub turns back into errno/-1.

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <sys/types.h>
#include <sys/stat.h>

#include "../griffin.generated.h"

extern "C"
{
    // newlib libgloss implementations (firmware/syscalls.c)
    ssize_t write(int fd, const void *buf, size_t len);
    ssize_t read(int fd, void *buf, size_t len);
    int     open(const char *path, int flags, ...);
    int     close(int fd);
    off_t   lseek(int fd, off_t off, int whence);
    int     fstat(int fd, struct stat *st);
    int     isatty(int fd);
    int     stat(const char *path, struct stat *st);

    void app_exit(int code);
    long sys_dispatch(long num, long a1, long a2, long a3);
}

// Weak default for SYS_EXIT: the application loader overrides this to unwind
// back to the monitor.  Until the loader exists, no app runs, so it is dormant.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-infinite-loop"
__attribute__((weak)) void app_exit(int code)
{
    printf("app_exit(%d): no loader context, halting\n", code);
    for (;;)
    {
    }
}
#pragma GCC diagnostic pop

long sys_dispatch(long num, long a1, long a2, long a3)
{
    errno = 0;
    long r = 0;
    switch (num)
    {
        case Griffin::SYS_WRITE:
            r = static_cast<long>(write(static_cast<int>(a1),
                                        reinterpret_cast<const void *>(a2),
                                        static_cast<size_t>(a3)));
            break;
        case Griffin::SYS_READ:
            r = static_cast<long>(read(static_cast<int>(a1),
                                       reinterpret_cast<void *>(a2),
                                       static_cast<size_t>(a3)));
            break;
        case Griffin::SYS_OPEN:
            r = open(reinterpret_cast<const char *>(a1),
                     static_cast<int>(a2), static_cast<int>(a3));
            break;
        case Griffin::SYS_CLOSE:
            r = close(static_cast<int>(a1));
            break;
        case Griffin::SYS_LSEEK:
            r = static_cast<long>(lseek(static_cast<int>(a1),
                                        static_cast<off_t>(a2),
                                        static_cast<int>(a3)));
            break;
        case Griffin::SYS_FSTAT:
            r = fstat(static_cast<int>(a1), reinterpret_cast<struct stat *>(a2));
            break;
        case Griffin::SYS_ISATTY:
            r = isatty(static_cast<int>(a1));
            break;
        case Griffin::SYS_STAT:
            r = stat(reinterpret_cast<const char *>(a1),
                     reinterpret_cast<struct stat *>(a2));
            break;
        case Griffin::SYS_EXIT:
            app_exit(static_cast<int>(a1));
            break;
        default:
            errno = ENOSYS;
            r = -1;
            break;
    }
    if (r < 0 && errno != 0)
    {
        return -errno;
    }
    return r;
}
