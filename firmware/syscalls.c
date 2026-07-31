#include <sys/types.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/times.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdint.h>
#include <stddef.h>

#define USE_FATFS


#include "../griffin_dirent.h"

#ifdef USE_FATFS
#include "ff.h"
#endif /* USE_FATFS */

#undef errno
extern int errno;

void* sbrk(ptrdiff_t incr)
{
    static char *heap_end;
    extern char heap_low;
    extern char heap_top;
    char *prev_heap_end;

    if (heap_end == nullptr)
    {
        heap_end = &heap_low;
    }

    prev_heap_end = heap_end;

    if (heap_end + incr > &heap_top)
    {
        write(1, "Heap and stack collision\n", 25);
        errno = ENOMEM;
        return (void*)-1;
    }

    heap_end += incr;

    return prev_heap_end;
}

int getpid(void)
{
    return 1;
}

int kill([[maybe_unused]] int pid, [[maybe_unused]] int sig)
{
    errno = EINVAL;
    return -1;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-infinite-loop"
[[noreturn]] void exit (int status)
{
    kill(status, -1);
    while (1) {}
}
#pragma GCC diagnostic pop

void _exit (int status)
{
    exit(status);
}

#define MAX_FILES 4
enum { FD_OFFSET = 3 };
static int filesOpened[MAX_FILES];

#ifdef USE_FATFS

static FIL files[MAX_FILES];    /* starting with fd=3, so fd 3 through 3 + MAX_FILES - 1 */

#endif /* USE_FATFS */

#include "keymap.h"

void debug_serial_putchar(uint8_t ch);
void duart_putchar(uint8_t ch);
uint8_t duart_getchar(void);
bool duart_received_ready(void);
int  textport_vt102_putchar(int c);
void early_log_push(uint8_t c);

// Sink enables; toggled by *_console_enable() and read only by
// console_tee_putchar().  write() never inspects these directly.
static int console_duart_enabled    = 0;
static int console_textport_enabled = 0;

// Internal: emit one byte to every currently-enabled sink.  No newline
// translation here — translation is the job of console_tee_putchar().
static void console_tee_emit_byte(uint8_t c)
{
    if (!console_textport_enabled)
    {
        early_log_push(c);
    }
    if (console_duart_enabled)
    {
        duart_putchar(c);
    }
    else if (!console_textport_enabled)
    {
        debug_serial_putchar(c);
    }
    if (console_textport_enabled)
    {
        textport_vt102_putchar((int)c);
    }
}

// All write() bytes flow through here.  Translates bare '\n' into "\r\n"
// so both VT102 and serial terminals advance to column 0.
static void console_tee_putchar(uint8_t c)
{
    if (c == '\n')
    {
        console_tee_emit_byte('\r');
    }
    console_tee_emit_byte(c);
}

static void (*putchar_fn)(uint8_t) = console_tee_putchar;

void duart_console_enable(void)
{
    console_duart_enabled = 1;
}

// Called by textport_console_enable() (C++) after it has replayed and
// frozen the ring.
void textport_console_set_enabled(int on)
{
    console_textport_enabled = on ? 1 : 0;
}

ssize_t write (int file,  const void *ptr, size_t len)
{
    if(file < 0) { errno =  EINVAL; return -1; }

    if((file == 0) || (file == 1) || (file == 2))
    {
        const uint8_t* chars = (const uint8_t*)ptr;
        for (size_t i = 0; i < len; i++)
        {
            putchar_fn(*chars++);
        }
        return (ssize_t) len;
    } else {
        int myFile = file - FD_OFFSET;
        if(!filesOpened[myFile])
        {
            printf("XXX write: file not opened\n");
            errno = EBADF;
            return -1;
        }
#ifdef USE_FATFS
        unsigned int wrote;
        FRESULT result = f_write(&files[myFile], ptr, len, &wrote);
        if(result != FR_OK)
        {
            printf("XXX write: file result %d\n", result);
            errno = EIO;
            return -1;
        }
        return (ssize_t)wrote;
#else /* not USE_FATFS */
        errno = EIO;
        return -1;
#endif /* USE_FATFS */
    }
}

int close(int file)
{
    int myFile = file - FD_OFFSET;
    if(!filesOpened[myFile])
    {
        errno = EBADF;
        return -1;
    }
#ifdef USE_FATFS
    f_close(&files[myFile]);
#endif /* USE_FATFS */
    filesOpened[myFile] = 0;
    return 0;
}

int fstat(int file, struct stat *st)
{
    memset(st, 0, sizeof(*st));

    if((file == 0) || (file == 1) || (file == 2))
    {
        st->st_mode = S_IFCHR;
        return 0;
    }

    int myFile = file - FD_OFFSET;
    if(myFile < 0 || myFile >= MAX_FILES || !filesOpened[myFile])
    {
        errno = EBADF;
        return -1;
    }
#ifdef USE_FATFS
    // Report a real file so newlib fully buffers the stream instead of
    // treating it as an interactive character device.
    st->st_mode = S_IFREG;
    st->st_size = (off_t)f_size(&files[myFile]);
    st->st_blksize = 512;
    return 0;
#else /* not USE_FATFS */
    errno = EBADF;
    return -1;
#endif /* USE_FATFS */
}

int isatty(int file)
{
    if((file == 0) || (file == 1) || (file == 2))
    {
        return 1;
    }
    errno = ENOTTY;
    return 0;
}

off_t lseek (int file, off_t ptr, int dir)
{
    if(file < 0) { errno =  EINVAL; return -1; }

    if((file == 0) || (file == 1) || (file == 2))
    {
        return 0;
    } else {

        int myFile = file - FD_OFFSET;
        if(!filesOpened[myFile]) {
            printf("XXX lseek: file not opened %d\n", myFile);
            errno = EBADF;
            return -1;
        }

#ifdef USE_FATFS

        FRESULT result;
        if(dir == SEEK_SET) {
            result = f_lseek(&files[myFile], (FSIZE_t)ptr);
        } else if(dir == SEEK_CUR) {
            result = f_lseek(&files[myFile], (FSIZE_t)ptr + f_tell(&files[myFile]));
        } else /* SEEK_END */ {
            result = f_lseek(&files[myFile], f_size(&files[myFile]) - 1 - (FSIZE_t)ptr);
        }
        if(result != FR_OK) {
            printf("XXX lseek: result not OK %d\n", result);
            errno = EIO;
            return -1;
        }
        return (off_t)f_tell(&files[myFile]);

#else /* not USE_FATFS */
        errno = EIO;
        return -1;
#endif /* USE_FATFS */
    }
}

// Console input policy in one place, the consumer "higher function": merge
// the keyboard facility (keymap.cpp, PS/2 -> ASCII) with the serial console
// (DUART).  Each facility owns its own ring.  In raw mode the keyboard hands
// back scancodes and the DUART is not consulted.  Non-blocking: returns the
// next byte, or -1 if none is waiting.  Polling this also pumps the keymap
// (translates any pending scancodes) as a side effect.
static int console_try_getchar(void)
{
    if (griffin_is_raw())
    {
        return keyboard_raw_ready() ? (int)keyboard_raw_getchar() : -1;
    }
    if (keyboard_ready())
    {
        return (int)keyboard_getchar();
    }
    if (duart_received_ready())
    {
        return (int)duart_getchar();
    }
    return -1;
}

// --- cooked line discipline (the ICANON/ECHO/ICRNL analog) -----------------
// Applied after the keyboard/DUART merge so both sources get identical
// treatment, and only in cooked mode -- raw scancodes pass through untouched.
// Chars accumulate in cons_line with echo and editing; read() sees nothing
// until the line is terminated by Enter (CR arrives from both the keyboard
// and serial terminals; translated to '\n' a la ICRNL) or by Ctrl-D.  Ctrl-D
// on an empty line makes the next read() return 0 (EOF), so stdio sees a
// clean end-of-file.  Backspace/DEL rubs out; Ctrl-U kills the line.

#define CONS_LINE_MAX 128
static char   cons_line[CONS_LINE_MAX];
static size_t cons_line_len   = 0;   /* chars in the line being edited */
static size_t cons_line_pos   = 0;   /* consumed by read() so far */
static int    cons_line_done  = 0;   /* line terminated, ready to drain */
static int    cons_eof_pending = 0;  /* Ctrl-D on empty line: read() -> 0 */

static void cons_rubout(void)
{
    if (cons_line_len > 0)
    {
        cons_line_len--;
        putchar_fn('\b');
        putchar_fn(' ');
        putchar_fn('\b');
    }
}

// Translate and edit any bytes waiting in the facilities' rings.  Stops as
// soon as a line is complete so type-ahead past Enter stays queued in the
// facilities until the current line is drained.
static void console_cooked_pump(void)
{
    int c;
    while (!cons_line_done && !cons_eof_pending
           && (c = console_try_getchar()) >= 0)
    {
        if (c == '\r')                      /* ICRNL */
        {
            c = '\n';
        }

        if (c == 0x04)                      /* Ctrl-D: VEOF */
        {
            if (cons_line_len == 0) { cons_eof_pending = 1; }
            else                    { cons_line_done = 1; }
        }
        else if (c == 0x08 || c == 0x7F)    /* BS/DEL: rub out */
        {
            cons_rubout();
        }
        else if (c == 0x15)                 /* Ctrl-U: kill line */
        {
            while (cons_line_len > 0) { cons_rubout(); }
        }
        else if (c == '\n')
        {
            putchar_fn('\n');
            if (cons_line_len < CONS_LINE_MAX)
            {
                cons_line[cons_line_len++] = '\n';
            }
            cons_line_done = 1;
        }
        else if (cons_line_len < CONS_LINE_MAX - 1)
        {
            cons_line[cons_line_len++] = (char)c;
            putchar_fn((uint8_t)c);         /* ECHO */
        }
        /* else: line full, drop the char */
    }
}

// Non-blocking: will read(0) return immediately?  Pumps the line editor as a
// side effect, so a poll loop (e.g. rom.cpp's clock loop) echoes and edits
// live while the user types; true only once a whole line (or EOF, or a raw
// scancode) is ready.  This is the no-fcntl substitute for O_NONBLOCK.
bool console_input_ready(void)
{
    if (griffin_is_raw())
    {
        return keyboard_raw_ready();
    }
    console_cooked_pump();
    return cons_line_done || cons_eof_pending;
}

ssize_t read(int file, void *buf, size_t len)
{
    char *ptr = (char *)buf;
    if(file < 0) { errno =  EINVAL; return -1; }

    if(len == 0) { return 0; }

    if((file == 0) || (file == 1) || (file == 2)) {
        if (griffin_is_raw())
        {
            // Raw: block for the first scancode, then return whatever else
            // is immediately available (POSIX short-read semantics).
            int c;
            while ((c = console_try_getchar()) < 0) { }
            size_t n = 0;
            ptr[n++] = (char)c;
            while (n < len && (c = console_try_getchar()) >= 0)
            {
                ptr[n++] = (char)c;
            }
            return (ssize_t)n;
        }

        // Cooked: block until a full line (or EOF) is ready, then return up
        // to len bytes of it.  Never demands len bytes -- newlib's stdio
        // refills with large reads and expects a tty to return a short read
        // at the line boundary.
        while (!cons_line_done && !cons_eof_pending)
        {
            console_cooked_pump();
        }

        if (cons_line_done)
        {
            size_t avail = cons_line_len - cons_line_pos;
            size_t n = (len < avail) ? len : avail;
            memcpy(ptr, &cons_line[cons_line_pos], n);
            cons_line_pos += n;
            if (cons_line_pos == cons_line_len)
            {
                cons_line_len  = 0;
                cons_line_pos  = 0;
                cons_line_done = 0;
            }
            return (ssize_t)n;
        }

        /* EOF: deliver it once, then resume normal reads. */
        cons_eof_pending = 0;
        return 0;
    } else {
        int myFile = file - FD_OFFSET;
        if(!filesOpened[myFile]) {
            printf("XXX read: file not opened %d\n", myFile);
            errno = EBADF;
            return -1;
        }
        unsigned int wasRead;
#ifdef USE_FATFS
        FRESULT result = f_read(&files[myFile], ptr, len, &wasRead);
        if(result != FR_OK) {
            printf("XXX read: result not OK %d\n", result);
            errno = EIO;
            return -1;
        }
        return (ssize_t)wasRead;
#else /* not USE_FATFS */
        errno = EIO;
        return -1;
#endif /* USE_FATFS */
    }
}

int open(const char *path, int flags, ...)
{
    if(path == nullptr) {
        errno = EFAULT;
        return -1;
    }

    int which = 0;
    while(which < MAX_FILES && filesOpened[which]) {
        which++;
    }
    if(which >= MAX_FILES) {
        errno = ENFILE;
        return -1;
    }

#ifdef USE_FATFS
    int FatFSFlags = 0;

    if((flags & O_ACCMODE) == O_RDONLY) {
        FatFSFlags |= FA_READ | FA_OPEN_EXISTING;
    } else if((flags & O_ACCMODE) == O_WRONLY) {
        FatFSFlags |= FA_WRITE;
    } else if((flags & O_ACCMODE) == O_RDWR) {
        FatFSFlags |= FA_WRITE | FA_READ;
    }

    /* POSIX creation semantics -> FatFs creation mode.  FatFs takes exactly
     * one creation mode, so these are chosen, never OR'd: O_APPEND's
     * FA_OPEN_APPEND already implies open-or-create, O_TRUNC must truncate
     * (creating if needed) and O_CREAT alone must open-or-create. */
    if(flags & O_APPEND) {
        FatFSFlags |= FA_OPEN_APPEND;
    } else if(flags & O_TRUNC) {
        FatFSFlags |= FA_CREATE_ALWAYS;
    } else if(flags & O_CREAT) {
        FatFSFlags |= FA_OPEN_ALWAYS;
    }
    errno = 0;
    FRESULT result = f_open (&files[which], path, (BYTE)FatFSFlags);
    if(result) {
        printf("XXX open couldn't open \"%s\" for reading, FatFS result %d\n", path, result);
        errno = EIO;
        return -1;
    }
    filesOpened[which] = 1;

    return which + FD_OFFSET;
#else /* not USE_FATFS */
    errno = EIO;
    return -1;
#endif /* USE_FATFS */
}

/* One directory entry by index, for SYS_READDIR.  Stateless on purpose: the
 * directory is reopened and skipped forward on every call, so no handle can
 * outlive the call (and none leaks if the app dies mid-iteration).  Returns 0
 * when *out was filled in, 1 when index is past the last entry, -errno on
 * failure. */
int sys_readdir(const char *path, int index, GriffinDirEnt *out)
{
    if(path == nullptr || out == nullptr || index < 0) {
        return -EINVAL;
    }

#ifdef USE_FATFS
    DIR dir;
    FILINFO fno;
    fno.fname[0] = '\0';

    if(f_opendir(&dir, path) != FR_OK) {
        return -ENOENT;
    }

    FRESULT result = FR_OK;
    for(int i = 0; i <= index; i++) {
        result = f_readdir(&dir, &fno);
        if(result != FR_OK || fno.fname[0] == '\0') {
            break;
        }
    }
    f_closedir(&dir);

    if(result != FR_OK) {
        return -EIO;
    }
    if(fno.fname[0] == '\0') {
        return 1;                       /* index is past the end */
    }

    int is_dir = (fno.fattrib & AM_DIR) ? 1 : 0;
    out->size = is_dir ? 0u : (uint32_t)fno.fsize;
    out->is_dir = (uint32_t)is_dir;

    size_t namelen = strlen(fno.fname);
    if(namelen > sizeof(out->name) - 1) {
        namelen = sizeof(out->name) - 1;
    }
    memcpy(out->name, fno.fname, namelen);
    out->name[namelen] = '\0';
    return 0;
#else /* not USE_FATFS */
    return -ENOSYS;
#endif /* USE_FATFS */
}

int wait([[maybe_unused]] int *status)
{
    errno = ECHILD;
    return -1;
}

int unlink([[maybe_unused]] const char *name)
{
    errno = ENOENT;
    return -1;
}

clock_t times([[maybe_unused]] struct tms *buf)
{
    return (clock_t)-1;
}

int stat([[maybe_unused]] const char *restrict file, struct stat *restrict st)
{
    st->st_mode = S_IFCHR;
    return 0;
}

int link([[maybe_unused]] const char *old, [[maybe_unused]] const char *new_path)
{
    errno = EMLINK;
    return -1;
}

int fork(void)
{
    errno = EAGAIN;
    return -1;
}

int execve([[maybe_unused]] const char *name, [[maybe_unused]] char *const argv[], [[maybe_unused]] char *const env[])
{
    errno = ENOMEM;
    return -1;
}
