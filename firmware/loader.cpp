// Application loader.
//
// Reads a flat application binary (built by apps/, linked at _app_base) from the
// filesystem into the app region and runs it.  The app is entered at offset 0
// and runs on the firmware supervisor stack; when it finishes it issues SYS_EXIT
// (the app's _exit), which the firmware dispatch routes to app_exit() below.
// app_exit() longjmps back to the setjmp point here, unwinding the app's frames
// (and the abandoned TRAP #15 frame) so control returns cleanly to the caller.
//
// The 68000/68010 have no instruction cache, so freshly-loaded code needs no
// cache flush before it is executed.

#include <cstdint>
#include <cstdio>
#include <csetjmp>
#include <cstddef>
#include <fcntl.h>
#include <unistd.h>

extern "C" char _app_base[];       // 0x001000  (app load/link base, linker.ld)
extern "C" char _firmware_ram[];   // 0x780000  (top of the app region)

static jmp_buf app_ctx;
static int     app_exit_code = 0;

// Strong override of the weak app_exit() in syscall_dispatch.cpp.  A finished
// application (SYS_EXIT) unwinds back to load_and_run_app()'s setjmp point.
extern "C" void app_exit(int code)
{
    app_exit_code = code;
    longjmp(app_ctx, 1);
}

extern "C" int load_and_run_app(const char *path)
{
    const uint32_t base     = reinterpret_cast<uint32_t>(_app_base);
    const uint32_t limit    = reinterpret_cast<uint32_t>(_firmware_ram);
    const size_t   maxbytes = limit - base;

    int fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        printf("loader: cannot open '%s'\n", path);
        return -1;
    }

    uint8_t *dst = reinterpret_cast<uint8_t *>(base);
    size_t total = 0;
    for (;;)
    {
        ssize_t n = read(fd, dst + total, 4096);
        if (n < 0)
        {
            printf("loader: read error on '%s'\n", path);
            close(fd);
            return -1;
        }
        if (n == 0)
        {
            break;
        }
        total += static_cast<size_t>(n);
        if (total > maxbytes)
        {
            printf("loader: '%s' too large for app region (> %u bytes)\n",
                   path, static_cast<unsigned>(maxbytes));
            close(fd);
            return -1;
        }
    }
    close(fd);

    printf("loader: loaded '%s' (%u bytes) at 0x%06lX; running...\n",
           path, static_cast<unsigned>(total), static_cast<unsigned long>(base));

    if (setjmp(app_ctx) == 0)
    {
        auto entry = reinterpret_cast<void (*)()>(base);
        entry();   // runs the app; returns here via app_exit() -> longjmp
    }

    printf("loader: app exited (code %d)\n", app_exit_code);
    return app_exit_code;
}
