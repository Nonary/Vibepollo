/** @file Passive Linux display-recovery incident gates. */
#include <gtest/gtest.h>
#include <src/platform/linux/private_display_cleanup_policy.h>
#include <src/platform/linux/private_display_recovery_policy.h>

#include <atomic>
#include <array>
#include <chrono>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace policy = platf::linux_private_display::recovery_policy;

namespace {
  policy::incident_t incident() {
    return {
      .cleanup_generation = 9,
      .session_owner = "1000:desktop",
      .snapshot_identity = "snapshot-a",
      .restore_callback_claimed = true,
      .transaction_failed = true,
      .helper_completion_known = true,
      .attempted = false,
    };
  }

  bool incident_allowed(const policy::incident_t &ticket,
                 std::uint64_t generation = 9,
                 std::string owner = "1000:desktop",
                 std::string snapshot = "snapshot-a",
                 bool helper_known = true,
                 bool capture_owner = false,
                 bool retained_monitor = false,
                 bool shutdown = false) {
    return policy::may_start(ticket, generation, owner, snapshot, helper_known,
                             capture_owner, retained_monitor, shutdown);
  }
}

TEST(LinuxPrivateDisplayRecoveryPolicy, RequiresClaimedConfirmedFailureAndKnownHelperCompletion) {
  auto ticket = incident();
  EXPECT_TRUE(incident_allowed(ticket));

  ticket.restore_callback_claimed = false;
  EXPECT_FALSE(incident_allowed(ticket));
  ticket = incident();
  ticket.transaction_failed = false;
  EXPECT_FALSE(incident_allowed(ticket));
  ticket = incident();
  ticket.helper_completion_known = false;
  EXPECT_FALSE(incident_allowed(ticket));
  EXPECT_FALSE(incident_allowed(incident(), 9, "1000:desktop", "snapshot-a", false));
}

TEST(LinuxPrivateDisplayRecoveryPolicy, StaleOwnerSnapshotGenerationAndShutdownCancelIncident) {
  const auto ticket = incident();
  EXPECT_FALSE(incident_allowed(ticket, 10));
  EXPECT_FALSE(incident_allowed(ticket, 9, "1001:desktop"));
  EXPECT_FALSE(incident_allowed(ticket, 9, "1000:desktop", "snapshot-b"));
  EXPECT_FALSE(incident_allowed(ticket, 9, "1000:desktop", "snapshot-a", true, true));
  EXPECT_FALSE(incident_allowed(ticket, 9, "1000:desktop", "snapshot-a", true, false, true));
  EXPECT_FALSE(incident_allowed(ticket, 9, "1000:desktop", "snapshot-a", true, false, false, true));
}

TEST(LinuxPrivateDisplayRecoveryPolicy, IncidentCanOnlyBeConsumedOnce) {
  auto ticket = incident();
  ASSERT_TRUE(incident_allowed(ticket));
  ticket.attempted = true;
  EXPECT_FALSE(incident_allowed(ticket));
}

TEST(LinuxPrivateDisplayRecoveryPolicy, FailureTicketUsesTheClaimedGenerationAfterRestoreAdmission) {
  constexpr std::uint64_t request_generation = 8;
  constexpr std::uint64_t claimed_generation = request_generation + 1;
  // run_delayed_restore claims request_generation+1 before calling restore;
  // a same-generation failure therefore leaves cleanup_generation at the
  // callback's claimed value.
  EXPECT_TRUE(policy::failed_claim_is_current(claimed_generation, claimed_generation));
  EXPECT_FALSE(policy::failed_claim_is_current(claimed_generation, claimed_generation + 1));
  EXPECT_FALSE(policy::failed_claim_is_current(0, 0));
}

TEST(LinuxPrivateDisplayRecoveryPolicy, FailedRestoreRetryCanArmOnlyItsActualClaimedGeneration) {
  std::mutex lifecycle;
  std::mutex display;
  std::atomic<std::uint64_t> generation {8};
  std::optional<std::uint64_t> callback_generation;
  constexpr std::array<std::chrono::milliseconds, 0> no_retries {};

  const auto result = platf::linux_private_display::cleanup_policy::run_delayed_restore_with_retries(
    lifecycle, display, generation, 8,
    [] { return false; },
    [&](const std::uint64_t claimed, const auto &) {
      callback_generation = claimed;
      return false;
    },
    [] { return true; }, no_retries);

  ASSERT_EQ(result, platf::linux_private_display::cleanup_policy::result_e::failed);
  ASSERT_TRUE(callback_generation);
  ASSERT_EQ(generation.load(), *callback_generation);
  auto ticket = incident();
  ticket.cleanup_generation = *callback_generation;
  EXPECT_TRUE(policy::failed_claim_is_current(*callback_generation, generation.load()));
  EXPECT_TRUE(incident_allowed(ticket, generation.load()));
  generation.fetch_add(1);
  EXPECT_FALSE(policy::failed_claim_is_current(*callback_generation, generation.load()));
  EXPECT_FALSE(incident_allowed(ticket, generation.load()));
}

TEST(LinuxPrivateDisplayRecoveryPolicy, AutomaticPartialFailureDoesNotRetryAgainstItsNewGuard) {
  std::mutex lifecycle;
  std::mutex display;
  std::atomic<std::uint64_t> generation {30};
  constexpr std::array<std::chrono::milliseconds, 0> no_retry_delays {};
  std::vector<std::string> durable_baseline {"A", "B"};
  bool guard_a_enabled = false;
  unsigned attempts = 0;
  policy::target_e first_target = policy::target_e::live_physical_layout;

  const auto result = platf::linux_private_display::cleanup_policy::run_delayed_restore_with_retries(
    lifecycle, display, generation, 30,
    [] { return false; },
    [&](const std::uint64_t, const auto &) {
      ++attempts;
      first_target = policy::select_target(policy::target_policy_e::automatic_recovery, guard_a_enabled);
      if (first_target == policy::target_e::live_physical_layout) durable_baseline = {"A"};
      // The guarded transaction activates A, then fails before capture handoff.
      // The snapshot must remain A+B and this incident gets no second attempt.
      guard_a_enabled = true;
      return false;
    },
    [] { return true; }, no_retry_delays);

  EXPECT_EQ(result, platf::linux_private_display::cleanup_policy::result_e::failed);
  EXPECT_EQ(attempts, 1u);
  EXPECT_EQ(first_target, policy::target_e::saved_baseline);
  EXPECT_EQ(policy::select_target(policy::target_policy_e::automatic_recovery, guard_a_enabled),
            policy::target_e::live_physical_layout);
  EXPECT_EQ(durable_baseline, (std::vector<std::string> {"A", "B"}));
}

TEST(LinuxPrivateDisplayRecoveryPolicy, ConnectedOutputStillRequiresKnownEnabledStateForReadiness) {
  const policy::connector_state_t unknown {};
  const policy::connector_state_t disconnected {
    .known = true,
    .connected = false,
    .enabled_known = true,
    .enabled = false,
  };
  const policy::connector_state_t returned_disabled {
    .known = true,
    .connected = true,
    .enabled_known = true,
    .enabled = false,
  };

  EXPECT_FALSE(unknown.present());
  EXPECT_FALSE(disconnected.present());
  EXPECT_TRUE(returned_disabled.present());
  EXPECT_FALSE(returned_disabled.ready());

  const policy::connector_state_t enabled_unknown {
    .known = true,
    .connected = true,
    .enabled_known = false,
    .enabled = false,
  };
  EXPECT_TRUE(enabled_unknown.present());
  EXPECT_FALSE(enabled_unknown.ready());
}

TEST(LinuxPrivateDisplayRecoveryPolicy, RequiresObservedDpmsWakeOrPhysicalTopologyEdge) {
  policy::wake_event_tracker_t events;
  const policy::connector_state_t awake {
    .known = true, .connected = true, .enabled_known = true, .enabled = true,
    .dpms_known = true, .dpms_on = true,
  };
  auto asleep = awake;
  asleep.dpms_on = false;
  events.prime("HDMI-A-1", awake);
  EXPECT_FALSE(static_cast<bool>(events.observe("HDMI-A-1", awake))); // Stable-connected is not a wake.
  EXPECT_FALSE(static_cast<bool>(events.observe("HDMI-A-1", asleep)));
  EXPECT_TRUE(events.observe("HDMI-A-1", awake).dpms_woke); // DPMS off -> on.
  EXPECT_FALSE(static_cast<bool>(events.observe("HDMI-A-1", awake)));

  auto disconnected = awake;
  disconnected.connected = false;
  EXPECT_TRUE(events.observe("HDMI-A-1", disconnected).topology_changed);
  EXPECT_FALSE(static_cast<bool>(events.observe("HDMI-A-1", policy::connector_state_t {})));
  EXPECT_FALSE(static_cast<bool>(events.observe("HDMI-A-1", awake))); // Unknown breaks edge continuity.
  EXPECT_FALSE(events.observe("DP-2", awake).topology_changed); // An incomplete enumeration cannot fabricate an edge.
  EXPECT_TRUE(events.observe("DP-2", disconnected).topology_changed);
  EXPECT_TRUE(events.observe("DP-2", awake).topology_changed);

  policy::wake_event_tracker_t initially_unknown;
  initially_unknown.prime("HDMI-A-1", {});
  EXPECT_FALSE(static_cast<bool>(initially_unknown.observe("HDMI-A-1", awake)));
  EXPECT_FALSE(static_cast<bool>(initially_unknown.observe("DP-3", {})));
  EXPECT_FALSE(static_cast<bool>(initially_unknown.observe("DP-3", awake)));
}

TEST(LinuxPrivateDisplayRecoveryPolicy, OrphanFallbackOnlyFollowsVirtualConnectorRetirementFailure) {
  EXPECT_TRUE(policy::solely_virtual_cleanup_failed(true, false));
  EXPECT_TRUE(policy::solely_virtual_cleanup_failed(false, true));
  EXPECT_FALSE(policy::solely_virtual_cleanup_failed(false, false));
}

TEST(LinuxPrivateDisplayRecoveryPolicy, PersistentManagedSysfsNodeMustReportDisconnectedAndCompositorInactive) {
  EXPECT_TRUE(policy::managed_connector_retired(true, true, true, true));
  EXPECT_FALSE(policy::managed_connector_retired(true, true, false, true));
  EXPECT_FALSE(policy::managed_connector_retired(true, false, false, true));
  EXPECT_FALSE(policy::managed_connector_retired(true, true, true, false));
  EXPECT_TRUE(policy::managed_connector_retired(false, false, false, false));
}

TEST(LinuxPrivateDisplayRecoveryPolicy, RechecksVerifiedPostGuardTopologyBeforeRetirement) {
  // A saved physical guard may need activation when every physical output was
  // disabled at incident time. Capture the topology after that planned change,
  // then reject later drift before removing a managed connector.
  const std::string verified_after_guard = "HDMI-A-1:connected,enabled,3840x2160@120";
  EXPECT_TRUE(policy::topology_still_matches(verified_after_guard, verified_after_guard));
  EXPECT_FALSE(policy::topology_still_matches(
    verified_after_guard, std::string {"HDMI-A-1:connected,enabled,1920x1080@60"}));
}

TEST(LinuxPrivateDisplayRecoveryPolicy, KScreenTopologyChangesAreEventsAndUnknownReadsResetBaseline) {
  policy::topology_change_tracker_t<std::string> events;
  events.prime("HDMI-A-1:connected,disabled");
  EXPECT_FALSE(events.observe(std::string {"HDMI-A-1:connected,disabled"}));
  EXPECT_TRUE(events.observe(std::string {"HDMI-A-1:connected,enabled,3840x2160@120"}));
  EXPECT_FALSE(events.observe(std::optional<std::string> {}));
  EXPECT_FALSE(events.observe(std::string {"HDMI-A-1:connected,enabled,3840x2160@120"}));
  EXPECT_TRUE(events.observe(std::string {"HDMI-A-1:connected,enabled,1920x1080@60"}));
}

TEST(LinuxPrivateDisplayRecoveryPolicy, MonitorRunnerContainsMalformedCompositorExceptions) {
  bool failed = false;
  std::string detail;
  policy::run_monitor_safely(
    [] { throw std::runtime_error("malformed KScreen output"); },
    [&](std::string_view message) {
      failed = true;
      detail = message;
    });
  EXPECT_TRUE(failed);
  EXPECT_EQ(detail, "malformed KScreen output");
}

TEST(LinuxPrivateDisplayRecoveryPolicy, AutomaticTargetPreservesFreshEnabledLayoutAndHandlesDisabledReturn) {
  EXPECT_EQ(policy::select_target(policy::target_policy_e::automatic_recovery, true),
            policy::target_e::live_physical_layout);
  EXPECT_EQ(policy::select_target(policy::target_policy_e::automatic_recovery, false),
            policy::target_e::saved_baseline);
  EXPECT_EQ(policy::select_target(policy::target_policy_e::saved_baseline, true),
            policy::target_e::saved_baseline);
}
