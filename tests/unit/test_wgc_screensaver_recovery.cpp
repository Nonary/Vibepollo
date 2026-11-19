/**
 * @file tests/unit/test_wgc_screensaver_recovery.cpp
 * @brief Replay saver recovery with no Windows/GPU dependencies or wall-clock waits.
 */
#include "../tests_common.h"
#include "src/platform/windows/wgc_screensaver_recovery.h"

using namespace std::chrono_literals;
using platf::dxgi::wgc_screensaver::recovery_t;

namespace {
  auto at(std::chrono::milliseconds value) {
    return recovery_t::clock::time_point(value);
  }
}  // namespace

TEST(WgcScreensaverRecovery, IdleAndUnknownNeverRestartEvenAfterLongCaptureSilence) {
  recovery_t state;
  for (int i = 0; i < 100; ++i) {
    state.observe(i % 2 ? std::optional<bool>(false) : std::nullopt, at(i * 1000ms));
    EXPECT_FALSE(state.restart_wgc(at(i * 1000ms)));
    EXPECT_FALSE(state.hold_dxgi(at(i * 1000ms)));
  }
}

TEST(WgcScreensaverRecovery, StartupRunningSelectsDxgiOnce) {
  recovery_t state;
  state.observe(true, at(0ms), true);
  EXPECT_TRUE(state.prefer_dxgi(at(0ms)));
  state.starting_dxgi();
  EXPECT_TRUE(state.hold_dxgi(at(0ms)));
  state.observe(true, at(2000ms), true);
  EXPECT_FALSE(state.prefer_dxgi(at(2000ms)));
  EXPECT_TRUE(state.hold_dxgi(at(2000ms)));
}

TEST(WgcScreensaverRecovery, MidSessionRequiresRepeatedPositiveObservation) {
  recovery_t state;
  state.observe(false, at(0ms), true);
  state.starting_wgc();
  state.observe(true, at(1000ms));
  EXPECT_FALSE(state.restart_wgc(at(1000ms)));
  state.observe(true, at(1249ms));
  EXPECT_FALSE(state.restart_wgc(at(1249ms)));
  state.observe(true, at(1250ms));
  EXPECT_TRUE(state.restart_wgc(at(1250ms)));
  // Does not depend on a no-frame timeout: live but wrong pixels also recover.
  state.observe(true, at(1260ms), true);
  state.starting_dxgi();
  EXPECT_FALSE(state.restart_wgc(at(1260ms)));
}

TEST(WgcScreensaverRecovery, UnknownBreaksConfirmationAndCannotRearmFailedFallback) {
  recovery_t state;
  state.observe(true, at(0ms));
  state.observe(std::nullopt, at(250ms));
  state.observe(true, at(500ms));
  EXPECT_FALSE(state.prefer_dxgi(at(500ms)));
  state.observe(true, at(750ms));
  EXPECT_TRUE(state.prefer_dxgi(at(750ms)));
  state.starting_dxgi();
  state.dxgi_failed();
  state.starting_wgc();
  for (int i = 1; i <= 20; ++i) {
    state.observe(i % 2 ? std::nullopt : std::optional<bool>(true), at(750ms + i * 1000ms), true);
    EXPECT_FALSE(state.restart_wgc(at(750ms + i * 1000ms)));
  }
}

TEST(WgcScreensaverRecovery, DismissalConfirmsBeforeReturningToFreshWgc) {
  recovery_t state;
  state.observe(true, at(0ms), true);
  state.starting_dxgi();
  state.observe(false, at(500ms));
  EXPECT_TRUE(state.hold_dxgi(at(500ms)));
  state.observe(false, at(750ms));
  EXPECT_FALSE(state.hold_dxgi(at(750ms)));
  EXPECT_TRUE(state.restart_wgc(at(750ms)));
  state.starting_wgc();
  state.observe(false, at(1000ms));
  EXPECT_FALSE(state.restart_wgc(at(1000ms)));
}

TEST(WgcScreensaverRecovery, UnknownExpiresLeaseWithoutRepeatingSameFallback) {
  recovery_t state;
  state.observe(true, at(0ms), true);
  state.starting_dxgi();
  state.observe(std::nullopt, at(1000ms));
  EXPECT_TRUE(state.hold_dxgi(at(1500ms)));
  EXPECT_FALSE(state.hold_dxgi(at(1501ms)));
  state.starting_wgc();
  state.observe(true, at(2000ms), true);
  EXPECT_FALSE(state.prefer_dxgi(at(2000ms)));
  EXPECT_FALSE(state.restart_wgc(at(2000ms)));
}

TEST(WgcScreensaverRecovery, FailedFallbackStillRefreshesOnceAfterConfirmedDismissal) {
  recovery_t state;
  state.observe(true, at(0ms), true);
  state.starting_dxgi();
  state.dxgi_failed();
  state.starting_wgc();
  state.observe(false, at(500ms));
  state.observe(std::nullopt, at(750ms));
  state.observe(false, at(1000ms));
  EXPECT_FALSE(state.restart_wgc(at(1000ms)));
  state.observe(false, at(1250ms));
  EXPECT_TRUE(state.restart_wgc(at(1250ms)));
  state.starting_wgc();
  EXPECT_FALSE(state.restart_wgc(at(1500ms)));
}

TEST(WgcScreensaverRecovery, DesktopChangedBeforeFactoryDoesNotCauseRestartLoop) {
  recovery_t state;
  state.observe(true, at(0ms));
  state.observe(true, at(250ms));
  EXPECT_TRUE(state.restart_wgc(at(250ms)));
  state.observe(std::nullopt, at(300ms), true);
  EXPECT_FALSE(state.prefer_dxgi(at(300ms)));
  state.starting_wgc();
  state.observe(true, at(600ms));
  EXPECT_FALSE(state.restart_wgc(at(600ms)));
}

TEST(WgcScreensaverRecovery, StaleRunningSampleCannotSelectOrHoldDxgi) {
  recovery_t state;
  state.observe(true, at(0ms), true);
  EXPECT_FALSE(state.prefer_dxgi(at(1501ms)));
  state.starting_dxgi();
  EXPECT_FALSE(state.hold_dxgi(at(1501ms)));
}

TEST(WgcScreensaverRecovery, RapidFalseTrueFlappingIsBoundedByCooldown) {
  recovery_t state;
  state.observe(true, at(0ms), true);
  state.starting_dxgi();
  state.observe(false, at(250ms));
  state.observe(false, at(500ms));
  state.starting_wgc();
  state.observe(true, at(750ms), true);
  state.observe(true, at(1000ms), true);
  EXPECT_FALSE(state.restart_wgc(at(1000ms)));
  state.observe(true, at(5500ms));
  EXPECT_FALSE(state.restart_wgc(at(5500ms)));
  state.observe(true, at(5750ms));
  EXPECT_TRUE(state.restart_wgc(at(5750ms)));
}

TEST(WgcScreensaverRecovery, PollingIsRateLimitedAndFreshAtClockEpoch) {
  recovery_t state;
  EXPECT_TRUE(state.probe_due(at(0ms)));
  state.observe(false, at(0ms));
  EXPECT_FALSE(state.probe_due(at(249ms)));
  EXPECT_TRUE(state.probe_due(at(250ms)));
}

TEST(WgcScreensaverRecovery, SeparateProducerStateDoesNotConsumeAnotherRecovery) {
  recovery_t first, second;
  first.observe(true, at(0ms), true);
  second.observe(true, at(0ms), true);
  first.starting_dxgi();
  EXPECT_FALSE(first.prefer_dxgi(at(0ms)));
  EXPECT_TRUE(second.prefer_dxgi(at(0ms)));
}

TEST(WgcScreensaverRecovery, LongObservationGapsDoNotConfirmActivationOrDismissal) {
  recovery_t state;
  state.observe(true, at(0ms));
  state.observe(true, at(60000ms));
  EXPECT_FALSE(state.restart_wgc(at(60000ms)));
  state.observe(true, at(60250ms));
  EXPECT_TRUE(state.restart_wgc(at(60250ms)));
  state.starting_dxgi();
  state.observe(false, at(60500ms));
  state.observe(false, at(120000ms));
  EXPECT_FALSE(state.restart_wgc(at(120000ms)));
  state.observe(false, at(120250ms));
  EXPECT_TRUE(state.restart_wgc(at(120250ms)));
}
