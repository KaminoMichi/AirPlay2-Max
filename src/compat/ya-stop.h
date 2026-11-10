#pragma once
#include <stdatomic.h>
#include <poll.h>
#include <sys/socket.h>
/* One context per worker. Init before create; destroy only after successful join.
 * Request is sticky. It never asynchronously unwinds code or unlocks mutexes. */
typedef struct { int wake_fd; atomic_int requested; } ya_stop;
int ya_stop_init(ya_stop *);
int ya_stop_request(ya_stop *);
void ya_stop_destroy(ya_stop *);
int ya_stop_wait(ya_stop *, struct pollfd *, unsigned long, int);
ssize_t ya_stop_recv(ya_stop *, int, void *, size_t, int);
