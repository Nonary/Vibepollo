#include "src/logging.h"

#include <cstdarg>
#include <iostream>
#include <gio/gio.h>
#include <thread>
#include <vector>

// Exercise the production lease owner with real child processes, replacing
// only the fixed broker spawn and logging. No live broker or desktop is used.
#undef BOOST_LOG
#define BOOST_LOG(level) std::cerr

static const char *test_executable;
static unsigned power_spawns;
static unsigned wake_spawns;
static unsigned retain_spawns;
static bool reject_wake;
static GSubprocess *observed_worker;
static GSubprocess *observed_retain;

static GSubprocess *test_spawn(GSubprocessFlags flags, GError **error, const char *path, ...) {
  g_assert_cmpstr(path, ==, "/usr/libexec/vibeshine/vibepollo-session-exec");
  va_list arguments;
  va_start(arguments, path);
  const char *operation = va_arg(arguments, const char *);
  g_assert_null(va_arg(arguments, const char *));
  va_end(arguments);
  const bool power = !strcmp(operation, "display-power");
  const bool retain = !strcmp(operation, "display-retain");
  g_assert_true(power || retain || !strcmp(operation, "display-wake"));
  const char *mode = power || retain ? "--power" : (reject_wake ? "--reject" : "--wake");
  GSubprocess *process = g_subprocess_new(flags, error, test_executable, mode, nullptr);
  if (power) {
    ++power_spawns;
    g_clear_object(&observed_worker);
    observed_worker = G_SUBPROCESS(g_object_ref(process));
  } else if (retain) {
    ++retain_spawns;
    g_clear_object(&observed_retain);
    observed_retain = G_SUBPROCESS(g_object_ref(process));
  } else {
    ++wake_spawns;
  }
  return process;
}

#define g_subprocess_new test_spawn
#include "../../../../src/platform/linux/display_power.cpp"
#undef g_subprocess_new

int main(int argc, char **argv) {
  if (argc == 2) {
    if (!strcmp(argv[1], "--reject")) return write(STDOUT_FILENO, "X", 1) == 1 ? 126 : 1;
    if (write(STDOUT_FILENO, "R", 1) != 1) return 1;
    if (!strcmp(argv[1], "--power")) {
      for (;;) pause();
    }
    return 0;
  }
  test_executable = argv[0];
  unsetenv("VIBEPOLLO_MACHINE_HOST");
  g_assert_nonnull(platf::display_power::acquire().get());
  g_assert_nonnull(platf::display_power::retain_screen().get());
  g_assert_cmpuint(power_spawns, ==, 0);
  g_assert_cmpuint(retain_spawns, ==, 0);
  setenv("VIBEPOLLO_MACHINE_HOST", "1", 1);
  auto first = platf::display_power::acquire();
  g_assert_nonnull(first.get());
  auto second = platf::display_power::acquire();
  g_assert_true(first == second);
  g_assert_cmpuint(power_spawns, ==, 1);
  g_assert_cmpuint(wake_spawns, ==, 1);

  // A retained topology gets its own screen-only worker. It must not share
  // (and so prolong) the capture lease, which also inhibits suspend.
  auto retained = platf::display_power::retain_screen();
  g_assert_nonnull(retained.get());
  g_assert_true(retained != first);
  g_assert_cmpuint(retain_spawns, ==, 1);
  g_assert_cmpuint(power_spawns, ==, 1);
  g_assert_cmpuint(wake_spawns, ==, 1);

  std::vector<std::shared_ptr<void>> peers(8);
  std::vector<std::thread> requests;
  for (auto &peer : peers) requests.emplace_back([&peer] { peer = platf::display_power::acquire(); });
  for (auto &request : requests) request.join();
  for (const auto &peer : peers) g_assert_true(peer == first);
  g_assert_cmpuint(power_spawns, ==, 1);
  g_assert_cmpuint(wake_spawns, ==, 9);
  first.reset();
  peers.clear();
  g_assert_true(std::static_pointer_cast<platf::display_power::lease_t>(second)->alive());
  second.reset();
  g_assert_true(g_subprocess_wait(observed_worker, nullptr, nullptr));
  g_assert_true(g_subprocess_get_if_signaled(observed_worker));
  // Capture teardown leaves the retained hold until its owner restores.
  g_assert_true(std::static_pointer_cast<platf::display_power::lease_t>(retained)->alive());
  retained.reset();
  g_assert_true(g_subprocess_wait(observed_retain, nullptr, nullptr));
  g_assert_true(g_subprocess_get_if_signaled(observed_retain));
  g_clear_object(&observed_retain);

  // Pending expiry/last capture teardown permits a fresh holder next time.
  first = platf::display_power::acquire();
  g_assert_cmpuint(power_spawns, ==, 2);
  // A dead generation must not be reused even while an old owner references it.
  g_subprocess_force_exit(observed_worker);
  g_assert_true(g_subprocess_wait(observed_worker, nullptr, nullptr));
  second = platf::display_power::acquire();
  g_assert_nonnull(second.get());
  g_assert_true(first == second); // Existing active holders own the replacement too.
  g_assert_cmpuint(power_spawns, ==, 3);
  second.reset();
  g_assert_true(std::static_pointer_cast<platf::display_power::lease_t>(first)->alive());
  reject_wake = true;
  g_assert_null(platf::display_power::acquire().get());
  g_assert_true(std::static_pointer_cast<platf::display_power::lease_t>(first)->alive());
  first.reset();
  g_assert_true(g_subprocess_wait(observed_worker, nullptr, nullptr));
  g_clear_object(&observed_worker);
  return 0;
}
