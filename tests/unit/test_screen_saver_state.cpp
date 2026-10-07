#include "../tests_common.h"
#include <src/screen_saver_state.h>
#include <src/utility.h>
#include <system_error>

namespace {
  struct ScreenSaverRestore: testing::Test {
    platf::screen_saver_state_t state;
    bool active = true;
    int reads = 0;
    int writes = 0;

    bool begin() {
      return state.begin([&] { ++reads; return std::optional {active}; });
    }
    bool write(bool value) {
      ++writes;
      active = value;
      return true;
    }
    bool restore() {
      return state.restore([&](bool value) { return write(value); });
    }
    bool finish(platf::screen_saver_state_t::token_t token) {
      return state.finish(token, [&](bool value) { return write(value); });
    }
  };
}  // namespace

TEST_F(ScreenSaverRestore, PauseRestoresImmediatelyBeforeWorkerCompletes) {
  ASSERT_TRUE(begin());
  active = false;
  const auto pause = state.defer();
  EXPECT_TRUE(restore());
  EXPECT_TRUE(active);  // A long-lived command cannot delay restoration.
  EXPECT_EQ(state.value(), true);  // The worker still owns the baseline.
  active = false;
  EXPECT_TRUE(finish(pause));
  EXPECT_TRUE(active);
  EXPECT_FALSE(state.value().has_value());
}

TEST_F(ScreenSaverRestore, ResumePreservesBaselineAndSuppressesOlderWorkerRestore) {
  begin();
  const auto pause = state.defer();
  restore();
  active = false;  // The old command writes before the next resume caches state.
  begin();
  EXPECT_EQ(reads, 1);
  const auto before_finish = writes;
  EXPECT_TRUE(finish(pause));
  EXPECT_EQ(writes, before_finish);
  EXPECT_FALSE(active);
  EXPECT_EQ(state.value(), true);
  EXPECT_TRUE(restore());
  EXPECT_TRUE(active);
}

TEST_F(ScreenSaverRestore, FailedImmediateRestoreRetainsBaselineForRetry) {
  begin();
  EXPECT_FALSE(state.restore([](bool) { return false; }));
  EXPECT_EQ(state.value(), true);
  EXPECT_TRUE(restore());
  EXPECT_FALSE(state.value().has_value());
}

TEST_F(ScreenSaverRestore, FailedLastWorkerRestoreRetainsBaselineForRetry) {
  begin();
  const auto pause = state.defer();
  restore();
  active = false;
  EXPECT_FALSE(state.finish(pause, [](bool) { return false; }));
  EXPECT_EQ(state.value(), true);
  EXPECT_TRUE(restore());
  EXPECT_TRUE(active);
  EXPECT_FALSE(state.value().has_value());
}

TEST_F(ScreenSaverRestore, CompletedPauseRecapturesUsersNewDisabledSetting) {
  begin();
  const auto old_pause = state.defer();
  restore();
  finish(old_pause);
  active = false;
  begin();
  const auto before_finish = writes;
  finish(old_pause);
  EXPECT_EQ(writes, before_finish);
  EXPECT_EQ(reads, 2);
  EXPECT_EQ(state.value(), false);
  restore();
  EXPECT_FALSE(active);
}

TEST_F(ScreenSaverRestore, PauseWithoutCommandsRecapturesUsersChangedSetting) {
  begin();
  restore();
  EXPECT_FALSE(state.value().has_value());
  active = false;
  begin();
  EXPECT_EQ(reads, 2);
  EXPECT_EQ(state.value(), false);
}

TEST_F(ScreenSaverRestore, FailedReadDoesNotInventEnabledState) {
  EXPECT_FALSE(state.begin([] { return std::optional<bool> {}; }));
  const auto pause = state.defer();
  restore();
  finish(pause);
  EXPECT_EQ(writes, 0);
  active = false;
  EXPECT_TRUE(begin());
  EXPECT_EQ(state.value(), false);
}

TEST_F(ScreenSaverRestore, LatePauseCanRestoreAgainAfterSameAppTerminates) {
  begin();
  const auto pause = state.defer();
  restore();  // Pause restores first.
  restore();  // Termination runs while its worker is still alive.
  active = false;
  finish(pause);
  EXPECT_TRUE(active);
  EXPECT_FALSE(state.value().has_value());
}

TEST_F(ScreenSaverRestore, NewAppCannotCacheOlderWorkersTemporaryDisabledSetting) {
  begin();
  const auto old_pause = state.defer();
  restore();
  restore();  // First app terminates while its worker is pending.
  active = false;
  begin();
  EXPECT_EQ(reads, 1);
  finish(old_pause);
  EXPECT_FALSE(active);  // An active app prevents older-worker restoration.
  restore();  // New app terminates with the original baseline intact.
  EXPECT_TRUE(active);
}

TEST_F(ScreenSaverRestore, OlderWorkerRestoresWhenNewerPauseAlreadyFinished) {
  begin();
  const auto old_pause = state.defer();
  restore();
  begin();
  const auto new_pause = state.defer();
  restore();
  finish(new_pause);
  EXPECT_EQ(state.value(), true);
  active = false;  // The older worker writes after the newer pause completes.
  finish(old_pause);
  EXPECT_TRUE(active);
  EXPECT_FALSE(state.value().has_value());
}

TEST_F(ScreenSaverRestore, DuplicateCompletionCannotReleaseAnotherWorker) {
  active = false;
  begin();
  const auto first = state.defer();
  const auto second = state.defer();
  restore();
  finish(first);
  const auto before_duplicate = writes;
  finish(first);
  EXPECT_EQ(writes, before_duplicate);
  EXPECT_EQ(state.value(), false);
  finish(second);
  EXPECT_EQ(writes, before_duplicate + 1);
  EXPECT_FALSE(active);
  EXPECT_FALSE(state.value().has_value());
}

TEST_F(ScreenSaverRestore, FailedWorkerLaunchReleasesItsPendingRestoration) {
  begin();
  const auto pending = state.defer();
  try {
    auto launch_failure = util::fail_guard([&] { finish(pending); });
    restore();
    throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
  } catch (const std::system_error &) {
  }
  EXPECT_EQ(writes, 2);
  EXPECT_FALSE(state.value().has_value());
  active = false;
  begin();
  EXPECT_EQ(state.value(), false);
}
