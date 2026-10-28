#include "vibepollo-touch-map.h"
#ifdef VIBEPOLLO_HAVE_WAYLAND_TOUCH_MAP
#include "vibepollo-touch-output.h"
#endif

#include <signal.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
  if (argc != 3) return 2;
  // Bound session-bus connection/authentication as well as property requests.
  signal(SIGALRM, SIG_DFL);
  signal(SIGTERM, SIG_DFL);
  signal(SIGINT, SIG_DFL);
  signal(SIGHUP, SIG_DFL);
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGALRM);
  sigaddset(&signals, SIGTERM);
  sigaddset(&signals, SIGINT);
  sigaddset(&signals, SIGHUP);
  sigprocmask(SIG_UNBLOCK, &signals, NULL);
  alarm(3);
#ifdef VIBEPOLLO_HAVE_WAYLAND_TOUCH_MAP
  if (!touch_map_arguments_are_safe(argv[1], argv[2])) return 126;
  struct touch_output_probe probe = {.display = wl_display_connect(NULL)};
  int result = 1;
  if (touch_output_prepare(&probe, argv[2]) && touch_output_available(&probe)) {
    result = touch_map_run(argv[1], argv[2]);
    // outputName readback alone is insufficient: KWin can echo a name for
    // an output which no longer exists. Retain the same wl_output identity.
    if (!touch_output_available(&probe)) result = 1;
  }
  touch_output_close(&probe);
#else
  const int result = 3; // No Wayland support: retain ordinary touch injection.
#endif
  if (result && result != 3) fprintf(stderr, "vibepollo-touch-map: unable to map %s to %s (%d)\n", argv[1], argv[2], result);
  return result;
}
