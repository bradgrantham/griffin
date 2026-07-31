/* <unistd.h> for Griffin applications.
 *
 * Apps build with a strict -std=c++23 / -std=c23, which sets __STRICT_ANSI__ and
 * so hides newlib's non-ISO declarations -- including usleep(), which sits
 * behind __BSD_VISIBLE in <sys/unistd.h>.  The function itself is provided by
 * apps/lib/syscalls.cpp (newlib has no implementation of it either), so all
 * that is missing is the prototype.
 *
 * Rather than force every app to define _DEFAULT_SOURCE, this header forwards
 * to the real newlib <unistd.h> and adds the declaration.  It is found first
 * because app Makefiles put -I<apps/lib> ahead of the system include path.
 */

#ifndef GRIFFIN_UNISTD_H
#define GRIFFIN_UNISTD_H

#include_next <unistd.h>

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sleep for at least `useconds' microseconds; see apps/lib/syscalls.cpp for the
 * 10 ms tick granularity.  Guarded because a relaxed -std=gnu* build gets the
 * declaration from newlib and would see a duplicate. */
#if !(__XSI_VISIBLE >= 500 && __POSIX_VISIBLE < 200809) && !__BSD_VISIBLE
int usleep(useconds_t useconds);
#endif

#ifdef __cplusplus
}
#endif

#endif /* GRIFFIN_UNISTD_H */
