// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the production controller backend. Only the Windows USB transport
// boundary is replaced, so controller state, Sony reports, feedback queues,
// capability checks, and callback ownership use their actual implementation.
#include "src/platform/windows/dualsense_usbip_gamepad.h"
#include "src/platform/windows/dualsense_usbip_gamepad_policy.h"
#include "src/platform/windows/dualsense_usbip_transport.h"
#include "third-party/libvirtualgamepad/driver/src/dualsense.h"

#include <array>
#include <atomic>
#include <cstring>
#include <functional>
#include <future>
#include <gtest/gtest.h>
#include <thread>
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

    void speaker(std::span<const std::uint8_t> data) {
      handlers.speaker_pcm(data, handlers.capture_speaker_incarnation());
    }

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
    return {LI_CTYPE_PS, capabilities, 0, false, 123};
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

TEST(DualSenseUsbipSelectionTests, DualSenseUsesUsbAudioWithoutApplicationOptIn) {
  for (const auto setting : {"auto", "vhf", "vhf_ds5"}) {
    const auto profile = platf::vhf_gamepad::select_desired_profile(setting, LI_CTYPE_PS, LI_CCAP_HAPTICS_PCM, true, true);
    EXPECT_TRUE(sessions::select_composite_dualsense(setting, profile, false, true)) << setting;
    EXPECT_FALSE(sessions::select_composite_dualsense(setting, profile, false, false)) << setting;
  }
}

TEST(DualSenseUsbipSelectionTests, ExplicitOtherProfilesAndOtherControllersKeepTheirBackend) {
  for (const auto setting : {"x360", "ds4", "vhf_xbox", "vhf_xbox_one", "vhf_ds4", "vhf_switch"}) {
    const auto profile = platf::vhf_gamepad::select_desired_profile(setting, LI_CTYPE_PS, LI_CCAP_HAPTICS_PCM, true, true);
    EXPECT_FALSE(sessions::select_composite_dualsense(setting, profile, false, true)) << setting;
  }
  for (const auto type : {LI_CTYPE_XBOX, LI_CTYPE_NINTENDO, LI_CTYPE_UNKNOWN}) {
    const auto profile = platf::vhf_gamepad::select_desired_profile("auto", type, 0, true, true);
    EXPECT_FALSE(sessions::select_composite_dualsense("auto", profile, false, true)) << type;
  }
}

TEST(DualSenseUsbipSelectionTests, ExplicitUsbAudioRequestsNeverFallBackWhenTransportIsMissing) {
  EXPECT_TRUE(sessions::select_composite_dualsense("usbip_ds5", platf::vhf_profile_e::automatic, false, false));
  EXPECT_TRUE(sessions::select_composite_dualsense("x360", platf::vhf_profile_e::automatic, true, false));
}

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

TEST_F(DualSenseUsbipGamepadTests, AutomaticDualSenseForwardsPcmWithoutApplicationScope) {
  for (const auto setting : {"auto", "vhf", "vhf_ds5"}) {
    config::input.gamepad = setting;
    platf::usbip_gamepad_t backend;
    auto feedback = queue();
    ASSERT_EQ(backend.alloc({2, 4}, arrival(), feedback), 0);
    ASSERT_FALSE(sessions::enabled());
    fake_transport::current[2]->pcm(samples(45));
    auto messages = drain(feedback);
    ASSERT_EQ(messages.size(), 1) << setting;
    EXPECT_EQ(messages[0].type, platf::gamepad_feedback_e::haptics_pcm);
    EXPECT_EQ(messages[0].id, 4);
    EXPECT_EQ(messages[0].data.haptics.samples, samples(45));
  }
}

TEST_F(DualSenseUsbipGamepadTests, AutomaticDualSenseStillRequiresClientWaveformCapability) {
  platf::usbip_gamepad_t backend;
  auto feedback = queue();
  ASSERT_EQ(backend.alloc({2, 4}, arrival(LI_CCAP_RUMBLE), feedback), 0);
  fake_transport::current[2]->pcm(samples(46));
  EXPECT_TRUE(drain(feedback).empty());
  fake_transport::current[2]->hid(rumble_report());
  auto messages = drain(feedback);
  ASSERT_EQ(messages.size(), 1);
  EXPECT_EQ(messages[0].type, platf::gamepad_feedback_e::rumble);
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

TEST_F(DualSenseUsbipGamepadTests, FinalApplicationScopeClearsOldPcmWithoutDisablingControllerHaptics) {
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
  auto outside_scope = drain(feedback);
  ASSERT_EQ(outside_scope.size(), 1);
  EXPECT_EQ(outside_scope[0].data.haptics.samples, samples(84));

  auto resumed_scope = sessions::start_session();
  const auto fresh = samples(85);
  endpoint->pcm(std::span {fresh}.first(480));
  EXPECT_TRUE(drain(feedback).empty());
  endpoint->pcm(std::span {fresh}.last(480));
  auto messages = drain(feedback);
  ASSERT_EQ(messages.size(), 1);
  EXPECT_EQ(messages[0].data.haptics.samples, fresh);
}

TEST_F(DualSenseUsbipGamepadTests, SpeakerQueueIsBoundedIndependentAndEpochInvalidated) {
  platf::usbip_gamepad_t backend;
  auto feedback = queue();
  ASSERT_EQ(backend.alloc({0, 7}, arrival(LI_CCAP_HAPTICS_PCM | 0x4000), feedback), 0);
  auto endpoint = fake_transport::current[0];
  drain(feedback);
  endpoint->pcm(samples(11));
  for (unsigned i = 0; i < 100; ++i) {
    endpoint->speaker(samples(std::uint8_t(i)));
  }
  auto messages = drain(feedback);
  ASSERT_EQ(messages.size(), 2u);
  EXPECT_EQ(messages[0].type, platf::gamepad_feedback_e::haptics_pcm);
  const auto speaker = messages[1];
  EXPECT_EQ(speaker.type, platf::gamepad_feedback_e::speaker_pcm);
  EXPECT_EQ(speaker.id, 7);
  EXPECT_EQ(speaker.data.speaker.sequence, 99u);
  EXPECT_EQ(speaker.data.speaker.sample_position, 99u * 240u);
  EXPECT_EQ(speaker.data.speaker.samples, samples(99));
  ASSERT_TRUE(controller_speaker_pcm::encode(8, 13, speaker.id, speaker.data.speaker, speaker.speaker_epoch));
  backend.free(0, false);
  // A block popped before teardown must not be emitted by the sender later.
  EXPECT_FALSE(controller_speaker_pcm::encode(8, 13, speaker.id, speaker.data.speaker, speaker.speaker_epoch));
  endpoint->speaker(samples(55));
  EXPECT_TRUE(drain(feedback).empty());
  ASSERT_EQ(backend.alloc({0, 7}, arrival(0x4000), feedback), 0);
  drain(feedback);
  fake_transport::current[0]->speaker(samples(33));
  messages = drain(feedback);
  ASSERT_EQ(messages.size(), 1u);
  EXPECT_GT(messages[0].data.speaker.generation, speaker.data.speaker.generation);
  EXPECT_EQ(messages[0].data.speaker.sequence, 0u);
  EXPECT_EQ(messages[0].data.speaker.sample_position, 0u);
  EXPECT_EQ(messages[0].data.speaker.flags, 1);
}

TEST_F(DualSenseUsbipGamepadTests, SpeakerAliasRejectsOtherControllerTypesAndMalformedFrames) {
  platf::usbip_gamepad_t backend;
  auto feedback = queue();
  auto metadata = arrival(0x4000);
  metadata.type = 4;  // Steam attributes uses the same bit, never speaker PCM.
  ASSERT_EQ(backend.alloc({0, 1}, metadata, feedback), 0);
  drain(feedback);
  fake_transport::current[0]->speaker(samples(1));
  EXPECT_TRUE(drain(feedback).empty());
  backend.free(0, false);
  ASSERT_EQ(backend.alloc({0, 1}, arrival(0x4000), feedback), 0);
  drain(feedback);
  auto endpoint = fake_transport::current[0];
  const auto pcm = samples(7);
  endpoint->speaker(std::span(pcm).first(480));
  endpoint->speaker(std::span(pcm).first(3));
  endpoint->speaker(std::span(pcm).first(480));
  EXPECT_TRUE(drain(feedback).empty());
  endpoint->speaker(std::span(pcm).last(480));
  auto messages = drain(feedback);
  ASSERT_EQ(messages.size(), 1u);
  EXPECT_EQ(messages[0].data.speaker.samples, pcm);
  EXPECT_EQ(messages[0].data.speaker.flags, 1);
}

TEST_F(DualSenseUsbipGamepadTests, FullQueueNeverEvictsOtherFeedbackForSpeaker) {
  platf::usbip_gamepad_t backend;
  auto feedback = queue();
  ASSERT_EQ(backend.alloc({0, 0}, arrival(0x4000), feedback), 0);
  drain(feedback);
  unsigned inserted = 0;
  while (feedback->try_raise(platf::gamepad_feedback_msg_t::make_rumble(8, 1, 2))) {
    ++inserted;
  }
  ASSERT_GT(inserted, 0u);
  fake_transport::current[0]->speaker(samples(1));
  const auto messages = drain(feedback);
  EXPECT_EQ(messages.size(), inserted);
  for (const auto &msg : messages) {
    EXPECT_EQ(msg.type, platf::gamepad_feedback_e::rumble);
  }
  fake_transport::current[0]->speaker(samples(2));
  const auto next = drain(feedback);
  ASSERT_EQ(next.size(), 1u);
  EXPECT_EQ(next[0].data.speaker.sample_position, 240u);
}

TEST_F(DualSenseUsbipGamepadTests, ActuatorTimeoutDoesNotInvalidateFreshSpeaker) {
  platf::usbip_gamepad_t backend;
  auto feedback = queue();
  ASSERT_EQ(backend.alloc({0, 0}, arrival(LI_CCAP_HAPTICS_PCM | 0x4000), feedback), 0);
  auto endpoint = fake_transport::current[0];
  drain(feedback);
  endpoint->pcm(samples(1));
  drain(feedback);
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  endpoint->speaker(samples(2));
  endpoint->pcm(samples(3));
  const auto messages = drain(feedback);
  ASSERT_EQ(messages.size(), 2u);
  EXPECT_EQ(messages[0].type, platf::gamepad_feedback_e::speaker_pcm);
  EXPECT_EQ(messages[1].type, platf::gamepad_feedback_e::haptics_pcm);
  EXPECT_TRUE(controller_speaker_pcm::encode(8, 13, 0, messages[0].data.speaker, messages[0].speaker_epoch));
}

TEST_F(DualSenseUsbipGamepadTests, SpeakerRequiresExplicitOwnerAndEchoesItAcrossReuse) {
  platf::usbip_gamepad_t backend;
  auto feedback = queue();
  auto metadata = arrival(0x4000);
  metadata.speaker_owner = 0;
  ASSERT_EQ(backend.alloc({0, 0}, metadata, feedback), 0);
  drain(feedback);
  fake_transport::current[0]->speaker(samples(1));
  EXPECT_TRUE(drain(feedback).empty());
  backend.free(0, false);
  metadata.speaker_owner = 0x123456789abcdef0ULL;
  ASSERT_EQ(backend.alloc({0, 0}, metadata, feedback), 0);
  drain(feedback);
  fake_transport::current[0]->speaker(samples(2));
  const auto messages = drain(feedback);
  ASSERT_EQ(messages.size(), 1u);
  EXPECT_EQ(messages[0].data.speaker.owner, metadata.speaker_owner);
}

TEST_F(DualSenseUsbipGamepadTests, PendingAudioUrbCannotAcquireRetainedRebindOwner) {
  auto scope = sessions::start_session();
  sessions::set_application_active(true);
  platf::usbip_gamepad_t backend;
  auto feedback = queue();
  auto metadata = arrival(LI_CCAP_HAPTICS_PCM | 0x4000);
  metadata.persist_after_disconnect = true;
  metadata.speaker_owner = 123;
  ASSERT_EQ(backend.alloc({0, 0}, metadata, feedback), 0);
  auto endpoint = fake_transport::current[0];
  drain(feedback);
  // Use the actual USB/IP scheduler and actual backend callbacks together.
  // Only the operating-system transport is replaced by the fixture.
  platf::dualsense_usbip::session protocol(0, endpoint->handlers);
  std::vector<std::uint8_t> import(40, 0);
  import[0] = 1; import[1] = 0x11; import[2] = 0x80; import[3] = 3;
  const auto bus = platf::dualsense_usbip::session::bus_id(0);
  std::copy(bus.begin(), bus.end(), import.begin() + 8);
  ASSERT_TRUE(protocol.feed(import));
  ASSERT_TRUE(protocol.imported());
  protocol.take_output();
  const auto audio = [](unsigned sequence, std::uint8_t sample) {
    std::vector<std::uint8_t> wire(48 + 1920 + 5 * 16, 0);
    const auto put = [&](unsigned offset, std::uint32_t value) {
      for (unsigned i = 0; i < 4; ++i) wire[offset + i] = std::uint8_t(value >> (24 - i * 8));
    };
    put(0, 1); put(4, sequence); put(8, 0x10001); put(16, 1);
    put(24, 1920); put(28, 0xffffffff); put(32, 5);
    for (unsigned i = 0; i < 240; ++i) {
      wire[48 + i * 8] = sample;
      wire[48 + i * 8 + 4] = 42;
    }
    for (unsigned i = 0; i < 5; ++i) {
      put(48 + 1920 + i * 16, i * 384);
      put(48 + 1920 + i * 16 + 4, 384);
    }
    return wire;
  };
  const auto start = std::chrono::steady_clock::now();
  ASSERT_TRUE(protocol.feed(audio(100, 17), start));
  protocol.poll(start);
  ASSERT_TRUE(drain(feedback).empty());
  backend.free(0, true);
  ASSERT_EQ(endpoint->destroyed.load(), 0u);
  metadata.speaker_owner = 456;
  ASSERT_EQ(backend.alloc({0, 0}, metadata, feedback), 0);
  ASSERT_EQ(fake_transport::current[0], endpoint);
  drain(feedback);
  protocol.poll(start + std::chrono::milliseconds(5));
  auto legacy = drain(feedback);
  ASSERT_EQ(legacy.size(), 1u);
  EXPECT_EQ(legacy[0].type, platf::gamepad_feedback_e::haptics_pcm);
  EXPECT_EQ(legacy[0].data.haptics.samples[0], 42u); // Actuator delivery is independent.
  ASSERT_TRUE(protocol.feed(audio(101, 85), start + std::chrono::milliseconds(6)));
  protocol.poll(start + std::chrono::milliseconds(20));
  auto messages = drain(feedback);
  ASSERT_EQ(messages.size(), 2u);
  EXPECT_EQ(messages[0].type, platf::gamepad_feedback_e::haptics_pcm);
  EXPECT_EQ(messages[1].type, platf::gamepad_feedback_e::speaker_pcm);
  EXPECT_EQ(messages[1].data.speaker.owner, 456u);
  EXPECT_EQ(messages[1].data.speaker.samples[0], 85u);
}

TEST_F(DualSenseUsbipGamepadTests, SubmissionIncarnationRejectsSameOwnerRebindAndReallocation) {
  auto scope = sessions::start_session();
  sessions::set_application_active(true);
  for (bool retain : {true, false}) {
    platf::usbip_gamepad_t backend;
    auto feedback = queue();
    auto metadata = arrival(0x4000);
    metadata.persist_after_disconnect = true;
    metadata.speaker_owner = 123;
    ASSERT_EQ(backend.alloc({0, 0}, metadata, feedback), 0);
    auto old = fake_transport::current[0];
    const auto tag = old->handlers.capture_speaker_incarnation();
    ASSERT_NE(tag, 0u);
    backend.free(0, retain);
    ASSERT_EQ(backend.alloc({0, 0}, metadata, feedback), 0);
    auto current = fake_transport::current[0];
    ASSERT_EQ(current == old, retain);
    if (retain) EXPECT_NE(current->handlers.capture_speaker_incarnation(), tag);
    drain(feedback);
    old->handlers.speaker_pcm(samples(17), tag);
    current->handlers.speaker_pcm(samples(17), 0);
    EXPECT_TRUE(drain(feedback).empty());
    current->speaker(samples(85));
    auto messages = drain(feedback);
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0].data.speaker.owner, 123u);
    EXPECT_EQ(messages[0].data.speaker.samples, samples(85));
    backend.free(0, false);
  }
}
