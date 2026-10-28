#include "../../../../src/platform/linux/input/inputtino_touch_mapping.h"
#include "../../../../src/platform/linux/input/inputtino_touch_wait.h"

#include <chrono>
#include <future>
#include <thread>

using namespace std::chrono_literals;

int main() {
  // Successful and failed mappings have the same bounded revalidation window.
  // A connector name is not proof that its original KWin output still exists.
  platf::touch::mapping_retry_t retry;
  const auto now = std::chrono::steady_clock::time_point {};
  g_assert_true(retry.begin("Virtual-1", true, now));
  g_assert_false(retry.begin("Virtual-1", true, now + 1s));
  g_assert_true(retry.begin("Virtual-1", true, now + 5s));
  g_assert_true(retry.begin("DP-3", true, now + 5s));
  g_assert_false(retry.begin("", true, now + 5s));
  g_assert_true(retry.begin("DP-3", true, now + 5s));
  g_assert_false(retry.begin("DP-3", false, now + 5s));
  g_assert_true(retry.begin("DP-3", true, now + 5s));
  // A new client starts without the preceding client's cached retry deadline.
  platf::touch::mapping_retry_t reconnected;
  g_assert_true(reconnected.begin("DP-3", true, now + 5s));

  auto *success = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, nullptr, "/usr/bin/true", nullptr);
  g_assert_nonnull(success);
  g_assert_cmpint(platf::touch::wait_for_mapping(success), ==, 0);
  g_object_unref(success);

  // An unresponsive broker/session-bus path remains bounded.
  auto *stalled = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, nullptr, "/usr/bin/sleep", "30", nullptr);
  g_assert_nonnull(stalled);
  auto started = std::chrono::steady_clock::now();
  g_assert_cmpint(platf::touch::wait_for_mapping(stalled, 100), !=, 0);
  g_assert_true(std::chrono::steady_clock::now() - started < 1s);
  g_object_unref(stalled);

  // Disconnect cancels immediately, independently of the four-second deadline.
  auto *cancelled = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, nullptr, "/usr/bin/sleep", "30", nullptr);
  g_assert_nonnull(cancelled);
  auto result = std::async(std::launch::async, [cancelled] {
    return platf::touch::wait_for_mapping(cancelled);
  });
  std::this_thread::sleep_for(10ms);
  started = std::chrono::steady_clock::now();
  g_subprocess_force_exit(cancelled);
  g_assert_true(result.wait_for(500ms) == std::future_status::ready);
  g_assert_cmpint(result.get(), !=, 0);
  g_assert_true(std::chrono::steady_clock::now() - started < 500ms);
  g_object_unref(cancelled);
}
