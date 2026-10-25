#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <sys/capability.h>
static unsigned identity_boundary_calls;
static cap_t fixture_cap_get_proc(void) {
  ++identity_boundary_calls;
  errno = EPERM;
  return NULL;
}
#define cap_get_proc fixture_cap_get_proc
#define main vibepollo_session_broker_entrypoint
#include "../../../../packaging/linux/vibepollo-session-broker.c"
#undef main
#undef cap_get_proc

int main(void) {
  char *observe[] = {"broker", "display-observe", NULL};
  assert(display_observe_arguments_are_safe(2, observe));
  struct session_identity identity = {0};
  strcpy(identity.role, "desktop");
  strcpy(identity.backend, "hyprland-kms");
  strcpy(identity.compositor, "1234567890abcdef1234567890abcdef-42-123");
  assert(valid_backend_identity(&identity));
  assert(validate_xauthority(&identity));
  // Hyprland's Xwayland uses no Xauthority. The controller publishes :N only
  // after binding that X socket's peer to the record's compositor.
  strcpy(identity.x_display, ":0");
  assert(valid_backend_identity(&identity));
  assert(validate_xauthority(&identity));
  strcpy(identity.xauthority, "/run/user/1000/xauth_fixture");
  assert(!valid_backend_identity(&identity));
  identity.xauthority[0] = 0;
  identity.x_display[0] = 0;
  strcpy(identity.compositor, "00000000000000000000000000000000-42-123");
  assert(!valid_backend_identity(&identity));
  strcpy(identity.compositor, "1234567890abcdef1234567890abcdef-42-123");
  strcpy(identity.role, "greeter");
  assert(!valid_backend_identity(&identity));
  strcpy(identity.role, "desktop");
  for (size_t i = 0; i < 4; ++i) {
    const char *verbs[] = {"display-query", "display-apply", "display-power", "display-wake"};
    char *args[] = {"broker", (char *) verbs[i], "output.DP-1.disable", NULL};
    // Production dispatch must reject before attempting identity drop. The
    // capability syscall adapter records that boundary and fails inertly;
    // removing the Hyprland guard must fail even if a later check returns 126.
    identity_boundary_calls = 0;
    assert(execute_request(i == 1 ? 3 : 2, args, &identity, getgid()) == 126);
    assert(identity_boundary_calls == 0);
  }
  assert(!hyprland_request_supported(&identity, "touch-map"));
  assert(!hyprland_request_supported(&identity, "managed-focus"));
  assert(!hyprland_request_supported(&identity, "app-wayland-hdr"));
  assert(hyprland_request_supported(&identity, "app"));
  assert(hyprland_request_supported(&identity, "audio-capture"));
  assert(hyprland_request_supported(&identity, "display-observe"));
  assert(hyprland_request_supported(&identity, "provider-steam-scan"));
  // Steam and Lutris launches need the bound Xwayland display.
  char *steam[] = {"broker", "steam", "3259780", NULL};
  char *lutris[] = {"broker", "lutris", "7", NULL};
  char *big_picture[] = {"broker", "app", "setsid steam steam://open/bigpicture", NULL};
  struct { char **args; int argc; } launches[] = {{steam, 3}, {lutris, 3}, {big_picture, 3}};
  for (size_t i = 0; i < sizeof(launches) / sizeof(launches[0]); ++i) {
    identity_boundary_calls = 0;
    assert(execute_request(launches[i].argc, launches[i].args, &identity, getgid()) == 126);
    assert(identity_boundary_calls == 0);
  }
  assert(!hyprland_request_supported(&identity, "steam-direct"));
  // systemd applies UnsetEnvironment= after --setenv assignments, so the unit
  // may drop only what the record does not set itself.
  assert(!strcmp(hyprland_unset_environment(&identity), "--property=UnsetEnvironment=DISPLAY XAUTHORITY"));
  strcpy(identity.x_display, ":0");
  assert(!strcmp(hyprland_unset_environment(&identity), "--property=UnsetEnvironment=XAUTHORITY"));
  for (size_t i = 0; i < sizeof(launches) / sizeof(launches[0]); ++i) {
    identity_boundary_calls = 0;
    assert(execute_request(launches[i].argc, launches[i].args, &identity, getgid()) == 126);
    assert(identity_boundary_calls == 1);
  }
  assert(hyprland_request_supported(&identity, "steam-direct"));
  assert(!hyprland_request_supported(&identity, "lutris-wayland-hdr"));
  identity.x_display[0] = 0;
  strcpy(identity.backend, "kwin");
  identity.compositor[0] = 0;
  assert(valid_backend_identity(&identity));
  assert(!validate_xauthority(&identity));
  char *control[] = {"broker", "display-query", NULL};
  identity_boundary_calls = 0;
  assert(execute_request(2, control, &identity, getgid()) == 126);
  assert(identity_boundary_calls == 1); // KWin actually reaches the inert edge.
  puts("PASS: explicit Hyprland identity and broker operation boundary; X11 launches need the bound Xwayland display; KWin still requires Xauthority");
  return 0;
}
