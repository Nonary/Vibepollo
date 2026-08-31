#define _GNU_SOURCE

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
static const char service_name[] = "vibepollo";

struct session_identity { char user[64]; uid_t uid; gid_t gid; gid_t groups[128]; size_t group_count; };

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
  bool have_user = false, have_uid = false, have_gid = false, have_groups = false;
  char *line = NULL; size_t capacity = 0;
  while (getline(&line, &capacity, input) >= 0) {
    line[strcspn(line, "\r\n")] = 0; char *separator = strchr(line, '='); if (!separator) continue; *separator++ = 0;
    unsigned long parsed = 0;
    if (!strcmp(line, "user")) {
      if (have_user || !separator[0] || strlen(separator) >= sizeof(identity->user)) goto invalid;
      strcpy(identity->user, separator); have_user = true;
    } else if (!strcmp(line, "uid")) {
      if (have_uid || !parse_id(separator, &parsed)) goto invalid;
      identity->uid = (uid_t) parsed;
      have_uid = true;
    } else if (!strcmp(line, "gid")) {
      if (have_gid || !parse_id(separator, &parsed)) goto invalid;
      identity->gid = (gid_t) parsed;
      have_gid = true;
    } else if (!strcmp(line, "groups")) {
      if (have_groups || !parse_groups(separator, identity)) goto invalid;
      have_groups = true;
    }
  }
  free(line); fclose(input);
  if (!have_user || !have_uid || !have_gid || !have_groups) { errno = EINVAL; return false; }
  struct passwd *target = getpwuid(identity->uid);
  if (!target || strcmp(target->pw_name, identity->user) || target->pw_gid != identity->gid) { errno = EPERM; return false; }
  return true;
invalid:
  free(line); fclose(input); errno = EINVAL; return false;
}

int main(int argc, char **argv) {
  if (argc < 2) return 2;
  struct passwd *service = getpwnam(service_name);
  if (!service || getuid() != service->pw_uid || geteuid() != service->pw_uid) return 126;
  struct session_identity identity = {0};
  if (!load_identity(&identity, service->pw_gid)) { perror("vibepollo-session-exec"); return 126; }
  if (setgroups(identity.group_count, identity.groups) || setgid(identity.gid) || setuid(identity.uid)) { perror("vibepollo-session-exec"); return 126; }
  cap_t empty = cap_init();
  if (!empty || cap_set_proc(empty)) { if (empty) cap_free(empty); return 126; }
  cap_free(empty);
  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) return 126;
  struct passwd *target = getpwuid(identity.uid); if (!target) return 126;
  setenv("HOME", target->pw_dir, 1); setenv("USER", target->pw_name, 1); setenv("LOGNAME", target->pw_name, 1);
  char config_home[PATH_MAX];
  if (snprintf(config_home, sizeof(config_home), "%s/.config", target->pw_dir) >= (int) sizeof(config_home)) return 126;
  setenv("XDG_CONFIG_HOME", config_home, 1);
  unsetenv("VIBEPOLLO_MACHINE_HOST"); unsetenv("VIBEPOLLO_SESSION_UID"); unsetenv("VIBEPOLLO_SESSION_GID"); unsetenv("VIBEPOLLO_SESSION_GROUPS");
  execvp(argv[1], &argv[1]); perror("vibepollo-session-exec"); return errno == ENOENT ? 127 : 126;
}
