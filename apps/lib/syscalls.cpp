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
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "../../griffin.generated.h"
#include "dirent.h"
#include "griffin_app.h"
#include "griffin_syscall.h"
#include "poll.h"
#include "unistd.h"

// griffin_syscall(), griffin_sys_ret() and griffin_syscall_raw() used to live
// here; they moved to griffin_syscall.h when a second apps/lib translation unit
// (griffin_video.cpp) needed the same trap sequence.

extern "C"
{

ssize_t write(int fd, const void *buf, size_t len)
{
    return static_cast<ssize_t>(griffin_sys_ret(griffin_syscall(
        Griffin::SYS_WRITE, fd, reinterpret_cast<long>(buf), static_cast<long>(len))));
}

ssize_t read(int fd, void *buf, size_t len)
{
    return static_cast<ssize_t>(griffin_sys_ret(griffin_syscall(
        Griffin::SYS_READ, fd, reinterpret_cast<long>(buf), static_cast<long>(len))));
}

int open(const char *path, int flags, ...)
{
    // The optional mode arg is unused by the FatFs-backed firmware open.
    return static_cast<int>(griffin_sys_ret(griffin_syscall(
        Griffin::SYS_OPEN, reinterpret_cast<long>(path), flags, 0)));
}

int close(int fd)
{
    return static_cast<int>(griffin_sys_ret(griffin_syscall(Griffin::SYS_CLOSE, fd, 0, 0)));
}

off_t lseek(int fd, off_t off, int whence)
{
    return static_cast<off_t>(griffin_sys_ret(griffin_syscall(
        Griffin::SYS_LSEEK, fd, static_cast<long>(off), whence)));
}

int fstat(int fd, struct stat *st)
{
    return static_cast<int>(griffin_sys_ret(griffin_syscall(
        Griffin::SYS_FSTAT, fd, reinterpret_cast<long>(st), 0)));
}

int isatty(int fd)
{
    return static_cast<int>(griffin_sys_ret(griffin_syscall(Griffin::SYS_ISATTY, fd, 0, 0)));
}

int stat(const char *path, struct stat *st)
{
    return static_cast<int>(griffin_sys_ret(griffin_syscall(
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

// ---- Griffin-specific syscalls (apps/lib/griffin_app.h) ----

int griffin_inputready(void)
{
    return static_cast<int>(griffin_sys_ret(griffin_syscall(Griffin::SYS_INPUTREADY, 0, 0, 0)));
}

int griffin_readdir(const char *path, int index, GriffinDirEnt *out)
{
    return static_cast<int>(griffin_sys_ret(griffin_syscall(
        Griffin::SYS_READDIR, reinterpret_cast<long>(path), index,
        reinterpret_cast<long>(out))));
}

uint32_t griffin_getticks(void)
{
    return griffin_syscall_raw(Griffin::SYS_GETTICKS, 0, 0, 0);
}

uint32_t griffin_gettime(void)
{
    return griffin_syscall_raw(Griffin::SYS_GETTIME, 0, 0, 0);
}

// ---- POSIX emulation on top of those (so apps can be ordinary POSIX code) ----

// newlib's time()/gmtime()/localtime()/strftime() and std::chrono's
// system_clock all bottom out here, and libc.a leaves it undefined.
int gettimeofday(struct timeval *__restrict tv, void *__restrict tz)
{
    (void)tz;   // POSIX.1-2008 dropped the timezone argument
    if (tv != nullptr)
    {
        // Seconds and sub-seconds come from two different reads, so they can
        // disagree by one tick right at a second boundary.  Nothing on this
        // machine resolves better than the 10 ms tick anyway.
        tv->tv_sec  = static_cast<time_t>(griffin_gettime());
        tv->tv_usec = static_cast<suseconds_t>((griffin_getticks() % 1000U) * 1000U);
    }
    return 0;
}

// newlib has neither usleep() nor nanosleep().  The only timebase is the
// 100 Hz firmware tick, so the resolution is 10 ms and a request shorter than
// that still costs up to one tick.  There is nothing else to run, so this
// spins; it always sleeps the full duration (no early return), as POSIX says.
int usleep(useconds_t useconds)
{
    const uint32_t ms    = static_cast<uint32_t>((useconds + 999U) / 1000U);
    const uint32_t start = griffin_getticks();
    while (griffin_getticks() - start < ms)
    {
    }
    return 0;
}

// Minimal poll(): enough to ask "is there console input waiting?", which is
// how an app implements BREAK without any line-discipline support.  Only the
// console (fd 0) is pollable and only timeout == 0 (a pure poll) is honored --
// a blocking poll would need a real wait, and no caller wants one yet, so it
// fails loudly rather than silently busy-waiting.
int poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    if (fds == nullptr && nfds != 0)
    {
        errno = EFAULT;
        return -1;
    }
    if (timeout != 0)
    {
        errno = EINVAL;
        return -1;
    }

    int ready = 0;
    for (nfds_t i = 0; i < nfds; i++)
    {
        fds[i].revents = 0;
        if (fds[i].fd < 0)
        {
            continue;
        }
        if (fds[i].fd != 0)
        {
            fds[i].revents = static_cast<short>(POLLNVAL);
        }
        else if ((fds[i].events & POLLIN) != 0 && griffin_inputready() > 0)
        {
            fds[i].revents = static_cast<short>(POLLIN);
        }
        if (fds[i].revents != 0)
        {
            ready++;
        }
    }
    return ready;
}

// Directory iteration (apps/lib/dirent.h).  SYS_READDIR is stateless, so a DIR
// is just the directory path plus how far we have walked; the path copy is
// allocated in the same block as the DIR.
struct DIR
{
    char         *path;
    int           index;
    struct dirent ent;
};

DIR *opendir(const char *path)
{
    if (path == nullptr)
    {
        errno = EFAULT;
        return nullptr;
    }

    // Ask for entry 0 now so a bad path fails here rather than at the first
    // readdir().  A return of 1 (empty directory) is a legitimate success.
    GriffinDirEnt probe;
    if (griffin_readdir(path, 0, &probe) < 0)
    {
        return nullptr;
    }

    const size_t pathlen = strlen(path) + 1;
    DIR *dirp = static_cast<DIR *>(malloc(sizeof(DIR) + pathlen));
    if (dirp == nullptr)
    {
        errno = ENOMEM;
        return nullptr;
    }
    dirp->path = reinterpret_cast<char *>(dirp + 1);
    memcpy(dirp->path, path, pathlen);
    dirp->index = 0;
    return dirp;
}

struct dirent *readdir(DIR *dirp)
{
    if (dirp == nullptr)
    {
        errno = EBADF;
        return nullptr;
    }

    GriffinDirEnt entry;
    const int r = griffin_readdir(dirp->path, dirp->index, &entry);
    if (r != 0)
    {
        return nullptr;   // 1 = end of directory (errno untouched), -1 = error
    }
    dirp->index++;

    dirp->ent.d_type = static_cast<unsigned char>(entry.is_dir ? DT_DIR : DT_REG);
    memcpy(dirp->ent.d_name, entry.name, sizeof(dirp->ent.d_name));
    dirp->ent.d_name[sizeof(dirp->ent.d_name) - 1] = '\0';
    return &dirp->ent;
}

int closedir(DIR *dirp)
{
    if (dirp == nullptr)
    {
        errno = EBADF;
        return -1;
    }
    free(dirp);
    return 0;
}

// libstdc++'s random.o (and newlib's arc4random) reference getentropy(), which
// this bare-metal newlib does not provide; linking the C++ library pulls them
// in whether or not the app uses randomness.  Griffin has no entropy source, so
// fail the call the way a kernel without the syscall would.
int getentropy(void *buffer, size_t length)
{
    (void)buffer;
    (void)length;
    errno = ENOSYS;
    return -1;
}

}  // extern "C"
