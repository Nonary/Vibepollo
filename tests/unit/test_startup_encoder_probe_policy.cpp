/**
 * @file tests/unit/test_startup_encoder_probe_policy.cpp
 * @brief Cold-start encoder-cache completion contracts.
 */
#include "../tests_common.h"

#include <src/platform/windows/startup_encoder_probe_policy.h>

namespace {
  using video::startup_encoder_probe_policy::should_probe;
  using video::startup_encoder_probe_policy::state;
  using video::startup_encoder_probe_policy::terminal_failure;
}  // namespace

TEST(StartupEncoderProbePolicy, RunsWithoutInteractiveDesktopState) {
  const state value {
    .cache_successful = false,
    .stream_active = false,
    .shutting_down = false,
    .attempts = 0,
    .max_attempts = 2,
  };
  EXPECT_TRUE(should_probe(value));
  EXPECT_FALSE(terminal_failure(value));
}

TEST(StartupEncoderProbePolicy, FailedAttemptRemainsRetryable) {
  const state value {
    .cache_successful = false,
    .stream_active = false,
    .shutting_down = false,
    .attempts = 1,
    .max_attempts = 2,
  };
  EXPECT_TRUE(should_probe(value));
  EXPECT_FALSE(terminal_failure(value));
}

TEST(StartupEncoderProbePolicy, SuccessfulCacheCompletesStartup) {
  const state value {
    .cache_successful = true,
    .stream_active = false,
    .shutting_down = false,
    .attempts = 1,
    .max_attempts = 2,
  };
  EXPECT_FALSE(should_probe(value));
  EXPECT_FALSE(terminal_failure(value));
}

TEST(StartupEncoderProbePolicy, RetryBudgetEndsInExplicitFailure) {
  const state value {
    .cache_successful = false,
    .stream_active = false,
    .shutting_down = false,
    .attempts = 2,
    .max_attempts = 2,
  };
  EXPECT_FALSE(should_probe(value));
  EXPECT_TRUE(terminal_failure(value));
}

TEST(StartupEncoderProbePolicy, ActiveSessionOwnsRuntimeLifecycle) {
  const state value {
    .cache_successful = false,
    .stream_active = true,
    .shutting_down = false,
    .attempts = 1,
    .max_attempts = 2,
  };
  EXPECT_FALSE(should_probe(value));
  EXPECT_FALSE(terminal_failure(value));
}

TEST(StartupEncoderProbePolicy, ShutdownStopsRetryWithoutReportingProbeFailure) {
  const state value {
    .cache_successful = false,
    .stream_active = false,
    .shutting_down = true,
    .attempts = 1,
    .max_attempts = 2,
  };
  EXPECT_FALSE(should_probe(value));
  EXPECT_FALSE(terminal_failure(value));
}
