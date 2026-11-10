#ifndef YA_SHM_COMPAT_H
#define YA_SHM_COMPAT_H

#include <sys/types.h>

int ya_shm_open(const char *name, int flags, mode_t mode);
int ya_shm_unlink(const char *name);

#define shm_open ya_shm_open
#define shm_unlink ya_shm_unlink

#endif
