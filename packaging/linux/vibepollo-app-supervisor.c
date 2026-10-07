#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/capability.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum {
  VIBESHINE_APP_MAX_ARGUMENTS = 64,
  VIBESHINE_APP_MAX_ARGUMENT_LENGTH = 65535,
  VIBESHINE_APP_MAX_ARGUMENT_BYTES = 1024 * 1024,
  VIBESHINE_APP_TERM_GRACE_MILLISECONDS = 2000,
  VIBESHINE_APP_KILL_GRACE_MILLISECONDS = 1000,
};

static uint64_t monotonic_milliseconds(void) {
  struct timespec now = {0};
  if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
  return (uint64_t) now.tv_sec * UINT64_C(1000) +
         (uint64_t) now.tv_nsec / UINT64_C(1000000);
}

static bool ambient_capabilities_are_empty(void) {
  bool reached_runtime_limit = false;
  for (unsigned long capability = 0; capability < 4096; ++capability) {
    errno = 0;
    const int ambient = prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_IS_SET,
                              capability, 0, 0);
    if (ambient == 0) continue;
    if (ambient > 0 || errno != EINVAL) {
      if (ambient > 0) errno = EPERM;
      return false;
    }
    reached_runtime_limit = true;
    break;
  }
  if (!reached_runtime_limit) errno = EOVERFLOW;
  return reached_runtime_limit;
}

static bool harden_supervisor_process(void) {
  cap_t empty = cap_init();
  cap_t verified = NULL;
  if (!empty || cap_set_proc(empty)) goto fail;
  verified = cap_get_proc();
  if (!verified) goto fail;
  const int comparison = cap_compare(empty, verified);
  if (comparison != 0) {
    errno = comparison < 0 ? errno : EPERM;
    goto fail;
  }
  if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0) ||
      !ambient_capabilities_are_empty() ||
      prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) ||
      prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1 ||
      prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) ||
      prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) != 0 ||
      prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0)) goto fail;
  int subreaper = 0;
  if (prctl(PR_GET_CHILD_SUBREAPER, &subreaper, 0, 0, 0) || subreaper != 1) {
    errno = errno ? errno : EPERM;
    goto fail;
  }
  cap_free(verified);
  cap_free(empty);
  return true;

fail: {
    const int failure_errno = errno ? errno : EPERM;
    if (verified) cap_free(verified);
    if (empty) cap_free(empty);
    errno = failure_errno;
    return false;
  }
}

static bool application_arguments_are_safe(int argc, char **argv) {
  if (!argv || argc < 3 || argc > VIBESHINE_APP_MAX_ARGUMENTS + 2 ||
      !argv[0] || !argv[1] || strcmp(argv[1], "--") ||
      !argv[2] || argv[2][0] != '/') return false;
  size_t total = 0;
  for (int index = 2; index < argc; ++index) {
    if (!argv[index]) return false;
    const size_t length = strnlen(argv[index], VIBESHINE_APP_MAX_ARGUMENT_LENGTH + 1);
    if (length > VIBESHINE_APP_MAX_ARGUMENT_LENGTH ||
        total > VIBESHINE_APP_MAX_ARGUMENT_BYTES - length - 1) return false;
    total += length + 1;
  }
  return argv[argc] == NULL;
}

static bool prepare_watchdog(int descriptor) {
  struct stat attributes = {0};
  const int flags = fcntl(descriptor, F_GETFL);
  if (flags < 0 || (flags & O_ACCMODE) != O_RDONLY ||
      fstat(descriptor, &attributes) || !S_ISFIFO(attributes.st_mode) ||
      fcntl(descriptor, F_SETFL, flags | O_NONBLOCK)) return false;
  return true;
}

static bool configure_signal_fd(sigset_t *signals, int *descriptor) {
  if (!signals || !descriptor || sigemptyset(signals) ||
      sigaddset(signals, SIGTERM) || sigaddset(signals, SIGINT) ||
      sigaddset(signals, SIGHUP) || sigaddset(signals, SIGCHLD) ||
      sigaddset(signals, SIGPIPE) ||
      sigprocmask(SIG_BLOCK, signals, NULL)) return false;
  *descriptor = signalfd(-1, signals, SFD_CLOEXEC | SFD_NONBLOCK);
  return *descriptor >= 0;
}

static bool reset_child_signals(void) {
  sigset_t empty;
  if (sigemptyset(&empty) || sigprocmask(SIG_SETMASK, &empty, NULL)) return false;
  static const int signals[] = {SIGTERM, SIGINT, SIGHUP, SIGCHLD, SIGPIPE};
  struct sigaction action = {.sa_handler = SIG_DFL};
  if (sigemptyset(&action.sa_mask)) return false;
  for (size_t index = 0; index < sizeof(signals) / sizeof(signals[0]); ++index) {
    if (sigaction(signals[index], &action, NULL)) return false;
  }
  return true;
}

static void execute_application(char **arguments, int startup_fd) {
  if (setpgid(0, 0)) _exit(126);
  unsigned char start = 0;
  ssize_t received;
  do {
    received = read(startup_fd, &start, 1);
  } while (received < 0 && errno == EINTR);
  close(startup_fd);
  if (received != 1 || start != 1) _exit(126);

  // Close the last launch race: if the worker disappeared while this child
  // was held behind the supervisor's start gate, never execute the app.
  struct pollfd watchdog = {.fd = STDIN_FILENO, .events = POLLIN};
  int polled;
  do {
    polled = poll(&watchdog, 1, 0);
  } while (polled < 0 && errno == EINTR);
  if (polled < 0 || (polled == 1 && watchdog.revents)) _exit(126);
  if (!reset_child_signals()) _exit(126);
  const int null_input = open("/dev/null", O_RDONLY | O_CLOEXEC | O_NOCTTY);
  if (null_input < 0 || dup2(null_input, STDIN_FILENO) < 0) _exit(126);
  if (null_input != STDIN_FILENO) close(null_input);
  if (close_range(STDERR_FILENO + 1, UINT_MAX, 0)) _exit(126);
  execv(arguments[0], arguments);
  _exit(errno == ENOENT ? 127 : 126);
}

static void reap_available_children(pid_t application, bool *application_reaped,
                                    int *application_status, bool *have_children) {
  for (;;) {
    int status = 0;
    const pid_t reaped = waitpid(-1, &status, WNOHANG);
    if (reaped > 0) {
      if (reaped == application && !*application_reaped) {
        *application_reaped = true;
        *application_status = status;
      }
      continue;
    }
    if (!reaped) {
      *have_children = true;
      return;
    }
    if (errno == EINTR) continue;
    *have_children = errno != ECHILD;
    return;
  }
}

static bool wait_for_all_children(pid_t application, bool *application_reaped,
                                  int *application_status, uint64_t deadline) {
  const struct timespec delay = {.tv_sec = 0, .tv_nsec = 20000000};
  for (;;) {
    bool have_children = false;
    reap_available_children(application, application_reaped,
                            application_status, &have_children);
    if (!have_children) return true;
    const uint64_t now = monotonic_milliseconds();
    if (!now || now >= deadline) return false;
    (void) nanosleep(&delay, NULL);
  }
}

static bool terminate_application_group(pid_t application, bool *application_reaped,
                                        int *application_status) {
  if (kill(-application, SIGTERM) && errno != ESRCH) return false;
  uint64_t now = monotonic_milliseconds();
  if (!now) return false;
  if (wait_for_all_children(application, application_reaped, application_status,
                            now + VIBESHINE_APP_TERM_GRACE_MILLISECONDS)) return true;
  if (kill(-application, SIGKILL) && errno != ESRCH) return false;
  now = monotonic_milliseconds();
  return now && wait_for_all_children(
                  application, application_reaped, application_status,
                  now + VIBESHINE_APP_KILL_GRACE_MILLISECONDS);
}

enum watchdog_result {
  WATCHDOG_OPEN,
  WATCHDOG_EOF,
  WATCHDOG_ERROR,
};

static enum watchdog_result consume_watchdog(int descriptor, short events) {
  unsigned char buffer[64];
  bool received_data = false;
  for (;;) {
    const ssize_t received = read(descriptor, buffer, sizeof(buffer));
    if (received > 0) {
      received_data = true;
      continue;
    }
    if (!received) return WATCHDOG_EOF;
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
    return WATCHDOG_ERROR;
  }
  // The watchdog is deliberately a one-way liveness token.  Data is not part
  // of its protocol, so fail closed if a compromised writer attempts to turn
  // it into an input channel.
  if (received_data || (events & (POLLERR | POLLNVAL))) return WATCHDOG_ERROR;
  return (events & POLLHUP) ? WATCHDOG_EOF : WATCHDOG_OPEN;
}

static enum watchdog_result inspect_watchdog(int descriptor) {
  struct pollfd watched = {.fd = descriptor, .events = POLLIN};
  int polled;
  do {
    polled = poll(&watched, 1, 0);
  } while (polled < 0 && errno == EINTR);
  if (polled < 0) return WATCHDOG_ERROR;
  return polled ? consume_watchdog(descriptor, watched.revents) : WATCHDOG_OPEN;
}

static int wait_status_exit_code(int status) {
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return 126;
}

static int supervise_application(char **arguments, int watchdog_fd, int signal_fd) {
  const enum watchdog_result initial_watchdog = inspect_watchdog(watchdog_fd);
  if (initial_watchdog != WATCHDOG_OPEN) {
    return initial_watchdog == WATCHDOG_EOF ? 128 + SIGTERM : 126;
  }
  int startup[2] = {-1, -1};
  if (pipe2(startup, O_CLOEXEC)) return 126;
  const pid_t application = fork();
  if (application < 0) {
    close(startup[0]);
    close(startup[1]);
    return 126;
  }
  if (!application) {
    close(startup[1]);
    execute_application(arguments, startup[0]);
  }
  close(startup[0]);
  if (setpgid(application, application) && errno != EACCES && errno != ESRCH) {
    close(startup[1]);
    (void) kill(application, SIGKILL);
    (void) waitpid(application, NULL, 0);
    return 126;
  }
  const pid_t process_group = getpgid(application);
  if (process_group != application) {
    close(startup[1]);
    (void) kill(application, SIGKILL);
    (void) waitpid(application, NULL, 0);
    return 126;
  }

  bool application_reaped = false;
  int application_status = 126 << 8;
  int cancellation_signal = 0;
  bool supervisor_error = false;
  const enum watchdog_result launch_watchdog = inspect_watchdog(watchdog_fd);
  if (launch_watchdog == WATCHDOG_OPEN) {
    unsigned char start = 1;
    ssize_t written;
    do {
      written = write(startup[1], &start, 1);
    } while (written < 0 && errno == EINTR);
    if (written != 1) supervisor_error = true;
  } else {
    cancellation_signal = SIGTERM;
    supervisor_error = launch_watchdog == WATCHDOG_ERROR;
  }
  close(startup[1]);
  while (!application_reaped && !cancellation_signal && !supervisor_error) {
    bool have_children = false;
    reap_available_children(application, &application_reaped,
                            &application_status, &have_children);
    if (application_reaped) break;
    if (!have_children) {
      supervisor_error = true;
      break;
    }

    struct pollfd watched[2] = {
      {.fd = watchdog_fd, .events = POLLIN},
      {.fd = signal_fd, .events = POLLIN},
    };
    int polled;
    do {
      polled = poll(watched, 2, -1);
    } while (polled < 0 && errno == EINTR);
    if (polled < 0) {
      supervisor_error = true;
      break;
    }
    if (watched[0].revents) {
      const enum watchdog_result watchdog =
        consume_watchdog(watchdog_fd, watched[0].revents);
      if (watchdog != WATCHDOG_OPEN) {
        cancellation_signal = SIGTERM;
        supervisor_error = watchdog == WATCHDOG_ERROR;
      }
    }
    if (watched[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
      supervisor_error = true;
    }
    if (watched[1].revents & POLLIN) {
      for (;;) {
        struct signalfd_siginfo signal_info = {0};
        const ssize_t received = read(signal_fd, &signal_info, sizeof(signal_info));
        if (received == (ssize_t) sizeof(signal_info)) {
          if (signal_info.ssi_signo == SIGTERM || signal_info.ssi_signo == SIGINT ||
              signal_info.ssi_signo == SIGHUP) {
            cancellation_signal = (int) signal_info.ssi_signo;
          }
          continue;
        }
        if (received < 0 && errno == EINTR) continue;
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        supervisor_error = true;
        break;
      }
    }
  }

  const bool cleaned = terminate_application_group(
    application, &application_reaped, &application_status);
  if (!cleaned || supervisor_error || !application_reaped) return 126;
  if (cancellation_signal) return 128 + cancellation_signal;
  return wait_status_exit_code(application_status);
}

int main(int argc, char **argv) {
  if (!harden_supervisor_process()) {
    perror("vibepollo-app-supervisor");
    return 126;
  }
  if (!application_arguments_are_safe(argc, argv)) return 2;
  if (!prepare_watchdog(STDIN_FILENO)) return 126;
  sigset_t signals;
  int signal_fd = -1;
  if (!configure_signal_fd(&signals, &signal_fd)) return 126;
  const int result = supervise_application(&argv[2], STDIN_FILENO, signal_fd);
  close(signal_fd);
  return result;
}
