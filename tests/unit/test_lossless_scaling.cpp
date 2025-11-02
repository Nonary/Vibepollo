/**
 * @file tests/unit/test_lossless_scaling.cpp
 */
#include "../tests_common.h"

#ifdef _WIN32
#include <tools/playnite_launcher/lossless_scaling.h>

namespace {

using playnite_launcher::lossless::lossless_scaling_runtime_state;

TEST(LosslessScalingRestart, LaunchesWhenNoHelperRunning) {
  lossless_scaling_runtime_state state;
  state.running_pids.clear();
  state.stopped = false;
  EXPECT_TRUE(playnite_launcher::lossless::should_launch_new_instance_for_tests(state, false));
}

TEST(LosslessScalingRestart, SkipsWhenExistingHelperRunning) {
  lossless_scaling_runtime_state state;
  state.running_pids.push_back(1234);
  state.stopped = false;
  EXPECT_FALSE(playnite_launcher::lossless::should_launch_new_instance_for_tests(state, false));
}

TEST(LosslessScalingRestart, LaunchesAfterStop) {
  lossless_scaling_runtime_state state;
  state.running_pids.push_back(1234);
  state.stopped = true;
  EXPECT_TRUE(playnite_launcher::lossless::should_launch_new_instance_for_tests(state, false));
}

TEST(LosslessScalingRestart, ForceLaunchOverridesState) {
  lossless_scaling_runtime_state state;
  state.running_pids.push_back(1234);
  state.stopped = false;
  EXPECT_TRUE(playnite_launcher::lossless::should_launch_new_instance_for_tests(state, true));
}

}  // namespace
#endif
