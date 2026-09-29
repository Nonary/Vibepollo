#include <gtest/gtest.h>
#include <src/platform/linux/display_helper_process.h>

#include <future>

using namespace std::chrono_literals;
namespace helper = platf::linux_private_display::helper_process;

namespace {
  GSubprocess *spawn(const char *command) {
    GError *error = nullptr;
    auto *process = g_subprocess_new(
      static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE),
      &error, "/bin/sh", "-c", command, nullptr);
    if (error) { ADD_FAILURE() << error->message; g_error_free(error); }
    return process;
  }
}

TEST(LinuxDisplayHelperProcess, CollectsBothStreamsAndExitStatus) {
  auto *process = spawn("printf reply; printf diagnostic >&2; exit 23");
  ASSERT_NE(process, nullptr);
  const auto result = helper::communicate_until(process, std::chrono::steady_clock::now() + 1s);
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.timed_out);
  EXPECT_FALSE(result.completion_unknown);
  EXPECT_EQ(result.stdout_text, "reply");
  EXPECT_EQ(result.stderr_text, "diagnostic");
  g_object_unref(process);
}

TEST(LinuxDisplayHelperProcess, HungHelperTimesOutAndIsTerminated) {
  auto *process = spawn("exec sleep 30");
  ASSERT_NE(process, nullptr);
  const auto start = std::chrono::steady_clock::now();
  const auto result = helper::communicate_until(process, start + 50ms);
  EXPECT_TRUE(result.timed_out);
  EXPECT_TRUE(result.completion_unknown);
  EXPECT_FALSE(result.success);
  EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
  EXPECT_TRUE(g_subprocess_wait(process, nullptr, nullptr));
  EXPECT_TRUE(g_subprocess_get_if_signaled(process));
  g_object_unref(process);
}

TEST(LinuxDisplayHelperProcess, LostBrokerAcknowledgementIsDistinctFromKnownFailure) {
  auto *process = spawn("exit 125");
  ASSERT_NE(process, nullptr);
  auto result = helper::communicate_until(process, std::chrono::steady_clock::now() + 1s, true);
  EXPECT_FALSE(result.success);
  EXPECT_TRUE(result.completion_unknown);
  g_object_unref(process);
  process = spawn("exit 126");
  ASSERT_NE(process, nullptr);
  result = helper::communicate_until(process, std::chrono::steady_clock::now() + 1s, true);
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.completion_unknown);
  g_object_unref(process);
}

TEST(LinuxDisplayHelperProcess, SignaledStandaloneHelperCannotConfirmCompositorCompletion) {
  auto *process = spawn("kill -KILL $$");
  ASSERT_NE(process, nullptr);
  const auto result = helper::communicate_until(process, std::chrono::steady_clock::now() + 1s);
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.timed_out);
  EXPECT_TRUE(result.completion_unknown);
  g_object_unref(process);
}

TEST(LinuxDisplayHelperProcess, ConcurrentRequestsUseIndependentMainContexts) {
  auto hung = std::async(std::launch::async, [] {
    auto *process = spawn("exec sleep 30");
    auto result = helper::communicate_until(process, std::chrono::steady_clock::now() + 100ms);
    g_object_unref(process);
    return result;
  });
  auto *process = spawn("printf ready");
  ASSERT_NE(process, nullptr);
  const auto result = helper::communicate_until(process, std::chrono::steady_clock::now() + 1s);
  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.stdout_text, "ready");
  EXPECT_TRUE(hung.get().timed_out);
  g_object_unref(process);
}
