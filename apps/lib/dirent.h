/* Directory iteration for Griffin applications.
 *
 * newlib ships a <dirent.h> whose <sys/dirent.h> is nothing but an #error, so
 * this header replaces it outright rather than extending it: an app Makefile
 * must put -I<apps/lib> ahead of the system include path, which makes this file
 * win.  It claims newlib's _DIRENT_H_ guard so that a later #include of the
 * newlib header is a no-op instead of an error.
 *
 * Implementation is in apps/lib/syscalls.cpp on top of SYS_READDIR: a DIR holds
 * the directory path and a running index, and each readdir() asks the firmware
 * for the next entry by number.  Only the four calls below are provided —
 * rewinddir/seekdir/telldir/scandir would be easy to add if an app wants them.
 */

#ifndef _DIRENT_H_
#define _DIRENT_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* d_type values (the BSD/Linux DT_* subset FatFs can distinguish) */
#define DT_UNKNOWN  0
#define DT_REG      8
#define DT_DIR      4

struct dirent
{
    unsigned char d_type;    /* DT_REG or DT_DIR */
    char          d_name[256];
};

typedef struct DIR DIR;

DIR           *opendir(const char *path);
struct dirent *readdir(DIR *dirp);
int            closedir(DIR *dirp);

#ifdef __cplusplus
}
#endif

#endif /* _DIRENT_H_ */
