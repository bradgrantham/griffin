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

void debug_serial_putchar(uint8_t ch);
void duart_putchar(uint8_t ch);
uint8_t duart_getchar(void);

static void (*putchar_fn)(uint8_t) = debug_serial_putchar;
static int console_is_duart = 0;

void duart_console_enable(void)
{
    putchar_fn = duart_putchar;
    console_is_duart = 1;
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

int fstat([[maybe_unused]] int file, [[maybe_unused]] struct stat *st)
{
    st->st_mode = S_IFCHR;
    return 0;
}

int isatty([[maybe_unused]] int file)
{
    return 1;
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

ssize_t read(int file, void *buf, size_t len)
{
    char *ptr = (char *)buf;
    if(file < 0) { errno =  EINVAL; return -1; }

    if((file == 0) || (file == 1) || (file == 2)) {
        for (size_t i = 0; i < len; i++)
        {
            if (console_is_duart)
            {
                *ptr++ = (char)duart_getchar();
            }
            else
            {
                *ptr++ = 0;
            }
        }
        return (ssize_t)len;
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

    if(flags & O_APPEND) {
        FatFSFlags |= FA_OPEN_APPEND;
    }
    if(flags & O_CREAT) {
        FatFSFlags |= FA_CREATE_NEW;
    }
    if(flags & O_TRUNC) {
        FatFSFlags |= FA_CREATE_ALWAYS;
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
