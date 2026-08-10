/**
 * @file tests/unit/test_display_helper_v2_core.cpp
 * @brief Unit tests for display helper v2 core components.
 */
#ifdef _WIN32

#include "../tests_common.h"

#include "src/platform/windows/display_helper_v2/operations.h"
#include "src/platform/windows/display_helper_v2/runtime_support.h"
#include "src/platform/windows/display_helper_v2/snapshot.h"
#include "src/platform/windows/display_helper_v2/topology_policy.h"

#include <algorithm>
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

    bool set_display_origin(const std::string &, const display_device::Point &) override {
      ++set_display_origin_calls;
      return set_display_origin_result;
    }

    bool set_device_refresh_rate(const std::string &, unsigned int, unsigned int) override {
      ++set_refresh_rate_calls;
      return set_refresh_rate_result;
    }

    std::set<std::string> set_device_refresh_rates(
      const std::vector<std::pair<std::string, std::pair<unsigned int, unsigned int>>> &overrides) override {
      ++set_refresh_rates_batch_calls;
      std::set<std::string> applied;
      if (set_refresh_rate_result) {
        for (const auto &[device_id, _] : overrides) {
          applied.insert(device_id);
        }
      }
      return applied;
    }

    std::unordered_map<std::string, std::optional<display_device::Resolution>>
    get_repositionable_display_resolutions(const std::set<std::string> &device_ids) override {
      ++repositionable_displays_query_calls;
      std::unordered_map<std::string, std::optional<display_device::Resolution>> displays;
      for (const auto &device_id : device_ids) {
        displays.emplace(device_id, std::nullopt);
      }
      return displays;
    }

    display_helper::v2::ApplyPreflightOutcome preflight_apply(
      const display_device::SingleDisplayConfiguration &config,
      const std::optional<display_device::ActiveTopology> &base_topology) override {
      events.emplace_back("preflight");
      ++preflight_calls;
      preflight_topology = base_topology;
      if (preflight_status != display_helper::v2::ApplyStatus::Ok) {
        return {.status = preflight_status};
      }
      if (apply_topology_plan) {
        return {
          .status = display_helper::v2::ApplyStatus::Ok,
          .plan = apply_topology_plan,
        };
      }
      auto result = display_helper::v2::IDisplaySettings::preflight_apply(config, base_topology);
      if (result.plan && planned_topology) {
        result.plan->topology = *planned_topology;
      }
      return result;
    }

    bool is_topology_same(const display_device::ActiveTopology &lhs, const display_device::ActiveTopology &rhs) override {
      return lhs == rhs;
    }

    bool recover_display_stack() override {
      events.emplace_back("recover");
      ++recovery_calls;
      return recovery_result;
    }

    display_helper::v2::ApplyStatus apply_status = display_helper::v2::ApplyStatus::Ok;
    display_helper::v2::ApplyStatus apply_topology_status = display_helper::v2::ApplyStatus::Ok;
    std::optional<display_device::ActiveTopology> applied_topology_override;
    std::optional<display_device::SingleDisplayConfiguration> applied_configuration;
    display_device::EnumeratedDeviceList enumerated_devices;
    display_device::ActiveTopology topology;
    bool validate_topology_result = true;
    display_device::DisplaySettingsSnapshot snapshot;
    bool apply_snapshot_result = true;
    bool snapshot_matches_result = true;
    bool configuration_matches_result = true;
    std::optional<display_device::SingleDisplayConfiguration> verification_configuration;
    std::optional<display_helper::v2::ResolvedConfigurationTarget> verification_target;
    bool set_display_origin_result = true;
    bool set_refresh_rate_result = true;
    std::optional<display_device::ActiveTopology> planned_topology;
    std::optional<display_helper::v2::ApplyTopologyPlan> apply_topology_plan;
    bool recovery_result = true;
    display_helper::v2::ApplyStatus preflight_status = display_helper::v2::ApplyStatus::Ok;
    std::optional<display_device::ActiveTopology> preflight_topology;
    std::vector<std::string> events;
    int inactive_enumeration_calls = 0;
    int apply_calls = 0;
    int apply_snapshot_calls = 0;
    int apply_topology_calls = 0;
    int enumerate_calls = 0;
    int recovery_calls = 0;
    int preflight_calls = 0;
    int set_display_origin_calls = 0;
    int set_refresh_rate_calls = 0;
    int set_refresh_rates_batch_calls = 0;
    int repositionable_displays_query_calls = 0;
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

TEST(DisplayHelperV2ApplyOperation, UsesExplicitTopologyAsSingleStagingBase) {
  FakeClock clock;
  FakeDisplaySettings display;
  display.topology = display_device::ActiveTopology {{"OLD"}};
  display.enumerated_devices = {make_active_device("A"), make_active_device("B")};
  display.planned_topology = display_device::ActiveTopology {{"A"}};

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
  EXPECT_EQ(display.preflight_calls, 1);
  ASSERT_TRUE(display.preflight_topology.has_value());
  EXPECT_EQ(*display.preflight_topology, display_device::ActiveTopology({{"A"}, {"B"}}));
  EXPECT_EQ(display.apply_topology_calls, 1);
  EXPECT_EQ(display.apply_calls, 1);
  EXPECT_EQ(display.apply_snapshot_calls, 0);
  EXPECT_EQ(display.recovery_calls, 0);
}

TEST(DisplayHelperV2ApplyOperation, DoesNotWaitForUnrelatedTopologyEnumeration) {
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
  EXPECT_EQ(display.apply_topology_calls, 1);
  EXPECT_EQ(display.apply_calls, 1);
  EXPECT_EQ(clock.slept_for, std::chrono::milliseconds::zero());
}

TEST(DisplayHelperV2ApplyOperation, AppliesSettingsWithoutASeparateTopologyWhenNoBaseWasSupplied) {
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
  EXPECT_EQ(display.apply_topology_calls, 0);
  EXPECT_EQ(display.apply_calls, 1);
  EXPECT_EQ(clock.slept_for, std::chrono::milliseconds::zero());
}

TEST(DisplayHelperV2ApplyOperation, DoesNotPollOrRecoverForAnAdjustedTopology) {
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

  EXPECT_EQ(outcome.status, display_helper::v2::ApplyStatus::Ok);
  EXPECT_EQ(display.apply_topology_calls, 1);
  EXPECT_EQ(display.apply_calls, 1);
  EXPECT_EQ(display.recovery_calls, 0);
  EXPECT_EQ(clock.slept_for, std::chrono::milliseconds::zero());
}

TEST(DisplayHelperV2ApplyOperation, ResolvesImplicitPrimaryFromPreflightPlan) {
  FakeClock clock;
  FakeDisplaySettings display;
  display.topology = display_device::ActiveTopology {{"OLD"}};
  // Preflight can select any member of the intended duplicate primary group
  // without rewriting the public empty-device request.
  display.applied_topology_override = display_device::ActiveTopology {{"PRIMARY_B"}};
  display.enumerated_devices = {make_active_device("PRIMARY_B")};
  display.apply_topology_plan = display_helper::v2::ApplyTopologyPlan {
    .topology = display_device::ActiveTopology {{"PRIMARY_B"}},
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
  request.topology = display_device::ActiveTopology {{"PRIMARY_B"}};

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

TEST(DisplayHelperV2ApplyOperation, ResolvesExplicitTargetFromTheSuppliedBase) {
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
  EXPECT_EQ(outcome.resolved_target->duplicate_device_ids, (std::set<std::string> {"EXPLICIT_A"}));
}

TEST(DisplayHelperV2VerificationOperation, PreservesDefaultRequestAndVerifiesPlannedDuplicateGroup) {
  FakeClock clock;
  FakeDisplaySettings display;
  display.topology = display_device::ActiveTopology {{"PRIMARY_B"}};
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

TEST(DisplayHelperV2VerificationOperation, RejectsWhenTargetedSettingsQueryCannotFindDevice) {
  FakeClock clock;
  FakeDisplaySettings display;
  display.topology = display_device::ActiveTopology {{"UNRELATED"}};
  display.configuration_matches_result = false;
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
  EXPECT_EQ(clock.slept_for, std::chrono::milliseconds::zero());
}

TEST(DisplayHelperV2ApplyOperation, DoesNotPollTopologyEnumerationBeforeApplyingSettings) {
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
  EXPECT_EQ(display.enumerate_calls, 0);
  EXPECT_EQ(display.apply_calls, 1);
  EXPECT_EQ(clock.slept_for, std::chrono::milliseconds::zero());

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
  EXPECT_TRUE(outcome.durable_recovery_attempted);
  EXPECT_FALSE(outcome.display_may_have_changed);
  EXPECT_EQ(display.apply_topology_calls, 0);
  EXPECT_EQ(display.apply_calls, 0);
}

TEST(DisplayHelperV2ApplyOperation, FailedPreflightDoesNotRecoverOrMutateDisplayStack) {
  for (const auto status : {
         display_helper::v2::ApplyStatus::InvalidRequest,
         display_helper::v2::ApplyStatus::Retryable,
       }) {
    SCOPED_TRACE(static_cast<int>(status));
    FakeClock clock;
    FakeDisplaySettings display;
    display.topology = display_device::ActiveTopology {{"PHYSICAL"}};
    display.enumerated_devices = {make_active_device("VIRTUAL")};
    display.preflight_status = status;

    display_helper::v2::ApplyOperation operation(display, clock);
    display_helper::v2::ApplyRequest request;
    display_device::SingleDisplayConfiguration config;
    config.m_device_id = "VIRTUAL";
    config.m_device_prep = display_device::SingleDisplayConfiguration::DevicePreparation::EnsureOnlyDisplay;
    request.configuration = config;
    request.topology = display_device::ActiveTopology {{"VIRTUAL"}};

    display_helper::v2::CancellationSource source;
    const auto outcome = operation.run(request, source.token());

    EXPECT_EQ(outcome.status, status);
    EXPECT_EQ(display.preflight_calls, 1);
    EXPECT_EQ(display.recovery_calls, 0);
    EXPECT_EQ(display.apply_topology_calls, 0);
    EXPECT_EQ(display.apply_calls, 0);
    EXPECT_EQ(clock.slept_for, std::chrono::milliseconds::zero());
  }
}

TEST(DisplayHelperV2ApplyOperation, SettingsOnlyRepairSkipsPreflightTopologyAndAdjuncts) {
  FakeClock clock;
  FakeDisplaySettings display;
  display_helper::v2::ApplyOperation operation(display, clock);

  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  request.configuration->m_device_id = "VIRTUAL";
  request.topology = display_device::ActiveTopology {{"VIRTUAL"}};
  request.monitor_positions.emplace_back("VIRTUAL", display_device::Point {100, 200});
  request.refresh_rate_overrides.emplace_back("PHYSICAL", std::pair<unsigned int, unsigned int> {120, 1});
  request.settings_only_repair = true;
  request.repair_target = display_helper::v2::ResolvedConfigurationTarget {
    .kind = display_helper::v2::DeviceTargetKind::ExplicitDevice,
    .representative_device_id = "VIRTUAL",
    .duplicate_device_ids = {"VIRTUAL"},
  };

  display_helper::v2::CancellationSource source;
  const auto outcome = operation.run(request, source.token());

  EXPECT_EQ(outcome.status, display_helper::v2::ApplyStatus::Ok);
  ASSERT_TRUE(outcome.resolved_target);
  EXPECT_EQ(outcome.resolved_target->kind, display_helper::v2::DeviceTargetKind::ExplicitDevice);
  EXPECT_EQ(outcome.resolved_target->representative_device_id, "VIRTUAL");
  EXPECT_EQ(outcome.resolved_target->duplicate_device_ids, (std::set<std::string> {"VIRTUAL"}));
  EXPECT_TRUE(outcome.display_may_have_changed);
  EXPECT_TRUE(outcome.staged_state_prepared);
  EXPECT_EQ(display.preflight_calls, 0);
  EXPECT_EQ(display.apply_topology_calls, 0);
  EXPECT_EQ(display.apply_calls, 1);
  EXPECT_EQ(display.set_display_origin_calls, 0);
  EXPECT_EQ(display.set_refresh_rate_calls, 0);
  EXPECT_EQ(display.set_refresh_rates_batch_calls, 0);
  EXPECT_EQ(display.repositionable_displays_query_calls, 0);
  EXPECT_EQ(clock.slept_for, std::chrono::milliseconds::zero());
}

TEST(DisplayHelperV2ApplyOperation, BatchesPositionAndPhysicalRefreshQueries) {
  FakeClock clock;
  FakeDisplaySettings display;
  display_helper::v2::ApplyOperation operation(display, clock);

  display_helper::v2::ApplyRequest request;
  request.configuration = display_device::SingleDisplayConfiguration {};
  request.configuration->m_device_id = "VIRTUAL";
  request.topology = display_device::ActiveTopology {{"PHYSICAL_A"}, {"PHYSICAL_B"}, {"VIRTUAL"}};
  request.monitor_positions.emplace_back("PHYSICAL_A", display_device::Point {100, 200});
  request.monitor_positions.emplace_back("PHYSICAL_B", display_device::Point {300, 400});
  request.refresh_rate_overrides.emplace_back("PHYSICAL_A", std::pair<unsigned int, unsigned int> {120, 1});
  request.refresh_rate_overrides.emplace_back("PHYSICAL_B", std::pair<unsigned int, unsigned int> {144, 1});

  display_helper::v2::CancellationSource source;
  const auto outcome = operation.run(request, source.token());

  EXPECT_EQ(outcome.status, display_helper::v2::ApplyStatus::Ok);
  EXPECT_EQ(display.repositionable_displays_query_calls, 1);
  EXPECT_EQ(display.set_display_origin_calls, 2);
  EXPECT_EQ(display.set_refresh_rates_batch_calls, 1);
  EXPECT_EQ(display.set_refresh_rate_calls, 0);
}

TEST(DisplayHelperV2ApplyOperation, DelegatesPrimaryToSettingsAfterPreflight) {
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

TEST(DisplayHelperV2ApplyOperation, ReliesOnSettingsTransactionAndKeepsDurableRecoveryOnFailure) {
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
  config.m_hdr_state = display_device::HdrState::Enabled;
  request.configuration = config;
  request.topology = display_device::ActiveTopology {{"VIRTUAL"}};
  request.virtual_layout = "single";

  display_helper::v2::CancellationSource source;
  const auto outcome = operation.run(request, source.token());

  EXPECT_EQ(outcome.status, display_helper::v2::ApplyStatus::Retryable);
  EXPECT_TRUE(outcome.display_may_have_changed);
  EXPECT_EQ(display.topology, (display_device::ActiveTopology {{"VIRTUAL"}}));
  EXPECT_EQ(display.apply_topology_calls, 1);
  EXPECT_EQ(display.apply_snapshot_calls, 0);
}

TEST(DisplayHelperV2ApplyOperation, RetainsVirtualTopologyWhenHdrSettingsStageFails) {
  FakeClock clock;
  FakeDisplaySettings display;
  const auto baseline = display_device::ActiveTopology {{"PHYSICAL"}};
  const auto staged_topology = display_device::ActiveTopology {{"VIRTUAL"}};
  display.topology = baseline;
  display.snapshot = make_snapshot({"PHYSICAL"});
  display.enumerated_devices = {make_active_device("PHYSICAL"), make_active_device("VIRTUAL")};
  display.apply_status = display_helper::v2::ApplyStatus::HdrStateFailed;

  display_helper::v2::ApplyOperation operation(display, clock);
  display_helper::v2::ApplyRequest request;
  display_device::SingleDisplayConfiguration config;
  config.m_device_id = "VIRTUAL";
  config.m_device_prep = display_device::SingleDisplayConfiguration::DevicePreparation::EnsureOnlyDisplay;
  config.m_hdr_state = display_device::HdrState::Enabled;
  request.configuration = config;
  request.topology = staged_topology;
  request.virtual_layout = "single";

  display_helper::v2::CancellationSource source;
  const auto outcome = operation.run(request, source.token());

  EXPECT_EQ(outcome.status, display_helper::v2::ApplyStatus::HdrStateFailed);
  EXPECT_TRUE(outcome.display_may_have_changed);
  EXPECT_EQ(display.topology, staged_topology);
  EXPECT_EQ(display.apply_topology_calls, 1);
  EXPECT_EQ(display.apply_snapshot_calls, 0);
}

TEST(DisplayHelperV2ApplyOperation, PhysicalHdrFailureDoesNotLaunchASecondRollbackTransaction) {
  FakeClock clock;
  FakeDisplaySettings display;
  const auto baseline = display_device::ActiveTopology {{"PHYSICAL_A"}};
  display.topology = baseline;
  display.snapshot = make_snapshot({"PHYSICAL_A"});
  display.enumerated_devices = {make_active_device("PHYSICAL_A"), make_active_device("PHYSICAL_B")};
  display.apply_status = display_helper::v2::ApplyStatus::HdrStateFailed;

  display_helper::v2::ApplyOperation operation(display, clock);
  display_helper::v2::ApplyRequest request;
  display_device::SingleDisplayConfiguration config;
  config.m_device_id = "PHYSICAL_B";
  config.m_device_prep = display_device::SingleDisplayConfiguration::DevicePreparation::EnsureOnlyDisplay;
  config.m_hdr_state = display_device::HdrState::Enabled;
  request.configuration = config;
  request.topology = display_device::ActiveTopology {{"PHYSICAL_B"}};

  display_helper::v2::CancellationSource source;
  const auto outcome = operation.run(request, source.token());

  EXPECT_EQ(outcome.status, display_helper::v2::ApplyStatus::HdrStateFailed);
  EXPECT_TRUE(outcome.display_may_have_changed);
  EXPECT_EQ(display.topology, (display_device::ActiveTopology {{"PHYSICAL_B"}}));
  EXPECT_EQ(display.apply_topology_calls, 1);
  EXPECT_EQ(display.apply_snapshot_calls, 0);
}

TEST(DisplayHelperV2ApplyOperation, VirtualSdrFailureDoesNotLaunchASecondRollbackTransaction) {
  FakeClock clock;
  FakeDisplaySettings display;
  const auto baseline = display_device::ActiveTopology {{"PHYSICAL"}};
  display.topology = baseline;
  display.snapshot = make_snapshot({"PHYSICAL"});
  display.enumerated_devices = {make_active_device("PHYSICAL"), make_active_device("VIRTUAL")};
  display.apply_status = display_helper::v2::ApplyStatus::HdrStateFailed;

  display_helper::v2::ApplyOperation operation(display, clock);
  display_helper::v2::ApplyRequest request;
  display_device::SingleDisplayConfiguration config;
  config.m_device_id = "VIRTUAL";
  config.m_device_prep = display_device::SingleDisplayConfiguration::DevicePreparation::EnsureOnlyDisplay;
  config.m_hdr_state = display_device::HdrState::Disabled;
  request.configuration = config;
  request.topology = display_device::ActiveTopology {{"VIRTUAL"}};
  request.virtual_layout = "single";

  display_helper::v2::CancellationSource source;
  const auto outcome = operation.run(request, source.token());

  EXPECT_EQ(outcome.status, display_helper::v2::ApplyStatus::HdrStateFailed);
  EXPECT_TRUE(outcome.display_may_have_changed);
  EXPECT_EQ(display.topology, (display_device::ActiveTopology {{"VIRTUAL"}}));
  EXPECT_EQ(display.apply_topology_calls, 1);
  EXPECT_EQ(display.apply_snapshot_calls, 0);
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

TEST(DisplayHelperV2ApplyOperation, DoesNotPollTopologyAfterPreflight) {
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

  EXPECT_EQ(outcome.status, display_helper::v2::ApplyStatus::Ok);
  EXPECT_EQ(display.apply_topology_calls, 1);
  EXPECT_EQ(display.recovery_calls, 0);
  EXPECT_EQ(display.apply_calls, 1);
  // This fake preflight owns no production enumeration. The operation itself
  // must not add a readiness-poll loop after that preflight returns.
  EXPECT_EQ(display.enumerate_calls, 0);
  EXPECT_EQ(clock.slept_for, std::chrono::milliseconds::zero());
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
  display_helper::v2::InMemoryTextStorage text_storage;
  display_helper::v2::TextSnapshotStorage storage({"current.json", "previous.json", "golden.json"}, text_storage);

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
  EXPECT_TRUE(display_helper::v2::topology::equal_snapshot(*loaded, snapshot));
}

TEST(DisplayHelperV2FileSnapshotStorage, ReportsMissingDevices) {
  display_helper::v2::InMemoryTextStorage text_storage;
  display_helper::v2::TextSnapshotStorage storage({"current.json", "previous.json", "golden.json"}, text_storage);

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
