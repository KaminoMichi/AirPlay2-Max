#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>

static int make_path(const char *name, char *path, size_t size) {
    if (!name || name[0] != '/' || !name[1]) {
        errno = EINVAL;
        return -1;
    }

    for (const char *p = name + 1; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') ||
              (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') ||
              *p == '_' || *p == '-')) {
            errno = EINVAL;
            return -1;
        }
    }

    int n = snprintf(path, size,
                     "/data/local/tmp/ya-airplay/run/shm-%s", name + 1);

    if (n < 0 || (size_t)n >= size) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}

int ya_shm_open(const char *name, int flags, mode_t mode) {
    (void)mode;
    char path[256];

    if (make_path(name, path, sizeof(path)) != 0)
        return -1;

    return open(path, flags | O_CLOEXEC | O_NOFOLLOW, 0600);
}

int ya_shm_unlink(const char *name) {
    char path[256];

    if (make_path(name, path, sizeof(path)) != 0)
        return -1;

    return unlink(path);
}
