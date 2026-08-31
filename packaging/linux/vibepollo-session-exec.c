#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/capability.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static const char session_path[] = "/run/vibepollo/session.env";
static const char command_manifest_path[] = "/etc/vibepollo/session-commands";
static const char service_name[] = "vibepollo";

struct session_identity {
  char user[64];
  char home[PATH_MAX];
  char runtime[PATH_MAX];
  char wayland_display[64];
  char x_display[64];
  uid_t uid;
  gid_t gid;
  gid_t groups[128];
  size_t group_count;
};

static bool parse_id(const char *value, unsigned long *result) {
  char *end = NULL; errno = 0; const unsigned long parsed = strtoul(value, &end, 10);
  if (errno || !value[0] || !end || *end || parsed == 0 || parsed > UINT_MAX) return false;
  *result = parsed; return true;
}

static bool parse_groups(char *value, struct session_identity *identity) {
  char *save = NULL;
  for (char *field = strtok_r(value, ",", &save); field; field = strtok_r(NULL, ",", &save)) {
    unsigned long parsed = 0;
    if (identity->group_count == 128 || !parse_id(field, &parsed)) return false;
    identity->groups[identity->group_count++] = (gid_t) parsed;
  }
  return identity->group_count > 0;
}

static bool copy_field(char *destination, size_t size, const char *value) {
  const size_t length = strlen(value);
  if (!length || length >= size || strchr(value, '\n') || strchr(value, '\r')) return false;
  memcpy(destination, value, length + 1);
  return true;
}

static bool numeric_suffix(const char *value, const char *prefix) {
  const size_t prefix_length = strlen(prefix);
  if (strncmp(value, prefix, prefix_length) || !value[prefix_length]) return false;
  for (const unsigned char *cursor = (const unsigned char *) value + prefix_length; *cursor; ++cursor) {
    if (!isdigit(*cursor)) return false;
  }
  return true;
}

static bool safe_argument(const char *value) {
  if (!value || strlen(value) > 4096) return false;
  for (const unsigned char *cursor = (const unsigned char *) value; *cursor; ++cursor) {
    if (*cursor < 0x20 || *cursor == 0x7f) return false;
  }
  return true;
}

static bool load_identity(struct session_identity *identity, gid_t service_gid) {
  const int fd = open(session_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return false;
  struct stat attributes;
  if (fstat(fd, &attributes) || !S_ISREG(attributes.st_mode) || attributes.st_uid != 0 ||
      attributes.st_gid != service_gid || (attributes.st_mode & 0777) != 0640 || attributes.st_size > 4096) {
    close(fd); errno = EPERM; return false;
  }
  FILE *input = fdopen(fd, "r");
  if (!input) { close(fd); return false; }
  bool have_user = false, have_uid = false, have_gid = false, have_home = false;
  bool have_runtime = false, have_wayland = false, have_groups = false;
  char *line = NULL; size_t capacity = 0;
  while (getline(&line, &capacity, input) >= 0) {
    line[strcspn(line, "\r\n")] = 0; char *separator = strchr(line, '='); if (!separator) continue; *separator++ = 0;
    unsigned long parsed = 0;
    if (!strcmp(line, "user")) {
      if (have_user || !copy_field(identity->user, sizeof(identity->user), separator)) goto invalid;
      have_user = true;
    } else if (!strcmp(line, "uid")) {
      if (have_uid || !parse_id(separator, &parsed)) goto invalid;
      identity->uid = (uid_t) parsed;
      have_uid = true;
    } else if (!strcmp(line, "gid")) {
      if (have_gid || !parse_id(separator, &parsed)) goto invalid;
      identity->gid = (gid_t) parsed;
      have_gid = true;
    } else if (!strcmp(line, "home")) {
      if (have_home || !copy_field(identity->home, sizeof(identity->home), separator)) goto invalid;
      have_home = true;
    } else if (!strcmp(line, "runtime")) {
      if (have_runtime || !copy_field(identity->runtime, sizeof(identity->runtime), separator)) goto invalid;
      have_runtime = true;
    } else if (!strcmp(line, "display")) {
      if (have_wayland || !copy_field(identity->wayland_display, sizeof(identity->wayland_display), separator)) goto invalid;
      have_wayland = true;
    } else if (!strcmp(line, "xdisplay")) {
      if (separator[0] && !copy_field(identity->x_display, sizeof(identity->x_display), separator)) goto invalid;
    } else if (!strcmp(line, "groups")) {
      if (have_groups || !parse_groups(separator, identity)) goto invalid;
      have_groups = true;
    }
  }
  free(line); fclose(input);
  if (!have_user || !have_uid || !have_gid || !have_home || !have_runtime || !have_wayland || !have_groups) { errno = EINVAL; return false; }
  struct passwd *target = getpwuid(identity->uid);
  char expected_runtime[PATH_MAX];
  if (!target || strcmp(target->pw_name, identity->user) || target->pw_gid != identity->gid ||
      strcmp(target->pw_dir, identity->home) ||
      snprintf(expected_runtime, sizeof(expected_runtime), "/run/user/%u", identity->uid) >= (int) sizeof(expected_runtime) ||
      strcmp(expected_runtime, identity->runtime) || !numeric_suffix(identity->wayland_display, "wayland-") ||
      (identity->x_display[0] && !numeric_suffix(identity->x_display, ":"))) { errno = EPERM; return false; }
  return true;
invalid:
  free(line); fclose(input); errno = EINVAL; return false;
}

static bool command_is_authorized(const char *command, gid_t service_gid) {
  if (!command || !command[0] || strlen(command) > 65535 || strchr(command, '\n') || strchr(command, '\r')) { errno = EINVAL; return false; }
  const int fd = open(command_manifest_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return false;
  struct stat attributes;
  if (fstat(fd, &attributes) || !S_ISREG(attributes.st_mode) || attributes.st_uid != 0 ||
      attributes.st_gid != service_gid || (attributes.st_mode & 0777) != 0640 || attributes.st_size > 1048576) {
    close(fd); errno = EPERM; return false;
  }
  FILE *input = fdopen(fd, "r");
  if (!input) { close(fd); return false; }
  bool authorized = false;
  char *line = NULL; size_t capacity = 0;
  while (getline(&line, &capacity, input) >= 0) {
    line[strcspn(line, "\r\n")] = 0;
    if (!strcmp(line, command)) { authorized = true; break; }
  }
  free(line); fclose(input);
  if (!authorized) errno = EPERM;
  return authorized;
}

static bool install_session_environment(const struct session_identity *identity) {
  char data_home[PATH_MAX], bus[PATH_MAX];
  if (snprintf(data_home, sizeof(data_home), "%s/.local/share", identity->home) >= (int) sizeof(data_home) ||
      snprintf(bus, sizeof(bus), "unix:path=%s/bus", identity->runtime) >= (int) sizeof(bus)) return false;
  if (clearenv()) return false;
  return !setenv("HOME", identity->home, 1) && !setenv("USER", identity->user, 1) && !setenv("LOGNAME", identity->user, 1) &&
         !setenv("PATH", "/usr/local/bin:/usr/bin:/bin", 1) && !setenv("LANG", "C.UTF-8", 1) && !setenv("SHELL", "/bin/sh", 1) &&
         !setenv("XDG_DATA_HOME", data_home, 1) && !setenv("XDG_RUNTIME_DIR", identity->runtime, 1) &&
         !setenv("PIPEWIRE_RUNTIME_DIR", identity->runtime, 1) && !setenv("XDG_SESSION_TYPE", "wayland", 1) &&
         !setenv("WAYLAND_DISPLAY", identity->wayland_display, 1) && !setenv("DBUS_SESSION_BUS_ADDRESS", bus, 1) &&
         (!identity->x_display[0] || !setenv("DISPLAY", identity->x_display, 1));
}

static bool drop_to_session(const struct session_identity *identity) {
  if (setgroups(identity->group_count, identity->groups) || setgid(identity->gid) || setuid(identity->uid)) return false;
  cap_t empty = cap_init();
  if (!empty || cap_set_proc(empty)) { if (empty) cap_free(empty); return false; }
  cap_free(empty);
  return !prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) && install_session_environment(identity);
}

static int exec_fixed(const char *path, int argc, char **argv) {
  for (int index = 2; index < argc; ++index) if (!safe_argument(argv[index])) return 126;
  argv[1] = (char *) path;
  execv(path, &argv[1]);
  return errno == ENOENT ? 127 : 126;
}

int main(int argc, char **argv) {
  if (argc < 2) return 2;
  struct passwd *service = getpwnam(service_name);
  if (!service || getuid() != service->pw_uid || geteuid() != service->pw_uid) return 126;

  bool app_command = false;
  const char *fixed_path = NULL;
  char fixed_argument[128] = {0};
  if (!strcmp(argv[1], "app")) {
    if (argc != 4 || !safe_argument(argv[3]) || (argv[3][0] && argv[3][0] != '/') || !command_is_authorized(argv[2], service->pw_gid)) return 126;
    app_command = true;
  } else if (!strcmp(argv[1], "kscreen")) fixed_path = "/usr/bin/kscreen-doctor";
  else if (!strcmp(argv[1], "pactl")) fixed_path = "/usr/bin/pactl";
  else if (!strcmp(argv[1], "parec")) fixed_path = "/usr/bin/parec";
  else if (!strcmp(argv[1], "steam")) {
    if (argc != 3 || !numeric_suffix(argv[2], "")) return 126;
    fixed_path = "/usr/bin/steam";
  } else if (!strcmp(argv[1], "lutris")) {
    if (argc != 3 || !numeric_suffix(argv[2], "")) return 126;
    if (snprintf(fixed_argument, sizeof(fixed_argument), "lutris:rungameid/%s", argv[2]) >= (int) sizeof(fixed_argument)) return 126;
    argv[2] = fixed_argument; fixed_path = "/usr/bin/lutris";
  } else return 126;

  struct session_identity identity = {0};
  if (!load_identity(&identity, service->pw_gid)) { perror("vibepollo-session-exec"); return 126; }
  if (!drop_to_session(&identity)) { perror("vibepollo-session-exec"); return 126; }
  if (app_command) {
    if (argv[3][0] && chdir(argv[3])) { perror("vibepollo-session-exec"); return 126; }
    char *const shell_argv[] = {"/bin/sh", "-c", argv[2], "--", NULL};
    execv(shell_argv[0], shell_argv);
    perror("vibepollo-session-exec"); return errno == ENOENT ? 127 : 126;
  }
  const int result = exec_fixed(fixed_path, argc, argv);
  perror("vibepollo-session-exec"); return result;
}
