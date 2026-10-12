/**
 * @file tests/unit/platform/linux/test_private_display_cleanup_policy.cpp
 * @brief Deterministic delayed-display cleanup admission races.
 */
#include <atomic>
#include <array>
#include <future>
#include <gtest/gtest.h>
#include <latch>
#include <mutex>
#include <src/platform/linux/private_display_cleanup_policy.h>
#include <src/platform/linux/private_display_emergency_policy.h>

#include <nlohmann/json.hpp>
#include <src/platform/linux/display_restore_dispatcher.h>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace policy = platf::linux_private_display::cleanup_policy;

namespace {
  nlohmann::json emergency_output(const std::string &name, const bool connected, const bool enabled) {
    return {
      {"name", name}, {"connected", connected}, {"enabled", enabled},
      {"currentModeId", "old"}, {"preferredModes", {"native"}},
      {"modes", nlohmann::json::array({
        {{"id", "old"}, {"size", {{"width", 1920}, {"height", 1080}}}, {"refreshRate", 60.0}},
        {{"id", "native"}, {"size", {{"width", 3840}, {"height", 2160}}}, {"refreshRate", 60.0}},
      })},
    };
  }
}

TEST(LinuxPrivateDisplayEmergencyPolicy, RecoversConnectedDisabledPhysicalMonitorWithNativeSdrMode) {
  namespace emergency = platf::linux_private_display::emergency_policy;
  const nlohmann::json configuration {{"outputs", nlohmann::json::array({
    emergency_output("Virtual-1", true, true),
    emergency_output("HDMI-A-1", true, false),
    emergency_output("DP-1", false, false),
  })}};
  const auto candidates = emergency::physical_candidates(configuration);
  ASSERT_EQ(candidates.size(), 1);
  EXPECT_EQ(candidates[0]["name"], "HDMI-A-1");
  EXPECT_EQ(candidates[0]["currentModeId"], "native");
  EXPECT_EQ(candidates[0]["scale"], 1.0);
  EXPECT_EQ(candidates[0]["rotation"], 1);
  EXPECT_EQ(candidates[0]["pos"], (nlohmann::json {{"x", 0}, {"y", 0}}));
  EXPECT_EQ(candidates[0]["hdr"], false);
  EXPECT_EQ(candidates[0]["enabled"], true);
}

TEST(LinuxPrivateDisplayEmergencyPolicy, PreservesAlreadyWorkingPhysicalLayout) {
  namespace emergency = platf::linux_private_display::emergency_policy;
  const nlohmann::json configuration {{"outputs", nlohmann::json::array({
    emergency_output("HDMI-A-1", true, true), emergency_output("DP-1", true, false),
  })}};
  EXPECT_TRUE(emergency::physical_active(configuration));
  EXPECT_TRUE(emergency::physical_candidates(configuration).empty());
}

TEST(LinuxPrivateDisplayEmergencyPolicy, NeverRecoversVirtualDisconnectedOrInvalidOutputs) {
  namespace emergency = platf::linux_private_display::emergency_policy;
  auto invalid_mode = emergency_output("HDMI-A-1", true, false);
  invalid_mode["modes"] = {{{"id", "1;bad"}, {"size", {{"width", 0}, {"height", 0}}}, {"refreshRate", 0.0}}};
  const nlohmann::json configuration {{"outputs", nlohmann::json::array({
    emergency_output("Virtual-1", true, true), emergency_output("DP-1", false, false), invalid_mode,
  })}};
  EXPECT_FALSE(emergency::physical_active(configuration));
  EXPECT_TRUE(emergency::physical_candidates(configuration).empty());
  EXPECT_TRUE(emergency::physical_candidates(nlohmann::json {{"outputs", "invalid"}}).empty());
}

TEST(LinuxPrivateDisplayEmergencyPolicy, TriesEachPhysicalMonitorAndFallsBackFromUnavailablePreferredMode) {
  namespace emergency = platf::linux_private_display::emergency_policy;
  auto hdmi = emergency_output("HDMI-A-1", true, false);
  hdmi["preferredModes"] = {"unavailable"};
  auto dp = emergency_output("DP-1", true, false);
  dp["preferredModes"] = {"unavailable"};
  dp["currentModeId"] = "gone";
  const nlohmann::json configuration {{"outputs", nlohmann::json::array({hdmi, dp})}};
  const auto candidates = emergency::physical_candidates(configuration);
  ASSERT_EQ(candidates.size(), 2);
  EXPECT_EQ(candidates[0]["currentModeId"], "old");
  EXPECT_EQ(candidates[1]["currentModeId"], "old");
}

TEST(LinuxPrivateDisplayCleanupPolicy, TerminalActionOverridesOwnersAndCompletesOutsideDisplayLock) {
  std::mutex lifecycle;
  std::mutex display;
  std::atomic<std::uint64_t> generation {1};
  bool owner_queried = false;
  bool completed = false;
  EXPECT_EQ(policy::run_delayed_restore(lifecycle, display, generation, 1,
    [&] { owner_queried = true; return true; }, [] { return true; }, {},
    std::chrono::steady_clock::time_point::max(), [&](std::uint64_t) {
      std::unique_lock display_lock {display, std::try_to_lock};
      EXPECT_TRUE(display_lock.owns_lock());
      completed = true;
    }, policy::admission_e::override_owners), policy::result_e::restored);
  EXPECT_FALSE(owner_queried);
  EXPECT_TRUE(completed);
}

TEST(LinuxPrivateDisplayCleanupPolicy, TerminalActionCannotOverrideSupersedingLaunchOrFailedRemoval) {
  std::mutex lifecycle;
  std::mutex display;
  std::atomic<std::uint64_t> generation {2};
  bool invoked = false;
  auto result = policy::run_delayed_restore(lifecycle, display, generation, 1,
    [] { return true; }, [&] { invoked = true; return true; }, {},
    std::chrono::steady_clock::time_point::max(), policy::no_completion_t {}, policy::admission_e::override_owners);
  EXPECT_EQ(result, policy::result_e::superseded);
  EXPECT_FALSE(invoked);
  bool completed = false;
  result = policy::run_delayed_restore(lifecycle, display, generation, 2,
    [] { return true; }, [] { return false; }, {},
    std::chrono::steady_clock::time_point::max(), [&](std::uint64_t) { completed = true; }, policy::admission_e::override_owners);
  EXPECT_EQ(result, policy::result_e::failed);
  EXPECT_FALSE(completed);
}

TEST(LinuxPrivateDisplayCleanupPolicy, TerminalRemovalContinuesWithoutPhysicalRestoreAndVerifiesEverySlot) {
  std::vector<std::string> steps;
  const std::array outputs {"Virtual-1", "Virtual-2"};
  const auto result = policy::terminate_outputs(outputs,
    [&] { steps.emplace_back("restore"); return false; },
    [&](const auto &name) { steps.emplace_back(name); return true; },
    [&](const auto &names) { EXPECT_EQ(names, outputs); steps.emplace_back("verify"); return true; },
    [] { return true; });
  EXPECT_FALSE(result.topology_restored);
  EXPECT_TRUE(result.virtual_displays_removed);
  EXPECT_EQ(steps, (std::vector<std::string> {"Virtual-1", "Virtual-2", "restore", "verify"}));
}

TEST(LinuxPrivateDisplayCleanupPolicy, TerminalRemovalAttemptsPeersButNeverReportsPartialOrUnverifiedSuccess) {
  for (const bool disconnect_ok : {false, true}) {
    int removals = 0;
    const auto result = policy::terminate_outputs(std::array {"Virtual-1", "Virtual-2"},
      [] { return true; },
      [&](const auto &) { return ++removals != 1 || disconnect_ok; },
      [&](const auto &) { return !disconnect_ok; }, [] { return true; });
    EXPECT_EQ(removals, 2);
    EXPECT_TRUE(result.topology_restored);
    EXPECT_FALSE(result.virtual_displays_removed);
  }
}

TEST(LinuxPrivateDisplayCleanupPolicy, TerminalRemovalPreservesUnknownHelperCompletionFence) {
  bool allowed = true;
  bool disconnected = false;
  bool verified = false;
  const auto result = policy::terminate_outputs(std::array {"Virtual-1"},
    [&] { allowed = false; return false; },
    [&](const auto &) { disconnected = true; return true; },
    [&](const auto &) { verified = true; return true; }, [&] { return allowed; });
  EXPECT_TRUE(disconnected);
  EXPECT_FALSE(verified);
  EXPECT_FALSE(result.virtual_displays_removed);
}

TEST(LinuxPrivateDisplayCleanupPolicy, UnknownDisconnectCompletionFencesRestoreAndRemainingOutputs) {
  bool allowed = true;
  bool restored = false;
  int disconnected = 0;
  const auto result = policy::terminate_outputs(std::array {"Virtual-1", "Virtual-2"},
    [&] { restored = true; return true; },
    [&](const auto &) { ++disconnected; allowed = false; return false; },
    [&](const auto &) { ADD_FAILURE() << "Must not verify after unknown helper completion"; return true; },
    [&] { return allowed; });
  EXPECT_EQ(disconnected, 1);
  EXPECT_FALSE(restored);
  EXPECT_FALSE(result.virtual_displays_removed);
}

TEST(LinuxPrivateDisplayCleanupPolicy, FailedPreparationPreservesPausedRetentionAndTimeoutPreferences) {
  EXPECT_EQ(policy::failed_preparation_restore_delay(false, false, 0, 5s), 5s);
  EXPECT_EQ(policy::failed_preparation_restore_delay(false, true, 300, 0ms), 0ms);
  EXPECT_EQ(policy::failed_preparation_restore_delay(true, true, 0, 5s), 5s);
  EXPECT_EQ(policy::failed_preparation_restore_delay(true, false, 60, 5s), 60s);
  EXPECT_FALSE(policy::failed_preparation_restore_delay(true, false, 0, 5s));
  EXPECT_FALSE(policy::failed_preparation_restore_delay(true, false, -1, 5s));
}

TEST(LinuxPrivateDisplayCleanupPolicy, NewAdmissionSupersedesWorkerWaitingForLifecycleGate) {
  std::mutex lifecycle;
  std::mutex display;
  std::atomic<std::uint64_t> generation {1};
  std::string output = "old game";
  std::latch worker_started {1};
  std::unique_lock admission {lifecycle};
  auto worker = std::async(std::launch::async, [&] {
    worker_started.count_down();
    return policy::run_delayed_restore(lifecycle, display, generation, 1,
      [] { return false; }, [&] { output.clear(); return true; });
  });
  worker_started.wait();
  {
    std::lock_guard reservation {display};
    generation.fetch_add(1);
    output = "new monitor";
  }
  admission.unlock();
  EXPECT_EQ(worker.get(), policy::result_e::superseded);
  EXPECT_EQ(output, "new monitor");
}

TEST(LinuxPrivateDisplayCleanupPolicy, CancellationBetweenOwnershipCheckAndDisplayLockPreservesNewReservation) {
  std::mutex lifecycle;
  std::mutex display;
  std::atomic<std::uint64_t> generation {1};
  std::string output = "old game";
  std::latch ownership_checked {1};
  std::latch reservation_published {1};
  auto worker = std::async(std::launch::async, [&] {
    return policy::run_delayed_restore(lifecycle, display, generation, 1,
      [&] {
        ownership_checked.count_down();
        reservation_published.wait();
        return false;
      },
      [&] { output.clear(); return true; });
  });
  ownership_checked.wait();
  {
    std::lock_guard reservation {display};
    generation.fetch_add(1);
    output = "new monitor";
  }
  reservation_published.count_down();
  EXPECT_EQ(worker.get(), policy::result_e::superseded);
  EXPECT_EQ(output, "new monitor");
}

TEST(LinuxPrivateDisplayCleanupPolicy, RetainedMonitorAndCaptureActivityPreventRestore) {
  for (const bool capture_active : {false, true}) {
    for (const bool retained_monitor : {false, true}) {
      std::mutex lifecycle;
      std::mutex display;
      std::atomic<std::uint64_t> generation {1};
      bool restored = false;
      const auto result = policy::run_delayed_restore(lifecycle, display, generation, 1,
        [&] { return capture_active || retained_monitor; },
        [&] { restored = true; return true; });
      EXPECT_EQ(restored, !capture_active && !retained_monitor);
      EXPECT_EQ(result, restored ? policy::result_e::restored : policy::result_e::owned);
    }
  }
}

TEST(LinuxPrivateDisplayCleanupPolicy, PausedTimeoutWithNoCaptureOrMonitorRestoresOnce) {
  std::mutex lifecycle;
  std::mutex display;
  std::atomic<std::uint64_t> generation {1};
  int restores = 0;
  auto restore = [&] { ++restores; return true; };
  EXPECT_EQ(policy::run_delayed_restore(lifecycle, display, generation, 1,
              [] { return false; }, restore), policy::result_e::restored);
  EXPECT_EQ(policy::run_delayed_restore(lifecycle, display, generation, 1,
              [] { return false; }, restore), policy::result_e::superseded);
  EXPECT_EQ(restores, 1);
}

TEST(LinuxPrivateDisplayCleanupPolicy, HelperShutdownDoesNotWaitForBlockedLifecycleGate) {
  std::mutex lifecycle, display;
  std::atomic<std::uint64_t> generation {1};
  std::stop_source stop;
  std::unique_lock gate {lifecycle};
  auto worker = std::async(std::launch::async, [&] {
    return policy::run_delayed_restore(lifecycle, display, generation, 1,
      [] { return false; }, [] { ADD_FAILURE() << "cancelled restore ran"; return true; }, stop.get_token());
  });
  stop.request_stop();
  const auto status = worker.wait_for(200ms);
  gate.unlock();
  EXPECT_EQ(status, std::future_status::ready);
  EXPECT_EQ(worker.get(), policy::result_e::superseded);
}

TEST(LinuxPrivateDisplayCleanupPolicy, OperationDeadlineIncludesWaitingForDisplayLock) {
  std::mutex lifecycle, display;
  std::atomic<std::uint64_t> generation {1};
  std::unique_lock gate {display};
  auto worker = std::async(std::launch::async, [&] {
    return policy::run_delayed_restore(lifecycle, display, generation, 1,
      [] { return false; }, [] { ADD_FAILURE() << "expired restore ran"; return true; }, {},
      std::chrono::steady_clock::now() + 30ms);
  });
  const auto status = worker.wait_for(200ms);
  gate.unlock();
  EXPECT_EQ(status, std::future_status::ready);
  EXPECT_EQ(worker.get(), policy::result_e::failed);
  EXPECT_TRUE(lifecycle.try_lock());
  lifecycle.unlock();
}

TEST(LinuxPrivateDisplayCleanupPolicy, ConfirmedTransientFailureRetriesWithFreshClaimAndPreservedBaseline) {
  std::mutex lifecycle, display;
  std::atomic<std::uint64_t> generation {1};
  constexpr std::array delays {0ms, 0ms};
  int attempts = 0;
  bool saved_baseline = true;
  const auto result = policy::run_delayed_restore_with_retries(lifecycle, display, generation, 1,
    [] { return false; }, [&](std::uint64_t claim, auto deadline) {
      EXPECT_EQ(claim, generation.load());
      EXPECT_EQ(claim, static_cast<std::uint64_t>(++attempts + 1));
      EXPECT_GT(deadline, std::chrono::steady_clock::now());
      EXPECT_TRUE(saved_baseline);
      if (attempts < 3) return false;
      saved_baseline = false;
      return true;
    }, [] { return true; }, delays);
  EXPECT_EQ(result, policy::result_e::restored);
  EXPECT_EQ(attempts, 3);
  EXPECT_FALSE(saved_baseline);
  EXPECT_EQ(generation, 4U);
}

TEST(LinuxPrivateDisplayCleanupPolicy, PersistentFailureStopsAfterConfiguredRetries) {
  std::mutex lifecycle, display;
  std::atomic<std::uint64_t> generation {1};
  constexpr std::array delays {0ms, 0ms};
  int attempts = 0;
  EXPECT_EQ(policy::run_delayed_restore_with_retries(lifecycle, display, generation, 1,
              [] { return false; }, [&](auto, auto) { ++attempts; return false; },
              [] { return true; }, delays), policy::result_e::failed);
  EXPECT_EQ(attempts, 3);
  EXPECT_EQ(generation, 4U);
}

TEST(LinuxPrivateDisplayCleanupPolicy, UncertainHelperCompletionFencesAllRetries) {
  std::mutex lifecycle, display;
  std::atomic<std::uint64_t> generation {1};
  constexpr std::array delays {0ms, 0ms};
  int attempts = 0;
  EXPECT_EQ(policy::run_delayed_restore_with_retries(lifecycle, display, generation, 1,
              [] { return false; }, [&](auto, auto) { ++attempts; return false; },
              [] { return false; }, delays), policy::result_e::failed);
  EXPECT_EQ(attempts, 1);
}

TEST(LinuxPrivateDisplayCleanupPolicy, NewAdmissionDuringFailedAttemptCannotBeClaimedByRetry) {
  std::mutex lifecycle, display;
  std::atomic<std::uint64_t> generation {1};
  constexpr std::array delays {0ms, 0ms};
  int attempts = 0;
  EXPECT_EQ(policy::run_delayed_restore_with_retries(lifecycle, display, generation, 1,
              [] { return false; }, [&](auto, auto) {
                ++attempts;
                generation.fetch_add(1);
                return false;
              }, [] { return true; }, delays), policy::result_e::superseded);
  EXPECT_EQ(attempts, 1);
}

TEST(LinuxPrivateDisplayCleanupPolicy, RetainedOwnerBetweenAttemptsPreventsFurtherMutation) {
  std::mutex lifecycle, display;
  std::atomic<std::uint64_t> generation {1};
  constexpr std::array delays {0ms};
  bool owned = false;
  int attempts = 0;
  EXPECT_EQ(policy::run_delayed_restore_with_retries(lifecycle, display, generation, 1,
              [&] { return owned; }, [&](auto, auto) { ++attempts; return false; },
              [&] {
                owned = true;
                // Backoff must release both locks so a new session can enter.
                const bool lifecycle_unlocked = lifecycle.try_lock();
                EXPECT_TRUE(lifecycle_unlocked);
                if (lifecycle_unlocked) lifecycle.unlock();
                const bool display_unlocked = display.try_lock();
                EXPECT_TRUE(display_unlocked);
                if (display_unlocked) display.unlock();
                return true;
              }, delays), policy::result_e::owned);
  EXPECT_EQ(attempts, 1);
  EXPECT_EQ(generation, 2U);
}

TEST(LinuxPrivateDisplayCleanupPolicy, ShutdownInterruptsRetryBackoffPromptly) {
  std::mutex lifecycle, display;
  std::atomic<std::uint64_t> generation {1};
  constexpr std::array delays {std::chrono::milliseconds(1h)};
  std::stop_source stop;
  std::latch backoff {1};
  std::atomic_bool notified {false};
  int attempts = 0;
  auto worker = std::async(std::launch::async, [&] {
    return policy::run_delayed_restore_with_retries(lifecycle, display, generation, 1,
      [] { return false; }, [&](auto, auto) { ++attempts; return false; },
      [&] { if (!notified.exchange(true)) backoff.count_down(); return true; }, delays, stop.get_token());
  });
  backoff.wait();
  stop.request_stop();
  EXPECT_EQ(worker.wait_for(200ms), std::future_status::ready);
  EXPECT_EQ(worker.get(), policy::result_e::superseded);
  EXPECT_EQ(attempts, 1);
}

TEST(LinuxPrivateDisplayCleanupPolicy, ReconnectDuringBackoffCancelsOldRestoreWithoutBlockingAdmission) {
  std::mutex lifecycle, display;
  std::atomic<std::uint64_t> generation {1};
  constexpr std::array delays {std::chrono::milliseconds(1h)};
  std::latch backoff {1};
  std::atomic_bool notified {false};
  int attempts = 0;
  auto worker = std::async(std::launch::async, [&] {
    return policy::run_delayed_restore_with_retries(lifecycle, display, generation, 1,
      [] { return false; }, [&](auto, auto) { ++attempts; return false; },
      [&] { if (!notified.exchange(true)) backoff.count_down(); return true; }, delays);
  });
  backoff.wait();
  {
    std::lock_guard admission {lifecycle};
    std::lock_guard reservation {display};
    generation.fetch_add(1);
  }
  EXPECT_EQ(worker.wait_for(200ms), std::future_status::ready);
  EXPECT_EQ(worker.get(), policy::result_e::superseded);
  EXPECT_EQ(attempts, 1);
}

TEST(LinuxPrivateDisplayCleanupPolicy, SuccessfulRestoreIsNeverReplayedByRetry) {
  std::mutex lifecycle, display;
  std::atomic<std::uint64_t> generation {1};
  constexpr std::array delays {0ms, 0ms};
  int attempts = 0;
  EXPECT_EQ(policy::run_delayed_restore_with_retries(lifecycle, display, generation, 1,
              [] { return false; }, [&](auto, auto) { ++attempts; return true; },
              [] { ADD_FAILURE() << "successful restore entered retry backoff"; return true; }, delays),
            policy::result_e::restored);
  EXPECT_EQ(attempts, 1);
}

TEST(LinuxPrivateDisplayCleanupPolicy, SuccessfulCompletionReconcilesOwnershipWithLifecycleHeldAndDisplayReleased) {
  std::mutex lifecycle, display;
  std::atomic<std::uint64_t> generation {1};
  constexpr std::array delays {0ms};
  int completions = 0;
  EXPECT_EQ(policy::run_delayed_restore_with_retries(lifecycle, display, generation, 1,
              [] { return false; }, [](auto, auto) { return true; },
              [] { return true; }, delays, {}, 1s, [&](const auto claimed_generation) {
                ++completions;
                EXPECT_EQ(claimed_generation, 2U);
                auto probe = std::async(std::launch::async, [&] {
                  const bool lifecycle_available = lifecycle.try_lock();
                  if (lifecycle_available) lifecycle.unlock();
                  const bool display_available = display.try_lock();
                  if (display_available) display.unlock();
                  return std::pair {lifecycle_available, display_available};
                });
                const auto [lifecycle_available, display_available] = probe.get();
                EXPECT_FALSE(lifecycle_available);
                EXPECT_TRUE(display_available);
              }), policy::result_e::restored);
  EXPECT_EQ(completions, 1);
}

TEST(LinuxPrivateDisplayCleanupPolicy, FailedOrSupersededRestorationCannotCommitOwnershipCleanup) {
  for (const bool restored : {false, true}) {
    SCOPED_TRACE(restored);
    std::mutex lifecycle, display;
    std::atomic<std::uint64_t> generation {1};
    constexpr std::array delays {0ms};
    int completions = 0;
    EXPECT_EQ(policy::run_delayed_restore_with_retries(lifecycle, display, generation, 1,
                [] { return false; }, [&](auto, auto) {
                  if (restored) generation.fetch_add(1);
                  return restored;
                }, [] { return false; }, delays, {}, 1s,
                [&](auto) { ++completions; }), restored ? policy::result_e::restored : policy::result_e::failed);
    EXPECT_EQ(completions, 0);
  }
}

TEST(LinuxPrivateDisplayCleanupPolicy, FailedReconnectRearmsCancelledDesktopRestoration) {
  platf::linux_private_display::restore_dispatcher_t dispatcher;
  std::atomic<std::uint64_t> generation {1};
  std::atomic_uint old_calls {0};
  ASSERT_TRUE(dispatcher.submit(1, [&](std::stop_token) { ++old_calls; }, 24h));
  std::promise<void> restored;
  EXPECT_FALSE(policy::prepare_with_restore_on_failure(
    [&] { dispatcher.cancel(generation.fetch_add(1) + 1); },
    [] { return false; },
    [&] {
      EXPECT_TRUE(dispatcher.submit(generation.fetch_add(1) + 1,
        [&](std::stop_token) { restored.set_value(); }));
    }));
  EXPECT_EQ(restored.get_future().wait_for(1s), std::future_status::ready);
  EXPECT_EQ(old_calls, 0U);
}

TEST(LinuxPrivateDisplayCleanupPolicy, SuccessfulReconnectCancelsOldRestoreWithoutQueuingAReplacement) {
  int cancellations = 0, restores = 0;
  EXPECT_TRUE(policy::prepare_with_restore_on_failure(
    [&] { ++cancellations; }, [] { return true; }, [&] { ++restores; }));
  EXPECT_EQ(cancellations, 1);
  EXPECT_EQ(restores, 0);
}

TEST(LinuxPrivateDisplayCleanupPolicy, PreparationExceptionRearmsRestoreAfterDisplayLockIsReleased) {
  std::mutex display;
  bool restored = false;
  EXPECT_THROW(policy::prepare_with_restore_on_failure(
    [] {}, [&]() -> bool {
      std::lock_guard mutation {display};
      throw std::runtime_error("KScreen preparation failed");
    }, [&] {
      const bool unlocked = display.try_lock();
      EXPECT_TRUE(unlocked);
      if (unlocked) display.unlock();
      restored = true;
    }), std::runtime_error);
  EXPECT_TRUE(restored);
}
