/**
 * @file tests/unit/test_display_helper_session_deferral.cpp
 * @brief Unit tests for display-helper deferral and request policy.
 */
#include "../tests_common.h"
#include "src/platform/windows/display_helper_request_policy.h"
#include "src/platform/windows/display_helper_session_deferral.h"

namespace {
  using namespace display_helper_integration;
  namespace policy = display_helper_integration::request_policy;

  class FakeClock {
  public:
    [[nodiscard]] std::chrono::steady_clock::time_point now() const { return now_; }
    void advance(const std::chrono::milliseconds duration) { now_ += duration; }

  private:
    std::chrono::steady_clock::time_point now_ {std::chrono::steady_clock::now()};
  };

  DisplayApplyRequest make_request() {
    DisplayApplyRequest request;
    request.action = DisplayApplyAction::Apply;
    request.configuration = display_device::SingleDisplayConfiguration {};
    return request;
  }

  SessionDeferralManager::PendingSessionSnapshot make_snapshot() {
    SessionDeferralManager::PendingSessionSnapshot snapshot;
    snapshot.width = 1920;
    snapshot.height = 1080;
    snapshot.fps = 60;
    snapshot.enable_hdr = true;
    snapshot.enable_sops = true;
    snapshot.virtual_display = true;
    snapshot.virtual_display_device_id = "VD";
    snapshot.framegen_refresh_rate = 120;
    snapshot.gen1_framegen_fix = true;
    return snapshot;
  }

  policy::Input virtual_display_input() {
    policy::Input input;
    input.virtual_display = true;
    input.target_device_id = "{virtual-device}";
    return input;
  }
}  // namespace

TEST(DisplayHelperSessionDeferral, DelaysAndRestoresSessionSnapshot) {
  FakeClock clock;
  SessionDeferralManager manager([&clock] { return clock.now(); });
  manager.set_pending(make_request(), make_snapshot(), 42, true);

  EXPECT_EQ(manager.take_ready(false).status, SessionDeferralManager::TakeStatus::SessionNotReady);
  EXPECT_EQ(manager.take_ready(true).status, SessionDeferralManager::TakeStatus::DelayStarted);

  clock.advance(SessionDeferralManager::initial_delay() - std::chrono::milliseconds(1));
  EXPECT_EQ(manager.take_ready(true).status, SessionDeferralManager::TakeStatus::DelayPending);

  clock.advance(std::chrono::milliseconds(1));
  const auto result = manager.take_ready(true);
  ASSERT_EQ(result.status, SessionDeferralManager::TakeStatus::Ready);
  ASSERT_TRUE(result.pending.has_value());
  const auto &snapshot = result.pending->session_snapshot;
  EXPECT_EQ(snapshot.width, 1920);
  EXPECT_EQ(snapshot.height, 1080);
  EXPECT_EQ(snapshot.fps, 60);
  EXPECT_TRUE(snapshot.enable_hdr);
  EXPECT_TRUE(snapshot.enable_sops);
  EXPECT_TRUE(snapshot.virtual_display);
  EXPECT_EQ(snapshot.virtual_display_device_id, "VD");
  ASSERT_TRUE(snapshot.framegen_refresh_rate.has_value());
  EXPECT_EQ(*snapshot.framegen_refresh_rate, 120);
  EXPECT_TRUE(snapshot.gen1_framegen_fix);
  EXPECT_FALSE(snapshot.gen2_framegen_fix);
}

TEST(DisplayHelperSessionDeferral, ReschedulesAndDropsForNewerPending) {
  FakeClock clock;
  SessionDeferralManager manager([&clock] { return clock.now(); });
  manager.set_pending(make_request(), {}, 1, true);
  EXPECT_EQ(manager.take_ready(true).status, SessionDeferralManager::TakeStatus::DelayStarted);

  clock.advance(SessionDeferralManager::initial_delay());
  auto result = manager.take_ready(true);
  ASSERT_EQ(result.status, SessionDeferralManager::TakeStatus::Ready);
  ASSERT_TRUE(result.pending.has_value());
  const auto reschedule = manager.reschedule(*result.pending);
  EXPECT_TRUE(reschedule.requeued);
  EXPECT_EQ(reschedule.delay, SessionDeferralManager::retry_delay(1));

  clock.advance(reschedule.delay);
  result = manager.take_ready(true);
  ASSERT_EQ(result.status, SessionDeferralManager::TakeStatus::Ready);
  ASSERT_TRUE(result.pending.has_value());
  manager.set_pending(make_request(), {}, 2, true);
  EXPECT_TRUE(manager.reschedule(*result.pending).dropped_for_newer);
}

TEST(DisplayHelperRequestPolicy, SkipsPhysicalOutputWhenDisplayConfigurationDisabled) {
  policy::Input input;
  input.configuration_option = policy::ConfigurationOption::Disabled;
  input.physical_output_override = true;
  EXPECT_FALSE(policy::evaluate(input).dispatch);
}

TEST(DisplayHelperRequestPolicy, AppliesExclusiveVirtualDisplayWhenDisplayConfigurationDisabled) {
  auto input = virtual_display_input();
  input.configuration_option = policy::ConfigurationOption::Disabled;
  const auto result = policy::evaluate(input);
  ASSERT_TRUE(result.dispatch);
  ASSERT_TRUE(result.device_preparation.has_value());
  EXPECT_EQ(*result.device_preparation, policy::DevicePreparation::EnsureOnlyDisplay);
}

TEST(DisplayHelperRequestPolicy, VirtualDisplayWithoutConfirmedTargetDoesNotDispatch) {
  auto input = virtual_display_input();
  input.target_device_id.clear();
  EXPECT_FALSE(policy::evaluate(input).dispatch);
}

TEST(DisplayHelperRequestPolicy, BlocksVirtualDisplayMutationDuringPhysicalRestore) {
  EXPECT_TRUE(policy::virtual_display_mutation_allowed(false));
  EXPECT_FALSE(policy::virtual_display_mutation_allowed(true));
}

TEST(DisplayHelperRequestPolicy, StreamAdmissionDisarmsRestoreBeforeCheckingMutationSafety) {
  bool restore_pending = true;
  std::vector<std::string> operations;

  const bool allowed = policy::supersede_restore_for_virtual_display(
    [&] {
      operations.emplace_back("disarm");
      restore_pending = false;
    },
    [&] {
      operations.emplace_back("query");
      return restore_pending;
    }
  );

  EXPECT_TRUE(allowed);
  EXPECT_EQ(operations, (std::vector<std::string> {"disarm", "query"}));
}

TEST(DisplayHelperRequestPolicy, StreamAdmissionStaysBlockedWhenDisarmCannotClearRestore) {
  bool restore_pending = true;
  std::vector<std::string> operations;

  const bool allowed = policy::supersede_restore_for_virtual_display(
    [&] {
      operations.emplace_back("disarm");
      // Simulate a failed or undelivered DISARM: the authoritative query must
      // still observe the helper restore and prevent driver mutation.
    },
    [&] {
      operations.emplace_back("query");
      return restore_pending;
    }
  );

  EXPECT_FALSE(allowed);
  EXPECT_EQ(operations, (std::vector<std::string> {"disarm", "query"}));
}

TEST(DisplayHelperRequestPolicy, PhysicalFallbackAfterVirtualFailureStillDispatches) {
  policy::Input input;
  input.configuration_option = policy::ConfigurationOption::EnsureActive;
  input.virtual_display_failed = true;
  input.target_device_id = "{physical-monitor-guid}";
  EXPECT_TRUE(policy::evaluate(input).dispatch);
}

TEST(DisplayHelperRequestPolicy, PhysicalFallbackStillAllowsSelectedHdrProfile) {
  policy::Input physical_input;
  physical_input.hdr_profile_selected = true;
  EXPECT_TRUE(policy::evaluate(physical_input).apply_hdr_profile_to_physical);

  auto fallback_input = physical_input;
  fallback_input.virtual_display_failed = true;
  EXPECT_TRUE(policy::evaluate(fallback_input).apply_hdr_profile_to_physical);

  auto virtual_input = physical_input;
  virtual_input.virtual_display = true;
  EXPECT_FALSE(policy::evaluate(virtual_input).apply_hdr_profile_to_physical);
}

TEST(DisplayHelperRequestPolicy, UsesRtxHdrSourcePolicyForInitialVirtualDisplayConfiguration) {
  auto input = virtual_display_input();
  input.rtx_hdr_source_enabled = true;
  input.hdr_requested = true;
  const auto result = policy::evaluate(input);
  ASSERT_TRUE(result.hdr_enabled.has_value());
  EXPECT_FALSE(*result.hdr_enabled);
}

TEST(DisplayHelperRequestPolicy, SkipsExtendedVirtualDisplayWhenDisplayConfigurationDisabled) {
  auto input = virtual_display_input();
  input.configuration_option = policy::ConfigurationOption::Disabled;
  input.layout = policy::VirtualDisplayLayout::Extended;
  EXPECT_FALSE(policy::evaluate(input).dispatch);
}

TEST(DisplayHelperRequestPolicy, PhysicalOutputDoesNotPinSingleDisplayTopology) {
  policy::Input input;
  input.configuration_option = policy::ConfigurationOption::EnsureActive;
  input.physical_output_override = true;
  input.target_device_id = "{physical-monitor-guid}";
  EXPECT_TRUE(policy::evaluate(input).topology.empty());
}

TEST(DisplayHelperRequestPolicy, PhysicalOutputEnsureOnlyDisplayPinsTopology) {
  policy::Input input;
  input.configuration_option = policy::ConfigurationOption::EnsureOnlyDisplay;
  input.physical_output_override = true;
  input.target_device_id = "{physical-monitor-guid}";
  EXPECT_EQ(policy::evaluate(input).topology, (std::vector<std::vector<std::string>> {{"{physical-monitor-guid}"}}));
}

TEST(DisplayHelperRequestPolicy, ExclusiveVirtualDisplayWithoutSnapshotPinsSingleDisplayTopology) {
  const auto result = policy::evaluate(virtual_display_input());
  EXPECT_EQ(result.topology, (std::vector<std::vector<std::string>> {{"{virtual-device}"}}));
}

TEST(DisplayHelperRequestPolicy, ExtendedVirtualDisplayWithoutSnapshotDoesNotPinSingleDisplayTopology) {
  for (const auto layout : {
         policy::VirtualDisplayLayout::Extended,
         policy::VirtualDisplayLayout::ExtendedPrimary,
         policy::VirtualDisplayLayout::ExtendedIsolated,
         policy::VirtualDisplayLayout::ExtendedPrimaryIsolated,
       }) {
    auto input = virtual_display_input();
    input.layout = layout;
    EXPECT_TRUE(policy::evaluate(input).topology.empty());
  }
}

TEST(DisplayHelperRequestPolicy, ExtendedVirtualDisplayMergesSnapshotWithVirtualDisplay) {
  auto input = virtual_display_input();
  input.layout = policy::VirtualDisplayLayout::Extended;
  input.topology_snapshot = {{"{physical-monitor-guid}"}};
  EXPECT_EQ(
    policy::evaluate(input).topology,
    (std::vector<std::vector<std::string>> {{"{physical-monitor-guid}"}, {"{virtual-device}"}})
  );
}

TEST(DisplayHelperRequestPolicy, UsesRemappedVirtualDisplayResolutionForSessionOverrides) {
  auto input = virtual_display_input();
  input.configuration_option = policy::ConfigurationOption::EnsureOnlyDisplay;
  input.remapped_resolution = policy::Resolution {3840, 2160};
  const auto result = policy::evaluate(input);
  ASSERT_TRUE(result.initial_resolution.has_value());
  ASSERT_TRUE(result.applied_resolution.has_value());
  EXPECT_EQ(result.initial_resolution->width, 3840);
  EXPECT_EQ(result.initial_resolution->height, 2160);
  EXPECT_EQ(result.applied_resolution->width, 3840);
  EXPECT_EQ(result.applied_resolution->height, 2160);
}
