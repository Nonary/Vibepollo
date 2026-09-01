#define _GNU_SOURCE

#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>

#define main vibepollo_kwin_session_environment_entrypoint
#include "../../../../packaging/linux/vibepollo-kwin-session-environment.c"
#undef main

#define CHECK(expression) \
  do { \
    if (!(expression)) { \
      fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expression); \
      return 1; \
    } \
  } while (0)

static int filesystem_validation_works(void) {
  char runtime[] = "/tmp/vibepollo-kwin-environment.XXXXXX";
  CHECK(mkdtemp(runtime));
  CHECK(!chmod(runtime, 0700));

  struct display_environment environment = {
    .wayland_display = "wayland-9",
    .x_display = ":9",
  };
  const int xauthority_length = snprintf(environment.xauthority, sizeof(environment.xauthority), "%s/xauth_test", runtime);
  CHECK(xauthority_length > 0 && (size_t) xauthority_length < sizeof(environment.xauthority));

  char socket_path[PATH_MAX] = {0};
  const int socket_path_length = snprintf(socket_path, sizeof(socket_path), "%s/%s", runtime, environment.wayland_display);
  CHECK(socket_path_length > 0 && (size_t) socket_path_length < sizeof(socket_path));

  const int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  CHECK(listener >= 0);
  struct sockaddr_un address = {
    .sun_family = AF_UNIX,
  };
  CHECK((size_t) socket_path_length < sizeof(address.sun_path));
  memcpy(address.sun_path, socket_path, (size_t) socket_path_length + 1);
  CHECK(!bind(listener, (const struct sockaddr *) &address, sizeof(address)));

  int authority = open(environment.xauthority, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  CHECK(authority >= 0);
  CHECK(write(authority, "x", 1) == 1);
  CHECK(!close(authority));
  CHECK(validate_environment_at_runtime(runtime, getuid(), &environment));

  struct display_environment wayland_only = {
    .wayland_display = "wayland-9",
  };
  CHECK(validate_environment_at_runtime(runtime, getuid(), &wayland_only));
  struct display_environment discovered = {0};
  CHECK(discover_wayland_socket_at_runtime(runtime, getuid(), &discovered));
  CHECK(!strcmp(discovered.wayland_display, "wayland-9"));

  char second_socket_path[PATH_MAX] = {0};
  const int second_socket_path_length = snprintf(second_socket_path, sizeof(second_socket_path), "%s/wayland-10", runtime);
  CHECK(second_socket_path_length > 0 && (size_t) second_socket_path_length < sizeof(second_socket_path));
  CHECK(!symlink(socket_path, second_socket_path));
  memset(&discovered, 0, sizeof(discovered));
  CHECK(!discover_wayland_socket_at_runtime(runtime, getuid(), &discovered));
  CHECK(!unlink(second_socket_path));

  const int second_listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  CHECK(second_listener >= 0);
  struct sockaddr_un second_address = {
    .sun_family = AF_UNIX,
  };
  CHECK((size_t) second_socket_path_length < sizeof(second_address.sun_path));
  memcpy(second_address.sun_path, second_socket_path, (size_t) second_socket_path_length + 1);
  CHECK(!bind(second_listener, (const struct sockaddr *) &second_address, sizeof(second_address)));
  memset(&discovered, 0, sizeof(discovered));
  CHECK(!discover_wayland_socket_at_runtime(runtime, getuid(), &discovered));
  CHECK(!unlink(second_socket_path));
  CHECK(!close(second_listener));

  CHECK(!chmod(environment.xauthority, 0644));
  CHECK(!validate_environment_at_runtime(runtime, getuid(), &environment));
  CHECK(!chmod(environment.xauthority, 0600));

  CHECK(!unlink(socket_path));
  const int regular_socket = open(socket_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  CHECK(regular_socket >= 0);
  CHECK(!close(regular_socket));
  CHECK(!validate_environment_at_runtime(runtime, getuid(), &environment));
  CHECK(!unlink(socket_path));
  CHECK(!symlink(environment.xauthority, socket_path));
  CHECK(!validate_environment_at_runtime(runtime, getuid(), &environment));
  CHECK(!unlink(socket_path));

  CHECK(!unlink(environment.xauthority));
  CHECK(!symlink("/dev/null", environment.xauthority));
  CHECK(!validate_environment_at_runtime(runtime, getuid(), &environment));
  CHECK(!unlink(environment.xauthority));
  CHECK(!close(listener));
  CHECK(!rmdir(runtime));
  return 0;
}

int main(void) {
  static const char valid[] =
    "/usr/bin/kwin_wayland\0--wayland-fd\0"
    "7\0--socket\0wayland-0\0"
    "--xwayland-fd\0"
    "8\0--xwayland-display\0:0\0"
    "--xwayland-xauthority\0/run/user/1000/xauth_aB09-Z\0--xwayland\0";
  struct display_environment parsed = {0};
  CHECK(parse_kwin_arguments(valid, sizeof(valid), &parsed));
  CHECK(!strcmp(parsed.wayland_display, "wayland-0"));
  CHECK(!strcmp(parsed.x_display, ":0"));
  CHECK(!strcmp(parsed.xauthority, "/run/user/1000/xauth_aB09-Z"));

  static const char duplicate[] =
    "/usr/bin/kwin_wayland\0--socket\0wayland-0\0--socket\0wayland-1\0"
    "--xwayland-display\0:0\0--xwayland-xauthority\0/run/user/1000/xauth_a\0";
  CHECK(!parse_kwin_arguments(duplicate, sizeof(duplicate), &parsed));

  static const char missing_xauthority[] =
    "/usr/bin/kwin_wayland\0--socket\0wayland-0\0--xwayland-display\0:0\0";
  CHECK(!parse_kwin_arguments(missing_xauthority, sizeof(missing_xauthority), &parsed));

  static const char wayland_only[] =
    "/usr/bin/kwin_wayland\0--no-lockscreen\0--no-global-shortcuts\0";
  CHECK(parse_kwin_arguments(wayland_only, sizeof(wayland_only), &parsed));
  CHECK(!parsed.wayland_display[0]);
  CHECK(!parsed.x_display[0]);
  CHECK(!parsed.xauthority[0]);

  static const char missing_xdisplay[] =
    "/usr/bin/kwin_wayland\0--socket\0wayland-0\0"
    "--xwayland-xauthority\0/run/user/1000/xauth_a\0";
  CHECK(!parse_kwin_arguments(missing_xdisplay, sizeof(missing_xdisplay), &parsed));

  static const char malformed_display[] =
    "/usr/bin/kwin_wayland\0--socket\0wayland-0\0--xwayland-display\0localhost:0\0"
    "--xwayland-xauthority\0/run/user/1000/xauth_a\0";
  CHECK(!parse_kwin_arguments(malformed_display, sizeof(malformed_display), &parsed));

  CHECK(valid_wayland_display("wayland-0"));
  CHECK(!valid_wayland_display("wayland-a"));
  CHECK(valid_xdisplay(":0"));
  CHECK(valid_xdisplay(":12.3"));
  CHECK(!valid_xdisplay("localhost:0"));
  CHECK(!valid_xdisplay(":0."));
  CHECK(xauthority_path_is_confined("/run/user/1000", "/run/user/1000/xauth_aB09-Z"));
  CHECK(!xauthority_path_is_confined("/run/user/1000", "/run/user/1000/../1001/xauth_a"));
  CHECK(!xauthority_path_is_confined("/run/user/1000", "/tmp/xauth_a"));
  CHECK(!xauthority_path_is_confined("/run/user/1000", "/run/user/1000/not-xauth"));
  CHECK(runtime_mode_is_safe(0700));
  CHECK(runtime_mode_is_safe(0710));
  CHECK(!runtime_mode_is_safe(0770));
  CHECK(!runtime_mode_is_safe(0701));
  CHECK(runtime_path_matches_uid("/run/user/1000", 1000));
  CHECK(!runtime_path_matches_uid("/run/user/1001", 1000));
  CHECK(!runtime_path_matches_uid("/tmp/runtime", 1000));
  CHECK(!runtime_path_matches_uid("/run/user/0", 0));
  CHECK(session_kind_from_cgroup("/user.slice/session.slice/plasma-kwin_wayland.service") == KWIN_SESSION_DESKTOP);
  CHECK(session_kind_from_cgroup("/user.slice/session.slice/plasma-login-kwin_wayland.service") == KWIN_SESSION_GREETER);
  CHECK(session_kind_from_cgroup("/user.slice/session.slice/other.service") == KWIN_SESSION_INVALID);
  struct display_environment desktop_environment = {
    .wayland_display = "wayland-0",
    .x_display = ":0",
    .xauthority = "/run/user/1000/xauth_a",
  };
  CHECK(environment_matches_session_kind(KWIN_SESSION_DESKTOP, &desktop_environment));
  CHECK(!environment_matches_session_kind(KWIN_SESSION_GREETER, &desktop_environment));
  struct display_environment greeter_environment = {
    .wayland_display = "wayland-0",
  };
  CHECK(environment_matches_session_kind(KWIN_SESSION_GREETER, &greeter_environment));
  CHECK(!environment_matches_session_kind(KWIN_SESSION_DESKTOP, &greeter_environment));
  CHECK(execution_identity_is_safe(1000, 1000, 1000, 1000));
  CHECK(!execution_identity_is_safe(0, 0, 0, 0));
  CHECK(!execution_identity_is_safe(1000, 0, 1000, 1000));
  CHECK(!execution_identity_is_safe(1000, 1000, 1000, 0));

  char self_cgroup[MAXIMUM_CGROUP_BYTES] = {0};
  char self_cgroup_at[MAXIMUM_CGROUP_BYTES] = {0};
  CHECK(read_unified_cgroup(getpid(), self_cgroup, sizeof(self_cgroup)));
  char self_path[64] = {0};
  const int self_path_length = snprintf(self_path, sizeof(self_path), "/proc/%ld", (long) getpid());
  CHECK(self_path_length > 0 && (size_t) self_path_length < sizeof(self_path));
  const int self = open(self_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  CHECK(self >= 0);
  CHECK(read_unified_cgroup_at(self, self_cgroup_at, sizeof(self_cgroup_at)));
  CHECK(!strcmp(self_cgroup, self_cgroup_at));
  CHECK(!process_is_session_kwin(self, getuid(), self_cgroup));
  char self_command_line[MAXIMUM_COMMAND_LINE_BYTES] = {0};
  size_t self_command_line_length = 0;
  CHECK(read_file_at_bounded(self, "cmdline", self_command_line, sizeof(self_command_line), &self_command_line_length));
  CHECK(self_command_line_length > 0);
  CHECK(!read_file_at_bounded(self, "../cmdline", self_command_line, sizeof(self_command_line), &self_command_line_length));
  CHECK(!close(self));
  CHECK(!filesystem_validation_works());

  puts("PASS: desktop and Wayland-only greeter KWin environment discovery and validation");
  return 0;
}
