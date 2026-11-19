#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

// Only /proc lookup is redirected. Socket credentials, connections, filesystem
// validation and all process/session parsers below are production code.
static char fixture_proc[4096];
static char foreign_proc[4096];
static pid_t foreign_pid;
static int proc_error;
static int executable_error;
static ssize_t fixture_readlinkat(int directory, const char *name, char *buffer, size_t capacity) {
  if (executable_error) { errno = executable_error; return -1; }
  return readlinkat(directory, name, buffer, capacity);
}
static int fixture_open(const char *path, int flags, ...) {
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list args;
    va_start(args, flags);
    mode = (mode_t) va_arg(args, int);
    va_end(args);
  }
  if (!strncmp(path, "/proc/", 6)) {
    if (proc_error) { errno = proc_error; return -1; }
    if (foreign_pid > 0 && strtol(path + 6, NULL, 10) == foreign_pid) return open(foreign_proc, flags, mode);
    return open(fixture_proc, flags, mode);
  }
  return open(path, flags, mode);
}
#define open fixture_open
#define readlinkat fixture_readlinkat
#define main production_environment_main
#include "../../../../packaging/linux/vibepollo-kwin-session-environment.c"
#undef main
#undef open
#undef readlinkat

static void write_process_file(const char *directory, const char *name, const char *value) {
  char path[PATH_MAX];
  assert(snprintf(path, sizeof(path), "%s/%s", directory, name) > 0);
  FILE *file = fopen(path, "w");
  assert(file);
  assert(fputs(value, file) >= 0);
  assert(!fclose(file));
}

static void write_fixture(const char *name, const char *value) {
  write_process_file(fixture_proc, name, value);
}

static void executable(const char *name) {
  char path[PATH_MAX];
  assert(snprintf(path, sizeof(path), "%s/exe", fixture_proc) > 0);
  unlink(path);
  assert(!symlink(name, path));
}

static int listener(const char *path) {
  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  assert(fd >= 0);
  struct sockaddr_un address = {.sun_family = AF_UNIX};
  assert(strlen(path) < sizeof(address.sun_path));
  strcpy(address.sun_path, path);
  assert(!bind(fd, (struct sockaddr *) &address, sizeof(address)));
  assert(!listen(fd, 64));
  return fd;
}

int main(void) {
  char runtime[] = "/tmp/vp-sddm.XXXXXX";
  assert(mkdtemp(runtime));
  assert(snprintf(fixture_proc, sizeof(fixture_proc), "%s/proc", runtime) > 0);
  assert(!mkdir(fixture_proc, 0700));
  const uid_t uid = getuid();
  assert(uid != 0); // These fixtures exercise the actual unprivileged boundary.
  char cgroup[256] = {0}, text[1024], identity[64] = {0}, expected[64];
  assert(sddm_session_cgroup(uid, "c1", cgroup, sizeof(cgroup)));
  assert(!sddm_session_cgroup(uid, "../c1", cgroup, sizeof(cgroup)));
  assert(!sddm_session_cgroup(0, "c1", cgroup, sizeof(cgroup)));
  snprintf(text, sizeof(text), "0::%s\n", cgroup);
  write_fixture("cgroup", text);
  snprintf(text, sizeof(text), "Uid:\t%lu\t%lu\t%lu\t%lu\n", (unsigned long) uid,
           (unsigned long) uid, (unsigned long) uid, (unsigned long) uid);
  write_fixture("status", text);
  // The command name intentionally includes ')' to exercise stat parsing.
  write_fixture("stat", "42 (kwin ) wayland) S 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 12345 0\n");
  executable("/usr/bin/kwin_wayland");
  snprintf(expected, sizeof(expected), "sddm-%ld-12345", (long) getpid());
  char socket_path[PATH_MAX];
  snprintf(socket_path, sizeof(socket_path), "%s/wayland-9", runtime);
  int first = listener(socket_path);
  assert(sddm_socket_identity(socket_path, uid, cgroup, identity, sizeof(identity)) == 0);
  assert(!strcmp(identity, expected));
  assert(sddm_check_identity(uid, "c1", identity) == 0);
  char *check_args[] = {"helper", "--sddm-check", "c1", identity, NULL};
  assert(production_environment_main(4, check_args) == 0);
  assert(sddm_check_identity(uid, "c2", identity) == 3);
  assert(sddm_check_identity(uid, "c1", "sddm-0-1") == 3);
  assert(sddm_check_identity(uid, "c1", "sddm-42-01") == 3);
  assert(sddm_check_identity(uid, "c1", "sddm-42-1-extra") == 3);
  assert(sddm_socket_identity(socket_path, uid + 1, cgroup, identity, sizeof(identity)) == 3);
  assert(sddm_socket_identity(socket_path, uid, "/other-session", identity, sizeof(identity)) == 3);
  struct display_environment environment = {0};
  assert(sddm_session_environment_at_runtime(runtime, uid, "c1", &environment, identity, sizeof(identity)) == 0);
  assert(!strcmp(environment.wayland_display, "wayland-9"));
  assert(!environment.x_display[0] && !environment.xauthority[0]);
  assert(sddm_session_environment(runtime, uid, "c1", &environment, identity, sizeof(identity)) == 1);

  // A live but unsupported compositor is distinct from an unavailable peer.
  executable("/usr/bin/weston");
  assert(sddm_socket_identity(socket_path, uid, cgroup, identity, sizeof(identity)) == 5);
  assert(sddm_session_environment_at_runtime(runtime, uid, "c1", &environment, identity, sizeof(identity)) == 5);
  executable("/usr/bin/kwin_wayland_wrapper");
  assert(sddm_socket_identity(socket_path, uid, cgroup, identity, sizeof(identity)) == 5);
  executable("/usr/bin/kwin_wayland");
  executable_error = EACCES; // Capability-bearing KWin is non-dumpable.
  write_fixture("comm", "kwin_wayland\n");
  assert(sddm_socket_identity(socket_path, uid, cgroup, identity, sizeof(identity)) == 0);
  write_fixture("comm", "kwin_wayland_wr\n");
  assert(sddm_socket_identity(socket_path, uid, cgroup, identity, sizeof(identity)) == 5);
  executable_error = 0;
  proc_error = EACCES;
  assert(sddm_check_identity(uid, "c1", expected) == 1);
  assert(sddm_socket_identity(socket_path, uid, cgroup, identity, sizeof(identity)) == 1);
  proc_error = ENOENT;
  assert(sddm_check_identity(uid, "c1", expected) == 3);
  proc_error = 0;
  write_fixture("stat", "42 (kwin) S 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 12346 0\n");
  assert(sddm_check_identity(uid, "c1", expected) == 3); // Reused PID.
  write_fixture("stat", "42 (kwin) Z 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 12345 0\n");
  assert(sddm_check_identity(uid, "c1", expected) == 3);
  write_fixture("stat", "42 (kwin) S 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 12345 0\n");

  char second_path[PATH_MAX];
  snprintf(second_path, sizeof(second_path), "%s/wayland-10", runtime);
  int second = listener(second_path);
  assert(sddm_session_environment_at_runtime(runtime, uid, "c1", &environment, identity, sizeof(identity)) == 3);
  assert(!close(second));
  assert(!unlink(second_path));

  // A different greeter's real listening peer must neither be selected nor
  // turn an unavailable own socket into immediate revocation. The controller
  // independently checks the saved own PID/start before deciding to revoke.
  snprintf(foreign_proc, sizeof(foreign_proc), "%s/foreign-proc", runtime);
  assert(!mkdir(foreign_proc, 0700));
  write_process_file(foreign_proc, "cgroup", "0::/different-session.scope\n");
  snprintf(text, sizeof(text), "Uid:\t%lu\t%lu\t%lu\t%lu\n", (unsigned long) uid,
           (unsigned long) uid, (unsigned long) uid, (unsigned long) uid);
  write_process_file(foreign_proc, "status", text);
  char foreign_path[PATH_MAX];
  snprintf(foreign_path, sizeof(foreign_path), "%s/wayland-11", runtime);
  int ready[2];
  assert(!pipe(ready));
  const pid_t parent = getpid();
  foreign_pid = fork();
  assert(foreign_pid >= 0);
  if (!foreign_pid) {
    assert(!prctl(PR_SET_PDEATHSIG, SIGTERM));
    if (getppid() != parent) _exit(1);
    close(first);
    close(ready[0]);
    listener(foreign_path);
    assert(write(ready[1], "x", 1) == 1);
    close(ready[1]);
    for (;;) pause();
  }
  close(ready[1]);
  char acknowledgement;
  assert(read(ready[0], &acknowledgement, 1) == 1);
  close(ready[0]);
  assert(sddm_session_environment_at_runtime(runtime, uid, "c1", &environment, identity, sizeof(identity)) == 0);
  assert(!strcmp(environment.wayland_display, "wayland-9"));
  assert(!close(first));
  // Stale socket paths and absent sockets are retryable, never new bindings.
  assert(sddm_socket_identity(socket_path, uid, cgroup, identity, sizeof(identity)) == 1);
  assert(sddm_session_environment_at_runtime(runtime, uid, "c1", &environment, identity, sizeof(identity)) == 1);
  assert(sddm_check_identity(uid, "c1", expected) == 0);
  assert(!unlink(socket_path));
  assert(sddm_session_environment_at_runtime(runtime, uid, "c1", &environment, identity, sizeof(identity)) == 1);
  assert(!symlink("/dev/null", socket_path));
  assert(sddm_session_environment_at_runtime(runtime, uid, "c1", &environment, identity, sizeof(identity)) != 0);
  assert(!unlink(socket_path));
  assert(!chmod(runtime, 0777));
  assert(sddm_session_environment_at_runtime(runtime, uid, "c1", &environment, identity, sizeof(identity)) == 3);
  assert(!chmod(runtime, 0700));
  assert(!kill(foreign_pid, SIGTERM));
  assert(waitpid(foreign_pid, NULL, 0) == foreign_pid);
  foreign_pid = 0;
  assert(!unlink(foreign_path));
  const char *foreign_names[] = {"cgroup", "status"};
  for (size_t i = 0; i < sizeof(foreign_names) / sizeof(foreign_names[0]); ++i) {
    char path[PATH_MAX + 16];
    snprintf(path, sizeof(path), "%s/%s", foreign_proc, foreign_names[i]);
    assert(!unlink(path));
  }
  assert(!rmdir(foreign_proc));
  const char *names[] = {"cgroup", "status", "stat", "exe", "comm"};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
    char path[PATH_MAX + 16];
    snprintf(path, sizeof(path), "%s/%s", fixture_proc, names[i]);
    assert(!unlink(path));
  }
  assert(!rmdir(fixture_proc));
  assert(!rmdir(runtime));
  puts("PASS: SDDM socket credentials, session scope, compositor identity, loss and retry boundaries");
  return 0;
}
