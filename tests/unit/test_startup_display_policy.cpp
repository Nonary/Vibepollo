/**
 * @file tests/unit/test_startup_display_policy.cpp
 * @brief Cold-start display/listener ordering contracts.
 */
#include "../tests_common.h"

#include <src/platform/windows/startup_display_policy.h>

namespace {
  using platf::startup_display_policy::should_retry;
  using platf::startup_display_policy::should_run;
  using platf::startup_display_policy::state;

  TEST(StartupDisplayPolicy, DefersBeforeInteractiveDesktopWithoutActiveStream) {
    const state value {.interactive_desktop = false, .stream_active = false, .shutting_down = false};
    EXPECT_FALSE(should_run(value));
    EXPECT_TRUE(should_retry(value));
  }

  TEST(StartupDisplayPolicy, RunsAfterDesktopReadyWhenNoSessionOwnsDisplay) {
    const state value {.interactive_desktop = true, .stream_active = false, .shutting_down = false};
    EXPECT_TRUE(should_run(value));
    EXPECT_FALSE(should_retry(value));
  }

  TEST(StartupDisplayPolicy, LeavesDisplayToAnActiveSession) {
    const state value {.interactive_desktop = true, .stream_active = true, .shutting_down = false};
    EXPECT_FALSE(should_run(value));
    EXPECT_FALSE(should_retry(value));
  }

  TEST(StartupDisplayPolicy, StopsRetryOnShutdown) {
    const state value {.interactive_desktop = false, .stream_active = false, .shutting_down = true};
    EXPECT_FALSE(should_run(value));
    EXPECT_FALSE(should_retry(value));
  }
}  // namespace
