/* poll() for Griffin applications.
 *
 * newlib has no <poll.h> at all, so this is the whole interface.  An app
 * Makefile must have -I<apps/lib> on the include path for it to be found.
 *
 * The implementation (apps/lib/syscalls.cpp) is deliberately minimal: it
 * answers POLLIN for the console (fd 0) out of SYS_INPUTREADY, and only for
 * timeout == 0 (a pure poll).  Anything else fails loudly rather than pretending
 * -- see the comments there.
 */

#ifndef GRIFFIN_POLL_H
#define GRIFFIN_POLL_H

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long nfds_t;

struct pollfd
{
    int   fd;         /* file descriptor, or negative to ignore this entry */
    short events;     /* requested events */
    short revents;    /* returned events */
};

#define POLLIN      0x0001    /* data may be read without blocking */
#define POLLPRI     0x0002    /* urgent data (never reported here) */
#define POLLOUT     0x0004    /* writable without blocking */
#define POLLERR     0x0008    /* error condition (output only) */
#define POLLHUP     0x0010    /* hang up (output only) */
#define POLLNVAL    0x0020    /* descriptor not pollable (output only) */

int poll(struct pollfd *fds, nfds_t nfds, int timeout);

#ifdef __cplusplus
}
#endif

#endif /* GRIFFIN_POLL_H */
