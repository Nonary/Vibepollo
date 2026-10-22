#include "../tests_common.h"
#include "src/capture_recovery.h"

TEST(CaptureRecovery, RequiresANewInstanceOfTheExactOutput) {
  using namespace video::capture_recovery;
  struct Device {
    std::string m_display_name;
    std::string m_device_id;
  };

  video::capture_recovery::observations_t state;
  const auto old = state.begin_capture();
  const auto watch = state.watch({"virtual-1"});
  EXPECT_FALSE(state.requires_identity_lookup(old));
  EXPECT_FALSE(watch->frame_observed.load());
  const auto next = state.begin_capture();
  observe_frame_from_current_device_list(
    state,
    "DISPLAY15",
    next,
    [] { return std::optional {std::vector<Device> {{"DISPLAY15", "physical"}}}; },
    [](const auto &device) -> const std::string & { return device.m_display_name; },
    [](const auto &device) -> const std::string & { return device.m_device_id; }
  );
  EXPECT_FALSE(watch->frame_observed.load());
  EXPECT_FALSE(state.requires_identity_lookup(next));
  const auto retried = state.begin_capture();
  observe_frame_from_current_device_list(
    state,
    "display15",
    retried,
    [] { return std::optional {std::vector<Device> {{"DISPLAY15", "VIRTUAL-1"}}}; },
    [](const auto &device) -> const std::string & { return device.m_display_name; },
    [](const auto &device) -> const std::string & { return device.m_device_id; }
  );
  EXPECT_TRUE(watch->frame_observed.load());
}

TEST(CaptureRecovery, RetriesTransientIdentityLookupFailureForTheSameCapture) {
  using namespace video::capture_recovery;
  using namespace std::chrono_literals;
  struct Device {
    std::string m_display_name;
    std::string m_device_id;
  };

  observations_t state;
  const auto watch = state.watch({"virtual-1"});
  const auto instance = state.begin_capture();
  const auto started = std::chrono::steady_clock::now();
  int queries = 0;
  bool enumeration_available = false;
  std::vector<Device> devices;
  const auto observe = [&](std::chrono::steady_clock::time_point now) {
    observe_frame_from_current_device_list(
      state,
      "DISPLAY15",
      instance,
      [&]() -> std::optional<std::vector<Device>> {
        ++queries;
        if (enumeration_available) {
          return devices;
        }
        return std::nullopt;
      },
      [](const auto &device) -> const std::string & { return device.m_display_name; },
      [](const auto &device) -> const std::string & { return device.m_device_id; },
      now
    );
  };

  observe(started);
  EXPECT_EQ(queries, 1);
  EXPECT_FALSE(watch->frame_observed.load());
  enumeration_available = true;
  devices = {{"DISPLAY15", "virtual-1"}};
  observe(started + 499ms);
  EXPECT_EQ(queries, 1);
  EXPECT_FALSE(watch->frame_observed.load());
  observe(started + 500ms);
  EXPECT_EQ(queries, 2);
  EXPECT_TRUE(watch->frame_observed.load());
}

TEST(CaptureRecovery, ConcurrentClientsAndRetiredObservationsStayIndependent) {
  video::capture_recovery::observations_t state;
  auto first = state.watch({"device1"});
  const auto second = state.watch({"device2"});
  first.reset();
  const auto next = state.begin_capture();
  state.frame("device1", next);
  EXPECT_FALSE(second->frame_observed.load());
  state.frame("device2", next);
  EXPECT_TRUE(second->frame_observed.load());
  const auto replacement = state.watch({"device2"});
  state.frame("device2", next);
  EXPECT_FALSE(replacement->frame_observed.load());
}

TEST(CaptureRecovery, ActiveTargetWithoutFramesRemainsPendingAfterBoundedRetries) {
  using namespace video::capture_recovery;
  using namespace std::chrono_literals;
  observations_t state;
  confirmation_t confirmation;
  const auto now = confirmation_t::clock::now();
  const auto watch = state.watch({"virtual1"});
  confirmation.start(watch, now);
  EXPECT_EQ(confirmation.poll(now + 4s), confirmation_status::waiting);
  EXPECT_EQ(confirmation.poll(now + 5s), confirmation_status::retry);
  EXPECT_EQ(confirmation.poll(now + 10s), confirmation_status::retry);
  EXPECT_EQ(confirmation.poll(now + 15s), confirmation_status::stalled);
  EXPECT_TRUE(confirmation.pending());
  EXPECT_EQ(confirmation.poll(now + 30s), confirmation_status::waiting);
  state.frame("virtual1", state.begin_capture());
  EXPECT_EQ(confirmation.poll(now + 31s), confirmation_status::confirmed);
  EXPECT_FALSE(confirmation.pending());
}

TEST(CaptureRecovery, CancellationDropsPendingObservationBeforeAnotherSession) {
  using namespace video::capture_recovery;
  observations_t state;
  confirmation_t confirmation;
  confirmation.start(state.watch({"virtual1"}), confirmation_t::clock::now());
  confirmation.reset();
  EXPECT_FALSE(confirmation.pending());
  const auto old_capture = state.begin_capture();
  confirmation.start(state.watch({"virtual1"}), confirmation_t::clock::now());
  state.frame("virtual1", old_capture);
  EXPECT_EQ(confirmation.poll(confirmation_t::clock::now()), confirmation_status::waiting);
}
