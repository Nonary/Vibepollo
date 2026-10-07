#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// This executable is root-owned, mode 0755, and has NO file capabilities.
// The broker execs it only after its permanent session UID/capability drop.
static int run_command_bounded(const char *path, char *const arguments[], unsigned timeout) {
  const pid_t parent = getpid();
  const pid_t child = fork();
  if (child < 0) return 126;
  if (!child) {
    // A startup deadline or broker cancellation must not orphan a Wayland
    // client. No shell or intermediary timeout process owns this child.
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent) _exit(126);
    struct sigaction action = {.sa_handler = SIG_DFL};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, NULL) || sigaction(SIGINT, &action, NULL) ||
        sigaction(SIGHUP, &action, NULL)) _exit(126);
    const int null_output = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (null_output < 0 || dup2(null_output, STDOUT_FILENO) < 0 ||
        dup2(null_output, STDERR_FILENO) < 0) _exit(126);
    if (null_output > STDERR_FILENO) close(null_output);
    execv(path, arguments);
    _exit(126);
  }
  struct timespec started;
  const bool have_clock = !clock_gettime(CLOCK_MONOTONIC, &started);
  int status = 0;
  while (have_clock) {
    const pid_t waited = waitpid(child, &status, WNOHANG);
    if (waited == child) return WIFEXITED(status) ? WEXITSTATUS(status) : 126;
    if (waited < 0 && errno != EINTR) break;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) break;
    const long long elapsed = (now.tv_sec - started.tv_sec) * 1000LL +
                              (now.tv_nsec - started.tv_nsec) / 1000000;
    if (elapsed >= timeout) break;
    const struct timespec delay = {.tv_nsec = 20000000};
    nanosleep(&delay, NULL);
  }
  kill(child, SIGKILL);
  for (unsigned retry = 0; retry < 25; ++retry) {
    const pid_t waited = waitpid(child, &status, WNOHANG);
    if (waited == child || (waited < 0 && errno == ECHILD)) return 124;
    const struct timespec delay = {.tv_nsec = 10000000};
    nanosleep(&delay, NULL);
  }
  // A task stuck in uninterruptible driver work may not reap promptly. Do not
  // retry and accumulate more children/zombies: fail this lease, releasing
  // its D-Bus ownership. The killed child is reparented for eventual reaping.
  _exit(126);
}

#include "vibepollo-display-power.h"

int main(int argc, char **argv) {
  if (argc != 2 || getuid() == 0 || geteuid() != getuid() || getegid() != getgid() ||
      prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1) return 126;
  if (!strcmp(argv[1], "wake")) {
    if (setenv("QT_QPA_PLATFORM", "wayland", 1)) return 126;
    return display_power_wake() && write(STDOUT_FILENO, "R", 1) == 1 ? 0 : 126;
  }
  if (!strcmp(argv[1], "desktop")) return run_display_power(true);
  if (!strcmp(argv[1], "greeter")) return run_display_power(false);
  return 126;
}
