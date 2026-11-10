/* RC1.1 process control. Linux/Android API28; no pidfd requirement (kernel4.9).
 * Only the unreaped direct children of THIS supervisor may receive signals.
 * PID files and /proc identities are diagnostics, never signal authority.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifndef YA_ROOT
#define YA_ROOT "/data/local/tmp/ya-airplay"
#endif
#ifndef YA_SHM
#define YA_SHM "/data/local/tmp/ya-airplay/run/shm-nqptp"
#endif
#ifndef YA_NSD
#define YA_NSD "ya-airplay-nsd-v1"
#endif
#ifndef YA_PTP1
#define YA_PTP1 319
#define YA_PTP2 320
#define YA_PTPCTL 9000
#define YA_RTSP 7000
#endif
#ifndef YA_CLOCK_MS
#define YA_CLOCK_MS 5000
#define YA_RECEIVER_MS 8000
#define YA_STOP_MS 15000
#endif
#ifndef YA_SHELL
#define YA_SHELL "/system/bin/sh"
#endif
#define RUN YA_ROOT "/run"
#define SOCK RUN "/control.sock"
#define SELF YA_ROOT "/bin/ya-supervisor"
_Static_assert(sizeof(SOCK) <= sizeof(((struct sockaddr_un *)0)->sun_path),
               "control path too long");
static volatile sig_atomic_t stop_requested;
static int journal = -1, server = -1, guard = -1;
static char logdir[PATH_MAX], phase[40] = "starting", reason[160] = "none";
typedef struct {
  pid_t pid;
  unsigned long long start;
  char state;
  pid_t ppid;
  dev_t dev;
  ino_t ino;
} identity;
typedef struct {
  const char *name;
  identity id;
  int live, waited, code, signal, core, exec_error, term_sent;
  int64_t term_at;
} child;
static child clock_child = {.name = "nqptp"}, receiver = {.name = "receiver"};
static int64_t ms(void) {
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t))
    _exit(71);
  return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static void nap(void) {
  struct timespec t = {0, 20000000};
  nanosleep(&t, NULL);
}
static void handler(int s) {
  (void)s;
  stop_requested = 1;
}
static void event(const char *fmt, ...) {
  if (journal < 0)
    return;
  dprintf(journal, "mono_ms=%lld ", (long long)ms());
  va_list a;
  va_start(a, fmt);
  vdprintf(journal, fmt, a);
  va_end(a);
  dprintf(journal, "\n");
  fsync(journal);
}
static int textfile(const char *path, const char *s) {
  char tmp[PATH_MAX];
  if (snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, getpid()) >=
      (int)sizeof(tmp))
    return -1;
  int fd =
      open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0)
    return -1;
  size_t n = strlen(s);
  ssize_t r = write(fd, s, n);
  int ok = r == (ssize_t)n && fsync(fd) == 0;
  close(fd);
  if (ok && rename(tmp, path) == 0)
    return 0;
  unlink(tmp);
  return -1;
}
/* comm may contain spaces and ')': field3 begins after the LAST ')'. */
static int stat_identity(pid_t pid, identity *v) {
  char path[80], buf[8192];
  snprintf(path, sizeof(path), "/proc/%d/stat", pid);
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return -1;
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0 || n == (ssize_t)sizeof(buf) - 1) {
    errno = EIO;
    return -1;
  }
  buf[n] = 0;
  char *end = strrchr(buf, ')');
  if (!end || end[1] != ' ') {
    errno = EINVAL;
    return -1;
  }
  char *tail = NULL;
  long p = strtol(buf, &tail, 10);
  if (p != pid || !tail || tail[0] != ' ' || tail[1] != '(') {
    errno = EINVAL;
    return -1;
  }
  char *save = NULL, *s = strtok_r(end + 2, " \n", &save);
  identity x = {.pid = pid};
  for (int i = 0; i <= 19; i++) {
    if (!s) {
      errno = EINVAL;
      return -1;
    }
    if (i == 0) {
      if (strlen(s) != 1)
        return -1;
      x.state = s[0];
    }
    if (i == 1)
      x.ppid = (pid_t)strtol(s, NULL, 10);
    if (i == 19) {
      char *e = NULL;
      errno = 0;
      x.start = strtoull(s, &e, 10);
      if (errno || !e || *e || !x.start) {
        errno = EINVAL;
        return -1;
      }
    }
    s = strtok_r(NULL, " \n", &save);
  }
  *v = x;
  return 0;
}
static int executable(pid_t pid, const char *expected, identity *out) {
  identity a, b;
  struct stat st, ex;
  char path[80], link[PATH_MAX];
  if (stat_identity(pid, &a))
    return -1;
  snprintf(path, sizeof(path), "/proc/%d/exe", pid);
  ssize_t n = readlink(path, link, sizeof(link) - 1);
  if (n < 0)
    return -1;
  link[n] = 0;
  if (strcmp(link, expected) || stat(path, &st) || stat(expected, &ex) ||
      st.st_dev != ex.st_dev || st.st_ino != ex.st_ino) {
    errno = ESTALE;
    return -1;
  }
  if (stat_identity(pid, &b) || a.start != b.start || a.ppid != b.ppid) {
    errno = ESRCH;
    return -1;
  }
  b.dev = st.st_dev;
  b.ino = st.st_ino;
  *out = b;
  return 0;
}
static void state_json(char *s, size_t n) {
  snprintf(s, n,
           "{\"phase\":\"%s\",\"reason\":\"%s\",\"supervisor_pid\":%d,"
           "\"nqptp\":{\"pid\":%d,\"start_ticks\":%llu,\"live\":%d,\"waited\":%"
           "d,\"exit_code\":%d,\"signal\":%d,\"core\":%d,\"exec_errno\":%d,"
           "\"term_sent\":%d},\"receiver\":{\"pid\":%d,\"start_ticks\":%llu,"
           "\"live\":%d,\"waited\":%d,\"exit_code\":%d,\"signal\":%d,\"core\":%"
           "d,\"exec_errno\":%d,\"term_sent\":%d},\"logs\":\"%s\"}\n",
           phase, reason, getpid(), clock_child.id.pid, clock_child.id.start,
           clock_child.live, clock_child.waited, clock_child.code,
           clock_child.signal, clock_child.core, clock_child.exec_error,
           clock_child.term_sent, receiver.id.pid, receiver.id.start,
           receiver.live, receiver.waited, receiver.code, receiver.signal,
           receiver.core, receiver.exec_error, receiver.term_sent, logdir);
}
static int persist(void) {
  char s[4096];
  state_json(s, sizeof(s));
  int r = textfile(RUN "/status.json", s);
  if (r)
    event("status_write_error errno=%d", errno);
  return r;
}
static void reap(child *c) {
  if (!c->live)
    return;
  int s;
  pid_t p = waitpid(c->id.pid, &s, WNOHANG);
  if (p == 0 || (p < 0 && errno == EINTR))
    return;
  if (p < 0) {
    event("%s wait_error errno=%d ownership_lost", c->name, errno);
    strcpy(reason, "waitpid_failed_ownership_unknown"); /* Never send a signal
                                                           after ECHILD. */
    c->live = 0;
    return;
  }
  c->live = 0;
  c->waited = 1;
  c->code = WIFEXITED(s) ? WEXITSTATUS(s) : -1;
  c->signal = WIFSIGNALED(s) ? WTERMSIG(s) : 0;
#ifdef WCOREDUMP
  c->core = WIFSIGNALED(s) ? !!WCOREDUMP(s) : 0;
#endif
  event(
      "%s pid=%d waitpid_exit=%d signal=%d core=%d exec_errno=%d term_sent=%d",
      c->name, p, c->code, c->signal, c->core, c->exec_error, c->term_sent);
  persist();
}
static int terminate(child *c) {
  reap(c);
  if (!c->live)
    return c->id.pid == 0 || c->waited ? 0 : -1;
  if (!c->term_sent) { /* Unreaped direct child: PID cannot be reused, even if
                          exit races here. */
    if (kill(c->id.pid, SIGTERM) && errno != ESRCH) {
      event("%s TERM_error errno=%d", c->name, errno);
      return -1;
    }
    c->term_sent = 1;
    c->term_at = ms();
    event("%s TERM pid=%d", c->name, c->id.pid);
  }
  int64_t end = ms() + YA_STOP_MS;
  while (c->live && ms() < end) {
    reap(c);
    if (c->live)
      nap();
  }
  reap(c);
  return c->live || !c->waited ? -1 : 0;
}
static int shutdown_children(void) {
  strcpy(phase, "stopping");
  persist();
  if (terminate(&receiver)) {
    strcpy(phase, "stop_failed");
    strcpy(reason, "receiver_stop_timeout_or_wait_error");
    persist();
    return -1;
  }
  if (terminate(&clock_child)) {
    strcpy(phase, "stop_failed");
    strcpy(reason, "nqptp_stop_timeout_or_wait_error");
    persist();
    return -1;
  }
  strcpy(phase, !strcmp(reason, "none") ? "stopped" : "failed_clean");
  persist();
  if (textfile(RUN "/clean-stop", "all direct children reaped\n"))
    return -1;
  if (unlink(RUN "/active") && errno != ENOENT)
    return -1;
  return 0;
}
static int write_all(int fd, const void *p, size_t n, int budget) {
  const char *s = p;
  int64_t end = ms() + budget;
  while (n) {
    int left = (int)(end - ms());
    if (left <= 0) {
      errno = ETIMEDOUT;
      return -1;
    }
    struct pollfd f = {fd, POLLOUT, 0};
    int r = poll(&f, 1, left);
    if (r < 0 && errno == EINTR)
      continue;
    if (r <= 0)
      return -1;
    ssize_t z = send(fd, s, n, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (z < 0 && (errno == EAGAIN || errno == EINTR))
      continue;
    if (z <= 0)
      return -1;
    s += z;
    n -= z;
  }
  return 0;
}
static int sockets(const char *table, unsigned port, unsigned long long *inodes,
                   size_t *count) {
  FILE *f = fopen(table, "re");
  if (!f)
    return -1;
  char *s = NULL;
  size_t cap = 0, n = 0;
  int result = 0;
  if (getline(&s, &cap, f) < 0) {
    result = -1;
    goto out;
  }
  while (getline(&s, &cap, f) > 0) {
    char *save = NULL, *tok = strtok_r(s, " \t\n", &save);
    unsigned local = 0, socket_state = 0;
    int columns = 0;
    unsigned long long inode = 0;
    for (int i = 0; tok && i <= 9; i++, tok = strtok_r(NULL, " \t\n", &save)) {
      columns++;
      if (i == 3)
        socket_state = (unsigned)strtoul(tok, NULL, 16);
      if (i == 1) {
        char *colon = strrchr(tok, ':'), *end = NULL;
        if (!colon) {
          result = -1;
          break;
        }
        unsigned long p = strtoul(colon + 1, &end, 16);
        if (!end || *end || p > 65535) {
          result = -1;
          break;
        }
        local = (unsigned)p;
      }
      if (i == 9)
        inode = strtoull(tok, NULL, 10);
    }
    if (columns < 10)
      result = -1;
    if (result)
      break;
    if (local == port && (!strstr(table, "/tcp") || socket_state == 0x0a)) {
      if (n >= 128) {
        result = -1;
        break;
      }
      inodes[n++] = inode;
    }
  }
  if (ferror(f))
    result = -1;
out:
  free(s);
  fclose(f);
  *count = n;
  return result;
}
static int has_inode(pid_t pid, const unsigned long long *inodes,
                     size_t count) {
  char dir[80];
  snprintf(dir, sizeof(dir), "/proc/%d/fd", pid);
  DIR *d = opendir(dir);
  if (!d)
    return -1;
  struct dirent *e;
  int result = 0;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.')
      continue;
    char p[400], buf[100];
    snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
    ssize_t n = readlink(p, buf, sizeof(buf) - 1);
    if (n < 0)
      continue;
    buf[n] = 0;
    unsigned long long x;
    char tail;
    if (sscanf(buf, "socket:[%llu]%c", &x, &tail) != 1)
      continue;
    for (size_t i = 0; i < count; i++)
      if (inodes[i] == x)
        result = 1;
  }
  closedir(d);
  return result;
}
static int owns_port(pid_t pid, int tcp, unsigned port) {
  unsigned long long ids[128];
  size_t n;
  const char *a = tcp ? "/proc/net/tcp" : "/proc/net/udp",
             *b = tcp ? "/proc/net/tcp6" : "/proc/net/udp6";
  if (sockets(a, port, ids, &n))
    return -1;
  int r = has_inode(pid, ids, n);
  if (r)
    return r;
  if (sockets(b, port, ids, &n))
    return -1;
  return has_inode(pid, ids, n);
}
static int occupied(unsigned port) {
  unsigned long long ids[128];
  size_t n;
  for (int i = 0; i < 2; i++) {
    if (sockets(i ? "/proc/net/udp6" : "/proc/net/udp", port, ids, &n))
      return -1;
    if (n)
      return 1;
  }
  return 0;
}
static int validate_ip(const char *s) {
  struct in_addr a;
  return inet_pton(AF_INET, s, &a) == 1 ? 0 : -1;
}
static int preflight(const char *ip) {
  if (validate_ip(ip)) {
    fprintf(stderr, "Invalid canonical IPv4 bind address\n");
    return 64;
  }
  struct stat s;
  if (lstat(YA_SHM, &s) == 0 || errno != ENOENT) {
    fprintf(stderr, "Existing or inaccessible NQPTP shared path; refusing\n");
    return 75;
  }
  unsigned ports[] = {YA_PTP1, YA_PTP2, YA_PTPCTL};
  for (size_t i = 0; i < 3; i++) {
    int r = occupied(ports[i]);
    if (r) {
      fprintf(stderr, "UDP %u %s; refusing\n", ports[i],
              r < 0 ? "inspection unavailable" : "occupied");
      return r < 0 ? 69 : 75;
    }
  }
  return 0;
}
static int shm_ready(pid_t pid) {
  struct stat st;
  int fd = open(YA_SHM, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0)
    return 0;
  uint16_t v = 0;
  int ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
           st.st_uid == getuid() && st.st_size >= 88 &&
           pread(fd, &v, 2, 0) == 2 && v == 11;
  close(fd);
  if (!ok)
    return 0;
  char path[80];
  snprintf(path, sizeof(path), "/proc/%d/maps", pid);
  FILE *f = fopen(path, "re");
  if (!f)
    return 0;
  char *s = NULL;
  size_t n = 0;
  ok = 0;
  while (getline(&s, &n, f) > 0) {
    unsigned ma, mi;
    unsigned long long ino, low, high, offset;
    char permissions[5];
    if (sscanf(s, "%llx-%llx %4s %llx %x:%x %llu", &low, &high, permissions,
               &offset, &ma, &mi, &ino) == 7 &&
        ino == (unsigned long long)st.st_ino && ma == major(st.st_dev) &&
        mi == minor(st.st_dev))
      ok = 1;
  }
  free(s);
  fclose(f);
  return ok;
}
static int nsd_present(void) {
  FILE *f = fopen("/proc/net/unix", "re");
  if (!f)
    return -1;
  char *s = NULL;
  size_t n = 0;
  int found = 0;
  while (getline(&s, &n, f) > 0) {
    char *p = strstr(s, "@" YA_NSD);
    if (p && (p[strlen("@" YA_NSD)] == '\n' || p[strlen("@" YA_NSD)] == 0))
      found = 1;
  }
  if (ferror(f))
    found = -1;
  free(s);
  fclose(f);
  return found;
}
static int bridge(unsigned uid) {
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0)
    return -1;
  struct sockaddr_un a = {.sun_family = AF_UNIX};
  memcpy(a.sun_path + 1, YA_NSD, sizeof(YA_NSD) - 1);
  int r = connect(
      fd, (struct sockaddr *)&a,
      (socklen_t)(offsetof(struct sockaddr_un, sun_path) + sizeof(YA_NSD)));
  if (r && errno == EINPROGRESS) {
    struct pollfd p = {fd, POLLOUT, 0};
    if (poll(&p, 1, 100) > 0) {
      int e;
      socklen_t n = sizeof(e);
      r = getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &n);
      if (!r && e) {
        errno = e;
        r = -1;
      }
    }
  }
  if (!r) {
    struct ucred c;
    socklen_t n = sizeof(c);
    r = getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &c, &n);
    if (!r && c.uid != uid) {
      errno = EACCES;
      r = -1;
    }
  }
  close(fd);
  return r;
}
static int spawn(child *c, const char *binary, char *const args[]) {
  char path[PATH_MAX + 64];
  snprintf(path, sizeof(path), "%s/%s.txt", logdir, c->name);
  int output =
      open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (output < 0) {
    event("%s spawn_log_open_failed errno=%d", c->name, errno);
    return -1;
  }
  int p[2];
  if (pipe2(p, O_CLOEXEC)) {
    event("%s spawn_pipe_failed errno=%d", c->name, errno);
    close(output);
    return -1;
  }
  pid_t owner = getpid(), pid = fork();
  if (pid == 0) {
    close(p[0]);
    /* Reset the inherited supervisor handler before arming PDEATHSIG. */
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
    if (prctl(PR_SET_PDEATHSIG, SIGTERM) || getppid() != owner)
      _exit(125);
    dup2(output, 1);
    dup2(output, 2);
    close(output);
    if (server >= 0)
      close(server);
    if (guard >= 0)
      close(guard);
    if (journal >= 0)
      close(journal);
    execv(binary, args);
    int e = errno;
    ssize_t z = write(p[1], &e, sizeof(e));
    (void)z;
    _exit(127);
  }
  close(p[1]);
  close(output);
  if (pid < 0) {
    event("%s fork_failed errno=%d", c->name, errno);
    close(p[0]);
    return -1;
  }
  c->id.pid = pid;
  c->live = 1;
  c->code = -1;
  persist();
  struct pollfd f = {p[0], POLLIN | POLLHUP, 0};
  int r;
  do {
    r = poll(&f, 1, 2000);
  } while (r < 0 && errno == EINTR && !stop_requested);
  if (r > 0) {
    int e = 0;
    ssize_t n = read(p[0], &e, sizeof(e));
    if (n == (ssize_t)sizeof(e)) {
      c->exec_error = e;
      event("%s exec_failed errno=%d", c->name, e);
    } else if (n != 0)
      c->exec_error = EIO;
  } else
    c->exec_error = ETIMEDOUT;
  close(p[0]);
  event("%s fork pid=%d exec_errno=%d", c->name, pid, c->exec_error);
  return c->exec_error ? -1 : 0;
}
static int ready(child *c, const char *path, int clock_mode) {
  int64_t end = ms() + (clock_mode ? YA_CLOCK_MS : YA_RECEIVER_MS), stable = 0;
  while (ms() < end && !stop_requested) {
    reap(c);
    if (!clock_mode) {
      reap(&clock_child);
      if (!clock_child.live)
        return -1;
    }
    if (!c->live)
      return -1;
    identity x;
    if (executable(c->id.pid, path, &x) == 0 && x.ppid == getpid() &&
        x.state != 'Z' && x.state != 'X') {
      c->id = x;
      int ok = clock_mode ? (owns_port(x.pid, 0, YA_PTP1) == 1 &&
                             owns_port(x.pid, 0, YA_PTP2) == 1 &&
                             owns_port(x.pid, 0, YA_PTPCTL) == 1)
                          : (owns_port(x.pid, 1, YA_RTSP) == 1 &&
                             shm_ready(clock_child.id.pid));
      if (ok) {
        if (!stable)
          stable = ms();
        if (ms() - stable >= 200) {
          event("%s ready pid=%d ppid=%d start_ticks=%llu exe_dev=%llu "
                "exe_ino=%llu path=%s",
                c->name, x.pid, x.ppid, x.start, (unsigned long long)x.dev,
                (unsigned long long)x.ino, path);
          persist();
          return 0;
        }
      } else
        stable = 0;
    } else
      stable = 0;
    nap();
  }
  reap(c);
  event("%s readiness_timeout live=%d udp_ptp1=%d udp_ptp2=%d udp_control=%d "
        "shm_mapped=%d tcp_rtsp=%d",
        c->name, c->live,
        clock_child.live ? owns_port(clock_child.id.pid, 0, YA_PTP1) : -1,
        clock_child.live ? owns_port(clock_child.id.pid, 0, YA_PTP2) : -1,
        clock_child.live ? owns_port(clock_child.id.pid, 0, YA_PTPCTL) : -1,
        clock_child.live ? shm_ready(clock_child.id.pid) : 0,
        receiver.live ? owns_port(receiver.id.pid, 1, YA_RTSP) : -1);
  return -1;
}
static int file_state(const char *path) {
  struct stat s;
  if (lstat(path, &s))
    return errno == ENOENT ? 0 : -1;
  return S_ISREG(s.st_mode) && s.st_uid == getuid() ? 1 : -1;
}
static int clean_offline(void) {
  int active = file_state(RUN "/active"),
      status = file_state(RUN "/status.json"),
      clean = file_state(RUN "/clean-stop");
  if (active != 0 || status < 0 || clean < 0)
    return 0;
  return status == 0 || clean == 1;
}
static int setup_server(void) {
  guard = open(RUN "/supervisor.lock",
               O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (guard < 0 || flock(guard, LOCK_EX | LOCK_NB))
    return -1;
  struct stat st;
  if (!clean_offline()) {
    fprintf(stderr, "Previous supervisor state unclean or inaccessible; "
                    "preserve evidence\n");
    return -1;
  }
  if (lstat(SOCK, &st) == 0) {
    if (!S_ISSOCK(st.st_mode) || st.st_uid != getuid())
      return -1; /* Only after exclusive flock and clean-stop proof. */
    if (unlink(SOCK))
      return -1;
  } else if (errno != ENOENT)
    return -1;
  return 0;
}
/* Bind after fork: Linux SO_PEERCRED records the creator of the listening
 * socket, so it must be the persistent supervisor, not the short-lived client.
 */
static int listen_server(void) {
  server = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (server < 0)
    return -1;
  struct sockaddr_un a = {.sun_family = AF_UNIX};
  strcpy(a.sun_path, SOCK);
  if (bind(server, (struct sockaddr *)&a, sizeof(a)) || chmod(SOCK, 0600) ||
      listen(server, 8))
    return -1;
  return 0;
}
static void finish_server(void) {
  close(server);
  server = -1;
  unlink(SOCK);
  close(guard);
  guard = -1;
}
static void respond(int fd, int result) {
  char s[4096], head[40];
  state_json(s, sizeof(s));
  snprintf(head, sizeof(head), "%d\n", result);
  write_all(fd, head, strlen(head), 250);
  write_all(fd, s, strlen(s), 250);
}
static int supervisor_loop(int startup_fd) {
  if (listen_server()) {
    int result = 69;
    ssize_t z = write(startup_fd, &result, sizeof(result));
    (void)z;
    close(startup_fd);
    finish_server();
    return 69;
  }
  signal(SIGPIPE, SIG_IGN);
  signal(SIGHUP, SIG_IGN);
  struct sigaction a = {.sa_handler = handler};
  sigemptyset(&a.sa_mask);
  sigaction(SIGTERM, &a, NULL);
  sigaction(SIGINT, &a, NULL);
  /* Durable ownership intent exists before any child can be forked. Offline
   * status must never treat a missing status.json as proof after a crash. */
  if (textfile(RUN "/active", "supervisor owns this session\n")) {
    int result = 69;
    ssize_t z = write(startup_fd, &result, sizeof(result));
    (void)z;
    close(startup_fd);
    finish_server();
    return 69;
  }
  if ((unlink(RUN "/clean-stop") && errno != ENOENT) || persist()) {
    int result = 69;
    ssize_t z = write(startup_fd, &result, sizeof(result));
    (void)z;
    close(startup_fd);
    finish_server();
    return 69;
  }
  char *clock_args[] = {YA_ROOT "/bin/ya-nqptp", NULL},
       *recv_args[] = {YA_ROOT "/bin/ya-shairport", "-c", RUN "/shairport.conf",
                       NULL};
  int result = 69;
  if (spawn(&clock_child, clock_args[0], clock_args) ||
      ready(&clock_child, clock_args[0], 1)) {
    strcpy(reason, !clock_child.id.pid      ? "nqptp_not_spawned"
                   : clock_child.waited     ? "nqptp_exited_before_ready"
                   : clock_child.exec_error ? "nqptp_exec_failed"
                                            : "nqptp_alive_ports_not_ready");
  } else if (spawn(&receiver, recv_args[0], recv_args) ||
             ready(&receiver, recv_args[0], 0)) {
    strcpy(reason, !receiver.id.pid  ? "receiver_not_spawned"
                   : receiver.waited ? "receiver_exited_before_ready"
                   : receiver.exec_error
                       ? "receiver_exec_failed"
                       : "receiver_or_shared_memory_not_ready");
  } else if (!clock_child.live || stop_requested) {
    strcpy(reason, "startup_interrupted");
  } else {
    reap(&clock_child);
    if (clock_child.live) {
      strcpy(phase, "running");
      result = 0;
    } else
      strcpy(reason, "nqptp_exited_during_receiver_start");
  }
  persist();
  if (result) {
    shutdown_children();
  }
  if (write(startup_fd, &result, sizeof(result)) != (ssize_t)sizeof(result))
    event("startup_client_disconnected");
  close(startup_fd);
  if (result && !clock_child.live && !receiver.live) {
    finish_server();
    return 69;
  }
  for (;;) {
    reap(&receiver);
    reap(&clock_child);
    if (stop_requested ||
        (!strcmp(phase, "running") && (!clock_child.live || !receiver.live))) {
      if (!stop_requested)
        strcpy(reason, "child_exited_after_ready");
      stop_requested = 0;
      if (!shutdown_children()) {
        finish_server();
        return result;
      }
    }
    struct pollfd p = {server, POLLIN, 0};
    int r = poll(&p, 1, 100);
    if (r < 0 && errno == EINTR)
      continue;
    if (r <= 0)
      continue;
    int fd = accept4(server, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd < 0)
      continue;
    struct ucred peer;
    socklen_t n = sizeof(peer);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &n) ||
        peer.uid != getuid()) {
      close(fd);
      continue;
    }
    struct pollfd q = {fd, POLLIN, 0};
    char cmd = 0;
    if (poll(&q, 1, 250) > 0 && recv(fd, &cmd, 1, 0) == 1) {
      if (cmd == 'S') {
        reap(&clock_child);
        reap(&receiver);
        persist();
        respond(fd,
                !strcmp(phase, "running") && clock_child.live && receiver.live
                    ? 0
                    : 1);
      } else if (cmd == 'T') {
        int z = shutdown_children();
        respond(fd, z ? 1 : 0);
        close(fd);
        if (!z) {
          finish_server();
          return 0;
        }
        continue;
      }
    }
    close(fd);
  }
}
static int control(char command) {
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0)
    return 69;
  struct sockaddr_un a = {.sun_family = AF_UNIX};
  strcpy(a.sun_path, SOCK);
  if (connect(fd, (struct sockaddr *)&a, sizeof(a))) {
    int e = errno;
    close(fd);
    if (e != ENOENT && e != ECONNREFUSED) {
      fprintf(stderr, "Control connection failed errno=%d\n", e);
      return 69;
    }
    int f = open(RUN "/supervisor.lock",
                 O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (f < 0 || flock(f, LOCK_EX | LOCK_NB)) {
      if (f >= 0)
        close(f);
      fprintf(stderr, "Supervisor busy or inaccessible\n");
      return 69;
    }
    int clean = clean_offline();
    close(f);
    printf("{\"supervisor\":\"absent\",\"clean_stop\":%s}\n",
           clean ? "true" : "false");
    if (!clean)
      fprintf(stderr, "Unclean supervisor state: no guessed PID signals; "
                      "inspect run/status.json and events.log\n");
    return clean ? (command == 'T' ? 0 : 3) : 69;
  }
  struct ucred p;
  socklen_t pn = sizeof(p);
  identity id;
  if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &p, &pn) || p.uid != getuid() ||
      executable(p.pid, SELF, &id)) {
    close(fd);
    fprintf(stderr, "Control peer identity mismatch\n");
    return 69;
  }
  if (write_all(fd, &command, 1, 500)) {
    close(fd);
    return 69;
  }
  char buf[8192];
  size_t used = 0;
  int64_t end = ms() + (command == 'T' ? 2 * YA_STOP_MS + 3000 : 2000);
  int ended = 0;
  while (used < sizeof(buf) - 1 && ms() < end) {
    struct pollfd f = {fd, POLLIN, 0};
    int r = poll(&f, 1, (int)(end - ms()));
    if (r < 0 && errno == EINTR)
      continue;
    if (r <= 0)
      break;
    ssize_t n = recv(fd, buf + used, sizeof(buf) - 1 - used, MSG_DONTWAIT);
    if (n < 0 && (errno == EAGAIN || errno == EINTR))
      continue;
    if (n <= 0) {
      ended = n == 0;
      break;
    }
    used += n;
  }
  close(fd);
  buf[used] = 0;
  if (!ended || used < 4 || buf[1] != '\n' ||
      (buf[0] != '0' && buf[0] != '1')) {
    fprintf(stderr, "Control response missing/truncated/timed out\n");
    return 69;
  }
  fputs(buf + 2, stdout);
  return buf[0] == '0' ? 0 : 1;
}
static int start_manager(void) {
  if (setup_server()) {
    perror("Supervisor guard/socket");
    return 75;
  }
  if (snprintf(logdir, sizeof(logdir), RUN "/log-%lld-%d",
               (long long)time(NULL), getpid()) >= (int)sizeof(logdir) ||
      mkdir(logdir, 0700)) {
    perror("logdir");
    finish_server();
    return 69;
  }
  char active[PATH_MAX + 2];
  snprintf(active, sizeof(active), "%s\n", logdir);
  if (textfile(RUN "/active-logdir", active)) {
    finish_server();
    return 69;
  }
  char p[PATH_MAX + 32];
  snprintf(p, sizeof(p), "%s/events.log", logdir);
  journal = open(p, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (journal < 0) {
    finish_server();
    return 69;
  }
  int pipefd[2];
  if (pipe2(pipefd, O_CLOEXEC)) {
    finish_server();
    return 69;
  }
  pid_t pid = fork();
  if (pid == 0) {
    close(pipefd[0]);
    setsid();
    int null = open("/dev/null", O_RDONLY);
    if (null >= 0) {
      dup2(null, 0);
      close(null);
    }
    snprintf(p, sizeof(p), "%s/supervisor.txt", logdir);
    int output =
        open(p, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (output < 0)
      _exit(69);
    dup2(output, 1);
    dup2(output, 2);
    close(output);
    _exit(supervisor_loop(pipefd[1]));
  }
  close(pipefd[1]);
  close(server);
  server = -1;
  close(guard);
  guard = -1;
  close(journal);
  journal = -1;
  if (pid < 0) {
    close(pipefd[0]);
    return 69;
  }
  struct pollfd f = {pipefd[0], POLLIN | POLLHUP, 0};
  int r;
  do {
    r = poll(&f, 1, YA_CLOCK_MS + YA_RECEIVER_MS + 2 * YA_STOP_MS + 5000);
  } while (r < 0 && errno == EINTR);
  int status = 69;
  if (r > 0 &&
      read(pipefd[0], &status, sizeof(status)) != (ssize_t)sizeof(status))
    status = 69;
  close(pipefd[0]);
  if (status)
    fprintf(stderr,
            "Startup failed (code %d). Read run/status.json and %s/events.log; "
            "no NQPTP crash is assumed.\n",
            status, logdir);
  else
    printf("Started; logs=%s\n", logdir);
  return status;
}
static int locked(int argc, char **argv) {
  if (argc < 3 || (strcmp(argv[2], "start") && strcmp(argv[2], "stop") &&
                   strcmp(argv[2], "uninstall")))
    return 64;
  int fd = open(RUN "/operation.lock",
                O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB)) {
    perror("Operation already active or lock unavailable");
    return 75;
  }
  pid_t owner = getpid(), pid = fork();
  if (pid == 0) {
    close(fd);
    if (prctl(PR_SET_PDEATHSIG, SIGTERM) || getppid() != owner)
      _exit(125);
    char script[PATH_MAX];
    snprintf(script, sizeof(script), YA_ROOT "/%s.sh", argv[2]);
    char *args[40] = {YA_SHELL, script, "--locked"};
    if (argc > 37)
      _exit(64);
    for (int i = 3; i < argc; i++)
      args[i] = argv[i];
    execv(YA_SHELL, args);
    _exit(127);
  }
  if (pid < 0) {
    close(fd);
    return 69;
  }
  int status;
  pid_t r;
  do {
    r = waitpid(pid, &status, 0);
  } while (r < 0 && errno == EINTR);
  close(fd);
  if (r < 0)
    return 69;
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
/* All four per-session regular logs are bounded, including NQPTP and this
 * supervisor. Ignore SIGXFSZ so a full diagnostic file does not kill audio. */
static int bound_logs(void) {
  struct rlimit old; if (getrlimit(RLIMIT_FSIZE, &old)) return -1;
  rlim_t cap = 8ULL * 1024 * 1024;
  if (old.rlim_cur != RLIM_INFINITY && old.rlim_cur < cap) cap = old.rlim_cur;
  if (old.rlim_max != RLIM_INFINITY && old.rlim_max < cap) cap = old.rlim_max;
  struct sigaction action = {0}; action.sa_handler = SIG_IGN;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGXFSZ, &action, NULL)) return -1;
  struct rlimit limit = {cap, cap}; return setrlimit(RLIMIT_FSIZE, &limit);
}
int main(int argc, char **argv) {
  umask(077);
  if (bound_logs()) return 69;
  if (getuid() != 0) {
    fprintf(stderr, "Owner-approved root required\n");
    return 77;
  }
  char self[PATH_MAX];
  if (!realpath("/proc/self/exe", self) || strcmp(self, SELF)) {
    fprintf(stderr, "Unexpected helper executable path\n");
    return 64;
  }
  struct stat st;
  if (lstat(YA_ROOT, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid() ||
      (st.st_mode & 0022)) {
    fprintf(stderr,
            "Install directory must be root-owned, not group/world writable\n");
    return 77;
  }
  if (mkdir(RUN, 0700) && errno != EEXIST)
    return 69;
  if (lstat(RUN, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid() ||
      (st.st_mode & 0077))
    return 77;
  if (argc < 2)
    return 64;
  if (!strcmp(argv[1], "locked"))
    return locked(argc, argv);
  if (!strcmp(argv[1], "preflight"))
    return argc == 3 ? preflight(argv[2]) : 64;
  if (!strcmp(argv[1], "nsd-present")) {
    int r = nsd_present();
    return r < 0 ? 69 : r ? 0 : 3;
  }
  if (!strcmp(argv[1], "bridge")) {
    if (argc != 3)
      return 64;
    char *e;
    unsigned long uid = strtoul(argv[2], &e, 10);
    if (*e || uid < 10000 || uid > INT_MAX)
      return 64;
    return bridge((unsigned)uid) ? 69 : 0;
  }
  if (!strcmp(argv[1], "start"))
    return start_manager();
  if (!strcmp(argv[1], "stop"))
    return control('T');
  if (!strcmp(argv[1], "status"))
    return control('S');
  return 64;
}
