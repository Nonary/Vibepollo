#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MAXIMUM_CGROUP_BYTES 4096
#define MAXIMUM_COMMAND_LINE_BYTES 65536
#define DISCOVERY_ATTEMPTS 50
#define DISCOVERY_DELAY_MICROSECONDS 100000

struct display_environment {
  char wayland_display[64];
  char x_display[64];
  char xauthority[PATH_MAX];
};

enum kwin_session_kind {
  KWIN_SESSION_INVALID = 0,
  KWIN_SESSION_DESKTOP,
  KWIN_SESSION_GREETER,
};

static bool copy_value(char *destination, size_t capacity, const char *source, size_t length) {
  if (!destination || !capacity || !source || !length || length >= capacity || destination[0]) {
    return false;
  }
  memcpy(destination, source, length);
  destination[length] = 0;
  return true;
}

static bool decimal_suffix(const char *value, const char *prefix, bool allow_screen) {
  if (!value || !prefix) {
    return false;
  }
  const size_t prefix_length = strlen(prefix);
  if (strncmp(value, prefix, prefix_length)) {
    return false;
  }
  const char *cursor = value + prefix_length;
  if (!isdigit((unsigned char) *cursor)) {
    return false;
  }
  while (isdigit((unsigned char) *cursor)) {
    ++cursor;
  }
  if (allow_screen && *cursor == '.') {
    ++cursor;
    if (!isdigit((unsigned char) *cursor)) {
      return false;
    }
    while (isdigit((unsigned char) *cursor)) {
      ++cursor;
    }
  }
  return !*cursor;
}

static bool valid_wayland_display(const char *value) {
  return decimal_suffix(value, "wayland-", false);
}

static bool valid_xdisplay(const char *value) {
  return decimal_suffix(value, ":", true);
}

static bool xauthority_path_is_confined(const char *runtime, const char *xauthority) {
  if (!runtime || runtime[0] != '/' || !xauthority) {
    return false;
  }
  const size_t runtime_length = strlen(runtime);
  if (!runtime_length || strncmp(runtime, xauthority, runtime_length) || xauthority[runtime_length] != '/') {
    return false;
  }
  const char *basename = xauthority + runtime_length + 1;
  if (strncmp(basename, "xauth_", 6) || !basename[6]) {
    return false;
  }
  for (const char *cursor = basename; *cursor; ++cursor) {
    if (!isalnum((unsigned char) *cursor) && *cursor != '_' && *cursor != '-') {
      return false;
    }
  }
  return true;
}

static bool parse_kwin_arguments(const char *command_line, size_t length, struct display_environment *environment) {
  if (!command_line || !length || !environment || command_line[length - 1] || length > MAXIMUM_COMMAND_LINE_BYTES) {
    return false;
  }
  memset(environment, 0, sizeof(*environment));

  const char *pending = NULL;
  size_t offset = 0;
  while (offset < length) {
    const char *argument = command_line + offset;
    const size_t remaining = length - offset;
    const size_t argument_length = strnlen(argument, remaining);
    if (argument_length == remaining) {
      return false;
    }

    if (pending) {
      bool copied = false;
      if (!strcmp(pending, "--socket")) {
        copied = copy_value(environment->wayland_display, sizeof(environment->wayland_display), argument, argument_length);
      } else if (!strcmp(pending, "--xwayland-display")) {
        copied = copy_value(environment->x_display, sizeof(environment->x_display), argument, argument_length);
      } else if (!strcmp(pending, "--xwayland-xauthority")) {
        copied = copy_value(environment->xauthority, sizeof(environment->xauthority), argument, argument_length);
      }
      if (!copied) {
        return false;
      }
      pending = NULL;
    } else if (!strcmp(argument, "--socket") || !strcmp(argument, "--xwayland-display") || !strcmp(argument, "--xwayland-xauthority")) {
      pending = argument;
    }
    offset += argument_length + 1;
  }

  if (pending || (environment->wayland_display[0] &&
                  !valid_wayland_display(environment->wayland_display))) {
    return false;
  }
  const bool has_x_display = environment->x_display[0];
  const bool has_xauthority = environment->xauthority[0];
  return has_x_display == has_xauthority &&
         (!has_x_display || valid_xdisplay(environment->x_display));
}

static bool read_descriptor_bounded(int descriptor, char *buffer, size_t capacity, size_t *length) {
  if (descriptor < 0 || !buffer || capacity < 2 || !length) {
    return false;
  }
  size_t used = 0;
  bool success = true;
  while (used < capacity) {
    const ssize_t received = read(descriptor, buffer + used, capacity - used);
    if (received > 0) {
      used += (size_t) received;
      continue;
    }
    if (!received) {
      break;
    }
    if (errno == EINTR) {
      continue;
    }
    success = false;
    break;
  }
  if (success && used == capacity) {
    char extra = 0;
    ssize_t received;
    do {
      received = read(descriptor, &extra, 1);
    } while (received < 0 && errno == EINTR);
    if (received != 0) {
      success = false;
    }
  }
  if (!success || !used) {
    return false;
  }
  *length = used;
  return true;
}

static bool read_file_bounded(const char *path, char *buffer, size_t capacity, size_t *length) {
  if (!path) {
    return false;
  }
  const int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    return false;
  }
  const bool success = read_descriptor_bounded(descriptor, buffer, capacity, length);
  close(descriptor);
  return success;
}

static bool read_file_at_bounded(int directory, const char *name, char *buffer, size_t capacity, size_t *length) {
  if (directory < 0 || !name || strchr(name, '/')) {
    return false;
  }
  const int descriptor = openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    return false;
  }
  const bool success = read_descriptor_bounded(descriptor, buffer, capacity, length);
  close(descriptor);
  return success;
}

static bool parse_unified_cgroup(char *contents, char *destination, size_t capacity) {
  if (!contents || !destination || capacity < 2) {
    return false;
  }
  char *line = contents;
  while (line && *line) {
    char *next = strchr(line, '\n');
    if (next) {
      *next = 0;
    }
    if (!strncmp(line, "0::/", 4)) {
      const char *cgroup = line + 3;
      const size_t cgroup_length = strlen(cgroup);
      if (!cgroup_length || cgroup_length >= capacity || strstr(cgroup, "/../") || !strcmp(cgroup, "/..") || (cgroup_length >= 3 && !strcmp(cgroup + cgroup_length - 3, "/.."))) {
        return false;
      }
      memcpy(destination, cgroup, cgroup_length + 1);
      return true;
    }
    line = next ? next + 1 : NULL;
  }
  return false;
}

static bool read_unified_cgroup(pid_t process, char *destination, size_t capacity) {
  char path[64] = {0};
  char contents[MAXIMUM_CGROUP_BYTES] = {0};
  size_t length = 0;
  const int path_length = snprintf(path, sizeof(path), "/proc/%ld/cgroup", (long) process);
  if (path_length <= 0 || (size_t) path_length >= sizeof(path) || !read_file_bounded(path, contents, sizeof(contents) - 1, &length)) {
    return false;
  }
  contents[length] = 0;
  return parse_unified_cgroup(contents, destination, capacity);
}

static bool read_unified_cgroup_at(int process_directory, char *destination, size_t capacity) {
  char contents[MAXIMUM_CGROUP_BYTES] = {0};
  size_t length = 0;
  if (!read_file_at_bounded(process_directory, "cgroup", contents, sizeof(contents) - 1, &length)) {
    return false;
  }
  contents[length] = 0;
  return parse_unified_cgroup(contents, destination, capacity);
}

static bool read_status_uid(int process_directory, uid_t *uid_out) {
  char contents[4096] = {0};
  size_t length = 0;
  if (!uid_out || !read_file_at_bounded(process_directory, "status", contents, sizeof(contents) - 1, &length)) {
    return false;
  }
  contents[length] = 0;
  char *line = contents;
  while (line && *line) {
    char *next = strchr(line, '\n');
    if (next) {
      *next = 0;
    }
    if (!strncmp(line, "Uid:", 4)) {
      unsigned long real_uid = 0;
      if (sscanf(line + 4, " %lu", &real_uid) != 1 || real_uid > UINT_MAX) {
        return false;
      }
      *uid_out = (uid_t) real_uid;
      return true;
    }
    line = next ? next + 1 : NULL;
  }
  return false;
}

static bool process_executable_is_kwin(int process_directory) {
  char executable[PATH_MAX] = {0};
  const ssize_t executable_length = readlinkat(process_directory, "exe", executable, sizeof(executable) - 1);
  if (executable_length > 0 && (size_t) executable_length < sizeof(executable) - 1) {
    executable[executable_length] = 0;
    return !strcmp(executable, "/usr/bin/kwin_wayland");
  }
  if (executable_length >= 0 || (errno != EACCES && errno != EPERM)) {
    return false;
  }
  // The distro KWin carries cap_sys_nice, so it runs non-dumpable and its
  // /proc/PID/exe link is readable by root only. The caller has already
  // confined the candidate to this unit's cgroup and real UID; the readable
  // thread name is the remaining identity check.
  char name[64] = {0};
  size_t length = 0;
  if (!read_file_at_bounded(process_directory, "comm", name, sizeof(name) - 1, &length) || !length) {
    return false;
  }
  name[length] = 0;
  if (name[length - 1] == '\n') {
    name[length - 1] = 0;
  }
  return !strcmp(name, "kwin_wayland");
}

static bool process_is_session_kwin(int process_directory, uid_t uid, const char *expected_cgroup) {
  char cgroup[MAXIMUM_CGROUP_BYTES] = {0};
  uid_t process_uid = 0;
  if (process_directory < 0 || !expected_cgroup) {
    return false;
  }
  // /proc/PID is root-owned for a capability-bearing process, so identify the
  // owner through the world-readable status file instead of the directory.
  if (!read_unified_cgroup_at(process_directory, cgroup, sizeof(cgroup)) ||
      strcmp(cgroup, expected_cgroup) ||
      !read_status_uid(process_directory, &process_uid) || process_uid != uid) {
    return false;
  }
  return process_executable_is_kwin(process_directory);
}

static enum kwin_session_kind session_kind_from_cgroup(const char *cgroup) {
  if (!cgroup) {
    return KWIN_SESSION_INVALID;
  }
  const char *unit = strrchr(cgroup, '/');
  unit = unit ? unit + 1 : cgroup;
  if (!strcmp(unit, "plasma-kwin_wayland.service")) {
    return KWIN_SESSION_DESKTOP;
  }
  if (!strcmp(unit, "plasma-login-kwin_wayland.service")) {
    return KWIN_SESSION_GREETER;
  }
  return KWIN_SESSION_INVALID;
}

static bool discover_session_kwin(int *result, enum kwin_session_kind *kind) {
  char expected_cgroup[MAXIMUM_CGROUP_BYTES] = {0};
  if (!result || !kind ||
      !read_unified_cgroup(getpid(), expected_cgroup, sizeof(expected_cgroup))) {
    return false;
  }
  const enum kwin_session_kind selected_kind =
    session_kind_from_cgroup(expected_cgroup);
  if (selected_kind == KWIN_SESSION_INVALID) {
    return false;
  }
  DIR *processes = opendir("/proc");
  if (!processes) {
    return false;
  }
  int selected = -1;
  struct dirent *entry = NULL;
  while ((entry = readdir(processes))) {
    if (!isdigit((unsigned char) entry->d_name[0])) {
      continue;
    }
    char *end = NULL;
    errno = 0;
    const long parsed = strtol(entry->d_name, &end, 10);
    if (errno || parsed <= 1 || parsed > INT_MAX || !end || *end) {
      continue;
    }
    char process_path[64] = {0};
    const int path_length = snprintf(process_path, sizeof(process_path), "/proc/%ld", parsed);
    if (path_length <= 0 || (size_t) path_length >= sizeof(process_path)) {
      continue;
    }
    const int candidate = open(process_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (candidate < 0) {
      continue;
    }
    if (!process_is_session_kwin(candidate, getuid(), expected_cgroup)) {
      close(candidate);
      continue;
    }
    if (selected >= 0) {
      close(candidate);
      close(selected);
      closedir(processes);
      return false;
    }
    selected = candidate;
  }
  closedir(processes);
  if (selected < 0) {
    return false;
  }
  *result = selected;
  *kind = selected_kind;
  return true;
}

static bool runtime_mode_is_safe(mode_t mode) {
  return (mode & 0700) == 0700 && !(mode & 0066) && !(mode & 0007);
}

static bool root_runtime_parent_is_safe(const char *path) {
  struct stat attributes = {0};
  return path && !lstat(path, &attributes) && S_ISDIR(attributes.st_mode) &&
         attributes.st_uid == 0 && !(attributes.st_mode & 0022);
}

static bool runtime_path_matches_uid(const char *runtime, uid_t uid) {
  char expected[64] = {0};
  const int length = snprintf(expected, sizeof(expected), "/run/user/%lu",
                              (unsigned long) uid);
  return uid && runtime && length > 0 && (size_t) length < sizeof(expected) &&
         !strcmp(runtime, expected);
}

static bool canonical_runtime_is_safe(const char *runtime, uid_t uid) {
  return runtime_path_matches_uid(runtime, uid) &&
         root_runtime_parent_is_safe("/run") &&
         root_runtime_parent_is_safe("/run/user");
}

static bool discover_wayland_socket_at_runtime(
  const char *runtime, uid_t uid, struct display_environment *environment) {
  if (!runtime || runtime[0] != '/' || !environment ||
      environment->wayland_display[0]) {
    return false;
  }
  const int descriptor = open(runtime, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    return false;
  }
  struct stat runtime_attributes = {0};
  if (fstat(descriptor, &runtime_attributes) ||
      !S_ISDIR(runtime_attributes.st_mode) || runtime_attributes.st_uid != uid ||
      !runtime_mode_is_safe(runtime_attributes.st_mode)) {
    close(descriptor);
    return false;
  }
  DIR *entries = fdopendir(descriptor);
  if (!entries) {
    close(descriptor);
    return false;
  }
  bool found = false;
  bool valid = true;
  struct dirent *entry = NULL;
  while ((entry = readdir(entries))) {
    if (!valid_wayland_display(entry->d_name)) {
      continue;
    }
    struct stat attributes = {0};
    if (found || fstatat(descriptor, entry->d_name, &attributes,
                         AT_SYMLINK_NOFOLLOW) ||
        !S_ISSOCK(attributes.st_mode) || attributes.st_uid != uid ||
        !copy_value(environment->wayland_display,
                    sizeof(environment->wayland_display), entry->d_name,
                    strlen(entry->d_name))) {
      valid = false;
      break;
    }
    found = true;
  }
  closedir(entries);
  if (!valid || !found) {
    environment->wayland_display[0] = 0;
    return false;
  }
  return true;
}

static bool discover_wayland_socket(const char *runtime, uid_t uid,
                                    struct display_environment *environment) {
  return canonical_runtime_is_safe(runtime, uid) &&
         discover_wayland_socket_at_runtime(runtime, uid, environment);
}

static bool validate_environment_at_runtime(
  const char *runtime, uid_t uid, const struct display_environment *environment) {
  char wayland_socket[PATH_MAX] = {0};
  struct stat runtime_attributes = {0}, wayland_attributes = {0}, xauth_attributes = {0};
  if (!runtime || !environment || runtime[0] != '/' ||
      !valid_wayland_display(environment->wayland_display)) {
    return false;
  }
  const int socket_length = snprintf(wayland_socket, sizeof(wayland_socket), "%s/%s", runtime, environment->wayland_display);
  if (socket_length <= 0 || (size_t) socket_length >= sizeof(wayland_socket) ||
      lstat(runtime, &runtime_attributes) || !S_ISDIR(runtime_attributes.st_mode) ||
      runtime_attributes.st_uid != uid || !runtime_mode_is_safe(runtime_attributes.st_mode) ||
      lstat(wayland_socket, &wayland_attributes) || !S_ISSOCK(wayland_attributes.st_mode) ||
      wayland_attributes.st_uid != uid) {
    return false;
  }
  const bool has_x_display = environment->x_display[0];
  const bool has_xauthority = environment->xauthority[0];
  if (has_x_display != has_xauthority) {
    return false;
  }
  if (!has_x_display) {
    return true;
  }
  return valid_xdisplay(environment->x_display) &&
         xauthority_path_is_confined(runtime, environment->xauthority) &&
         !lstat(environment->xauthority, &xauth_attributes) &&
         S_ISREG(xauth_attributes.st_mode) && xauth_attributes.st_uid == uid &&
         !(xauth_attributes.st_mode & 0077);
}

static bool validate_environment(const char *runtime, uid_t uid,
                                 const struct display_environment *environment) {
  return canonical_runtime_is_safe(runtime, uid) &&
         validate_environment_at_runtime(runtime, uid, environment);
}

static bool environment_matches_session_kind(
  enum kwin_session_kind kind, const struct display_environment *environment) {
  if (!environment) {
    return false;
  }
  if (kind == KWIN_SESSION_GREETER) {
    return !environment->x_display[0] && !environment->xauthority[0];
  }
  if (kind == KWIN_SESSION_DESKTOP) {
    return valid_wayland_display(environment->wayland_display) &&
           valid_xdisplay(environment->x_display) &&
           environment->xauthority[0];
  }
  return false;
}

static bool read_kwin_environment(int kwin_directory, struct display_environment *environment) {
  char command_line[MAXIMUM_COMMAND_LINE_BYTES] = {0};
  size_t length = 0;
  if (!read_file_at_bounded(kwin_directory, "cmdline", command_line, sizeof(command_line), &length)) {
    return false;
  }
  return parse_kwin_arguments(command_line, length, environment);
}

static bool execution_identity_is_safe(uid_t uid, uid_t effective_uid, gid_t gid, gid_t effective_gid) {
  return uid && effective_uid == uid && effective_gid == gid;
}

int main(void) {
  const uid_t uid = getuid();
  if (!execution_identity_is_safe(uid, geteuid(), getgid(), getegid())) {
    fputs("vibepollo-kwin-session-environment: refusing privileged execution\n", stderr);
    return 1;
  }

  char default_runtime[64] = {0};
  const char *runtime = getenv("XDG_RUNTIME_DIR");
  if (!runtime || !*runtime) {
    const int runtime_length = snprintf(default_runtime, sizeof(default_runtime), "/run/user/%lu", (unsigned long) uid);
    if (runtime_length <= 0 || (size_t) runtime_length >= sizeof(default_runtime)) {
      return 1;
    }
    runtime = default_runtime;
    if (setenv("XDG_RUNTIME_DIR", runtime, 1)) {
      return 1;
    }
  }

  struct display_environment environment = {0};
  bool ready = false;
  for (unsigned int attempt = 0; attempt < DISCOVERY_ATTEMPTS; ++attempt) {
    int kwin_directory = -1;
    enum kwin_session_kind kind = KWIN_SESSION_INVALID;
    if (discover_session_kwin(&kwin_directory, &kind) &&
        read_kwin_environment(kwin_directory, &environment)) {
      if (kind == KWIN_SESSION_GREETER && !environment.wayland_display[0]) {
        discover_wayland_socket(runtime, uid, &environment);
      }
      ready = environment_matches_session_kind(kind, &environment) &&
              validate_environment(runtime, uid, &environment);
    }
    if (kwin_directory >= 0) {
      close(kwin_directory);
    }
    if (ready) {
      break;
    }
    usleep(DISCOVERY_DELAY_MICROSECONDS);
  }
  if (!ready) {
    fputs("vibepollo-kwin-session-environment: KWin did not publish valid display credentials\n", stderr);
    return 1;
  }

  char default_bus[PATH_MAX] = {0};
  const char *bus = getenv("DBUS_SESSION_BUS_ADDRESS");
  if (!bus || !*bus) {
    const int bus_length = snprintf(default_bus, sizeof(default_bus), "unix:path=%s/bus", runtime);
    if (bus_length <= 0 || (size_t) bus_length >= sizeof(default_bus) || setenv("DBUS_SESSION_BUS_ADDRESS", default_bus, 1)) {
      return 1;
    }
  }
  const bool has_x11 = environment.x_display[0];
  if (setenv("WAYLAND_DISPLAY", environment.wayland_display, 1) ||
      (has_x11 && (setenv("DISPLAY", environment.x_display, 1) ||
                   setenv("XAUTHORITY", environment.xauthority, 1))) ||
      prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) {
    return 1;
  }

  alarm(5);
  if (has_x11) {
    execl("/usr/bin/dbus-update-activation-environment",
          "dbus-update-activation-environment", "--systemd",
          "WAYLAND_DISPLAY", "DISPLAY", "XAUTHORITY", (char *) NULL);
  } else {
    execl("/usr/bin/dbus-update-activation-environment",
          "dbus-update-activation-environment", "--systemd",
          "WAYLAND_DISPLAY", (char *) NULL);
  }
  perror("vibepollo-kwin-session-environment: dbus-update-activation-environment");
  return 1;
}
