#define main display_power_helper_entry
#include "../../../../packaging/linux/vibepollo-display-power.c"
#undef main

int main(void) {
  char *const success[] = {"true", NULL};
  char *const failure[] = {"false", NULL};
  char *const stalled[] = {"sleep", "30", NULL};
  g_assert_cmpint(run_command_bounded("/usr/bin/true", success, 500), ==, 0);
  g_assert_cmpint(run_command_bounded("/usr/bin/false", failure, 500), ==, 1);
  const gint64 started = g_get_monotonic_time();
  g_assert_cmpint(run_command_bounded("/usr/bin/sleep", stalled, 100), ==, 124);
  g_assert_cmpint(g_get_monotonic_time() - started, <, G_TIME_SPAN_SECOND);
  int status;
  g_assert_cmpint(waitpid(-1, &status, WNOHANG), ==, -1);
  g_assert_cmpint(errno, ==, ECHILD);
  return 0;
}
