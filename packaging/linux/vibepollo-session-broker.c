#define _GNU_SOURCE

#include "vibepollo-session-protocol.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <regex.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/capability.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static const char session_path[] = "/run/vibepollo/session.env";
static const char command_manifest_path[] = "/etc/vibepollo/session-commands";
static const char command_manifest_header[] = "# vibepollo-session-commands-v2";
static const char service_name[] = "vibepollo";
static const char fixed_path[] = "/usr/local/bin:/usr/bin:/bin";
static const char application_supervisor_path[] =
  "/usr/libexec/vibeshine/vibepollo-app-supervisor";
static const char steam_launch_path[] =
  "/usr/libexec/vibeshine/vibepollo-steam-launch";
// Steam can still be settling its previous transient launch unit when the
// next request arrives.  Retry only the desktop Steam fallback; the direct
// launcher and arbitrary app commands must never be replayed automatically.
static const unsigned int steam_launch_attempts = 2;

static bool sanitize_startup_capabilities(void) {
  const int entry_errno = errno;
  static const cap_value_t allowed[] = {CAP_SETGID, CAP_SETUID, CAP_KILL};
  cap_t original = cap_get_proc();
  cap_t expected = cap_init();
  cap_t verified = NULL;
  if (!original || !expected ||
      cap_set_flag(expected, CAP_PERMITTED,
                   (int) (sizeof(allowed) / sizeof(allowed[0])),
                   allowed, CAP_SET)) goto fail;

  // The file capability is permitted-only.  Reject any broader, partial, or
  // effective/inheritable entry context rather than normalizing it after the
  // dynamic loader has already run.
  const int entry_comparison = cap_compare(original, expected);
  if (entry_comparison != 0) {
    errno = entry_comparison < 0 ? errno : EPERM;
    goto fail;
  }
  if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0) ||
      cap_set_proc(expected)) goto fail;
  verified = cap_get_proc();
  if (!verified) goto fail;
  const int verified_comparison = cap_compare(expected, verified);
  if (verified_comparison != 0) {
    errno = verified_comparison < 0 ? errno : EPERM;
    goto fail;
  }
  bool reached_runtime_limit = false;
  for (unsigned long capability = 0; capability < 4096; ++capability) {
    errno = 0;
    const int ambient = prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_IS_SET,
                              capability, 0, 0);
    if (ambient == 0) continue;
    if (ambient > 0 || errno != EINVAL) {
      if (ambient > 0) errno = EPERM;
      goto fail;
    }
    reached_runtime_limit = true;
    break;
  }
  if (!reached_runtime_limit) {
    errno = EOVERFLOW;
    goto fail;
  }
  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) ||
      prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1) goto fail;

  cap_free(verified);
  cap_free(expected);
  cap_free(original);
  errno = entry_errno;
  return true;

fail: {
    const int failure_errno = errno ? errno : EPERM;
    if (verified) cap_free(verified);
    if (expected) cap_free(expected);
    if (original) cap_free(original);
    errno = failure_errno;
    return false;
  }
}

static bool set_effective_capabilities(const cap_value_t *capabilities,
                                       int capability_count) {
  cap_t desired = cap_get_proc();
  cap_t verified = NULL;
  if (!desired || cap_clear_flag(desired, CAP_EFFECTIVE) ||
      (capability_count > 0 &&
       cap_set_flag(desired, CAP_EFFECTIVE, capability_count,
                    capabilities, CAP_SET)) ||
      cap_set_proc(desired)) goto fail;
  verified = cap_get_proc();
  if (!verified) goto fail;
  const int comparison = cap_compare(desired, verified);
  if (comparison != 0) {
    errno = comparison < 0 ? errno : EPERM;
    goto fail;
  }
  cap_free(verified);
  cap_free(desired);
  return true;

fail:
  if (verified) cap_free(verified);
  if (desired) cap_free(desired);
  return false;
}

static bool signal_cross_uid_worker(pid_t target, int signal_number) {
  static const cap_value_t kill_capability[] = {CAP_KILL};
  if (!set_effective_capabilities(kill_capability, 1)) return false;
  const int signal_result = kill(target, signal_number);
  const int signal_errno = errno;
  if (!set_effective_capabilities(NULL, 0)) return false;
  errno = signal_errno;
  return !signal_result || errno == ESRCH;
}

struct session_identity {
  char session[128];
  char role[16];
  char user[64];
  char home[PATH_MAX];
  char runtime[PATH_MAX];
  char wayland_display[64];
  char x_display[64];
  char xauthority[PATH_MAX];
  unsigned long generation;
  uid_t uid;
  gid_t gid;
  gid_t groups[128];
  size_t group_count;
};

static bool parse_number(const char *value, unsigned long minimum, unsigned long maximum,
                         unsigned long *result) {
  if (!value || !value[0] || !result) return false;
  for (const unsigned char *cursor = (const unsigned char *) value; *cursor; ++cursor) {
    if (*cursor < '0' || *cursor > '9') return false;
  }
  char *end = NULL;
  errno = 0;
  const unsigned long parsed = strtoul(value, &end, 10);
  if (errno || !end || *end || parsed < minimum || parsed > maximum) return false;
  *result = parsed;
  return true;
}

static bool safe_text(const char *value, size_t maximum, bool allow_empty) {
  if (!value) return false;
  const size_t length = strlen(value);
  if ((!allow_empty && !length) || length > maximum) return false;
  for (const unsigned char *cursor = (const unsigned char *) value; *cursor; ++cursor) {
    if (*cursor < 0x20 || *cursor == 0x7f) return false;
  }
  return true;
}

static bool copy_field(char *destination, size_t size, const char *value, bool allow_empty) {
  if (!safe_text(value, size - 1, allow_empty)) return false;
  memcpy(destination, value, strlen(value) + 1);
  return true;
}

static bool safe_identifier(const char *value) {
  if (!safe_text(value, 127, false)) return false;
  for (const unsigned char *cursor = (const unsigned char *) value; *cursor; ++cursor) {
    if (!isalnum(*cursor) && *cursor != '_' && *cursor != '-' && *cursor != '.') return false;
  }
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

static bool artwork_request_is_safe(const char *value, const char *prefix, unsigned long maximum) {
  unsigned long id = 0;
  return numeric_suffix(value, prefix) && parse_number(value + strlen(prefix), 1, maximum, &id);
}

static bool valid_xdisplay(const char *value) {
  if (!value || *value++ != ':' || !isdigit((unsigned char) *value)) return false;
  while (isdigit((unsigned char) *value)) ++value;
  if (*value == '.') {
    ++value;
    if (!isdigit((unsigned char) *value)) return false;
    while (isdigit((unsigned char) *value)) ++value;
  }
  return !*value;
}

static bool parse_groups(char *value, struct session_identity *identity) {
  char *save = NULL;
  for (char *field = strtok_r(value, ",", &save); field; field = strtok_r(NULL, ",", &save)) {
    unsigned long parsed = 0;
    if (identity->group_count == 128 || !parse_number(field, 1, UINT_MAX, &parsed)) return false;
    identity->groups[identity->group_count++] = (gid_t) parsed;
  }
  return identity->group_count > 0;
}

static bool xauthority_mode_is_safe(mode_t mode) {
  return !(mode & 0077);
}

static bool runtime_mode_is_safe(mode_t mode) {
  return (mode & 0777) == 0700;
}

static bool xauthority_path_is_confined(const struct session_identity *identity) {
  const size_t runtime_length = strlen(identity->runtime);
  const size_t xauthority_length = strlen(identity->xauthority);
  return runtime_length &&
         !strncmp(identity->xauthority, identity->runtime, runtime_length) &&
         identity->xauthority[runtime_length] == '/' &&
         !strstr(identity->xauthority, "/../") &&
         (xauthority_length < 3 || strcmp(identity->xauthority + xauthority_length - 3, "/.."));
}

static bool validate_xauthority(const struct session_identity *identity) {
  if (!identity->xauthority[0]) return !strcmp(identity->role, "greeter");
  if (!xauthority_path_is_confined(identity)) return false;
  struct stat attributes;
  return !lstat(identity->xauthority, &attributes) && S_ISREG(attributes.st_mode) &&
         attributes.st_uid == identity->uid && xauthority_mode_is_safe(attributes.st_mode);
}

static bool validate_session_endpoints(const struct session_identity *identity) {
  struct stat attributes;
  char wayland[PATH_MAX], bus[PATH_MAX];
  if (lstat(identity->runtime, &attributes) || !S_ISDIR(attributes.st_mode) ||
      attributes.st_uid != identity->uid || !runtime_mode_is_safe(attributes.st_mode) ||
      snprintf(wayland, sizeof(wayland), "%s/%s", identity->runtime,
               identity->wayland_display) >= (int) sizeof(wayland) ||
      lstat(wayland, &attributes) || !S_ISSOCK(attributes.st_mode) ||
      attributes.st_uid != identity->uid ||
      snprintf(bus, sizeof(bus), "%s/bus", identity->runtime) >= (int) sizeof(bus) ||
      lstat(bus, &attributes) || !S_ISSOCK(attributes.st_mode) ||
      attributes.st_uid != identity->uid) return false;
  return true;
}

static int compare_gid(const void *left, const void *right) {
  const gid_t first = *(const gid_t *) left;
  const gid_t second = *(const gid_t *) right;
  return first < second ? -1 : first > second;
}

static bool groups_match_account(struct session_identity *identity) {
  gid_t expected[128];
  int expected_count = (int) (sizeof(expected) / sizeof(expected[0]));
  if (getgrouplist(identity->user, identity->gid, expected, &expected_count) < 0 ||
      expected_count < 1 || expected_count > (int) (sizeof(expected) / sizeof(expected[0])) ||
      (size_t) expected_count != identity->group_count) return false;
  qsort(expected, (size_t) expected_count, sizeof(expected[0]), compare_gid);
  qsort(identity->groups, identity->group_count, sizeof(identity->groups[0]), compare_gid);
  for (size_t index = 0; index < identity->group_count; ++index) {
    if (identity->groups[index] != expected[index] ||
        (index && identity->groups[index] == identity->groups[index - 1])) return false;
  }
  return true;
}

static bool load_identity(struct session_identity *identity, gid_t service_gid,
                          uint64_t expected_generation) {
  const int fd = open(session_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return false;
  struct stat attributes;
  if (fstat(fd, &attributes) || !S_ISREG(attributes.st_mode) || attributes.st_uid != 0 ||
      attributes.st_gid != service_gid || (attributes.st_mode & 0777) != 0640 || attributes.st_size > 8192) {
    close(fd);
    errno = EPERM;
    return false;
  }
  FILE *input = fdopen(fd, "r");
  if (!input) {
    close(fd);
    return false;
  }

  bool have_session = false, have_generation = false, have_role = false, have_user = false;
  bool have_uid = false, have_gid = false, have_home = false, have_runtime = false;
  bool have_wayland = false, have_xdisplay = false, have_xauthority = false, have_groups = false;
  char *line = NULL;
  size_t capacity = 0;
  while (getline(&line, &capacity, input) >= 0) {
    line[strcspn(line, "\r\n")] = 0;
    char *separator = strchr(line, '=');
    if (!separator) goto invalid;
    *separator++ = 0;
    unsigned long parsed = 0;
    if (!strcmp(line, "session")) {
      if (have_session || !copy_field(identity->session, sizeof(identity->session), separator, false)) goto invalid;
      have_session = true;
    } else if (!strcmp(line, "generation")) {
      if (have_generation || !parse_number(separator, 1, ULONG_MAX, &parsed) ||
          parsed != expected_generation) goto invalid;
      identity->generation = parsed;
      have_generation = true;
    } else if (!strcmp(line, "role")) {
      if (have_role || !copy_field(identity->role, sizeof(identity->role), separator, false)) goto invalid;
      have_role = true;
    } else if (!strcmp(line, "user")) {
      if (have_user || !copy_field(identity->user, sizeof(identity->user), separator, false)) goto invalid;
      have_user = true;
    } else if (!strcmp(line, "uid")) {
      if (have_uid || !parse_number(separator, 1, UINT_MAX, &parsed)) goto invalid;
      identity->uid = (uid_t) parsed;
      have_uid = true;
    } else if (!strcmp(line, "gid")) {
      if (have_gid || !parse_number(separator, 1, UINT_MAX, &parsed)) goto invalid;
      identity->gid = (gid_t) parsed;
      have_gid = true;
    } else if (!strcmp(line, "home")) {
      if (have_home || !copy_field(identity->home, sizeof(identity->home), separator, false)) goto invalid;
      have_home = true;
    } else if (!strcmp(line, "runtime")) {
      if (have_runtime || !copy_field(identity->runtime, sizeof(identity->runtime), separator, false)) goto invalid;
      have_runtime = true;
    } else if (!strcmp(line, "display")) {
      if (have_wayland || !copy_field(identity->wayland_display, sizeof(identity->wayland_display), separator, false)) goto invalid;
      have_wayland = true;
    } else if (!strcmp(line, "xdisplay")) {
      if (have_xdisplay || !copy_field(identity->x_display, sizeof(identity->x_display), separator, true)) goto invalid;
      have_xdisplay = true;
    } else if (!strcmp(line, "xauthority")) {
      if (have_xauthority || !copy_field(identity->xauthority, sizeof(identity->xauthority), separator, true)) goto invalid;
      have_xauthority = true;
    } else if (!strcmp(line, "groups")) {
      if (have_groups || !parse_groups(separator, identity)) goto invalid;
      have_groups = true;
    } else {
      goto invalid;
    }
  }
  free(line);
  fclose(input);

  if (!have_session || !have_generation || !have_role || !have_user || !have_uid || !have_gid ||
      !have_home || !have_runtime || !have_wayland || !have_xdisplay || !have_xauthority || !have_groups ||
      !safe_identifier(identity->session) ||
      (strcmp(identity->role, "desktop") && strcmp(identity->role, "greeter"))) {
    errno = EINVAL;
    return false;
  }
  struct passwd *target = getpwuid(identity->uid);
  char expected_runtime[PATH_MAX];
  if (!target || strcmp(target->pw_name, identity->user) || target->pw_gid != identity->gid ||
      strcmp(target->pw_dir, identity->home) ||
      snprintf(expected_runtime, sizeof(expected_runtime), "/run/user/%u", identity->uid) >= (int) sizeof(expected_runtime) ||
      strcmp(expected_runtime, identity->runtime) || !numeric_suffix(identity->wayland_display, "wayland-") ||
      (identity->x_display[0] && !valid_xdisplay(identity->x_display)) ||
      (!strcmp(identity->role, "desktop") && (!identity->x_display[0] || !identity->xauthority[0])) ||
      !groups_match_account(identity)) {
    errno = EPERM;
    return false;
  }
  return true;

invalid:
  free(line);
  fclose(input);
  errno = EINVAL;
  return false;
}

static bool parse_first_shell_word(const char *command, char *word, size_t word_size) {
  if (!safe_text(command, 65535, false) || !word || word_size < 2) return false;
  const unsigned char *cursor = (const unsigned char *) command;
  while (*cursor == ' ') ++cursor;
  if (!*cursor) return false;

  enum quote_state { UNQUOTED, SINGLE_QUOTED, DOUBLE_QUOTED } quote = UNQUOTED;
  size_t length = 0;
  while (*cursor) {
    unsigned char character = *cursor++;
    if (quote == UNQUOTED) {
      if (character == ' ') break;
      if (character == '\'') {
        quote = SINGLE_QUOTED;
        continue;
      }
      if (character == '"') {
        quote = DOUBLE_QUOTED;
        continue;
      }
      if (character == '\\') {
        if (!*cursor) return false;
        character = *cursor++;
      }
    } else if (quote == SINGLE_QUOTED) {
      if (character == '\'') {
        quote = UNQUOTED;
        continue;
      }
    } else {
      if (character == '"') {
        quote = UNQUOTED;
        continue;
      }
      if (character == '\\' && (*cursor == '$' || *cursor == '`' || *cursor == '"' || *cursor == '\\')) {
        character = *cursor++;
      }
    }
    if (length + 1 >= word_size) return false;
    word[length++] = (char) character;
  }
  if (quote != UNQUOTED || !length) return false;
  word[length] = 0;
  return true;
}

static bool executable_parent_directory(const char *command, char *directory,
                                        size_t directory_size) {
  static const char *const search_directories[] = {
    "/usr/local/bin", "/usr/bin", "/bin"
  };
  if (!directory || directory_size < 2) return false;
  directory[0] = 0;

  char executable[PATH_MAX], resolved[PATH_MAX];
  if (!parse_first_shell_word(command, executable, sizeof(executable)) ||
      strstr(executable, "://")) return false;
  if (executable[0] == '/') {
    if (!copy_field(resolved, sizeof(resolved), executable, false)) return false;
  } else {
    if (strchr(executable, '/')) return false;
    bool found = false;
    for (size_t index = 0; index < sizeof(search_directories) / sizeof(search_directories[0]); ++index) {
      struct stat attributes;
      if (snprintf(resolved, sizeof(resolved), "%s/%s", search_directories[index], executable) >=
            (int) sizeof(resolved) ||
          stat(resolved, &attributes) || !S_ISREG(attributes.st_mode) || access(resolved, X_OK)) continue;
      found = true;
      break;
    }
    if (!found) return false;
  }

  const char *separator = strrchr(resolved, '/');
  if (!separator) return false;
  const size_t parent_length = separator == resolved ? 1 : (size_t) (separator - resolved);
  if (parent_length >= directory_size) return false;
  memcpy(directory, resolved, parent_length);
  directory[parent_length] = 0;
  return true;
}

static bool command_is_authorized(const char *role, const char *command, gid_t service_gid,
                                  char *directory, size_t directory_size) {
  if (!safe_text(role, 15, false) || !safe_text(command, 65535, false) ||
      !directory || directory_size < 2) {
    errno = EINVAL;
    return false;
  }
  const int fd = open(command_manifest_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return false;
  struct stat attributes;
  if (fstat(fd, &attributes) || !S_ISREG(attributes.st_mode) || attributes.st_uid != 0 ||
      attributes.st_gid != service_gid || (attributes.st_mode & 0777) != 0640 || attributes.st_size > 1048576) {
    close(fd);
    errno = EPERM;
    return false;
  }
  FILE *input = fdopen(fd, "r");
  if (!input) {
    close(fd);
    return false;
  }
  bool authorized = false;
  char *line = NULL;
  size_t capacity = 0;
  if (getline(&line, &capacity, input) < 0) goto complete;
  line[strcspn(line, "\r\n")] = 0;
  if (strcmp(line, command_manifest_header)) {
    errno = EPERM;
    goto complete;
  }
  while (getline(&line, &capacity, input) >= 0) {
    line[strcspn(line, "\r\n")] = 0;
    char *first = strchr(line, '\t');
    char *second = first ? strchr(first + 1, '\t') : NULL;
    if (!first || !second) continue;
    *first++ = 0;
    *second++ = 0;
    if (!safe_text(first, PATH_MAX - 1, true) || (first[0] && first[0] != '/')) {
      errno = EPERM;
      authorized = false;
      break;
    }
    if (!strcmp(line, role) && !strcmp(second, command)) {
      if (authorized || !copy_field(directory, directory_size, first, true)) {
        errno = EPERM;
        authorized = false;
        break;
      }
      authorized = true;
    }
  }
complete:
  free(line);
  fclose(input);
  if (authorized && !directory[0]) {
    (void) executable_parent_directory(command, directory, directory_size);
  }
  if (!authorized) errno = EPERM;
  return authorized;
}

static bool install_session_environment(const struct session_identity *identity) {
  char data_home[PATH_MAX], bus[PATH_MAX];
  if (snprintf(data_home, sizeof(data_home), "%s/.local/share", identity->home) >= (int) sizeof(data_home) ||
      snprintf(bus, sizeof(bus), "unix:path=%s/bus", identity->runtime) >= (int) sizeof(bus)) return false;
  if (clearenv()) return false;
  return !setenv("HOME", identity->home, 1) && !setenv("USER", identity->user, 1) &&
         !setenv("LOGNAME", identity->user, 1) && !setenv("PATH", "/usr/local/bin:/usr/bin:/bin", 1) &&
         !setenv("LANG", "C.UTF-8", 1) && !setenv("SHELL", "/bin/sh", 1) &&
         !setenv("XDG_DATA_HOME", data_home, 1) && !setenv("XDG_RUNTIME_DIR", identity->runtime, 1) &&
         !setenv("PIPEWIRE_RUNTIME_DIR", identity->runtime, 1) &&
         !setenv("XDG_SESSION_TYPE", "wayland", 1) &&
         !setenv("WAYLAND_DISPLAY", identity->wayland_display, 1) &&
         !setenv("DBUS_SESSION_BUS_ADDRESS", bus, 1) &&
         (!identity->x_display[0] || !setenv("DISPLAY", identity->x_display, 1)) &&
         (!identity->xauthority[0] || !setenv("XAUTHORITY", identity->xauthority, 1));
}

static bool drop_to_session(const struct session_identity *identity) {
  static const cap_value_t identity_capabilities[] = {CAP_SETGID, CAP_SETUID};
  if (!set_effective_capabilities(identity_capabilities, 2)) return false;
  if (setgroups(identity->group_count, identity->groups) ||
      setgid(identity->gid) || setuid(identity->uid)) {
    const int identity_errno = errno;
    (void) set_effective_capabilities(NULL, 0);
    errno = identity_errno;
    return false;
  }
  cap_t empty = cap_init();
  if (!empty || cap_set_proc(empty)) {
    if (empty) cap_free(empty);
    return false;
  }
  cap_free(empty);
  return !prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) && install_session_environment(identity);
}

static bool display_argument_is_safe(const char *argument) {
  static const char pattern[] =
    "^output\\.[A-Za-z0-9_-]+\\.(enable|disable|vrrpolicy\\.always|hdr\\.(enable|disable)|"
    "mode\\.[A-Za-z0-9_-]+|scale\\.[0-9]+(\\.[0-9]+)?|position\\.-?[0-9]+,-?[0-9]+|"
    "priority\\.[0-9]+|addCustomMode\\.[1-9][0-9]*\\.[1-9][0-9]*\\.[1-9][0-9]*\\.reduced)$";
  regex_t expression;
  if (!safe_text(argument, 512, false) || regcomp(&expression, pattern, REG_EXTENDED | REG_NOSUB)) return false;
  const bool matches = regexec(&expression, argument, 0, NULL, 0) == 0;
  regfree(&expression);
  return matches;
}

static bool sink_name_is_safe(const char *name) {
  if (!safe_text(name, 127, false)) return false;
  for (const unsigned char *cursor = (const unsigned char *) name; *cursor; ++cursor) {
    if (!isalnum(*cursor) && *cursor != '_' && *cursor != '-' && *cursor != '.' && *cursor != '@') return false;
  }
  return true;
}

static bool steam_direct_arguments_are_safe(int argc, char **argv) {
  if (argc != 10 || !argv) return false;
  unsigned long app_id = 0, limit_millihz = 0;
  if (!parse_number(argv[2], 1, UINT32_MAX, &app_id) ||
      !parse_number(argv[4], 0, 1000000, &limit_millihz)) return false;
  (void) app_id;

  const bool mangohud = !strcmp(argv[3], "mangohud");
  const bool proton = !strcmp(argv[3], "proton");
  const bool mangohud_proton = !strcmp(argv[3], "mangohud-proton");
  const bool disabled = !strcmp(argv[3], "disabled");
  const bool overlay = mangohud || mangohud_proton;
  const bool limited = overlay || proton;
  const bool preset = !strcmp(argv[5], "custom") || !strcmp(argv[5], "1") ||
                      !strcmp(argv[5], "2") || !strcmp(argv[5], "3") ||
                      !strcmp(argv[5], "4");
  const bool graph = !strcmp(argv[6], "0") || !strcmp(argv[6], "1");
  const bool method = !strcmp(argv[7], "early") || !strcmp(argv[7], "late");
  const bool smooth = !strcmp(argv[8], "0") || !strcmp(argv[8], "1");
  const bool queue = !strcmp(argv[9], "0") || !strcmp(argv[9], "1");
  return (limited || disabled) && preset && graph && method && smooth && queue &&
         ((limited && limit_millihz >= 1000) || (disabled && !limit_millihz)) &&
         (overlay || (!strcmp(argv[5], "custom") && !strcmp(argv[6], "0"))) &&
         (mangohud || !strcmp(argv[7], "late")) &&
         (strcmp(argv[8], "0") || !strcmp(argv[9], "0")) &&
         (limited || strcmp(argv[8], "0"));
}

static bool parse_channel_mapping(const char *value, size_t channels,
                                  unsigned char mapping[8]) {
  if (!value || !mapping || channels < 1 || channels > 8) return false;
  const unsigned char *cursor = (const unsigned char *) value;
  for (size_t index = 0; index < channels; ++index) {
    if (*cursor < '0' || *cursor > '9') return false;
    unsigned int position = 0;
    do {
      position = position * 10 + (*cursor++ - '0');
      if (position > 7) return false;
    } while (*cursor >= '0' && *cursor <= '9');
    if (position >= channels) return false;
    mapping[index] = (unsigned char) position;
    if (index + 1 < channels) {
      if (*cursor++ != ',') return false;
    } else if (*cursor) {
      return false;
    }
  }
  return true;
}

static bool format_channel_mapping(const unsigned char mapping[8], size_t channels,
                                   const char *prefix, char *result, size_t result_size) {
  static const char *const positions[] = {
    "front-left", "front-right", "front-center", "lfe",
    "rear-left", "rear-right", "side-left", "side-right"
  };
  if (!mapping || channels < 1 || channels > 8 || !prefix || !result || !result_size) return false;
  const int prefix_length = snprintf(result, result_size, "%s", prefix);
  if (prefix_length < 0 || (size_t) prefix_length >= result_size) return false;
  size_t offset = (size_t) prefix_length;
  for (size_t index = 0; index < channels; ++index) {
    if (mapping[index] >= channels || mapping[index] >= 8) return false;
    const int written = snprintf(result + offset, result_size - offset, "%s%s",
                                 index ? "," : "", positions[mapping[index]]);
    if (written < 0 || (size_t) written >= result_size - offset) return false;
    offset += (size_t) written;
  }
  return true;
}

static size_t layout_channel_count(const char *layout) {
  if (!layout) return 0;
  if (!strcmp(layout, "stereo")) return 2;
  if (!strcmp(layout, "surround51")) return 6;
  if (!strcmp(layout, "surround71")) return 8;
  return 0;
}

static volatile sig_atomic_t termination_signal = 0;

static void request_user_service_stop(int signal_number) {
  if (!termination_signal) termination_signal = signal_number;
}

static bool set_termination_handlers(void (*handler)(int)) {
  struct sigaction action = {0};
  action.sa_handler = handler;
  if (sigemptyset(&action.sa_mask)) return false;
  if (sigaction(SIGTERM, &action, NULL) ||
      sigaction(SIGINT, &action, NULL) ||
      sigaction(SIGHUP, &action, NULL)) return false;

  struct sigaction child_action = {0};
  child_action.sa_handler = SIG_DFL;
  if (sigemptyset(&child_action.sa_mask) || sigaction(SIGCHLD, &child_action, NULL)) return false;

  sigset_t unblocked;
  if (sigemptyset(&unblocked) || sigaddset(&unblocked, SIGTERM) ||
      sigaddset(&unblocked, SIGINT) || sigaddset(&unblocked, SIGHUP) ||
      sigaddset(&unblocked, SIGCHLD)) return false;
  return !sigprocmask(SIG_UNBLOCK, &unblocked, NULL);
}

static bool reset_termination_handlers(void) {
  return set_termination_handlers(SIG_DFL);
}

static int wait_status_exit_code(int status) {
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return 126;
}

static uint64_t monotonic_milliseconds(void);

enum bounded_command_result {
  BOUNDED_COMMAND_ERROR = -1,
  BOUNDED_COMMAND_TIMEOUT = -2,
};

static bool reap_child_until(pid_t child, uint64_t deadline, int *status) {
  const struct timespec retry_delay = {.tv_sec = 0, .tv_nsec = 20000000};
  for (;;) {
    const pid_t waited = waitpid(child, status, WNOHANG);
    if (waited == child) return true;
    if (waited < 0 && errno == ECHILD) return true;
    if (waited < 0 && errno != EINTR) return false;
    const uint64_t now = monotonic_milliseconds();
    if (!now || now >= deadline) return false;
    (void) nanosleep(&retry_delay, NULL);
  }
}

static bool terminate_and_reap_child(pid_t child, int signal_number,
                                     unsigned int grace_milliseconds,
                                     int *status) {
  if (kill(child, signal_number) && errno != ESRCH) return false;
  uint64_t now = monotonic_milliseconds();
  if (!now) return false;
  if (reap_child_until(child, now + grace_milliseconds, status)) return true;
  if (kill(child, SIGKILL) && errno != ESRCH) return false;
  now = monotonic_milliseconds();
  return now && reap_child_until(child, now + 250, status);
}

static int run_command_bounded(const char *path, char *const arguments[],
                               unsigned int timeout_milliseconds,
                               char *output, size_t output_size) {
  if (!path || path[0] != '/' || !arguments || !arguments[0] ||
      !timeout_milliseconds || (output && output_size < 2) || (!output && output_size)) {
    return BOUNDED_COMMAND_ERROR;
  }
  if (output) output[0] = 0;
  int output_pipe[2] = {-1, -1};
  if (output && pipe2(output_pipe, O_CLOEXEC | O_NONBLOCK)) return BOUNDED_COMMAND_ERROR;

  const pid_t child = fork();
  if (child < 0) {
    if (output_pipe[0] >= 0) close(output_pipe[0]);
    if (output_pipe[1] >= 0) close(output_pipe[1]);
    return BOUNDED_COMMAND_ERROR;
  }
  if (!child) {
    if (!reset_termination_handlers()) _exit(126);
    const int null_output = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (null_output < 0) _exit(126);
    if (output_pipe[0] >= 0) close(output_pipe[0]);
    const int standard_output = output ? output_pipe[1] : null_output;
    if (dup2(standard_output, STDOUT_FILENO) < 0 ||
        dup2(null_output, STDERR_FILENO) < 0) _exit(126);
    if (output_pipe[1] >= 0 && output_pipe[1] != STDOUT_FILENO) close(output_pipe[1]);
    if (null_output != STDOUT_FILENO && null_output != STDERR_FILENO) close(null_output);
    execv(path, arguments);
    _exit(errno == ENOENT ? 127 : 126);
  }
  if (output_pipe[1] >= 0) close(output_pipe[1]);

  int status = 126 << 8;
  const uint64_t started = monotonic_milliseconds();
  if (!started || !reap_child_until(child, started + timeout_milliseconds, &status)) {
    (void) terminate_and_reap_child(child, SIGKILL, 250, &status);
    if (output_pipe[0] >= 0) close(output_pipe[0]);
    return BOUNDED_COMMAND_TIMEOUT;
  }

  bool truncated = false;
  if (output_pipe[0] >= 0) {
    size_t used = 0;
    unsigned char buffer[256];
    for (;;) {
      const ssize_t received = read(output_pipe[0], buffer, sizeof(buffer));
      if (received > 0) {
        const size_t available = output_size - 1 - used;
        const size_t copied = (size_t) received < available ? (size_t) received : available;
        if (copied) memcpy(output + used, buffer, copied);
        used += copied;
        if (copied != (size_t) received) truncated = true;
      } else if (!received || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
        break;
      } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
    }
    output[used] = 0;
    close(output_pipe[0]);
  }
  return truncated ? BOUNDED_COMMAND_ERROR : wait_status_exit_code(status);
}

static bool application_unit_name_is_safe(const char *unit) {
  static const char prefix[] = "vibepollo-app-";
  static const char suffix[] = ".service";
  if (!unit || strncmp(unit, prefix, sizeof(prefix) - 1)) return false;
  const char *cursor = unit + sizeof(prefix) - 1;
  if (!isdigit((unsigned char) *cursor)) return false;
  while (isdigit((unsigned char) *cursor)) ++cursor;
  if (*cursor++ != '-' || !isdigit((unsigned char) *cursor)) return false;
  while (isdigit((unsigned char) *cursor)) ++cursor;
  // A retry gets a distinct numeric suffix so systemd cannot confuse it with
  // a collected unit from the previous attempt.  Keep the original two-field
  // form valid for cleanup of units created by older brokers.
  if (*cursor == '-') {
    ++cursor;
    if (!isdigit((unsigned char) *cursor)) return false;
    while (isdigit((unsigned char) *cursor)) ++cursor;
  }
  return !strcmp(cursor, suffix);
}

static bool application_cgroup_path_is_safe(const char *path) {
  if (!path || path[0] != '/' || !path[1] || strlen(path) >= PATH_MAX / 2 ||
      strstr(path, "//")) return false;
  const char *component = path + 1;
  for (const unsigned char *cursor = (const unsigned char *) component;; ++cursor) {
    if (!*cursor || *cursor == '/') {
      const size_t length = (const char *) cursor - component;
      if (!length || (length == 1 && component[0] == '.') ||
          (length == 2 && component[0] == '.' && component[1] == '.')) return false;
      if (!*cursor) return true;
      component = (const char *) cursor + 1;
      continue;
    }
    if (!isalnum(*cursor) && *cursor != '_' && *cursor != '-' &&
        *cursor != '.' && *cursor != '@' && *cursor != ':' &&
        *cursor != '\\') return false;
  }
}

static bool cgroup_is_unpopulated_using(const char *cgroup_root,
                                        const char *control_group) {
  if (!cgroup_root || cgroup_root[0] != '/' ||
      !safe_text(cgroup_root, PATH_MAX / 2 - 1, false) ||
      !application_cgroup_path_is_safe(control_group)) return false;
  char events_path[PATH_MAX];
  if (snprintf(events_path, sizeof(events_path), "%s%s/cgroup.events",
               cgroup_root, control_group) >= (int) sizeof(events_path)) return false;
  const int events_fd = open(events_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (events_fd < 0) return false;
  char contents[4096];
  size_t used = 0;
  while (used < sizeof(contents) - 1) {
    const ssize_t received = read(events_fd, contents + used,
                                  sizeof(contents) - 1 - used);
    if (received > 0) {
      used += (size_t) received;
      continue;
    }
    if (!received) break;
    if (errno == EINTR) continue;
    close(events_fd);
    return false;
  }
  unsigned char overflow = 0;
  const ssize_t extra = read(events_fd, &overflow, 1);
  close(events_fd);
  if (extra || !used) return false;
  contents[used] = 0;

  bool found_populated = false;
  char *save = NULL;
  for (char *line = strtok_r(contents, "\n", &save); line;
       line = strtok_r(NULL, "\n", &save)) {
    if (!strncmp(line, "populated ", sizeof("populated ") - 1)) {
      if (found_populated || strcmp(line, "populated 0")) return false;
      found_populated = true;
    }
  }
  return found_populated;
}

static bool unit_state_is_quiescent(const char *state,
                                    const char *cgroup_root) {
  if (!state || !state[0]) return false;
  char copy[4096];
  if (strlen(state) >= sizeof(copy)) return false;
  memcpy(copy, state, strlen(state) + 1);
  char load_state[32] = {0}, control_group[PATH_MAX / 2] = {0};
  unsigned long main_pid = ULONG_MAX;
  bool have_load = false, have_main_pid = false, have_control_group = false;
  char *save = NULL;
  for (char *line = strtok_r(copy, "\n", &save); line;
       line = strtok_r(NULL, "\n", &save)) {
    char *separator = strchr(line, '=');
    if (!separator) return false;
    *separator++ = 0;
    if (!strcmp(line, "LoadState")) {
      if (have_load || !copy_field(load_state, sizeof(load_state), separator, false))
        return false;
      have_load = true;
    } else if (!strcmp(line, "MainPID")) {
      if (have_main_pid || !parse_number(separator, 0, INT_MAX, &main_pid)) return false;
      have_main_pid = true;
    } else if (!strcmp(line, "ControlGroup")) {
      if (have_control_group ||
          !copy_field(control_group, sizeof(control_group), separator, true)) return false;
      have_control_group = true;
    } else {
      return false;
    }
  }
  if (!have_load || !have_main_pid || !have_control_group || main_pid != 0) return false;
  if (!strcmp(load_state, "not-found")) return !control_group[0];
  return !strcmp(load_state, "loaded") && control_group[0] &&
         cgroup_is_unpopulated_using(cgroup_root, control_group);
}

static bool user_service_is_quiescent_using(
    const char *systemctl_path, const char *cgroup_root, const char *unit,
    unsigned int command_timeout_milliseconds) {
  char state[4096] = {0};
  char *const show_arguments[] = {
    "systemctl", "--user", "--quiet", "show",
    "--property=LoadState", "--property=MainPID", "--property=ControlGroup",
    (char *) unit, NULL
  };
  const int show_result = run_command_bounded(
    systemctl_path, show_arguments, command_timeout_milliseconds,
    state, sizeof(state));
  if (show_result == 0 && unit_state_is_quiescent(state, cgroup_root)) return true;
  if (show_result == 0) return false;

  // A collected transient unit has no object left for Show().  systemctl's
  // exact-unit status 4 is its explicit "unknown/not found" proof.
  char *const missing_arguments[] = {
    "systemctl", "--user", "--quiet", "is-active", (char *) unit, NULL
  };
  return run_command_bounded(systemctl_path, missing_arguments,
                             command_timeout_milliseconds, NULL, 0) == 4;
}

static bool stop_user_service_using(const char *systemctl_path, const char *unit,
                                    unsigned int command_timeout_milliseconds) {
  if (!systemctl_path || systemctl_path[0] != '/' ||
      !application_unit_name_is_safe(unit) || !command_timeout_milliseconds) return false;
  char *const stop_arguments[] = {
    "systemctl", "--user", "--quiet", "--no-block", "stop", (char *) unit, NULL
  };
  (void) run_command_bounded(systemctl_path, stop_arguments,
                             command_timeout_milliseconds, NULL, 0);
  const struct timespec retry_delay = {.tv_sec = 0, .tv_nsec = 50000000};
  unsigned int consecutive_clean_checks = 0;
  for (unsigned int attempt = 0; attempt < 100; ++attempt) {
    if (user_service_is_quiescent_using(systemctl_path, "/sys/fs/cgroup", unit,
                                        command_timeout_milliseconds)) {
      if (++consecutive_clean_checks == 10) return true;
    } else {
      consecutive_clean_checks = 0;
      // Registration may have completed after an earlier not-found proof.
      // Re-issue the exact stop whenever the unit reappears.
      (void) run_command_bounded(systemctl_path, stop_arguments,
                                 command_timeout_milliseconds, NULL, 0);
    }
    if (attempt + 1 < 100) (void) nanosleep(&retry_delay, NULL);
  }
  return false;
}

static bool stop_user_service(const char *unit) {
  return stop_user_service_using("/usr/bin/systemctl", unit, 250);
}

static bool make_watchdog_pipe(int descriptors[2]) {
  if (!descriptors || pipe2(descriptors, O_CLOEXEC)) return false;
  const int read_flags = fcntl(descriptors[0], F_GETFL);
  const int write_flags = fcntl(descriptors[1], F_GETFL);
  if (read_flags < 0 || write_flags < 0 ||
      (read_flags & O_ACCMODE) != O_RDONLY ||
      (write_flags & O_ACCMODE) != O_WRONLY) {
    close(descriptors[0]);
    close(descriptors[1]);
    descriptors[0] = -1;
    descriptors[1] = -1;
    return false;
  }
  return true;
}

static bool steam_launch_retry_is_safe(int result, bool cleanup_verified,
                                       unsigned int attempt, unsigned int attempts) {
  return result == 126 && cleanup_verified && !termination_signal && attempt + 1 < attempts;
}

static int supervise_user_service(const char *unit, char *const arguments[], bool *cleanup_verified) {
  *cleanup_verified = false;
  // A cancellation received during the retry delay belongs to this request,
  // not just the previous attempt. Never clear it on entry to supervision.
  if (termination_signal) return 128 + termination_signal;
  if (!set_termination_handlers(request_user_service_stop)) return 126;
  if (termination_signal) return 128 + termination_signal;

  int watchdog[2] = {-1, -1};
  if (!make_watchdog_pipe(watchdog)) return 126;

  const pid_t runner = fork();
  if (runner < 0) {
    close(watchdog[0]);
    close(watchdog[1]);
    (void) stop_user_service(unit);
    return 126;
  }
  if (!runner) {
    close(watchdog[1]);
    if (dup2(watchdog[0], STDIN_FILENO) < 0) _exit(126);
    if (watchdog[0] != STDIN_FILENO) close(watchdog[0]);
    if (!reset_termination_handlers()) _exit(126);
    execv("/usr/bin/systemd-run", arguments);
    _exit(errno == ENOENT ? 127 : 126);
  }
  close(watchdog[0]);
  watchdog[0] = -1;

  int status = 126 << 8;
  bool runner_exited = false;
  bool worker_error = false;
  const struct timespec runner_wait_delay = {.tv_sec = 0, .tv_nsec = 20000000};
  while (!termination_signal) {
    if (termination_signal) break;
    const pid_t waited = waitpid(runner, &status, WNOHANG);
    if (waited == runner) {
      runner_exited = true;
      break;
    }
    if (waited < 0 && errno == ECHILD) {
      runner_exited = true;
      worker_error = true;
      break;
    }
    if (waited < 0 && errno != EINTR) {
      worker_error = true;
      break;
    }
    (void) nanosleep(&runner_wait_delay, NULL);
  }

  // Only this worker ever held the write end.  Closing it requests in-unit
  // cancellation even if the transient-unit control path is unavailable.
  close(watchdog[1]);
  watchdog[1] = -1;

  if (!runner_exited) {
    // This first stop may race registration.  It is only an acceleration: the
    // authoritative proof is repeated after the runner can no longer create
    // the exact generated unit.
    (void) stop_user_service(unit);
    runner_exited = terminate_and_reap_child(runner, SIGTERM, 500, &status);
  }
  const bool unit_stopped = runner_exited && stop_user_service(unit);
  if (!runner_exited || !unit_stopped || worker_error) {
    fprintf(stderr,
            "vibepollo-session-broker: user service %s cleanup failed "
            "(runner=%s, unit=%s, worker=%s)\n",
            unit, runner_exited ? "exited" : "running",
            unit_stopped ? "stopped" : "not-stopped",
            worker_error ? "error" : "ok");
    return 126;
  }
  if (termination_signal) return 128 + termination_signal;
  *cleanup_verified = true;
  return wait_status_exit_code(status);
}

static int exec_user_service(const struct session_identity *identity, const char *directory,
                             char *const command_argv[], bool recover_steam_launch) {
  char unit[192], environment_home[PATH_MAX + 16], environment_user[80], environment_logname[80];
  char environment_runtime[PATH_MAX + 32], environment_config[PATH_MAX + 32];
  char environment_data[PATH_MAX + 32], environment_pipewire[PATH_MAX + 32];
  char environment_bus[PATH_MAX + 48], environment_wayland[96], environment_display[96];
  char environment_xauthority[PATH_MAX + 16];
  char environment_path[sizeof(fixed_path) + sizeof("PATH=")];
  if (snprintf(environment_home, sizeof(environment_home), "HOME=%s", identity->home) >= (int) sizeof(environment_home) ||
      snprintf(environment_user, sizeof(environment_user), "USER=%s", identity->user) >= (int) sizeof(environment_user) ||
      snprintf(environment_logname, sizeof(environment_logname), "LOGNAME=%s", identity->user) >= (int) sizeof(environment_logname) ||
      snprintf(environment_path, sizeof(environment_path), "PATH=%s", fixed_path) >= (int) sizeof(environment_path) ||
      snprintf(environment_runtime, sizeof(environment_runtime), "XDG_RUNTIME_DIR=%s", identity->runtime) >= (int) sizeof(environment_runtime) ||
      snprintf(environment_config, sizeof(environment_config), "XDG_CONFIG_HOME=%s/.config", identity->home) >= (int) sizeof(environment_config) ||
      snprintf(environment_data, sizeof(environment_data), "XDG_DATA_HOME=%s/.local/share", identity->home) >= (int) sizeof(environment_data) ||
      snprintf(environment_pipewire, sizeof(environment_pipewire), "PIPEWIRE_RUNTIME_DIR=%s", identity->runtime) >= (int) sizeof(environment_pipewire) ||
      snprintf(environment_bus, sizeof(environment_bus), "DBUS_SESSION_BUS_ADDRESS=unix:path=%s/bus", identity->runtime) >= (int) sizeof(environment_bus) ||
      snprintf(environment_wayland, sizeof(environment_wayland), "WAYLAND_DISPLAY=%s", identity->wayland_display) >= (int) sizeof(environment_wayland) ||
      (identity->x_display[0] && snprintf(environment_display, sizeof(environment_display), "DISPLAY=%s", identity->x_display) >= (int) sizeof(environment_display)) ||
      (identity->xauthority[0] && snprintf(environment_xauthority, sizeof(environment_xauthority), "XAUTHORITY=%s", identity->xauthority) >= (int) sizeof(environment_xauthority))) return 126;
  char *arguments[72];
  const unsigned int attempts = recover_steam_launch ? steam_launch_attempts : 1;
  for (unsigned int attempt = 0; attempt < attempts; ++attempt) {
    const int unit_length = recover_steam_launch
      ? snprintf(unit, sizeof(unit), "vibepollo-app-%lu-%ld-%u.service",
                 identity->generation, (long) getpid(), attempt + 1)
      : snprintf(unit, sizeof(unit), "vibepollo-app-%lu-%ld.service",
                 identity->generation, (long) getpid());
    if (unit_length < 0 || unit_length >= (int) sizeof(unit)) return 126;

    size_t index = 0;
    arguments[index++] = "systemd-run";
    arguments[index++] = "--user";
    arguments[index++] = "--quiet";
    arguments[index++] = "--wait";
    arguments[index++] = "--pipe";
    arguments[index++] = "--collect";
    arguments[index++] = "--service-type=exec";
    arguments[index++] = "--expand-environment=no";
    arguments[index++] = "--property=ExitType=main";
    arguments[index++] = "--property=KillMode=control-group";
    arguments[index++] = "--property=TimeoutStopSec=5s";
    arguments[index++] = "--property=Restart=no";
    arguments[index++] = "--property=RemainAfterExit=no";
    arguments[index++] = "--unit";
    arguments[index++] = unit;
    arguments[index++] = "--setenv";
    arguments[index++] = environment_home;
    arguments[index++] = "--setenv";
    arguments[index++] = environment_user;
    arguments[index++] = "--setenv";
    arguments[index++] = environment_logname;
    arguments[index++] = "--setenv";
    arguments[index++] = environment_path;
    arguments[index++] = "--setenv";
    arguments[index++] = environment_runtime;
    arguments[index++] = "--setenv";
    arguments[index++] = environment_config;
    arguments[index++] = "--setenv";
    arguments[index++] = environment_data;
    arguments[index++] = "--setenv";
    arguments[index++] = environment_pipewire;
    arguments[index++] = "--setenv";
    arguments[index++] = environment_bus;
    arguments[index++] = "--setenv";
    arguments[index++] = "XDG_SESSION_TYPE=wayland";
    arguments[index++] = "--setenv";
    arguments[index++] = environment_wayland;
    if (identity->x_display[0]) {
      arguments[index++] = "--setenv";
      arguments[index++] = environment_display;
    }
    if (identity->xauthority[0]) {
      arguments[index++] = "--setenv";
      arguments[index++] = environment_xauthority;
    }
    if (directory && directory[0]) {
      arguments[index++] = "--working-directory";
      arguments[index++] = (char *) directory;
    }
    arguments[index++] = "--";
    arguments[index++] = (char *) application_supervisor_path;
    arguments[index++] = "--";
    for (size_t command_index = 0; command_argv[command_index]; ++command_index) {
      if (index + 1 >= sizeof(arguments) / sizeof(arguments[0])) return 126;
      arguments[index++] = command_argv[command_index];
    }
    arguments[index] = NULL;

    bool cleanup_verified = false;
    const int result = supervise_user_service(unit, arguments, &cleanup_verified);
    if (termination_signal) return 128 + termination_signal;
    if (!steam_launch_retry_is_safe(result, cleanup_verified, attempt, attempts)) return result;
    fprintf(stderr,
            "vibepollo-session-broker: Steam launch attempt %u failed with "
            "exit 126; retrying with a fresh transient unit\n", attempt + 1);
    const struct timespec retry_delay = {.tv_sec = 0, .tv_nsec = 250000000};
    (void) nanosleep(&retry_delay, NULL);
  }
  return 126;
}

static int execute_request(int argc, char **argv,
                           const struct session_identity *identity,
                           gid_t service_gid) {
  if (argc < 2) return 2;
  enum operation {
    DISPLAY_QUERY, DISPLAY_APPLY, DISPLAY_POWER, DISPLAY_WAKE, AUDIO_GET_DEFAULT, AUDIO_LIST_SINKS, AUDIO_SET_DEFAULT,
    AUDIO_CREATE_NULL, AUDIO_REMOVE_NULL, AUDIO_CAPTURE, STEAM, STEAM_DIRECT, LUTRIS,
    PROVIDER_STEAM_SCAN, PROVIDER_LUTRIS_SCAN, PROVIDER_STEAM_ARTWORK, PROVIDER_LUTRIS_ARTWORK, APP
  } operation;
  unsigned long first_number = 0, second_number = 0, third_number = 0;
  unsigned char channel_mapping[8] = {0};
  size_t audio_channel_count = 0;
  char authorized_directory[PATH_MAX] = {0};
  if (!strcmp(argv[1], "display-query") && argc == 2) operation = DISPLAY_QUERY;
  else if (!strcmp(argv[1], "display-power") && argc == 2) operation = DISPLAY_POWER;
  else if (!strcmp(argv[1], "display-wake") && argc == 2) operation = DISPLAY_WAKE;
  else if (!strcmp(argv[1], "display-apply") && argc >= 3 && argc <= 66) {
    operation = DISPLAY_APPLY;
    for (int index = 2; index < argc; ++index) if (!display_argument_is_safe(argv[index])) return 126;
  } else if (!strcmp(argv[1], "audio-get-default") && argc == 2) operation = AUDIO_GET_DEFAULT;
  else if (!strcmp(argv[1], "audio-list-sinks") && argc == 2) operation = AUDIO_LIST_SINKS;
  else if (!strcmp(argv[1], "audio-set-default") && argc == 3 && sink_name_is_safe(argv[2])) operation = AUDIO_SET_DEFAULT;
  else if (!strcmp(argv[1], "audio-create-null") && argc == 4 &&
           (audio_channel_count = layout_channel_count(argv[2])) &&
           parse_channel_mapping(argv[3], audio_channel_count, channel_mapping)) operation = AUDIO_CREATE_NULL;
  else if (!strcmp(argv[1], "audio-remove-null") && argc == 3 &&
           parse_number(argv[2], 0, UINT32_MAX - 1, &first_number)) operation = AUDIO_REMOVE_NULL;
  else if (!strcmp(argv[1], "audio-capture") && argc == 6 &&
           parse_number(argv[2], 8000, 192000, &first_number) &&
           parse_number(argv[3], 2, 8, &second_number) &&
           (second_number == 2 || second_number == 6 || second_number == 8) &&
           parse_number(argv[4], 1, 8192, &third_number) &&
           parse_channel_mapping(argv[5], second_number, channel_mapping)) operation = AUDIO_CAPTURE;
  else if (!strcmp(argv[1], "steam") && argc == 3 && numeric_suffix(argv[2], "") && !strcmp(identity->role, "desktop")) operation = STEAM;
  else if (!strcmp(argv[1], "steam-direct") &&
           steam_direct_arguments_are_safe(argc, argv) &&
           !strcmp(identity->role, "desktop")) operation = STEAM_DIRECT;
  else if (!strcmp(argv[1], "lutris") && argc == 3 && numeric_suffix(argv[2], "") && !strcmp(identity->role, "desktop")) operation = LUTRIS;
  else if (!strcmp(argv[1], "provider-steam-scan") && argc == 2 && !strcmp(identity->role, "desktop")) operation = PROVIDER_STEAM_SCAN;
  else if (!strcmp(argv[1], "provider-lutris-scan") && argc == 2 && !strcmp(identity->role, "desktop")) operation = PROVIDER_LUTRIS_SCAN;
  else if (argc == 2 && !strcmp(identity->role, "desktop") &&
           artwork_request_is_safe(argv[1], "provider-steam-artwork:", UINT32_MAX)) operation = PROVIDER_STEAM_ARTWORK;
  else if (argc == 2 && !strcmp(identity->role, "desktop") &&
           artwork_request_is_safe(argv[1], "provider-lutris-artwork:", INT64_MAX)) operation = PROVIDER_LUTRIS_ARTWORK;
  else if (!strcmp(argv[1], "app") && argc == 3 && !strcmp(identity->role, "desktop") &&
           command_is_authorized(identity->role, argv[2], service_gid,
                                 authorized_directory, sizeof(authorized_directory))) operation = APP;
  else {
    fprintf(stderr, "vibepollo-session-broker: rejected request '%s' (argc=%d, role=%s)\n",
            argv[1], argc, identity->role);
    return 126;
  }

  if (!drop_to_session(identity) || !validate_session_endpoints(identity) ||
      !validate_xauthority(identity)) {
    perror("vibepollo-session-broker");
    return 126;
  }

  switch (operation) {
    case DISPLAY_POWER:
    case DISPLAY_WAKE: {
      char *const arguments[] = {"vibepollo-display-power",
        operation == DISPLAY_WAKE ? "wake" :
        (!strcmp(identity->role, "desktop") ? "desktop" : "greeter"), NULL};
      execv("/usr/libexec/vibeshine/vibepollo-display-power", arguments);
      break;
    }
    case DISPLAY_QUERY: {
      char *const arguments[] = {"kscreen-doctor", "-j", NULL};
      execv("/usr/bin/kscreen-doctor", arguments);
      break;
    }
    case DISPLAY_APPLY:
      argv[1] = "kscreen-doctor";
      execv("/usr/bin/kscreen-doctor", &argv[1]);
      break;
    case AUDIO_GET_DEFAULT: {
      char *const arguments[] = {"pactl", "get-default-sink", NULL};
      execv("/usr/bin/pactl", arguments);
      break;
    }
    case AUDIO_LIST_SINKS: {
      char *const arguments[] = {"pactl", "list", "short", "sinks", NULL};
      execv("/usr/bin/pactl", arguments);
      break;
    }
    case AUDIO_SET_DEFAULT: {
      char *const arguments[] = {"pactl", "set-default-sink", argv[2], NULL};
      execv("/usr/bin/pactl", arguments);
      break;
    }
    case AUDIO_CREATE_NULL: {
      const char *name = !strcmp(argv[2], "stereo") ? "sink-sunshine-stereo" :
                         !strcmp(argv[2], "surround51") ? "sink-sunshine-surround51" : "sink-sunshine-surround71";
      const char *channels = !strcmp(argv[2], "stereo") ? "channels=2" :
                             !strcmp(argv[2], "surround51") ? "channels=6" : "channels=8";
      char sink_name[160], description[192], mapping[192];
      if (snprintf(sink_name, sizeof(sink_name), "sink_name=%s", name) >= (int) sizeof(sink_name) ||
          snprintf(description, sizeof(description), "sink_properties=device.description=%s", name) >= (int) sizeof(description) ||
          !format_channel_mapping(channel_mapping, audio_channel_count, "channel_map=", mapping, sizeof(mapping))) return 126;
      char *const arguments[] = {"pactl", "load-module", "module-null-sink", sink_name, "format=float32",
                                 "rate=48000", (char *) channels, mapping, description, NULL};
      execv("/usr/bin/pactl", arguments);
      break;
    }
    case AUDIO_REMOVE_NULL: {
      char *const arguments[] = {"pactl", "unload-module", argv[2], NULL};
      execv("/usr/bin/pactl", arguments);
      break;
    }
    case AUDIO_CAPTURE: {
      char rate[64], channels[64], latency[64], process_time[64], mapping[192];
      const unsigned long bytes = third_number * second_number * sizeof(float);
      if (snprintf(rate, sizeof(rate), "--rate=%lu", first_number) >= (int) sizeof(rate) ||
          snprintf(channels, sizeof(channels), "--channels=%lu", second_number) >= (int) sizeof(channels) ||
          snprintf(latency, sizeof(latency), "--latency=%lu", bytes) >= (int) sizeof(latency) ||
          snprintf(process_time, sizeof(process_time), "--process-time=%lu", bytes) >= (int) sizeof(process_time) ||
          !format_channel_mapping(channel_mapping, second_number, "--channel-map=", mapping, sizeof(mapping))) return 126;
      char *const arguments[] = {"parec", "--record", "--raw", "--client-name=vibepollo",
                                 "--stream-name=vibepollo-record", "--device=@DEFAULT_MONITOR@",
                                 "--format=float32le", rate, channels, mapping, latency, process_time, NULL};
      execv("/usr/bin/parec", arguments);
      break;
    }
    case STEAM: {
      // This is a short-lived handoff to the already-running desktop Steam
      // daemon, not a directly owned game process.  The broker worker has
      // already dropped to the selected desktop UID and validated its session
      // endpoints, so execute the handoff here.  Wrapping it in a transient
      // user unit can fail before Steam is reached when the broker itself is
      // running inside systemd's hardened system-service namespace.
      char *const arguments[] = {"/usr/bin/steam", "-applaunch", argv[2], NULL};
      execv("/usr/bin/steam", arguments);
      break;
    }
    case STEAM_DIRECT: {
      char *const arguments[] = {
        (char *) steam_launch_path, argv[2], argv[3], argv[4], argv[5],
        argv[6], argv[7], argv[8], argv[9], NULL
      };
      return exec_user_service(identity, NULL, arguments, false);
    }
    case LUTRIS: {
      // Apply the same ownership boundary to an already-running Lutris daemon.
      char uri[160];
      if (snprintf(uri, sizeof(uri), "lutris:rungameid/%s", argv[2]) >= (int) sizeof(uri)) return 126;
      char *const arguments[] = {"/usr/bin/lutris", uri, NULL};
      return exec_user_service(identity, NULL, arguments, false);
    }
    case PROVIDER_STEAM_ARTWORK:
    case PROVIDER_LUTRIS_ARTWORK: {
      char *const arguments[] = {"vibepollo-provider-scan",
                                 operation == PROVIDER_STEAM_ARTWORK ? "steam-artwork" : "lutris-artwork",
                                 strchr(argv[1], ':') + 1, NULL};
      execv("/usr/libexec/vibeshine/vibepollo-provider-scan", arguments);
      break;
    }
    case PROVIDER_STEAM_SCAN: {
      char *const arguments[] = {"vibepollo-provider-scan", "steam", NULL};
      execv("/usr/libexec/vibeshine/vibepollo-provider-scan", arguments);
      break;
    }
    case PROVIDER_LUTRIS_SCAN: {
      char *const arguments[] = {"vibepollo-provider-scan", "lutris", NULL};
      execv("/usr/libexec/vibeshine/vibepollo-provider-scan", arguments);
      break;
    }
    case APP: {
      char *const arguments[] = {"/bin/sh", "-c", argv[2], "--", NULL};
      return exec_user_service(identity, authorized_directory, arguments, false);
    }
  }
  perror("vibepollo-session-broker");
  return errno == ENOENT ? 127 : 126;
}

struct decoded_request {
  struct vibepollo_session_message header;
  int argc;
  char *argv[VIBEPOLLO_SESSION_PROTOCOL_MAX_ARGUMENTS + 2];
};

static bool decode_request(unsigned char *packet, size_t packet_length,
                           struct decoded_request *request) {
  if (!packet || !request || packet_length < sizeof(request->header) ||
      packet_length > VIBEPOLLO_SESSION_PROTOCOL_MAX_MESSAGE) return false;
  memset(request, 0, sizeof(*request));
  memcpy(&request->header, packet, sizeof(request->header));
  const size_t payload_length = packet_length - sizeof(request->header);
  if (request->header.magic != VIBEPOLLO_SESSION_PROTOCOL_MAGIC ||
      request->header.version != VIBEPOLLO_SESSION_PROTOCOL_VERSION ||
      request->header.type != VIBEPOLLO_SESSION_REQUEST || request->header.status ||
      request->header.reserved || !request->header.generation ||
      request->header.generation > ULONG_MAX ||
      request->header.payload_length != payload_length ||
      request->header.argument_count < 1 ||
      request->header.argument_count > VIBEPOLLO_SESSION_PROTOCOL_MAX_ARGUMENTS) return false;

  request->argc = (int) request->header.argument_count + 1;
  request->argv[0] = (char *) "vibepollo-session-exec";
  unsigned char *cursor = packet + sizeof(request->header);
  size_t remaining = payload_length;
  for (uint32_t index = 0; index < request->header.argument_count; ++index) {
    unsigned char *terminator = memchr(cursor, 0, remaining);
    if (!terminator) return false;
    const size_t length = (size_t) (terminator - cursor);
    if (!safe_text((char *) cursor, 65535, true) || length > 65535) return false;
    request->argv[index + 1] = (char *) cursor;
    cursor = terminator + 1;
    remaining -= length + 1;
  }
  if (remaining) return false;
  request->argv[request->argc] = NULL;
  return true;
}

static bool peer_is_authorized(int socket_fd, uid_t expected_uid, gid_t expected_gid) {
  int socket_type = 0;
  socklen_t socket_type_size = sizeof(socket_type);
  if (getsockopt(socket_fd, SOL_SOCKET, SO_TYPE, &socket_type, &socket_type_size) ||
      socket_type_size != sizeof(socket_type) || socket_type != SOCK_SEQPACKET) return false;

  struct sockaddr_storage local_address = {0};
  socklen_t local_address_size = sizeof(local_address);
  if (getsockname(socket_fd, (struct sockaddr *) &local_address, &local_address_size) ||
      local_address.ss_family != AF_UNIX) return false;

  struct ucred peer = {0};
  socklen_t peer_size = sizeof(peer);
  return !getsockopt(socket_fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) &&
         peer_size == sizeof(peer) && peer.pid > 0 &&
         peer.uid == expected_uid && peer.gid == expected_gid;
}

static ssize_t receive_request(int socket_fd, unsigned char *packet, size_t packet_size) {
  struct pollfd ready = {.fd = socket_fd, .events = POLLIN};
  int polled;
  do {
    polled = poll(&ready, 1, 5000);
  } while (polled < 0 && errno == EINTR);
  if (polled != 1 || !(ready.revents & POLLIN)) {
    if (!polled) errno = ETIMEDOUT;
    else if (polled >= 0) errno = ECONNRESET;
    return -1;
  }

  unsigned char control[CMSG_SPACE(sizeof(int) * 4)] = {0};
  struct iovec vector = {.iov_base = packet, .iov_len = packet_size};
  struct msghdr message = {
    .msg_iov = &vector,
    .msg_iovlen = 1,
    .msg_control = control,
    .msg_controllen = sizeof(control),
  };
  ssize_t received;
  do {
    received = recvmsg(socket_fd, &message, 0);
  } while (received < 0 && errno == EINTR);
  if (received <= 0 || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
      message.msg_controllen) {
    if (received >= 0) errno = EPROTO;
    for (struct cmsghdr *header = CMSG_FIRSTHDR(&message); header;
         header = CMSG_NXTHDR(&message, header)) {
      if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS) {
        const size_t descriptor_count =
          (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        const int *descriptors = (const int *) CMSG_DATA(header);
        for (size_t index = 0; index < descriptor_count; ++index) close(descriptors[index]);
      }
    }
    return -1;
  }
  return received;
}

static bool send_frame(int socket_fd, uint16_t type, uint64_t generation,
                       int status, const void *payload, size_t payload_length) {
  if (payload_length > VIBEPOLLO_SESSION_PROTOCOL_OUTPUT_CHUNK) return false;
  const struct vibepollo_session_message header = {
    .magic = VIBEPOLLO_SESSION_PROTOCOL_MAGIC,
    .version = VIBEPOLLO_SESSION_PROTOCOL_VERSION,
    .type = type,
    .payload_length = (uint32_t) payload_length,
    .generation = generation,
    .status = status,
  };
  struct iovec vectors[2] = {
    {.iov_base = (void *) &header, .iov_len = sizeof(header)},
    {.iov_base = (void *) payload, .iov_len = payload_length},
  };
  struct msghdr message = {.msg_iov = vectors, .msg_iovlen = payload_length ? 2 : 1};
  ssize_t sent;
  do {
    sent = sendmsg(socket_fd, &message, MSG_NOSIGNAL);
  } while (sent < 0 && errno == EINTR);
  return sent == (ssize_t) (sizeof(header) + payload_length);
}

static void send_rejection(int socket_fd, uint64_t generation, const char *reason) {
  if (reason && reason[0]) {
    (void) send_frame(socket_fd, VIBEPOLLO_SESSION_STDERR, generation, 0,
                      reason, strlen(reason));
  }
  (void) send_frame(socket_fd, VIBEPOLLO_SESSION_EXIT, generation, 126, NULL, 0);
}

static bool identities_match(const struct session_identity *first,
                             const struct session_identity *second) {
  return first->generation == second->generation && first->uid == second->uid &&
         first->gid == second->gid && first->group_count == second->group_count &&
         !strcmp(first->session, second->session) && !strcmp(first->role, second->role) &&
         !strcmp(first->user, second->user) && !strcmp(first->home, second->home) &&
         !strcmp(first->runtime, second->runtime) &&
         !strcmp(first->wayland_display, second->wayland_display) &&
         !strcmp(first->x_display, second->x_display) &&
         !strcmp(first->xauthority, second->xauthority) &&
         !memcmp(first->groups, second->groups,
                 first->group_count * sizeof(first->groups[0]));
}

static bool identity_is_current(const struct session_identity *identity,
                                gid_t service_gid) {
  struct session_identity current = {0};
  return load_identity(&current, service_gid, identity->generation) &&
         identities_match(identity, &current);
}

static bool make_output_pipe(int descriptors[2]) {
  if (pipe2(descriptors, O_CLOEXEC)) return false;
  const int flags = fcntl(descriptors[0], F_GETFL);
  if (flags < 0 || fcntl(descriptors[0], F_SETFL, flags | O_NONBLOCK)) {
    close(descriptors[0]);
    close(descriptors[1]);
    descriptors[0] = -1;
    descriptors[1] = -1;
    return false;
  }
  return true;
}

static pid_t spawn_worker(struct decoded_request *request,
                          const struct session_identity *identity,
                          gid_t service_gid, int output_fd[2]) {
  int output[2] = {-1, -1}, error_output[2] = {-1, -1};
  if (!make_output_pipe(output) || !make_output_pipe(error_output)) {
    if (output[0] >= 0) close(output[0]);
    if (output[1] >= 0) close(output[1]);
    return -1;
  }
  const pid_t worker = fork();
  if (worker < 0) {
    close(output[0]);
    close(output[1]);
    close(error_output[0]);
    close(error_output[1]);
    return -1;
  }
  if (!worker) {
    (void) setpgid(0, 0);
    close(output[0]);
    close(error_output[0]);
    const int null_input = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (null_input < 0 || dup2(null_input, STDIN_FILENO) < 0 ||
        dup2(output[1], STDOUT_FILENO) < 0 ||
        dup2(error_output[1], STDERR_FILENO) < 0) _exit(126);
    if (null_input != STDIN_FILENO) close(null_input);
    if (output[1] != STDOUT_FILENO) close(output[1]);
    if (error_output[1] != STDERR_FILENO) close(error_output[1]);
    if (!reset_termination_handlers()) _exit(126);
    struct session_identity current = {0};
    if (!load_identity(&current, service_gid, identity->generation) ||
        !identities_match(identity, &current)) _exit(126);
    _exit(execute_request(request->argc, request->argv, &current, service_gid));
  }
  close(output[1]);
  close(error_output[1]);
  (void) setpgid(worker, worker);
  output_fd[0] = output[0];
  output_fd[1] = error_output[0];
  return worker;
}

static uint64_t monotonic_milliseconds(void) {
  struct timespec now = {0};
  if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
  return (uint64_t) now.tv_sec * UINT64_C(1000) + (uint64_t) now.tv_nsec / UINT64_C(1000000);
}

static bool terminate_worker(pid_t worker, bool *worker_exited, int *status) {
  if (*worker_exited) return true;
  // The worker has already changed to the selected session UID. CAP_KILL is
  // therefore the broker supervisor's only capability beyond SETUID/SETGID,
  // and is required solely to cancel this direct descendant on disconnect or
  // generation change. It is never inherited past drop_to_session().
  if (!signal_cross_uid_worker(worker, SIGTERM)) return false;
  const struct timespec delay = {.tv_sec = 0, .tv_nsec = 50000000};
  for (unsigned int attempt = 0; attempt < 240; ++attempt) {
    const pid_t waited = waitpid(worker, status, WNOHANG);
    if (waited == worker || (waited < 0 && errno == ECHILD)) {
      *worker_exited = true;
      return true;
    }
    if (waited < 0 && errno != EINTR) break;
    (void) nanosleep(&delay, NULL);
  }
  (void) signal_cross_uid_worker(-worker, SIGKILL);
  (void) signal_cross_uid_worker(worker, SIGKILL);
  const uint64_t now = monotonic_milliseconds();
  *worker_exited = now && reap_child_until(worker, now + 1000, status);
  return *worker_exited;
}

static bool forward_output(int socket_fd, int *pipe_fd, uint16_t type,
                           uint64_t generation) {
  unsigned char output[VIBEPOLLO_SESSION_PROTOCOL_OUTPUT_CHUNK];
  const ssize_t received = read(*pipe_fd, output, sizeof(output));
  if (received > 0) {
    return send_frame(socket_fd, type, generation, 0, output, (size_t) received);
  }
  if (!received) {
    close(*pipe_fd);
    *pipe_fd = -1;
    return true;
  }
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

static int relay_worker(int socket_fd, pid_t worker, int output_fd[2],
                        const struct session_identity *identity,
                        gid_t service_gid) {
  bool worker_exited = false;
  int status = 126 << 8;
  const char *cancellation_reason = NULL;
  bool client_connected = true;
  uint64_t last_identity_check = monotonic_milliseconds();

  while (!worker_exited || output_fd[0] >= 0 || output_fd[1] >= 0) {
    const uint64_t now = monotonic_milliseconds();
    if (!now || !last_identity_check || now - last_identity_check >= 250) {
      if (!identity_is_current(identity, service_gid)) {
        cancellation_reason = "Vibepollo session changed; request cancelled.\n";
        break;
      }
      last_identity_check = now;
    }

    struct pollfd watched[3] = {
      {.fd = socket_fd, .events = POLLIN},
      {.fd = output_fd[0], .events = POLLIN},
      {.fd = output_fd[1], .events = POLLIN},
    };
    int polled;
    do {
      polled = poll(watched, 3, 200);
    } while (polled < 0 && errno == EINTR);
    if (polled < 0) {
      client_connected = false;
      break;
    }
#ifdef POLLRDHUP
    const short disconnected_events = POLLHUP | POLLERR | POLLNVAL | POLLRDHUP;
#else
    const short disconnected_events = POLLHUP | POLLERR | POLLNVAL;
#endif
    if (watched[0].revents & disconnected_events) {
      client_connected = false;
      break;
    }
    if (watched[0].revents & POLLIN) {
      unsigned char unexpected;
      const ssize_t received = recv(socket_fd, &unexpected, sizeof(unexpected), MSG_DONTWAIT);
      if (received >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        client_connected = false;
        break;
      }
    }
    if (output_fd[0] >= 0 && watched[1].revents & (POLLIN | POLLHUP | POLLERR)) {
      if (!forward_output(socket_fd, &output_fd[0], VIBEPOLLO_SESSION_STDOUT,
                          identity->generation)) {
        client_connected = false;
        break;
      }
    }
    if (output_fd[1] >= 0 && watched[2].revents & (POLLIN | POLLHUP | POLLERR)) {
      if (!forward_output(socket_fd, &output_fd[1], VIBEPOLLO_SESSION_STDERR,
                          identity->generation)) {
        client_connected = false;
        break;
      }
    }
    if (!worker_exited) {
      const pid_t waited = waitpid(worker, &status, WNOHANG);
      if (waited == worker || (waited < 0 && errno == ECHILD)) worker_exited = true;
      else if (waited < 0 && errno != EINTR) {
        client_connected = false;
        break;
      }
    }
  }

  if (!worker_exited) (void) terminate_worker(worker, &worker_exited, &status);
  if (output_fd[0] >= 0) close(output_fd[0]);
  if (output_fd[1] >= 0) close(output_fd[1]);
  if (!client_connected) return 126;
  if (cancellation_reason) {
    send_rejection(socket_fd, identity->generation, cancellation_reason);
    return 126;
  }
  const int exit_code = wait_status_exit_code(status);
  (void) send_frame(socket_fd, VIBEPOLLO_SESSION_EXIT, identity->generation,
                    exit_code, NULL, 0);
  return exit_code;
}

int main(int argc, char **argv) {
  if (!sanitize_startup_capabilities()) return 126;
  (void) argv;
  if (argc != 1) return 2;
  if (getuid() != 0 || geteuid() != 0) return 126;
  struct passwd *service = getpwnam(service_name);
  if (!service) return 126;
  const uid_t service_uid = service->pw_uid;
  const gid_t service_gid = service->pw_gid;
  if (!peer_is_authorized(STDIN_FILENO, service_uid, service_gid)) {
    fputs("vibepollo-session-broker: rejected unauthorized peer\n", stderr);
    return 126;
  }
  struct sigaction ignore_pipe = {.sa_handler = SIG_IGN};
  if (sigemptyset(&ignore_pipe.sa_mask) || sigaction(SIGPIPE, &ignore_pipe, NULL)) return 126;

  unsigned char *packet = malloc(VIBEPOLLO_SESSION_PROTOCOL_MAX_MESSAGE);
  if (!packet) return 126;
  const ssize_t packet_length =
    receive_request(STDIN_FILENO, packet, VIBEPOLLO_SESSION_PROTOCOL_MAX_MESSAGE);
  struct decoded_request request = {0};
  if (packet_length < 0 || !decode_request(packet, (size_t) packet_length, &request)) {
    fputs("vibepollo-session-broker: rejected malformed request\n", stderr);
    free(packet);
    return 126;
  }

  struct session_identity identity = {0};
  if (!load_identity(&identity, service_gid, request.header.generation)) {
    send_rejection(STDIN_FILENO, request.header.generation,
                   "Vibepollo session is unavailable or changed.\n");
    free(packet);
    return 126;
  }
  int output_fd[2] = {-1, -1};
  const pid_t worker = spawn_worker(&request, &identity, service_gid, output_fd);
  if (worker < 0) {
    send_rejection(STDIN_FILENO, request.header.generation,
                   "Vibepollo could not start the session request.\n");
    free(packet);
    return 126;
  }
  const int result = relay_worker(STDIN_FILENO, worker, output_fd, &identity, service_gid);
  free(packet);
  return result;
}
