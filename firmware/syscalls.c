/* Support files for GNU libc.  Files in the system namespace go here.
   Files in the C namespace (ie those that do not start with an
   underscore) go in .c.  */

#include <_ansi.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/times.h>
#include <errno.h>
#include <reent.h>
#include <unistd.h>
#include <sys/wait.h>

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

    if (heap_end == 0)
    {
        heap_end = &heap_low;
    }

    prev_heap_end = heap_end;

    if (heap_end + incr > &heap_top)
    {
        write(1, "Heap and stack collision\n", 25);
        errno = ENOMEM;
        return (caddr_t) -1;
    }

    heap_end += incr;

    return (caddr_t) prev_heap_end;
}

int getpid(void)
{
    return 1;
}

int kill(int pid, int sig)
{
    errno = EINVAL;
    return -1;
}

void exit (int status)
{
    kill(status, -1);
    while (1) {}
}

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

_ssize_t write (int file,  const void *ptr, size_t len)
{
    if(file < 0) { errno =  EINVAL; return -1; }

    if((file == 0) || (file == 1) || (file == 2))
    {
        int DataIdx;

        for (DataIdx = 0; DataIdx < len; DataIdx++)
        {
            debug_serial_putchar(*(const char *)ptr++); // __io_putchar( *ptr++ );
        }
        return len;
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
        return wrote;
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
    st->st_mode = S_IFCHR;
    return 0;
}

int isatty(int file)
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
            result = f_lseek(&files[myFile], ptr);
        } else if(dir == SEEK_CUR) {
            result = f_lseek(&files[myFile], ptr + f_tell(&files[myFile]));
        } else /* SEEK_END */ {
            result = f_lseek(&files[myFile], f_size(&files[myFile]) - 1 - ptr);
        }
        if(result != FR_OK) {
            printf("XXX lseek: result not OK %d\n", result);
            errno = EIO;
            return -1;
        }
        return f_tell(&files[myFile]);

#else /* not USE_FATFS */
        errno = EIO;
        return -1;
#endif /* USE_FATFS */
    }
}

long read(int file, void *__buf, size_t len)
{
    char *ptr = (char *)__buf;
    if(file < 0) { errno =  EINVAL; return -1; }

    if((file == 0) || (file == 1) || (file == 2)) {
        int DataIdx;

        for (DataIdx = 0; DataIdx < len; DataIdx++)
        {
          *ptr++ = 0; // getchar_timeout_us(1000000); // __io_getchar();
        }
        return len;
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
        return wasRead;
#else /* not USE_FATFS */
        errno = EIO;
        return -1;
#endif /* USE_FATFS */
    }
}

int open(const char *path, int flags, ...)
{
    if(path == NULL) {
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
    FRESULT result = f_open (&files[which], path, FatFSFlags);
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

int wait(int *status)
{
    errno = ECHILD;
    return -1;
}

int unlink(const char *name)
{
    errno = ENOENT;
    return -1;
}

clock_t times(struct tms *buf)
{
    return -1;
}

int stat(const char *__restrict file, struct stat *__restrict st)
{
    st->st_mode = S_IFCHR;
    return 0;
}

int link(const char *old, const char *_new)
{
    errno = EMLINK;
    return -1;
}

int fork(void)
{
    errno = EAGAIN;
    return -1;
}

int execve(const char *name, char *const argv[], char *const env[])
{
    errno = ENOMEM;
    return -1;
}
