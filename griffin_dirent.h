/* One directory entry as exchanged by the SYS_READDIR syscall.
 *
 * Shared verbatim by the firmware (which fills it from a FatFs FILINFO) and by
 * applications (which iterate it through the apps/lib dirent layer), so it must
 * stay plain C with a fixed, ABI-stable layout.
 *
 * SYS_READDIR is stateless index-iteration: the firmware reopens the directory
 * and skips to `index` on every call, so no handle is leaked if an app dies.
 * It returns 0 when an entry was filled in, 1 when `index` is past the end of
 * the directory, and -errno on failure.
 */

#ifndef GRIFFIN_DIRENT_H
#define GRIFFIN_DIRENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GriffinDirEnt
{
    uint32_t size;      /* file size in bytes; 0 for directories */
    uint32_t is_dir;    /* nonzero if this entry is a directory */
    char     name[256]; /* NUL-terminated LFN (FF_USE_LFN=1, max 255 chars) */
} GriffinDirEnt;

#ifdef __cplusplus
}
#endif

#endif /* GRIFFIN_DIRENT_H */
