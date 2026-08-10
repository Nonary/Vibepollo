/**
 * @file tests/unit/test_display_helper_v2_topology_transition.cpp
 * @brief Focused regression tests for post-activation topology readiness.
 */
#ifdef _WIN32

#include "../tests_common.h"

#include "src/platform/windows/display_helper_request_policy.h"
#include "src/platform/windows/display_helper_v2/operations.h"

#include <chrono>
#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
  using display_helper::v2::ActiveTopology;
  using display_helper::v2::ApplyStatus;
  using display_helper::v2::CancellationSource;
  using display_helper::v2::IDisplaySettings;
  using display_helper::v2::IClock;
  using display_helper::v2::TopologyActivationTarget;
  using display_helper::v2::TopologyTransition;
  using display_helper::v2::TopologyValidationMode;

  class FakeClock final : public IClock {
  public:
    std::chrono::steady_clock::time_point now() override {
      return now_;
    }

    void sleep_for(std::chrono::milliseconds duration) override {
      now_ += duration;
    }

  private:
    std::chrono::steady_clock::time_point now_ {std::chrono::steady_clock::now()};
  };

  display_device::EnumeratedDevice make_active_device(const std::string &device_id) {
    display_device::EnumeratedDevice device;
    device.m_device_id = device_id;
    device.m_info = display_device::EnumeratedDevice::Info {};
    return device;
  }

  class FakeDisplaySettings final : public IDisplaySettings {
  public:
    ApplyStatus apply(const display_device::SingleDisplayConfiguration &) override {
      return ApplyStatus::Ok;
    }

    ApplyStatus apply_topology(const ActiveTopology &) override {
      ++apply_topology_calls;
      events.push_back("apply_topology");
      current_topology = topology_after_apply;
      return topology_status;
    }

    display_device::EnumeratedDeviceList enumerate(display_device::DeviceEnumerationDetail) override {
      ++enumerate_calls;
      events.push_back("enumerate");
      if (!enumeration_sequence.empty()) {
        auto result = std::move(enumeration_sequence.front());
        enumeration_sequence.pop_front();
        return result;
      }
      return devices;
    }

    ActiveTopology capture_topology() override {
      return current_topology;
    }

    bool validate_topology(const ActiveTopology &) override {
      return true;
    }

    display_device::DisplaySettingsSnapshot capture_snapshot() override {
      return {};
    }

    bool apply_snapshot(const display_device::DisplaySettingsSnapshot &) override {
      return true;
    }

    bool snapshot_matches_current(const display_device::DisplaySettingsSnapshot &) override {
      return true;
    }

    bool configuration_matches(const display_device::SingleDisplayConfiguration &) override {
      return true;
    }

    bool set_display_origin(const std::string &, const display_device::Point &) override {
      return true;
    }

    std::optional<ActiveTopology> compute_expected_topology(
      const display_device::SingleDisplayConfiguration &, const std::optional<ActiveTopology> &) override {
      return std::nullopt;
    }

    bool is_topology_same(const ActiveTopology &lhs, const ActiveTopology &rhs) override {
      return lhs == rhs;
    }

    bool recover_display_stack() override {
      ++recover_calls;
      return true;
    }

    ApplyStatus topology_status = ApplyStatus::Ok;
    ActiveTopology current_topology;
    ActiveTopology topology_after_apply;
    display_device::EnumeratedDeviceList devices;
    std::deque<display_device::EnumeratedDeviceList> enumeration_sequence;
    int apply_topology_calls = 0;
    int enumerate_calls = 0;
    int recover_calls = 0;
    std::vector<std::string> events;
  };

  TopologyActivationTarget explicit_target(const std::string &device_id) {
    TopologyActivationTarget target;
    target.kind = display_helper::v2::DeviceTargetKind::ExplicitDevice;
    target.acceptable_device_ids.insert(device_id);
    return target;
  }
}  // namespace

TEST(DisplayHelperV2TopologyTransition, DeferredPolicyKeepsCompleteTopologyWhenDispatchIsSuppressed) {
  namespace policy = display_helper_integration::request_policy;
  policy::Input input;
  input.virtual_display = true;
  input.configuration_option = policy::ConfigurationOption::Disabled;
  input.layout = policy::VirtualDisplayLayout::Extended;
  input.target_device_id = "VIRTUAL";
  input.topology_snapshot = {{"PHYSICAL"}};

  const auto result = policy::evaluate(input);
  EXPECT_FALSE(result.dispatch);
  EXPECT_EQ(result.topology, (std::vector<std::vector<std::string>> {{"PHYSICAL"}, {"VIRTUAL"}}));
}

TEST(DisplayHelperV2TopologyTransition, ApplyWaitsForInitiallyUnpublishedTargetAndAcceptsOsAdjustment) {
  FakeClock clock;
  FakeDisplaySettings display;
  // The requested target is initially absent from capture and is not
  // enumerable until after activation settles.
  display.current_topology = {{"PHYSICAL"}};
  display.topology_after_apply = {{"PHYSICAL-ADJUSTED"}, {"VIRTUAL"}};
  display.enumeration_sequence.push_back({});
  display.enumeration_sequence.push_back({make_active_device("VIRTUAL")});

  bool recovery_armed = false;
  TopologyTransition transition(display, clock);
  CancellationSource cancellation;
  const auto outcome = transition.run(
    {{"PHYSICAL"}, {"VIRTUAL"}},
    TopologyValidationMode::StrictApply,
    explicit_target("VIRTUAL"),
    cancellation.token(),
    [&recovery_armed] {
      recovery_armed = true;
      return true;
    });

  ASSERT_EQ(outcome.status, ApplyStatus::Ok);
  ASSERT_TRUE(outcome.applied_topology.has_value());
  EXPECT_EQ(*outcome.applied_topology, display.topology_after_apply);
  EXPECT_EQ(display.apply_topology_calls, 1);
  EXPECT_EQ(display.enumerate_calls, 2);
  EXPECT_EQ(display.events, (std::vector<std::string> {"apply_topology", "enumerate", "enumerate"}));
  EXPECT_TRUE(recovery_armed);
  EXPECT_TRUE(outcome.durable_recovery_armed);
}

TEST(DisplayHelperV2TopologyTransition, ExactCurrentTopologyUsesNoOpFastPath) {
  FakeClock clock;
  FakeDisplaySettings display;
  display.current_topology = {{"PHYSICAL"}, {"VIRTUAL"}};
  display.topology_after_apply = {{"UNEXPECTED"}};

  bool recovery_armed = false;
  TopologyTransition transition(display, clock);
  CancellationSource cancellation;
  const auto outcome = transition.run(
    {{"PHYSICAL"}, {"VIRTUAL"}},
    TopologyValidationMode::StrictApply,
    explicit_target("VIRTUAL"),
    cancellation.token(),
    [&recovery_armed] {
      recovery_armed = true;
      return true;
    });

  ASSERT_EQ(outcome.status, ApplyStatus::Ok);
  ASSERT_TRUE(outcome.applied_topology.has_value());
  EXPECT_EQ(*outcome.applied_topology, display.current_topology);
  EXPECT_EQ(display.apply_topology_calls, 0);
  EXPECT_EQ(display.enumerate_calls, 0);
  EXPECT_FALSE(recovery_armed);
  EXPECT_FALSE(outcome.display_may_have_changed);
  EXPECT_FALSE(outcome.durable_recovery_armed);
}

TEST(DisplayHelperV2TopologyTransition, ApplyRejectsTargetThatNeverBecomesEnumerable) {
  FakeClock clock;
  FakeDisplaySettings display;
  display.current_topology = {{"PHYSICAL"}};
  display.topology_after_apply = {{"VIRTUAL"}};

  TopologyTransition transition(display, clock);
  CancellationSource cancellation;
  const auto outcome = transition.run(
    {{"VIRTUAL"}},
    TopologyValidationMode::StrictApply,
    explicit_target("VIRTUAL"),
    cancellation.token());

  EXPECT_EQ(outcome.status, ApplyStatus::Retryable);
  EXPECT_FALSE(outcome.applied_topology.has_value());
  EXPECT_EQ(display.apply_topology_calls, 2);
  EXPECT_EQ(display.recover_calls, 1);
}

TEST(DisplayHelperV2TopologyTransition, RestoreDoesNotAcceptOsAdjustedPartialSnapshot) {
  FakeClock clock;
  FakeDisplaySettings display;
  display.current_topology = {{"PHYSICAL"}};
  display.topology_after_apply = {{"RESTORED-A"}};
  display.devices = {make_active_device("RESTORED-A")};

  TopologyTransition transition(display, clock);
  CancellationSource cancellation;
  const auto outcome = transition.run(
    {{"RESTORED-A"}, {"RESTORED-B"}},
    TopologyValidationMode::StructuralRestore,
    TopologyActivationTarget {},
    cancellation.token());

  EXPECT_EQ(outcome.status, ApplyStatus::Retryable);
  EXPECT_FALSE(outcome.applied_topology.has_value());
  EXPECT_EQ(display.apply_topology_calls, 2);
  EXPECT_EQ(display.recover_calls, 1);
  EXPECT_EQ(display.enumerate_calls, 0);
}

TEST(DisplayHelperV2TopologyTransition, CancellationAtMutationBoundaryStopsBeforeApply) {
  FakeClock clock;
  FakeDisplaySettings display;
  display.current_topology = {{"PHYSICAL"}};
  display.topology_after_apply = {{"VIRTUAL"}};

  TopologyTransition transition(display, clock);
  CancellationSource cancellation;
  const auto outcome = transition.run(
    {{"VIRTUAL"}},
    TopologyValidationMode::StrictApply,
    explicit_target("VIRTUAL"),
    cancellation.token(),
    [&cancellation] {
      cancellation.cancel();
      return true;
    });

  EXPECT_EQ(outcome.status, ApplyStatus::Fatal);
  EXPECT_FALSE(outcome.applied_topology.has_value());
  EXPECT_TRUE(outcome.durable_recovery_armed);
  EXPECT_FALSE(outcome.display_may_have_changed);
  EXPECT_EQ(display.apply_topology_calls, 0);
}

#endif  // _WIN32
