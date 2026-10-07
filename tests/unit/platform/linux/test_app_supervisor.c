#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/capability.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int vibepollo_app_supervisor_entrypoint(int argc, char **argv);
#define main vibepollo_app_supervisor_entrypoint
#include "../../../../packaging/linux/vibepollo-app-supervisor.c"
#undef main

#define CHECK(expression) do { \
  if (!(expression)) { \
    fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expression); \
    return 1; \
  } \
} while (0)

struct launched_supervisor {
  pid_t pid;
  int watchdog_write;
  int output_read;
};

static bool wait_for_process(pid_t process, unsigned int timeout_milliseconds,
                             int *status) {
  const uint64_t started = monotonic_milliseconds();
  const struct timespec delay = {.tv_sec = 0, .tv_nsec = 20000000};
  if (!started) return false;
  for (;;) {
    const pid_t waited = waitpid(process, status, WNOHANG);
    if (waited == process) return true;
    if (waited < 0 && errno == ECHILD) return true;
    if (waited < 0 && errno != EINTR) return false;
    const uint64_t now = monotonic_milliseconds();
    if (!now || now - started >= timeout_milliseconds) return false;
    (void) nanosleep(&delay, NULL);
  }
}

static bool launch_supervisor(const char *supervisor, const char *self,
                              const char *mode, bool watchdog_already_closed,
                              struct launched_supervisor *launched) {
  int watchdog[2] = {-1, -1}, output[2] = {-1, -1};
  if (!supervisor || supervisor[0] != '/' || !self || self[0] != '/' ||
      !mode || !launched || pipe2(watchdog, O_CLOEXEC) ||
      pipe2(output, O_CLOEXEC)) goto fail;
  if (watchdog_already_closed) {
    close(watchdog[1]);
    watchdog[1] = -1;
  }
  const pid_t child = fork();
  if (child < 0) goto fail;
  if (!child) {
    if (watchdog[1] >= 0) close(watchdog[1]);
    close(output[0]);
    if (dup2(watchdog[0], STDIN_FILENO) < 0 ||
        dup2(output[1], STDOUT_FILENO) < 0) _exit(126);
    if (watchdog[0] != STDIN_FILENO) close(watchdog[0]);
    if (output[1] != STDOUT_FILENO) close(output[1]);
    execl(supervisor, supervisor, "--", self, mode, (char *) NULL);
    _exit(126);
  }
  close(watchdog[0]);
  close(output[1]);
  launched->pid = child;
  launched->watchdog_write = watchdog[1];
  launched->output_read = output[0];
  return true;

fail:
  if (watchdog[0] >= 0) close(watchdog[0]);
  if (watchdog[1] >= 0) close(watchdog[1]);
  if (output[0] >= 0) close(output[0]);
  if (output[1] >= 0) close(output[1]);
  return false;
}

static int application_fd_probe(void) {
  struct stat input = {0}, null_device = {0};
  if (fstat(STDIN_FILENO, &input) || stat("/dev/null", &null_device) ||
      !S_ISCHR(input.st_mode) || input.st_rdev != null_device.st_rdev ||
      prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1) return 10;
  cap_t actual = cap_get_proc();
  cap_t empty = cap_init();
  if (!actual || !empty) return 11;
  const int comparison = cap_compare(actual, empty);
  cap_free(empty);
  cap_free(actual);
  if (comparison != 0 || !ambient_capabilities_are_empty()) return 12;
  for (int descriptor = STDERR_FILENO + 1; descriptor < 1024; ++descriptor) {
    errno = 0;
    if (fcntl(descriptor, F_GETFD) >= 0 || errno != EBADF) return 13;
  }
  return 0;
}

static int stubborn_application(void) {
  struct sigaction ignore = {.sa_handler = SIG_IGN};
  if (sigemptyset(&ignore.sa_mask) || sigaction(SIGTERM, &ignore, NULL)) return 20;
  if (dprintf(STDOUT_FILENO, "%ld\n", (long) getpid()) < 0) return 21;
  for (;;) pause();
}

static bool read_reported_pid(int descriptor, pid_t *reported) {
  struct pollfd ready = {.fd = descriptor, .events = POLLIN};
  int polled;
  do {
    polled = poll(&ready, 1, 2000);
  } while (polled < 0 && errno == EINTR);
  if (polled != 1 || !(ready.revents & POLLIN)) return false;
  char text[64] = {0};
  const ssize_t received = read(descriptor, text, sizeof(text) - 1);
  if (received <= 1) return false;
  char *end = NULL;
  errno = 0;
  const long parsed = strtol(text, &end, 10);
  if (errno || parsed <= 1 || parsed > INT_MAX || !end || *end != '\n') return false;
  *reported = (pid_t) parsed;
  return true;
}

static bool supervisor_hardening_works(void) {
  const pid_t child = fork();
  if (child < 0) return false;
  if (!child) {
    if (!harden_supervisor_process()) _exit(1);
    cap_t actual = cap_get_proc();
    cap_t empty = cap_init();
    const bool capabilities_empty = actual && empty && cap_compare(actual, empty) == 0;
    if (empty) cap_free(empty);
    if (actual) cap_free(actual);
    int subreaper = 0;
    if (!capabilities_empty || !ambient_capabilities_are_empty() ||
        prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1 ||
        prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) != 0 ||
        prctl(PR_GET_CHILD_SUBREAPER, &subreaper, 0, 0, 0) || subreaper != 1) _exit(1);
    _exit(0);
  }
  int status = 0;
  return wait_for_process(child, 1000, &status) &&
         WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "--probe-fds")) return application_fd_probe();
  if (argc == 2 && !strcmp(argv[1], "--stubborn")) return stubborn_application();
  if (argc == 2 && !strcmp(argv[1], "--unexpected-exec")) {
    return write(STDOUT_FILENO, "executed\n", 9) == 9 ? 0 : 22;
  }
  CHECK(argc == 2 && argv[1][0] == '/');

  char self[PATH_MAX] = {0};
  const ssize_t self_length = readlink("/proc/self/exe", self, sizeof(self) - 1);
  CHECK(self_length > 0 && (size_t) self_length < sizeof(self) - 1);
  self[self_length] = 0;

  char *valid[] = {"vibepollo-app-supervisor", "--", "/usr/bin/true", NULL};
  char *relative[] = {"vibepollo-app-supervisor", "--", "usr/bin/true", NULL};
  char *wrong_separator[] = {"vibepollo-app-supervisor", "-", "/usr/bin/true", NULL};
  CHECK(application_arguments_are_safe(3, valid));
  CHECK(!application_arguments_are_safe(3, relative));
  CHECK(!application_arguments_are_safe(3, wrong_separator));
  CHECK(!application_arguments_are_safe(2, valid));
  CHECK(supervisor_hardening_works());

  int watchdog[2] = {-1, -1};
  CHECK(!pipe2(watchdog, O_CLOEXEC));
  CHECK(prepare_watchdog(watchdog[0]));
  CHECK(!prepare_watchdog(watchdog[1]));
  close(watchdog[0]);
  close(watchdog[1]);

  struct launched_supervisor probe = {0};
  CHECK(launch_supervisor(argv[1], self, "--probe-fds", false, &probe));
  int status = 0;
  CHECK(wait_for_process(probe.pid, 3000, &status));
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  close(probe.watchdog_write);
  close(probe.output_read);

  struct launched_supervisor preclosed = {0};
  CHECK(launch_supervisor(argv[1], self, "--unexpected-exec", true, &preclosed));
  CHECK(wait_for_process(preclosed.pid, 3000, &status));
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 128 + SIGTERM);
  char unexpected[16] = {0};
  CHECK(read(preclosed.output_read, unexpected, sizeof(unexpected)) == 0);
  close(preclosed.output_read);

  struct launched_supervisor eof = {0};
  CHECK(launch_supervisor(argv[1], self, "--stubborn", false, &eof));
  pid_t stubborn = 0;
  CHECK(read_reported_pid(eof.output_read, &stubborn));
  const uint64_t cancellation_started = monotonic_milliseconds();
  CHECK(cancellation_started);
  close(eof.watchdog_write);
  CHECK(wait_for_process(eof.pid, 5000, &status));
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 128 + SIGTERM);
  CHECK(monotonic_milliseconds() - cancellation_started < 4500);
  errno = 0;
  CHECK(kill(stubborn, 0) < 0 && errno == ESRCH);
  close(eof.output_read);

  struct launched_supervisor signalled = {0};
  CHECK(launch_supervisor(argv[1], self, "--stubborn", false, &signalled));
  CHECK(read_reported_pid(signalled.output_read, &stubborn));
  CHECK(!kill(signalled.pid, SIGTERM));
  CHECK(wait_for_process(signalled.pid, 5000, &status));
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 128 + SIGTERM);
  errno = 0;
  CHECK(kill(stubborn, 0) < 0 && errno == ESRCH);
  close(signalled.watchdog_write);
  close(signalled.output_read);

  puts("PASS: app supervisor hardening, FD isolation, EOF, and TERM cancellation");
  return 0;
}
