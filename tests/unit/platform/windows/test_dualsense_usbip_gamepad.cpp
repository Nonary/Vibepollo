// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the production controller backend. Only the Windows USB transport
// boundary is replaced, so controller state, Sony reports, feedback queues,
// capability checks, and callback ownership use their actual implementation.
#include "src/platform/windows/dualsense_usbip_gamepad.h"
#include "src/platform/windows/dualsense_usbip_transport.h"
#include "third-party/libvirtualgamepad/driver/src/dualsense.h"

#include <array>
#include <atomic>
#include <cstring>
#include <functional>
#include <future>
#include <gtest/gtest.h>
#include <moonlight-common-c/src/ControllerHaptics.h>
#include <mutex>
#include <vector>

namespace config {
  input_t input {};
}

boost::log::sources::severity_logger<int> verbose;
boost::log::sources::severity_logger<int> debug;
boost::log::sources::severity_logger<int> info;
boost::log::sources::severity_logger<int> warning;
boost::log::sources::severity_logger<int> error;
boost::log::sources::severity_logger<int> fatal;

namespace fake_transport {
  struct endpoint {
    platf::dualsense_usbip::callbacks handlers;
    bool ready = true;
    std::atomic<bool> connected {true};
    std::atomic<unsigned> destroyed {0};
    std::mutex mutex;
    std::vector<std::vector<std::uint8_t>> reports;
    std::function<void()> on_destroy;
    std::function<void()> before_ready;

    lvg::driver::ds5_input_report latest_input() {
      std::lock_guard lock {mutex};
      EXPECT_FALSE(reports.empty());
      lvg::driver::ds5_input_report result {};
      if (!reports.empty()) {
        EXPECT_EQ(reports.back().size(), sizeof(result));
        if (reports.back().size() == sizeof(result)) {
          std::memcpy(&result, reports.back().data(), sizeof(result));
        }
      }
      return result;
    }

    void hid(const lvg::driver::ds5_output_report &report) {
      handlers.hid_output({reinterpret_cast<const std::uint8_t *>(&report), sizeof(report)});
    }

    void pcm(std::span<const std::uint8_t> samples) {
      handlers.haptics_pcm(samples);
    }
  };

  bool installed = true;
  unsigned create_count = 0;
  std::array<bool, platf::MAX_GAMEPADS> create_succeeds;
  std::array<bool, platf::MAX_GAMEPADS> enumeration_succeeds;
  std::array<std::function<void()>, platf::MAX_GAMEPADS> before_ready;
  std::array<std::shared_ptr<endpoint>, platf::MAX_GAMEPADS> current;

  void reset() {
    installed = true;
    create_count = 0;
    create_succeeds.fill(true);
    enumeration_succeeds.fill(true);
    before_ready.fill({});
    current.fill({});
  }
}  // namespace fake_transport

namespace platf::dualsense_usbip {
  struct controller::impl {
    std::shared_ptr<fake_transport::endpoint> endpoint;
  };

  bool available() {
    return fake_transport::installed;
  }

  controller::controller(std::shared_ptr<impl> state):
      state_ {std::move(state)} {
  }

  controller::~controller() {
    auto endpoint = state_->endpoint;
    endpoint->connected = false;
    // A real transport joins callbacks during destruction. Invoking the last
    // callback here detects destruction while the backend's callback mutex is
    // held, and verifies that released slots reject late feedback.
    if (endpoint->on_destroy) {
      endpoint->on_destroy();
    }
    endpoint->destroyed.fetch_add(1);
  }

  bool controller::set_input_report(std::span<const std::uint8_t> report) {
    std::lock_guard lock {state_->endpoint->mutex};
    state_->endpoint->reports.emplace_back(report.begin(), report.end());
    return connected();
  }

  bool controller::connected() const noexcept {
    return state_->endpoint->connected;
  }

  bool controller::wait_until_ready(std::chrono::milliseconds) {
    if (state_->endpoint->before_ready) {
      state_->endpoint->before_ready();
    }
    return state_->endpoint->ready;
  }

  std::unique_ptr<controller> create(std::uint8_t slot, callbacks handlers) {
    ++fake_transport::create_count;
    if (!fake_transport::installed || !fake_transport::create_succeeds.at(slot)) {
      return {};
    }
    auto endpoint = std::make_shared<fake_transport::endpoint>();
    endpoint->handlers = std::move(handlers);
    endpoint->ready = fake_transport::enumeration_succeeds.at(slot);
    endpoint->before_ready = fake_transport::before_ready.at(slot);
    fake_transport::current.at(slot) = endpoint;
    auto state = std::make_shared<controller::impl>();
    state->endpoint = std::move(endpoint);
    return std::unique_ptr<controller> {new controller {std::move(state)}};
  }
}  // namespace platf::dualsense_usbip

namespace {
  using namespace std::chrono_literals;
  namespace sessions = platf::dualsense_usbip_gamepad;

  platf::gamepad_arrival_t arrival(std::uint16_t capabilities = LI_CCAP_HAPTICS_PCM) {
    return {LI_CTYPE_PS, capabilities, 0};
  }

  platf::feedback_queue_t queue(std::uint32_t capacity = 32) {
    return std::make_shared<platf::feedback_queue_t::element_type>(std::make_shared<safe::mail_raw_t>(), capacity);
  }

  std::array<std::uint8_t, 960> samples(std::uint8_t value) {
    std::array<std::uint8_t, 960> result;
    result.fill(value);
    return result;
  }

  std::vector<platf::gamepad_feedback_msg_t> drain(const platf::feedback_queue_t &feedback) {
    std::vector<platf::gamepad_feedback_msg_t> result;
    while (auto message = feedback->pop(0ms)) {
      result.emplace_back(*message);
    }
    return result;
  }

  lvg::driver::ds5_output_report rumble_report(std::uint8_t left = 64, std::uint8_t right = 32) {
    lvg::driver::ds5_output_report report {};
    report.report_id = 2;
    report.valid_flag0 = lvg::driver::k_ds5_flag0_compatible_vibration;
    report.motor_left = left;
    report.motor_right = right;
    return report;
  }

  class DualSenseUsbipGamepadTests: public testing::Test {
  protected:
    void SetUp() override {
      ASSERT_FALSE(sessions::enabled());
      ASSERT_FALSE(sessions::has_ready_controller());
      fake_transport::reset();
      config::input.gamepad = "auto";
    }

    void TearDown() override {
      sessions::set_application_active(false);
      EXPECT_FALSE(sessions::enabled());
      EXPECT_FALSE(sessions::has_ready_controller());
      fake_transport::reset();
    }
  };
}  // namespace

TEST_F(DualSenseUsbipGamepadTests, ScopeRequiresInstalledTransportWithoutCreatingHardware) {
  fake_transport::installed = false;
  EXPECT_FALSE(sessions::start_session());
  EXPECT_FALSE(sessions::enabled());
  EXPECT_EQ(fake_transport::create_count, 0);

  fake_transport::installed = true;
  auto first = sessions::start_session();
  auto second = sessions::start_session();
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_TRUE(sessions::enabled());
  EXPECT_EQ(fake_transport::create_count, 0);
  first.reset();
  EXPECT_TRUE(sessions::enabled());
  second.reset();
  EXPECT_FALSE(sessions::enabled());
}

TEST_F(DualSenseUsbipGamepadTests, ReadinessAndPcmCapabilityBelongToEachArrivingController) {
  auto scope = sessions::start_session();
  platf::usbip_gamepad_t backend;
  auto feedback = queue();
  ASSERT_TRUE(backend.probe());
  ASSERT_EQ(backend.alloc({0, 7}, arrival(LI_CCAP_RUMBLE | LI_CCAP_GYRO), feedback), 0);
  auto incapable = fake_transport::current[0];
  EXPECT_FALSE(sessions::has_ready_controller());
  incapable->pcm(samples(11));
  auto messages = drain(feedback);
  ASSERT_EQ(messages.size(), 1);
  EXPECT_EQ(messages[0].type, platf::gamepad_feedback_e::set_motion_event_state);
  EXPECT_EQ(messages[0].id, 7);
  EXPECT_EQ(messages[0].data.motion_event_state.motion_type, LI_MOTION_TYPE_GYRO);
  EXPECT_EQ(messages[0].data.motion_event_state.report_rate, 100);

  ASSERT_EQ(backend.alloc({3, 2}, arrival(), feedback), 0);
  auto capable = fake_transport::current[3];
  EXPECT_TRUE(sessions::has_ready_controller());
  EXPECT_TRUE(drain(feedback).empty());
  capable->pcm(samples(22));
  messages = drain(feedback);
  ASSERT_EQ(messages.size(), 1);
  EXPECT_EQ(messages[0].type, platf::gamepad_feedback_e::haptics_pcm);
  EXPECT_EQ(messages[0].id, 2);
  EXPECT_EQ(messages[0].data.haptics.samples, samples(22));

  capable->connected = false;
  EXPECT_FALSE(sessions::has_ready_controller());
  incapable->hid(rumble_report());
  messages = drain(feedback);
  ASSERT_EQ(messages.size(), 1);
  EXPECT_EQ(messages[0].id, 7);
  EXPECT_EQ(messages[0].type, platf::gamepad_feedback_e::rumble);
}

TEST_F(DualSenseUsbipGamepadTests, FailedEnumerationDetachesAndAllowsSlotReuse) {
  auto scope = sessions::start_session();
  platf::usbip_gamepad_t backend;
  auto feedback = queue();
  fake_transport::enumeration_succeeds[4] = false;
  EXPECT_EQ(backend.alloc({4, 1}, arrival(), feedback), -1);
  auto failed = fake_transport::current[4];
  ASSERT_TRUE(failed);
  EXPECT_EQ(failed->destroyed.load(), 1u);
  EXPECT_FALSE(sessions::has_ready_controller());
  failed->hid(rumble_report());
  failed->pcm(samples(33));
  EXPECT_TRUE(drain(feedback).empty());

  fake_transport::enumeration_succeeds[4] = true;
  ASSERT_EQ(backend.alloc({4, 1}, arrival(), feedback), 0);
  EXPECT_TRUE(sessions::has_ready_controller());
  EXPECT_NE(fake_transport::current[4], failed);
  EXPECT_EQ(backend.alloc({4, 6}, arrival(), feedback), -1);
  EXPECT_EQ(fake_transport::create_count, 2);
}

TEST_F(DualSenseUsbipGamepadTests, EnumeratingOneControllerDoesNotBlockAnotherControllersInput) {
  auto scope = sessions::start_session();
  platf::usbip_gamepad_t backend;
  auto feedback = queue();
  ASSERT_EQ(backend.alloc({0, 0}, arrival(0), feedback), 0);
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  std::promise<void> resume;
  auto resume_future = resume.get_future().share();
  fake_transport::before_ready[1] = [&] {
    // Audio callbacks can arrive during enumeration. They must not queue
    // waveform data before the same controller's HID/audio readiness passes.
    fake_transport::current[1]->pcm(samples(91));
    entered.set_value();
    resume_future.wait();
  };
  auto allocation = std::async(std::launch::async, [&] {
    return backend.alloc({1, 1}, arrival(), feedback);
  });
  const auto entered_status = entered_future.wait_for(2s);
  EXPECT_TRUE(drain(feedback).empty());
  EXPECT_FALSE(sessions::has_ready_controller());
  auto update = std::async(std::launch::async, [&] {
    backend.update(0, {platf::A, 0, 0, 20000, 0, 0, 0});
  });
  const auto update_status = update.wait_for(1s);
  // Release both workers even if a global-lock regression blocked the update.
  resume.set_value();
  update.get();
  EXPECT_EQ(allocation.get(), 0);
  EXPECT_EQ(entered_status, std::future_status::ready);
  EXPECT_EQ(update_status, std::future_status::ready);
  EXPECT_EQ(fake_transport::current[0]->latest_input().left_x, 206);
  EXPECT_TRUE(sessions::has_ready_controller());
  EXPECT_TRUE(drain(feedback).empty());
}

TEST_F(DualSenseUsbipGamepadTests, RealSonyEncoderCarriesInputTouchMotionAndBattery) {
  platf::usbip_gamepad_t backend;
  auto feedback = queue();
  ASSERT_EQ(backend.alloc({5, 1}, arrival(), feedback), 0);
  auto endpoint = fake_transport::current[5];
  auto report = endpoint->latest_input();
  EXPECT_EQ(report.report_id, 1);
  EXPECT_EQ(report.left_x, 128);
  EXPECT_EQ(report.left_y, 128);
  EXPECT_EQ(report.buttons[0], 8);

  backend.update(5, {platf::DPAD_UP | platf::DPAD_RIGHT | platf::A | platf::START | platf::TOUCHPAD_BUTTON, 17, 255, -32768, 32767, 32767, -32768});
  report = endpoint->latest_input();
  EXPECT_EQ(report.left_x, 0);
  EXPECT_EQ(report.left_y, 0);
  EXPECT_EQ(report.right_x, 255);
  EXPECT_EQ(report.right_y, 255);
  EXPECT_EQ(report.left_trigger, 17);
  EXPECT_EQ(report.right_trigger, 255);
  EXPECT_EQ(report.buttons[0], 0x21);  // Cross + northeast hat.
  EXPECT_EQ(report.buttons[1], 0x2C);  // Options and both digital triggers.
  EXPECT_EQ(report.buttons[2], 0x02);  // Touchpad button.

  backend.touch(5, {{5, 1}, LI_TOUCH_EVENT_DOWN, 41, 1.0f, 1.0f, 1.0f});
  report = endpoint->latest_input();
  EXPECT_EQ(report.touch[0].contact & 0x80, 0);
  EXPECT_EQ(report.touch[0].coordinates[0], 0x7F);  // x = 1919.
  EXPECT_EQ(report.touch[0].coordinates[1], 0x77);  // y = 1079.
  EXPECT_EQ(report.touch[0].coordinates[2], 0x43);
  backend.touch(5, {{5, 1}, LI_TOUCH_EVENT_UP, 41, 0, 0, 0});
  EXPECT_NE(endpoint->latest_input().touch[0].contact & 0x80, 0);

  backend.motion(5, {{5, 1}, LI_MOTION_TYPE_GYRO, 90.0f, -45.0f, 0});
  report = endpoint->latest_input();
  EXPECT_EQ(report.gyro[0], 1440);
  EXPECT_EQ(report.gyro[1], -720);
  backend.battery(5, {{5, 1}, LI_BATTERY_STATE_CHARGING, 70});
  EXPECT_EQ(endpoint->latest_input().status, 0x17);
}

TEST_F(DualSenseUsbipGamepadTests, GlobalSelectionForwardsPcmWithoutApplicationScope) {
  config::input.gamepad = "usbip_ds5";
  platf::usbip_gamepad_t backend;
  auto feedback = queue();
  ASSERT_EQ(backend.alloc({2, 4}, arrival(), feedback), 0);
  ASSERT_FALSE(sessions::enabled());
  fake_transport::current[2]->pcm(samples(44));
  auto messages = drain(feedback);
  ASSERT_EQ(messages.size(), 1);
  EXPECT_EQ(messages[0].id, 4);
  EXPECT_EQ(messages[0].data.haptics.samples, samples(44));
}

TEST_F(DualSenseUsbipGamepadTests, StalledFeedbackQueueDropsOnlyThisControllersOldPcm) {
  auto scope = sessions::start_session();
  platf::usbip_gamepad_t backend;
  auto feedback = queue(3);
  ASSERT_EQ(backend.alloc({6, 2}, arrival(), feedback), 0);
  ASSERT_EQ(backend.alloc({7, 3}, arrival(), feedback), 0);
  feedback->raise(platf::gamepad_feedback_msg_t::make_rgb_led(9, 1, 2, 3));
  fake_transport::current[6]->pcm(samples(51));
  fake_transport::current[7]->pcm(samples(61));
  fake_transport::current[6]->pcm(samples(52));

  auto messages = drain(feedback);
  ASSERT_EQ(messages.size(), 3);
  EXPECT_EQ(messages[0].type, platf::gamepad_feedback_e::set_rgb_led);
  EXPECT_EQ(messages[0].id, 9);
  EXPECT_EQ(messages[1].id, 3);
  EXPECT_EQ(messages[1].data.haptics.samples, samples(61));
  EXPECT_EQ(messages[2].id, 2);
  EXPECT_EQ(messages[2].data.haptics.samples, samples(52));
}

TEST_F(DualSenseUsbipGamepadTests, OutputWaitsForQueueCapacityAndKeepsItsLatestState) {
  platf::usbip_gamepad_t backend;
  auto feedback = queue(1);
  ASSERT_EQ(backend.alloc({8, 5}, arrival(), feedback), 0);
  auto endpoint = fake_transport::current[8];
  feedback->raise(platf::gamepad_feedback_msg_t::make_rgb_led(11, 1, 2, 3));
  auto report = rumble_report(100, 50);
  report.valid_flag1 = lvg::driver::k_ds5_flag1_lightbar;
  report.lightbar_red = 17;
  endpoint->hid(report);
  report.motor_left = 120;
  report.lightbar_red = 29;
  endpoint->hid(report);

  auto messages = drain(feedback);
  ASSERT_EQ(messages.size(), 1);
  EXPECT_EQ(messages[0].id, 11);
  backend.update(8, {});
  messages = drain(feedback);
  ASSERT_EQ(messages.size(), 1);
  EXPECT_EQ(messages[0].id, 5);
  EXPECT_EQ(messages[0].type, platf::gamepad_feedback_e::rumble);
  EXPECT_EQ(messages[0].data.rumble.lowfreq, 120 << 8);
  EXPECT_EQ(messages[0].data.rumble.highfreq, 50 << 8);
  backend.update(8, {});
  messages = drain(feedback);
  ASSERT_EQ(messages.size(), 1);
  EXPECT_EQ(messages[0].type, platf::gamepad_feedback_e::set_rgb_led);
  EXPECT_EQ(messages[0].data.rgb_led.r, 29);
  backend.update(8, {});
  EXPECT_TRUE(drain(feedback).empty());
}

TEST_F(DualSenseUsbipGamepadTests, IndependentTriggerCommandsKeepEnablesAndOrderAcrossQueueStalls) {
  platf::usbip_gamepad_t backend;
  auto feedback = queue(2);
  ASSERT_EQ(backend.alloc({11, 7}, arrival(), feedback), 0);
  auto endpoint = fake_transport::current[11];
  lvg::driver::ds5_output_report report {};
  report.report_id = 2;
  report.valid_flag0 = lvg::driver::k_ds5_flag0_left_trigger_effect;
  report.left_trigger.mode = 0x21;
  report.left_trigger.parameters[7] = 42;
  endpoint->hid(report);
  report.valid_flag0 = lvg::driver::k_ds5_flag0_right_trigger_effect;
  report.left_trigger = {};  // These bytes are invalid in this right-only command.
  report.right_trigger.mode = 0x26;
  report.right_trigger.parameters[0] = 88;
  endpoint->hid(report);
  auto messages = drain(feedback);
  ASSERT_EQ(messages.size(), 2);
  EXPECT_EQ(messages[0].type, platf::gamepad_feedback_e::set_adaptive_triggers);
  EXPECT_EQ(messages[0].id, 7);
  EXPECT_EQ(messages[0].data.adaptive_triggers.event_flags, DS_EFFECT_LEFT_TRIGGER);
  EXPECT_EQ(messages[0].data.adaptive_triggers.type_left, 0x21);
  EXPECT_EQ(messages[0].data.adaptive_triggers.left[7], 42);
  EXPECT_EQ(messages[1].data.adaptive_triggers.event_flags, DS_EFFECT_RIGHT_TRIGGER);
  EXPECT_EQ(messages[1].data.adaptive_triggers.type_right, 0x26);
  EXPECT_EQ(messages[1].data.adaptive_triggers.right[0], 88);
  EXPECT_EQ(messages[1].data.adaptive_triggers.type_left, 0x21);
  EXPECT_EQ(messages[1].data.adaptive_triggers.left[7], 42);

  feedback->raise(platf::gamepad_feedback_msg_t::make_rgb_led(12, 1, 2, 3));
  feedback->raise(platf::gamepad_feedback_msg_t::make_rumble(12, 10, 20));
  report.valid_flag0 = lvg::driver::k_ds5_flag0_left_trigger_effect;
  report.left_trigger.mode = 0x01;
  endpoint->hid(report);
  report.valid_flag0 = lvg::driver::k_ds5_flag0_right_trigger_effect;
  report.right_trigger = {};
  endpoint->hid(report);
  messages = drain(feedback);
  ASSERT_EQ(messages.size(), 2);
  EXPECT_EQ(messages[0].id, 12);
  EXPECT_EQ(messages[1].id, 12);
  backend.update(11, {});
  messages = drain(feedback);
  ASSERT_EQ(messages.size(), 1);
  EXPECT_EQ(messages[0].data.adaptive_triggers.event_flags, DS_EFFECT_LEFT_TRIGGER | DS_EFFECT_RIGHT_TRIGGER);
  EXPECT_EQ(messages[0].data.adaptive_triggers.type_left, 0x01);
  EXPECT_EQ(messages[0].data.adaptive_triggers.type_right, 0);
}

TEST_F(DualSenseUsbipGamepadTests, PausePreservesAudioEndpointAndResumeRebindsFeedback) {
  auto scope = sessions::start_session();
  sessions::set_application_active(true);
  platf::usbip_gamepad_t backend;
  auto old_feedback = queue();
  auto persistent_arrival = arrival();
  persistent_arrival.persist_after_disconnect = true;
  ASSERT_EQ(backend.alloc({0, 0}, persistent_arrival, old_feedback), 0);
  auto endpoint = fake_transport::current[0];
  endpoint->pcm(samples(1));
  backend.free(0);
  EXPECT_TRUE(endpoint->connected);
  EXPECT_EQ(endpoint->destroyed.load(), 0u);
  EXPECT_FALSE(sessions::has_ready_controller());
  endpoint->pcm(samples(2));
  endpoint->hid(rumble_report(90, 45));
  EXPECT_TRUE(drain(old_feedback).empty());

  auto resumed_feedback = queue();
  ASSERT_EQ(backend.alloc({0, 3}, persistent_arrival, resumed_feedback), 0);
  EXPECT_EQ(fake_transport::create_count, 1u);
  EXPECT_EQ(fake_transport::current[0], endpoint);
  auto replay = drain(resumed_feedback);
  ASSERT_FALSE(replay.empty());
  EXPECT_EQ(replay[0].id, 3);
  EXPECT_EQ(replay[0].data.rumble.lowfreq, 90u << 8);
  endpoint->pcm(samples(3));
  auto messages = drain(resumed_feedback);
  ASSERT_EQ(messages.size(), 1);
  EXPECT_EQ(messages[0].id, 3);
  EXPECT_EQ(messages[0].data.haptics.samples, samples(3));
  EXPECT_TRUE(drain(old_feedback).empty());

  backend.free(0);
  sessions::set_application_active(false);
  EXPECT_FALSE(endpoint->connected);
  EXPECT_EQ(endpoint->destroyed.load(), 1u);
  ASSERT_EQ(backend.alloc({0, 0}, arrival(), resumed_feedback), 0);
  EXPECT_EQ(fake_transport::create_count, 2u);
}

TEST_F(DualSenseUsbipGamepadTests, ClientWithoutPersistenceStillRemovesControllerWhileApplicationRuns) {
  sessions::set_application_active(true);
  platf::usbip_gamepad_t backend;
  auto feedback = queue();
  ASSERT_EQ(backend.alloc({0, 0}, arrival(), feedback), 0);
  auto endpoint = fake_transport::current[0];
  backend.free(0);
  EXPECT_FALSE(endpoint->connected);
  EXPECT_EQ(endpoint->destroyed.load(), 1u);
}

TEST_F(DualSenseUsbipGamepadTests, ResumeRefreshesCapabilitiesAndCanDisablePersistence) {
  sessions::set_application_active(true);
  platf::usbip_gamepad_t backend;
  auto feedback = queue();
  auto metadata = arrival();
  metadata.persist_after_disconnect = true;
  ASSERT_EQ(backend.alloc({0, 0}, metadata, feedback), 0);
  auto endpoint = fake_transport::current[0];
  backend.free(0);
  ASSERT_EQ(backend.alloc({0, 1}, arrival(0), feedback), 0);
  drain(feedback);
  endpoint->pcm(samples(4));
  EXPECT_TRUE(drain(feedback).empty());
  backend.free(0);
  EXPECT_EQ(endpoint->destroyed.load(), 1u);
}

TEST_F(DualSenseUsbipGamepadTests, TeardownRejectsInFlightCallbacksAndCannotReachReusedSlot) {
  auto scope = sessions::start_session();
  platf::usbip_gamepad_t backend;
  auto feedback = queue();
  ASSERT_EQ(backend.alloc({9, 6}, arrival(), feedback), 0);
  auto old = fake_transport::current[9];
  old->pcm(samples(71));
  auto first = drain(feedback);
  ASSERT_EQ(first.size(), 1);
  const auto old_sequence = first[0].data.haptics.sequence;
  old->pcm(samples(72));
  feedback->raise(platf::gamepad_feedback_msg_t::make_rumble(6, 55, 66));
  old->on_destroy = [old] {
    old->hid(rumble_report());
    old->pcm(samples(73));
  };
  backend.free(9);
  // Release the test callback's own reference after synchronous delivery.
  old->on_destroy = {};
  EXPECT_EQ(old->destroyed.load(), 1u);
  EXPECT_FALSE(sessions::has_ready_controller());
  auto messages = drain(feedback);
  ASSERT_EQ(messages.size(), 1);
  EXPECT_EQ(messages[0].type, platf::gamepad_feedback_e::rumble);
  EXPECT_EQ(messages[0].data.rumble.lowfreq, 55);

  ASSERT_EQ(backend.alloc({9, 6}, arrival(), feedback), 0);
  auto replacement = fake_transport::current[9];
  ASSERT_NE(replacement, old);
  old->hid(rumble_report(255, 255));
  old->pcm(samples(74));
  EXPECT_TRUE(drain(feedback).empty());
  replacement->pcm(samples(75));
  messages = drain(feedback);
  ASSERT_EQ(messages.size(), 1);
  EXPECT_EQ(messages[0].id, 6);
  EXPECT_GT(messages[0].data.haptics.sequence, old_sequence);
  EXPECT_EQ(messages[0].data.haptics.samples, samples(75));
}

TEST_F(DualSenseUsbipGamepadTests, FinalApplicationScopeRemovesQueuedAndPartialPcm) {
  auto scope = sessions::start_session();
  auto other_scope = sessions::start_session();
  platf::usbip_gamepad_t backend;
  auto feedback = queue();
  ASSERT_EQ(backend.alloc({10, 0}, arrival(), feedback), 0);
  auto endpoint = fake_transport::current[10];
  endpoint->pcm(samples(81));
  scope.reset();
  EXPECT_TRUE(sessions::enabled());
  EXPECT_EQ(drain(feedback).size(), 1);
  endpoint->pcm(samples(82));
  const auto partial = samples(83);
  endpoint->pcm(std::span {partial}.first(480));
  other_scope.reset();
  EXPECT_FALSE(sessions::enabled());
  EXPECT_TRUE(drain(feedback).empty());
  endpoint->pcm(samples(84));
  EXPECT_TRUE(drain(feedback).empty());

  auto resumed_scope = sessions::start_session();
  const auto fresh = samples(85);
  endpoint->pcm(std::span {fresh}.first(480));
  EXPECT_TRUE(drain(feedback).empty());
  endpoint->pcm(std::span {fresh}.last(480));
  auto messages = drain(feedback);
  ASSERT_EQ(messages.size(), 1);
  EXPECT_EQ(messages[0].data.haptics.samples, fresh);
}
