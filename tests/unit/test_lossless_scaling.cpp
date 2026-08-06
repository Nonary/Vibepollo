/**
 * @file tests/unit/test_lossless_scaling.cpp
 */
#include "../tests_common.h"

#include <tools/playnite_launcher/lossless_scaling_policy.h>

namespace {

  using playnite_launcher::lossless::policy::restart_state;

  TEST(LosslessScalingRestart, LaunchesWhenNoHelperRunning) {
    restart_state state;
    state.stopped = false;
    EXPECT_TRUE(playnite_launcher::lossless::policy::should_launch_new_instance(state, false));
  }

  TEST(LosslessScalingRestart, SkipsWhenExistingHelperRunning) {
    restart_state state {.running_process_count = 1};
    state.stopped = false;
    EXPECT_FALSE(playnite_launcher::lossless::policy::should_launch_new_instance(state, false));
  }

  TEST(LosslessScalingRestart, LaunchesAfterStop) {
    restart_state state {.running_process_count = 1};
    state.stopped = true;
    EXPECT_TRUE(playnite_launcher::lossless::policy::should_launch_new_instance(state, false));
  }

  TEST(LosslessScalingRestart, ForceLaunchOverridesState) {
    restart_state state {.running_process_count = 1};
    state.stopped = false;
    EXPECT_TRUE(playnite_launcher::lossless::policy::should_launch_new_instance(state, true));
  }

  TEST(LosslessScalingFocusCandidate, FilteredCandidateRequiresWindow) {
    EXPECT_FALSE(playnite_launcher::lossless::policy::should_accept_focus_candidate(true, true, false));
  }

  TEST(LosslessScalingFocusCandidate, FilteredCandidateAcceptsMatchingWindowedProcess) {
    EXPECT_TRUE(playnite_launcher::lossless::policy::should_accept_focus_candidate(true, true, true));
  }

  TEST(LosslessScalingFocusCandidate, FilteredCandidateRejectsPathMismatch) {
    EXPECT_FALSE(playnite_launcher::lossless::policy::should_accept_focus_candidate(true, false, true));
  }

  TEST(LosslessScalingFocusCandidate, UnfilteredCandidateStillRequiresWindow) {
    EXPECT_FALSE(playnite_launcher::lossless::policy::should_accept_focus_candidate(false, false, false));
    EXPECT_TRUE(playnite_launcher::lossless::policy::should_accept_focus_candidate(false, false, true));
  }

  TEST(LosslessScalingFilter, NormalizesAndJoinsExecutableNames) {
    EXPECT_EQ(
      playnite_launcher::lossless::policy::build_executable_filter({L"Re9.exe", L"helper.EXE"}),
      L"re9.exe;helper.exe"
    );
  }

  TEST(LosslessScalingLaunchExe, ExplicitPathOverridesRuntimePath) {
    auto selected = playnite_launcher::lossless::policy::select_launch_executable(L"custom-lossless.exe", L"runtime-lossless.exe");
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(*selected, L"custom-lossless.exe");
  }

}  // namespace
