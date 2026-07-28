/**
 * @file tests/unit/test_display_helper_v2_core.cpp
 * @brief Unit tests for display helper v2 core components.
 */
#ifdef _WIN32

#include "../tests_common.h"

#include "src/platform/windows/display_helper_v2/operations.h"
#include "src/platform/windows/display_helper_v2/runtime_support.h"
#include "src/platform/windows/display_helper_v2/snapshot.h"
#include "src/platform/windows/display_helper_v2/staged_settings.h"

#include <algorithm>
#include <filesystem>
#include <future>
#include <set>

namespace {
  class FakeClock final : public display_helper::v2::IClock {
  public:
    std::chrono::steady_clock::time_point now() override {
      return now_;
    }

    void sleep_for(std::chrono::milliseconds duration) override {
      slept_for += duration;
      now_ += duration;
    }

    void advance(std::chrono::milliseconds duration) {
      now_ += duration;
    }

    std::chrono::milliseconds slept_for {0};

  private:
    std::chrono::steady_clock::time_point now_ {std::chrono::steady_clock::now()};
  };

  class FakeDisplaySettings final : public display_helper::v2::IDisplaySettings {
  public:
    display_helper::v2::ApplyStatus apply(const display_device::SingleDisplayConfiguration &config) override {
      events.emplace_back("settings");
      ++apply_calls;
      applied_configuration = config;
      return apply_status;
    }

    display_helper::v2::ApplyStatus apply_topology(const display_device::ActiveTopology &requested) override {
      events.emplace_back("topology");
      ++apply_topology_calls;
      if (apply_topology_status == display_helper::v2::ApplyStatus::Ok) {
        topology = applied_topology_override.value_or(requested);
      }
      return apply_topology_status;
    }

    display_device::EnumeratedDeviceList enumerate(display_device::DeviceEnumerationDetail) override {
      events.emplace_back("enumerate");
      ++enumerate_calls;
      auto result = enumerated_devices;
      if (enumerate_calls <= inactive_enumeration_calls) {
        for (auto &device : result) {
          device.m_info.reset();
        }
      }
      return result;
    }

    display_device::ActiveTopology capture_topology() override {
      return topology;
    }

    bool validate_topology(const display_device::ActiveTopology &) override {
      return validate_topology_result;
    }

    bool validate_topology_for_apply(const display_device::ActiveTopology &) override {
      events.emplace_back("validate");
      ++strict_validation_calls;
      return strict_validation_result;
    }

    display_device::DisplaySettingsSnapshot capture_snapshot() override {
      return snapshot;
    }

    bool apply_snapshot(const display_device::DisplaySettingsSnapshot &) override {
      ++apply_snapshot_calls;
      return apply_snapshot_result;
    }

    bool snapshot_matches_current(const display_device::DisplaySettingsSnapshot &) override {
      return snapshot_matches_result;
    }

    bool configuration_matches(const display_device::SingleDisplayConfiguration &) override {
      return configuration_matches_result;
    }

    bool configuration_matches(
      const display_device::SingleDisplayConfiguration &config,
      const display_helper::v2::ResolvedConfigurationTarget &target) override {
      verification_configuration = config;
      verification_target = target;
      return configuration_matches_result;
    }

    bool is_primary_device(const std::string &device_id) override {
      return primary_devices.count(device_id) != 0;
    }

    bool set_display_origin(const std::string &, const display_device::Point &) override {
      return set_display_origin_result;
    }

    std::optional<display_device::ActiveTopology> compute_expected_topology(
      const display_device::SingleDisplayConfiguration &,
      const std::optional<display_device::ActiveTopology> &) override {
      return expected_topology;
    }

    std::optional<display_helper::v2::ApplyTopologyPlan> compute_apply_topology_plan(
      const display_device::SingleDisplayConfiguration &config,
      const std::optional<display_device::ActiveTopology> &base_topology) override {
      if (apply_topology_plan) {
        return apply_topology_plan;
      }
      return display_helper::v2::IDisplaySettings::compute_apply_topology_plan(config, base_topology);
    }

    bool is_topology_same(const display_device::ActiveTopology &lhs, const display_device::ActiveTopology &rhs) override {
      return lhs == rhs;
    }

    bool recover_display_stack() override {
      events.emplace_back("recover");
      ++recovery_calls;
      return recovery_result;
    }

    bool prepare_staged_apply(const display_device::ActiveTopology &current_topology) override {
      events.emplace_back("prepare");
      ++prepare_staged_apply_calls;
      prepared_topology = current_topology;
      return prepare_staged_apply_result;
    }

    display_helper::v2::ApplyStatus apply_status = display_helper::v2::ApplyStatus::Ok;
    display_helper::v2::ApplyStatus apply_topology_status = display_helper::v2::ApplyStatus::Ok;
    std::optional<display_device::ActiveTopology> applied_topology_override;
    std::optional<display_device::SingleDisplayConfiguration> applied_configuration;
    display_device::EnumeratedDeviceList enumerated_devices;
    display_device::ActiveTopology topology;
    bool validate_topology_result = true;
    bool strict_validation_result = true;
    display_device::DisplaySettingsSnapshot snapshot;
    bool apply_snapshot_result = true;
    bool snapshot_matches_result = true;
    bool configuration_matches_result = true;
    std::optional<display_device::SingleDisplayConfiguration> verification_configuration;
    std::optional<display_helper::v2::ResolvedConfigurationTarget> verification_target;
    std::set<std::string> primary_devices;
    bool set_display_origin_result = true;
    std::optional<display_device::ActiveTopology> expected_topology;
    std::optional<display_helper::v2::ApplyTopologyPlan> apply_topology_plan;
    bool recovery_result = true;
    bool prepare_staged_apply_result = true;
    std::optional<display_device::ActiveTopology> prepared_topology;
    std::vector<std::string> events;
    int inactive_enumeration_calls = 0;
    int apply_calls = 0;
    int apply_snapshot_calls = 0;
    int apply_topology_calls = 0;
    int enumerate_calls = 0;
    int strict_validation_calls = 0;
    int recovery_calls = 0;
    int prepare_staged_apply_calls = 0;
  };

  display_device::EnumeratedDevice make_active_device(const std::string &id) {
    display_device::EnumeratedDevice device;
    device.m_device_id = id;
    device.m_display_name = "\\\\.\\DISPLAY_" + id;
    device.m_info = display_device::EnumeratedDevice::Info {};
    return device;
  }

  display_device::DisplaySettingsSnapshot make_snapshot(const std::vector<std::string> &ids) {
    display_device::DisplaySettingsSnapshot snapshot;
    if (!ids.empty()) {
      snapshot.m_topology.push_back(ids);
    }
    for (const auto &id : ids) {
      snapshot.m_modes[id] = display_device::DisplayMode {};
      snapshot.m_hdr_states[id] = std::nullopt;
    }
    return snapshot;
  }

  struct TempDir {
    std::filesystem::path path;

    TempDir() {
      std::error_code ec;
      const auto base = std::filesystem::temp_directory_path(ec);
      const auto token = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
      path = (ec ? std::filesystem::path(".") : base) / ("sunshine_display_helper_v2_test_" + token);
      std::filesystem::create_directories(path, ec);
    }

    ~TempDir() {
      std::error_code ec;
      std::filesystem::remove_all(path, ec);
    }
  };
}  // namespace

TEST(DisplayHelperV2Queue, PushPopOrder) {
  display_helper::v2::MessageQueue<int> queue;
  queue.push(1);
  queue.push(2);
  queue.push(3);

  auto first = queue.try_pop();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(*first, 1);

  auto second = queue.try_pop();
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(*second, 2);

  auto third = queue.try_pop();
  ASSERT_TRUE(third.has_value());
  EXPECT_EQ(*third, 3);
}

TEST(DisplayHelperV2Queue, WaitPopBlocksUntilValue) {
  display_helper::v2::MessageQueue<int> queue;
  auto future = std::async(std::launch::async, [&queue]() {
    return queue.wait_pop();
  });

  EXPECT_EQ(future.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout);
  queue.push(42);
  EXPECT_EQ(future.wait_for(std::chrono::milliseconds(200)), std::future_status::ready);
  EXPECT_EQ(future.get(), 42);
}

TEST(DisplayHelperV2Queue, WaitForTimesOut) {
  display_helper::v2::MessageQueue<int> queue;
  auto value = queue.wait_for(std::chrono::milliseconds(10));
  EXPECT_FALSE(value.has_value());
}

TEST(DisplayHelperV2Cancellation, CancelInvalidatesToken) {
  display_helper::v2::CancellationSource source;
  auto token = source.token();
  EXPECT_FALSE(token.is_cancelled());

  source.cancel();
  EXPECT_TRUE(token.is_cancelled());

  auto token2 = source.token();
  EXPECT_FALSE(token2.is_cancelled());
}

TEST(DisplayHelperV2DisconnectGrace, TriggersAfterGrace) {
  FakeClock clock;
  display_helper::v2::DisconnectGrace grace(clock, std::chrono::seconds(30));

  grace.on_disconnect();
  EXPECT_FALSE(grace.should_trigger());

  clock.advance(std::chrono::seconds(29));
  EXPECT_FALSE(grace.should_trigger());

  clock.advance(std::chrono::seconds(1));
  EXPECT_TRUE(grace.should_trigger());
  EXPECT_FALSE(grace.should_trigger());
}

TEST(DisplayHelperV2DisconnectGrace, ReconnectCancelsPendingTrigger) {
  FakeClock clock;
  display_helper::v2::DisconnectGrace grace(clock, std::chrono::seconds(30));

  grace.on_disconnect();
  clock.advance(std::chrono::seconds(10));
  grace.on_reconnect();

  clock.advance(std::chrono::seconds(40));
  EXPECT_FALSE(grace.should_trigger());
}

TEST(DisplayHelperV2DisconnectGrace, SubsequentDisconnectResetsTimer) {
  FakeClock clock;
  display_helper::v2::DisconnectGrace grace(clock, std::chrono::seconds(30));

  grace.on_disconnect();
  clock.advance(std::chrono::seconds(20));
  grace.on_reconnect();

  grace.on_disconnect();
  clock.advance(std::chrono::seconds(29));
  EXPECT_FALSE(grace.should_trigger());

  clock.advance(std::chrono::seconds(1));
  EXPECT_TRUE(grace.should_trigger());
}

TEST(DisplayHelperV2ReconnectController, TriggersRevertAfterGrace) {
  FakeClock clock;
  display_helper::v2::ReconnectController controller(clock, std::chrono::seconds(30));

  controller.update_connection(true);
  controller.update_connection(false);

  clock.advance(std::chrono::seconds(29));
  EXPECT_FALSE(controller.update_connection(false));

  clock.advance(std::chrono::seconds(1));
  EXPECT_TRUE(controller.update_connection(false));
}

TEST(DisplayHelperV2ReconnectController, NoRevertBeforeGraceWindow) {
  FakeClock clock;
  display_helper::v2::ReconnectController controller(clock, std::chrono::seconds(30));

  controller.update_connection(true);
  controller.update_connection(false);

  clock.advance(std::chrono::seconds(15));
  EXPECT_FALSE(controller.update_connection(false));
  EXPECT_FALSE(controller.should_restart_pipe());
}

TEST(DisplayHelperV2ReconnectController, ReconnectWithinGraceDefersRevert) {
  FakeClock clock;
  display_helper::v2::ReconnectController controller(clock, std::chrono::seconds(30));

  controller.update_connection(true);
  controller.update_connection(false);

  clock.advance(std::chrono::seconds(10));
  controller.update_connection(true);

  clock.advance(std::chrono::seconds(40));
  EXPECT_FALSE(controller.update_connection(false));

  clock.advance(std::chrono::seconds(30));
  EXPECT_TRUE(controller.update_connection(false));
}

TEST(DisplayHelperV2ReconnectController, ReconnectDoesNotRestartHelper) {
  FakeClock clock;
  display_helper::v2::ReconnectController controller(clock, std::chrono::seconds(30));

  controller.update_connection(true);
  controller.update_connection(false);

  clock.advance(std::chrono::seconds(5));
  controller.update_connection(true);

  EXPECT_FALSE(controller.should_restart_pipe());
}

TEST(DisplayHelperV2ReconnectController, BrokenPipeRequestsRestart) {
  FakeClock clock;
  display_helper::v2::ReconnectController controller(clock, std::chrono::seconds(30));

  controller.on_broken();
  EXPECT_TRUE(controller.should_restart_pipe());
  EXPECT_FALSE(controller.update_connection(false));
}

TEST(DisplayHelperV2ApplyPolicy, RespectsVirtualDisplayCooldown) {
  FakeClock clock;
  display_helper::v2::ApplyPolicy policy(clock);

  EXPECT_EQ(
    policy.maybe_reset_virtual_display(display_helper::v2::ApplyStatus::NeedsVirtualDisplayReset, true),
    display_helper::v2::PolicyDecision::ResetVirtualDisplay);

  EXPECT_EQ(
    policy.maybe_reset_virtual_display(display_helper::v2::ApplyStatus::NeedsVirtualDisplayReset, true),
    display_helper::v2::PolicyDecision::Proceed);

  clock.advance(std::chrono::seconds(31));
  EXPECT_EQ(
    policy.maybe_reset_virtual_display(display_helper::v2::ApplyStatus::NeedsVirtualDisplayReset, true),
    display_helper::v2::PolicyDecision::ResetVirtualDisplay);
}

TEST(DisplayHelperV2ApplyPolicy, RetryDelayUsesBoundedLinearBackoff) {
  FakeClock clock;
  display_helper::v2::ApplyPolicy policy(clock);
  EXPECT_EQ(policy.retry_delay(0), std::chrono::milliseconds(500));
  EXPECT_EQ(policy.retry_delay(1), std::chrono::milliseconds(500));
  EXPECT_EQ(policy.retry_delay(2), std::chrono::milliseconds(1000));
  EXPECT_EQ(policy.retry_delay(3), std::chrono::milliseconds(1500));
  EXPECT_EQ(policy.retry_delay(99), std::chrono::milliseconds(1500));
}

TEST(DisplayHelperV2ApplyPolicy, SkipTierOnFatal) {
  FakeClock clock;
  display_helper::v2::ApplyPolicy policy(clock);
  EXPECT_TRUE(policy.should_skip_tier(display_helper::v2::ApplyStatus::InvalidRequest));
  EXPECT_TRUE(policy.should_skip_tier(display_helper::v2::ApplyStatus::Fatal));
  EXPECT_FALSE(policy.should_skip_tier(display_helper::v2::ApplyStatus::Retryable));
}

TEST(DisplayHelperV2StagedSettingsState, RebasePreservesOriginalSettings) {
  display_device::SingleDisplayConfigState previous;
  previous.m_initial.m_topology = {{"OLD"}};
  previous.m_modified.m_topology = {{"OLD"}};
  previous.m_modified.m_original_primary_device = "PHYSICAL";
  previous.m_modified.m_original_modes["PHYSICAL"] = display_device::DisplayMode {};
  previous.m_modified.m_original_hdr_states["PHYSICAL"] = display_device::HdrState::Disabled;

  display_device::SingleDisplayConfigState::Initial current_initial;
  current_initial.m_topology = {{"PHYSICAL"}, {"VIRTUAL"}};
  current_initial.m_primary_devices = {"PHYSICAL"};

  const auto rebased = display_helper::v2::StagedSettingsState::rebase(
    previous,
    current_initial,
    current_initial.m_topology);

  EXPECT_EQ(rebased.m_initial, current_initial);
  EXPECT_EQ(rebased.m_modified.m_topology, current_initial.m_topology);
  EXPECT_EQ(rebased.m_modified.m_original_primary_device, "PHYSICAL");
  EXPECT_EQ(rebased.m_modified.m_original_modes, previous.m_modified.m_original_modes);
  EXPECT_EQ(rebased.m_modified.m_original_hdr_states, previous.m_modified.m_original_hdr_states);
}

TEST(DisplayHelperV2StagedSettingsState, ConsecutiveApplyUsesSessionTopologyBaseline) {
  display_device::SingleDisplayConfigState::Initial session_initial;
  session_initial.m_topology = {{"PHYSICAL"}};
  session_initial.m_primary_devices = {"PHYSICAL"};

  display_device::EnumeratedDeviceList devices {
    make_active_device("PHYSICAL"),
    make_active_device("OLD_VIRTUAL"),
    make_active_device("NEW_VIRTUAL"),
  };
  const auto base = display_helper::v2::StagedSettingsState::topology_base(
    session_initial,
    display_device::ActiveTopology {{"OLD_VIRTUAL"}},
    devices);
  ASSERT_TRUE(base.has_value());

  const auto [topology, target, duplicates] = display_device::win_utils::computeNewTopologyAndMetadata(
    display_device::SingleDisplayConfiguration::DevicePreparation::EnsureActive,
    "NEW_VIRTUAL",
    *base);

  EXPECT_EQ(topology, display_device::ActiveTopology({{"PHYSICAL"}, {"NEW_VIRTUAL"}}));
  EXPECT_EQ(target, "NEW_VIRTUAL");
  EXPECT_TRUE(duplicates.empty());
}

TEST(DisplayHelperV2StagedSettingsState, PreservesPrimaryIntentWithoutReopeningTopology) {
  using Prep = display_device::SingleDisplayConfiguration::DevicePreparation;

  display_device::SingleDisplayConfiguration primary;
  primary.m_device_prep = Prep::EnsurePrimary;
  EXPECT_EQ(
    display_helper::v2::StagedSettingsState::settings_configuration(primary).m_device_prep,
    Prep::EnsurePrimary);

  display_device::SingleDisplayConfiguration active;
  active.m_device_prep = Prep::EnsureActive;
  EXPECT_EQ(
    display_helper::v2::StagedSettingsState::settings_configuration(active).m_device_prep,
    Prep::VerifyOnly);

  display_device::SingleDisplayConfiguration only;
  only.m_device_prep = Prep::EnsureOnlyDisplay;
  EXPECT_EQ(
    display_helper::v2::StagedSettingsState::settings_configuration(only).m_device_prep,
    Prep::VerifyOnly);
}

TEST(DisplayHelperV2StagedSettingsState, RebasesTopologyButRetainsOriginalPrimaryGroup) {
  display_device::SingleDisplayConfigState::Initial session_initial {
    .m_topology = {{"PRIMARY_A", "PRIMARY_B"}, {"AUXILIARY"}},
    .m_primary_devices = {"PRIMARY_A", "PRIMARY_B"},
  };
  const display_device::ActiveTopology accepted_topology {{"VIRTUAL"}};
  display_device::EnumeratedDeviceList devices {
    make_active_device("PRIMARY_A"),
    make_active_device("PRIMARY_B"),
    make_active_device("AUXILIARY"),
    make_active_device("VIRTUAL"),
  };
  devices.back().m_info->m_primary = true;

  const auto rebased = display_helper::v2::StagedSettingsState::rebased_initial(
    std::optional<display_device::SingleDisplayConfigState::Initial> {session_initial},
    accepted_topology,
    devices);

  ASSERT_TRUE(rebased.has_value());
  EXPECT_EQ(rebased->m_topology, accepted_topology);
  EXPECT_EQ(rebased->m_primary_devices, session_initial.m_primary_devices);
}

TEST(DisplayHelperV2ApplyOperation, UsesExplicitTopologyAsStagingBase) {
  FakeClock clock;
  FakeDisplaySettings display;
  display.topology = display_device::ActiveTopology {{"OLD"}};
  display.enumerated_devices = {make_active_device("A"), make_active_device("B")};
  display.expected_topology = display_device::ActiveTopology {{"A"}};

  display_helper::v2::ApplyOperation operation(display, clock);
  display_helper::v2::ApplyRequest request;
  display_device::SingleDisplayConfiguration config;
  config.m_device_id = "A";
  config.m_device_prep = display_device::SingleDisplayConfiguration::DevicePreparation::EnsureOnlyDisplay;
  request.configuration = config;
  request.topology = display_device::ActiveTopology {{"A"}, {"B"}};

  display_helper::v2::CancellationSource source;
  auto outcome = operation.run(request, source.token());

  EXPECT_EQ(outcome.status, display_helper::v2::ApplyStatus::Ok);
  EXPECT_FALSE(outcome.expected_topology.has_value());
  EXPECT_EQ(display.prepare_staged_apply_calls, 1);
  ASSERT_TRUE(display.prepared_topology.has_value());
  EXPECT_EQ(*display.prepared_topology, display_device::ActiveTopology({{"OLD"}}));
  EXPECT_EQ(display.apply_topology_calls, 1);
  EXPECT_EQ(display.apply_calls, 1);
}

TEST(DisplayHelperV2ApplyOperation, AcceptsUsableOsAdjustedTopology) {
  FakeClock clock;
  FakeDisplaySettings display;
  display.topology = display_device::ActiveTopology {{"OLD"}};
  display.applied_topology_override = display_device::ActiveTopology {{"TARGET"}};
  display.enumerated_devices = {make_active_device("TARGET")};

  display_helper::v2::ApplyOperation operation(display, clock);
  display_helper::v2::ApplyRequest request;
  display_device::SingleDisplayConfiguration config;
  config.m_device_id = "TARGET";
  config.m_device_prep = display_device::SingleDisplayConfiguration::DevicePreparation::EnsureActive;
  request.configuration = config;
  request.topology = display_device::ActiveTopology {{"TARGET"}, {"SLEEPING"}};

  display_helper::v2::CancellationSource source;
  const auto outcome = operation.run(request, source.token());

  EXPECT_EQ(outcome.status, display_helper::v2::ApplyStatus::Ok);
  EXPECT_FALSE(outcome.expected_topology.has_value());
  EXPECT_EQ(display.apply_topology_calls, 1);
  EXPECT_EQ(display.apply_calls, 1);
  EXPECT_EQ(clock.slept_for, std::chrono::milliseconds::zero());
}

TEST(DisplayHelperV2ApplyOperation, WaitsOnlyForTheResolvedTargetGroup) {
  FakeClock clock;
  FakeDisplaySettings display;
  display.topology = display_device::ActiveTopology {{"OLD"}};
  display.applied_topology_override = display_device::ActiveTopology {{"TARGET"}, {"SLEEPING"}};
  auto sleeping = make_active_device("SLEEPING");
  sleeping.m_info.reset();
  display.enumerated_devices = {make_active_device("TARGET"), std::move(sleeping)};
  display.apply_topology_plan = display_helper::v2::ApplyTopologyPlan {
    .topology = display_device::ActiveTopology {{"TARGET"}, {"SLEEPING"}},
    .activation_target = display_helper::v2::TopologyActivationTarget {
      .kind = display_helper::v2::DeviceTargetKind::ExplicitDevice,
      .acceptable_device_ids = {"TARGET"},
    },
  };

  display_helper::v2::ApplyOperation operation(display, clock);
  display_helper::v2::ApplyRequest request;
  display_device::SingleDisplayConfiguration config;
  config.m_device_id = "TARGET";
  config.m_device_prep = display_device::SingleDisplayConfiguration::DevicePreparation::EnsureActive;
  request.configuration = config;

  display_helper::v2::CancellationSource source;
  const auto outcome = operation.run(request, source.token());

  EXPECT_EQ(outcome.status, display_helper::v2::ApplyStatus::Ok);
  EXPECT_EQ(display.apply_topology_calls, 1);
  EXPECT_EQ(display.apply_calls, 1);
  EXPECT_EQ(clock.slept_for, std::chrono::milliseconds::zero());
}

TEST(DisplayHelperV2ApplyOperation, RejectsOsAdjustedTopologyWithoutTarget) {
  FakeClock clock;
  FakeDisplaySettings display;
  display.topology = display_device::ActiveTopology {{"OLD"}};
  display.applied_topology_override = display_device::ActiveTopology {{"OTHER"}};
  display.enumerated_devices = {make_active_device("OTHER")};

  display_helper::v2::ApplyOperation operation(display, clock);
  display_helper::v2::ApplyRequest request;
  display_device::SingleDisplayConfiguration config;
  config.m_device_id = "TARGET";
  config.m_device_prep = display_device::SingleDisplayConfiguration::DevicePreparation::EnsureActive;
  request.configuration = config;
  request.topology = display_device::ActiveTopology {{"TARGET"}};

  display_helper::v2::CancellationSource source;
  const auto outcome = operation.run(request, source.token());

  EXPECT_EQ(outcome.status, display_helper::v2::ApplyStatus::Retryable);
  EXPECT_EQ(display.apply_topology_calls, 2);
  EXPECT_EQ(display.apply_calls, 0);
  EXPECT_EQ(clock.slept_for, std::chrono::milliseconds(10500));
}

TEST(DisplayHelperV2ApplyOperation, ResolvesImplicitPrimaryBeforeAcceptingAdjustedTopology) {
  FakeClock clock;
  FakeDisplaySettings display;
  display.topology = display_device::ActiveTopology {{"OLD"}};
  // Windows can reorder or reduce a duplicate primary group. A usable member
  // must remain acceptable without pinning verification to the pre-transition
  // representative.
  display.applied_topology_override = display_device::ActiveTopology {{"PRIMARY_B"}};
  display.enumerated_devices = {make_active_device("PRIMARY_B")};
  display.apply_topology_plan = display_helper::v2::ApplyTopologyPlan {
    .topology = display_device::ActiveTopology {{"TARGET"}},
    .activation_target = display_helper::v2::TopologyActivationTarget {
      .kind = display_helper::v2::DeviceTargetKind::DefaultPrimaryGroup,
      .acceptable_device_ids = {"PRIMARY_A", "PRIMARY_B"},
    },
  };

  display_helper::v2::ApplyOperation operation(display, clock);
  display_helper::v2::ApplyRequest request;
  display_device::SingleDisplayConfiguration config;
  config.m_device_prep = display_device::SingleDisplayConfiguration::DevicePreparation::EnsureActive;
  request.configuration = config;  // v1 resolves the original primary for this form.
  request.topology = display_device::ActiveTopology {{"TARGET"}};

  display_helper::v2::CancellationSource source;
  const auto outcome = operation.run(request, source.token());

  EXPECT_EQ(outcome.status, display_helper::v2::ApplyStatus::Ok);
  EXPECT_EQ(display.apply_topology_calls, 1);
  EXPECT_EQ(display.apply_calls, 1);
  ASSERT_TRUE(outcome.resolved_target.has_value());
  EXPECT_EQ(outcome.resolved_target->representative_device_id, "PRIMARY_B");
  EXPECT_EQ(
    outcome.resolved_target->duplicate_device_ids,
    (std::set<std::string> {"PRIMARY_B"}));
}

TEST(DisplayHelperV2ApplyOperation, ResolvesExplicitTargetGroupFromAcceptedTopology) {
  FakeClock clock;
  FakeDisplaySettings display;
  display.topology = display_device::ActiveTopology {{"OLD"}};
  display.applied_topology_override = display_device::ActiveTopology {{"EXPLICIT_A", "DUPLICATE_B"}};
  display.enumerated_devices = {make_active_device("EXPLICIT_A"), make_active_device("DUPLICATE_B")};
  display.apply_topology_plan = display_helper::v2::ApplyTopologyPlan {
    .topology = display_device::ActiveTopology {{"EXPLICIT_A"}},
    .activation_target = display_helper::v2::TopologyActivationTarget {
      .kind = display_helper::v2::DeviceTargetKind::ExplicitDevice,
      .acceptable_device_ids = {"EXPLICIT_A"},
    },
  };

  display_helper::v2::ApplyOperation operation(display, clock);
  display_helper::v2::ApplyRequest request;
  display_device::SingleDisplayConfiguration config;
  config.m_device_id = "EXPLICIT_A";
  config.m_device_prep = display_device::SingleDisplayConfiguration::DevicePreparation::EnsureActive;
  request.configuration = config;
  request.topology = display_device::ActiveTopology {{"EXPLICIT_A"}};

  display_helper::v2::CancellationSource source;
  const auto outcome = operation.run(request, source.token());

  EXPECT_EQ(outcome.status, display_helper::v2::ApplyStatus::Ok);
  ASSERT_TRUE(outcome.resolved_target.has_value());
  EXPECT_EQ(outcome.resolved_target->representative_device_id, "EXPLICIT_A");
  EXPECT_EQ(
    outcome.resolved_target->duplicate_device_ids,
    (std::set<std::string> {"EXPLICIT_A", "DUPLICATE_B"}));
}

TEST(DisplayHelperV2VerificationOperation, PreservesDefaultRequestAndVerifiesAcceptedDuplicateGroup) {
  FakeClock clock;
  FakeDisplaySettings display;
  display.topology = display_device::ActiveTopology {{"PRIMARY_B"}};
  display.primary_devices.insert("PRIMARY_B");
  display_helper::v2::VerificationOperation operation(display, clock);
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  request.configuration->m_device_prep =
    display_device::SingleDisplayConfiguration::DevicePreparation::EnsurePrimary;
  EXPECT_TRUE(request.configuration->m_device_id.empty());

  display_helper::v2::ResolvedConfigurationTarget target {
    .kind = display_helper::v2::DeviceTargetKind::DefaultPrimaryGroup,
    .representative_device_id = "PRIMARY_A",
    .duplicate_device_ids = {"PRIMARY_A", "PRIMARY_B"},
  };
  display_helper::v2::CancellationSource source;

  EXPECT_TRUE(operation.run(request, std::nullopt, target, source.token()));
  ASSERT_TRUE(display.verification_configuration.has_value());
  EXPECT_TRUE(display.verification_configuration->m_device_id.empty());
  ASSERT_TRUE(display.verification_target.has_value());
  EXPECT_EQ(display.verification_target->duplicate_device_ids, target.duplicate_device_ids);
}

TEST(DisplayHelperV2VerificationOperation, RejectsWhenResolvedTargetDisappears) {
  FakeClock clock;
  FakeDisplaySettings display;
  display.topology = display_device::ActiveTopology {{"UNRELATED"}};
  display_helper::v2::VerificationOperation operation(display, clock);
  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  request.configuration->m_device_id = "TARGET";
  request.configuration->m_device_prep =
    display_device::SingleDisplayConfiguration::DevicePreparation::EnsureActive;
  const display_helper::v2::ResolvedConfigurationTarget target {
    .kind = display_helper::v2::DeviceTargetKind::ExplicitDevice,
    .representative_device_id = "TARGET",
    .duplicate_device_ids = {"TARGET"},
  };
  display_helper::v2::CancellationSource source;

  EXPECT_FALSE(operation.run(request, std::nullopt, target, source.token()));
  EXPECT_EQ(clock.slept_for, std::chrono::milliseconds(250));
}

TEST(DisplayHelperV2ApplyOperation, WaitsForTopologyEnumerationBeforeApplyingSettings) {
  FakeClock clock;
  FakeDisplaySettings display;
  display.topology = display_device::ActiveTopology {{"PHYSICAL"}};
  display.enumerated_devices = {make_active_device("VIRTUAL")};
  display.inactive_enumeration_calls = 2;

  display_helper::v2::ApplyOperation operation(display, clock);
  display_helper::v2::ApplyRequest request;
  display_device::SingleDisplayConfiguration config;
  config.m_device_id = "VIRTUAL";
  config.m_device_prep = display_device::SingleDisplayConfiguration::DevicePreparation::EnsureOnlyDisplay;
  request.configuration = config;
  request.topology = display_device::ActiveTopology {{"VIRTUAL"}};

  display_helper::v2::CancellationSource source;
  const auto outcome = operation.run(request, source.token());

  EXPECT_EQ(outcome.status, display_helper::v2::ApplyStatus::Ok);
  EXPECT_EQ(display.apply_topology_calls, 1);
  EXPECT_EQ(display.enumerate_calls, 3);
  EXPECT_EQ(display.apply_calls, 1);
  EXPECT_EQ(clock.slept_for, std::chrono::milliseconds(200));

  const auto topology = std::ranges::find(display.events, "topology");
  const auto settings = std::ranges::find(display.events, "settings");
  ASSERT_NE(topology, display.events.end());
  ASSERT_NE(settings, display.events.end());
  EXPECT_LT(topology, settings);
}

TEST(DisplayHelperV2ApplyOperation, CancellationAfterRecoveryBoundarySkipsDisplayMutation) {
  FakeClock clock;
  FakeDisplaySettings display;
  display.topology = display_device::ActiveTopology {{"OLD"}};
  display.enumerated_devices = {make_active_device("TARGET")};
  display_helper::v2::CancellationSource source;
  display_helper::v2::ApplyOperation operation(
    display,
    clock,
    [&source] {
      source.cancel();
      return true;
    });

  display_helper::v2::ApplyRequest request;
  display_device::SingleDisplayConfiguration config;
  config.m_device_id = "TARGET";
  config.m_device_prep = display_device::SingleDisplayConfiguration::DevicePreparation::EnsureActive;
  request.configuration = config;
  request.topology = display_device::ActiveTopology {{"TARGET"}};

  const auto outcome = operation.run(request, source.token());

  EXPECT_EQ(outcome.status, display_helper::v2::ApplyStatus::Fatal);
  EXPECT_TRUE(outcome.durable_recovery_armed);
  EXPECT_FALSE(outcome.display_may_have_changed);
  EXPECT_EQ(display.apply_topology_calls, 0);
  EXPECT_EQ(display.apply_calls, 0);
}

TEST(DisplayHelperV2ApplyOperation, RejectsTopologyThatFailsStrictOsValidation) {
  FakeClock clock;
  FakeDisplaySettings display;
  display.topology = display_device::ActiveTopology {{"PHYSICAL"}};
  display.enumerated_devices = {make_active_device("VIRTUAL")};
  display.strict_validation_result = false;

  display_helper::v2::ApplyOperation operation(display, clock);
  display_helper::v2::ApplyRequest request;
  display_device::SingleDisplayConfiguration config;
  config.m_device_id = "VIRTUAL";
  config.m_device_prep = display_device::SingleDisplayConfiguration::DevicePreparation::EnsureOnlyDisplay;
  request.configuration = config;
  request.topology = display_device::ActiveTopology {{"VIRTUAL"}};

  display_helper::v2::CancellationSource source;
  const auto outcome = operation.run(request, source.token());

  EXPECT_EQ(outcome.status, display_helper::v2::ApplyStatus::Retryable);
  EXPECT_EQ(display.strict_validation_calls, 2);
  EXPECT_EQ(display.recovery_calls, 1);
  EXPECT_EQ(display.apply_topology_calls, 0);
  EXPECT_EQ(display.apply_calls, 0);
  EXPECT_EQ(clock.slept_for, std::chrono::milliseconds(500));
}

TEST(DisplayHelperV2ApplyOperation, DelegatesPrimaryToSettingsAfterTopologySettles) {
  FakeClock clock;
  FakeDisplaySettings display;
  display.topology = display_device::ActiveTopology {{"PHYSICAL"}};
  display.enumerated_devices = {make_active_device("PHYSICAL"), make_active_device("VIRTUAL")};

  display_helper::v2::ApplyOperation operation(display, clock);
  display_helper::v2::ApplyRequest request;
  display_device::SingleDisplayConfiguration config;
  config.m_device_id = "VIRTUAL";
  config.m_device_prep = display_device::SingleDisplayConfiguration::DevicePreparation::EnsurePrimary;
  request.configuration = config;
  request.topology = display_device::ActiveTopology {{"PHYSICAL"}, {"VIRTUAL"}};

  display_helper::v2::CancellationSource source;
  const auto outcome = operation.run(request, source.token());

  EXPECT_EQ(outcome.status, display_helper::v2::ApplyStatus::Ok);
  ASSERT_TRUE(display.applied_configuration.has_value());
  EXPECT_EQ(
    display.applied_configuration->m_device_prep,
    display_device::SingleDisplayConfiguration::DevicePreparation::EnsurePrimary);
  const auto topology = std::ranges::find(display.events, "topology");
  const auto settings = std::ranges::find(display.events, "settings");
  ASSERT_NE(topology, display.events.end());
  ASSERT_NE(settings, display.events.end());
  EXPECT_LT(topology, settings);
}

TEST(DisplayHelperV2ApplyOperation, ArmsRecoveryAtTopologyMutationBoundary) {
  FakeClock clock;
  FakeDisplaySettings display;
  display.topology = display_device::ActiveTopology {{"PHYSICAL"}};
  display.enumerated_devices = {make_active_device("PHYSICAL"), make_active_device("VIRTUAL")};

  bool boundary_called = false;
  display_helper::v2::ApplyOperation operation(display, clock, [&] {
    boundary_called = true;
    display.events.emplace_back("recovery-boundary");
    return true;
  });
  display_helper::v2::ApplyRequest request;
  display_device::SingleDisplayConfiguration config;
  config.m_device_id = "VIRTUAL";
  config.m_device_prep = display_device::SingleDisplayConfiguration::DevicePreparation::EnsureOnlyDisplay;
  request.configuration = config;
  request.topology = display_device::ActiveTopology {{"VIRTUAL"}};

  display_helper::v2::CancellationSource source;
  const auto outcome = operation.run(request, source.token());

  EXPECT_EQ(outcome.status, display_helper::v2::ApplyStatus::Ok);
  EXPECT_TRUE(boundary_called);
  EXPECT_TRUE(outcome.durable_recovery_armed);
  const auto topology = std::ranges::find(display.events, "topology");
  const auto boundary = std::ranges::find(display.events, "recovery-boundary");
  const auto settings = std::ranges::find(display.events, "settings");
  ASSERT_NE(topology, display.events.end());
  ASSERT_NE(boundary, display.events.end());
  ASSERT_NE(settings, display.events.end());
  EXPECT_LT(boundary, topology);
  EXPECT_LT(boundary, settings);
}

TEST(DisplayHelperV2ApplyOperation, RestoresBaselineWhenSettingsStageFails) {
  FakeClock clock;
  FakeDisplaySettings display;
  const auto baseline = display_device::ActiveTopology {{"PHYSICAL"}};
  display.topology = baseline;
  display.snapshot = make_snapshot({"PHYSICAL"});
  display.enumerated_devices = {make_active_device("PHYSICAL"), make_active_device("VIRTUAL")};
  display.apply_status = display_helper::v2::ApplyStatus::Retryable;

  display_helper::v2::ApplyOperation operation(display, clock);
  display_helper::v2::ApplyRequest request;
  display_device::SingleDisplayConfiguration config;
  config.m_device_id = "VIRTUAL";
  config.m_device_prep = display_device::SingleDisplayConfiguration::DevicePreparation::EnsureOnlyDisplay;
  request.configuration = config;
  request.topology = display_device::ActiveTopology {{"VIRTUAL"}};

  display_helper::v2::CancellationSource source;
  const auto outcome = operation.run(request, source.token());

  EXPECT_EQ(outcome.status, display_helper::v2::ApplyStatus::Retryable);
  EXPECT_FALSE(outcome.display_may_have_changed);
  EXPECT_EQ(display.topology, baseline);
  EXPECT_EQ(display.apply_topology_calls, 2);
  EXPECT_EQ(display.apply_snapshot_calls, 1);
}

TEST(DisplayHelperV2ApplyOperation, IncompleteBaselineKeepsRecoveryArmedAfterSettingsFailure) {
  FakeClock clock;
  FakeDisplaySettings display;
  display.topology = display_device::ActiveTopology {{"PHYSICAL"}};
  display.snapshot.m_topology = display.topology;
  // Deliberately omit mode data: this is not a complete rollback baseline.
  display.enumerated_devices = {make_active_device("PHYSICAL"), make_active_device("VIRTUAL")};
  display.apply_status = display_helper::v2::ApplyStatus::Retryable;

  display_helper::v2::ApplyOperation operation(display, clock);
  display_helper::v2::ApplyRequest request;
  display_device::SingleDisplayConfiguration config;
  config.m_device_id = "VIRTUAL";
  config.m_device_prep = display_device::SingleDisplayConfiguration::DevicePreparation::EnsureOnlyDisplay;
  request.configuration = config;
  request.topology = display_device::ActiveTopology {{"VIRTUAL"}};

  display_helper::v2::CancellationSource source;
  const auto outcome = operation.run(request, source.token());

  EXPECT_EQ(outcome.status, display_helper::v2::ApplyStatus::Retryable);
  EXPECT_TRUE(outcome.display_may_have_changed);
  EXPECT_EQ(display.apply_snapshot_calls, 0);
}

TEST(DisplayHelperV2ApplyOperation, RecoversAndRetriesWhenTopologyNeverEnumerates) {
  FakeClock clock;
  FakeDisplaySettings display;
  display.topology = display_device::ActiveTopology {{"PHYSICAL"}};
  display.enumerated_devices = {make_active_device("VIRTUAL")};
  display.inactive_enumeration_calls = 1000;

  display_helper::v2::ApplyOperation operation(display, clock);
  display_helper::v2::ApplyRequest request;
  display_device::SingleDisplayConfiguration config;
  config.m_device_id = "VIRTUAL";
  config.m_device_prep = display_device::SingleDisplayConfiguration::DevicePreparation::EnsureOnlyDisplay;
  request.configuration = config;
  request.topology = display_device::ActiveTopology {{"VIRTUAL"}};

  display_helper::v2::CancellationSource source;
  const auto outcome = operation.run(request, source.token());

  EXPECT_EQ(outcome.status, display_helper::v2::ApplyStatus::Retryable);
  EXPECT_EQ(display.apply_topology_calls, 2);
  EXPECT_EQ(display.recovery_calls, 1);
  EXPECT_EQ(display.apply_calls, 0);
  EXPECT_EQ(clock.slept_for, std::chrono::milliseconds(10500));
}

TEST(DisplayHelperV2SnapshotPersistence, SaveFiltersBlacklistedDevices) {
  display_helper::v2::InMemorySnapshotStorage storage;
  display_helper::v2::SnapshotPersistence persistence(storage);

  auto snapshot = make_snapshot({"A", "B"});
  std::set<std::string> blacklist {"B"};

  EXPECT_TRUE(persistence.save(display_helper::v2::SnapshotTier::Current, snapshot, blacklist));

  auto loaded = storage.load(display_helper::v2::SnapshotTier::Current);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->m_topology.size(), 1u);
  EXPECT_EQ(loaded->m_topology.front().size(), 1u);
  EXPECT_EQ(loaded->m_topology.front().front(), "A");
  EXPECT_EQ(loaded->m_modes.count("B"), 0u);
}

TEST(DisplayHelperV2SnapshotPersistence, SaveRejectsAllBlacklisted) {
  display_helper::v2::InMemorySnapshotStorage storage;
  display_helper::v2::SnapshotPersistence persistence(storage);

  auto snapshot = make_snapshot({"B"});
  std::set<std::string> blacklist {"B"};

  EXPECT_FALSE(persistence.save(display_helper::v2::SnapshotTier::Current, snapshot, blacklist));
}

TEST(DisplayHelperV2SnapshotPersistence, LoadRejectsMissingDevices) {
  display_helper::v2::InMemorySnapshotStorage storage;
  display_helper::v2::SnapshotPersistence persistence(storage);

  auto snapshot = make_snapshot({"A"});
  EXPECT_TRUE(storage.save(display_helper::v2::SnapshotTier::Current, snapshot));

  std::set<std::string> available {"B"};
  auto loaded = persistence.load(display_helper::v2::SnapshotTier::Current, available);
  EXPECT_FALSE(loaded.has_value());
}

TEST(DisplayHelperV2SnapshotPersistence, RecoveryOrderRespectsGoldenPreference) {
  display_helper::v2::InMemorySnapshotStorage storage;
  display_helper::v2::SnapshotPersistence persistence(storage);

  auto order = persistence.recovery_order();
  ASSERT_EQ(order.size(), 3u);
  EXPECT_EQ(order[0], display_helper::v2::SnapshotTier::Current);
  EXPECT_EQ(order[1], display_helper::v2::SnapshotTier::Previous);
  EXPECT_EQ(order[2], display_helper::v2::SnapshotTier::Golden);

  persistence.set_prefer_golden_first(true);
  order = persistence.recovery_order();
  ASSERT_EQ(order.size(), 3u);
  EXPECT_EQ(order[0], display_helper::v2::SnapshotTier::Golden);
  EXPECT_EQ(order[1], display_helper::v2::SnapshotTier::Current);
  EXPECT_EQ(order[2], display_helper::v2::SnapshotTier::Previous);
}

TEST(DisplayHelperV2SnapshotPersistence, RotateCopiesCurrentToPrevious) {
  display_helper::v2::InMemorySnapshotStorage storage;
  display_helper::v2::SnapshotPersistence persistence(storage);

  auto snapshot = make_snapshot({"A"});
  EXPECT_TRUE(storage.save(display_helper::v2::SnapshotTier::Current, snapshot));
  EXPECT_TRUE(persistence.rotate_current_to_previous());

  auto previous = storage.load(display_helper::v2::SnapshotTier::Previous);
  ASSERT_TRUE(previous.has_value());
  EXPECT_EQ(previous->m_topology.front().front(), "A");
}

TEST(DisplayHelperV2SnapshotService, CaptureReturnsSnapshot) {
  FakeDisplaySettings display;
  display.snapshot = make_snapshot({"A"});

  display_helper::v2::SnapshotService service(display);
  auto captured = service.capture();
  EXPECT_EQ(captured.m_topology, display.snapshot.m_topology);
}

TEST(DisplayHelperV2SnapshotService, ApplyRejectsInvalidTopology) {
  FakeDisplaySettings display;
  display.validate_topology_result = false;

  display_helper::v2::SnapshotService service(display);
  display_helper::v2::CancellationSource source;
  auto status = service.apply(display.snapshot, source.token());
  EXPECT_EQ(status, display_helper::v2::ApplyStatus::InvalidRequest);
}

TEST(DisplayHelperV2SnapshotService, ApplyReturnsRetryableOnFailure) {
  FakeDisplaySettings display;
  display.apply_snapshot_result = false;

  display_helper::v2::SnapshotService service(display);
  display_helper::v2::CancellationSource source;
  auto status = service.apply(display.snapshot, source.token());
  EXPECT_EQ(status, display_helper::v2::ApplyStatus::Retryable);
}

TEST(DisplayHelperV2SnapshotService, ApplyReturnsOkOnSuccess) {
  FakeDisplaySettings display;

  display_helper::v2::SnapshotService service(display);
  display_helper::v2::CancellationSource source;
  auto status = service.apply(display.snapshot, source.token());
  EXPECT_EQ(status, display_helper::v2::ApplyStatus::Ok);
}

TEST(DisplayHelperV2SnapshotService, ApplyReturnsFatalWhenCancelled) {
  FakeDisplaySettings display;
  display_helper::v2::SnapshotService service(display);
  display_helper::v2::CancellationSource source;
  auto token = source.token();
  source.cancel();

  auto status = service.apply(display.snapshot, token);
  EXPECT_EQ(status, display_helper::v2::ApplyStatus::Fatal);
}

TEST(DisplayHelperV2SnapshotService, MatchesCurrentUsesDisplayBackend) {
  FakeDisplaySettings display;
  display.snapshot_matches_result = false;

  display_helper::v2::SnapshotService service(display);
  EXPECT_FALSE(service.matches_current(display.snapshot));
}

TEST(DisplayHelperV2FileSnapshotStorage, SaveLoadRoundTrip) {
  TempDir temp;
  display_helper::v2::SnapshotPaths paths {
    temp.path / "current.json",
    temp.path / "previous.json",
    temp.path / "golden.json"
  };
  display_helper::v2::FileSnapshotStorage storage(paths);

  display_device::DisplaySettingsSnapshot snapshot;
  snapshot.m_topology = {{"A", "B"}};
  snapshot.m_modes["A"] = display_device::DisplayMode {};
  snapshot.m_modes["B"] = display_device::DisplayMode {};
  snapshot.m_hdr_states["A"] = display_device::HdrState::Enabled;
  snapshot.m_hdr_states["B"] = std::nullopt;
  snapshot.m_primary_device = "A";

  EXPECT_TRUE(storage.save(display_helper::v2::SnapshotTier::Current, snapshot));

  auto loaded = storage.load(display_helper::v2::SnapshotTier::Current);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(*loaded, snapshot);
}

TEST(DisplayHelperV2FileSnapshotStorage, ReportsMissingDevices) {
  TempDir temp;
  display_helper::v2::SnapshotPaths paths {
    temp.path / "current.json",
    temp.path / "previous.json",
    temp.path / "golden.json"
  };
  display_helper::v2::FileSnapshotStorage storage(paths);

  display_device::DisplaySettingsSnapshot snapshot;
  snapshot.m_topology = {{"A", "B"}};
  snapshot.m_modes["A"] = display_device::DisplayMode {};
  snapshot.m_modes["B"] = display_device::DisplayMode {};
  snapshot.m_hdr_states["A"] = std::nullopt;
  snapshot.m_hdr_states["B"] = std::nullopt;

  std::set<std::string> available {"A"};
  auto missing = storage.missing_devices(snapshot, available);

  ASSERT_EQ(missing.size(), 1u);
  EXPECT_EQ(missing.front(), "B");
}

#endif  // _WIN32
