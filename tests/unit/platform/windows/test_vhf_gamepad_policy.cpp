/**
 * @file tests/unit/platform/windows/test_vhf_gamepad_policy.cpp
 * @brief Test the translation between normalized gamepad state and the VHF driver protocol.
 */
#include "../../../tests_common.h"

#include <cmath>
#include <cstring>
#include <limits>

extern "C" {
#include <moonlight-common-c/src/Limelight.h>
}

#include <src/platform/common.h>
#include <src/platform/windows/vhf_gamepad_policy.h>
#include <src/thread_safe.h>

namespace {

  using platf::gamepad_feedback_e;
  using platf::gamepad_feedback_msg_t;
  using platf::feedback_queue_t;
  using platf::vhf_gamepad::backend_e;
  using platf::vhf_gamepad::decode_rumble_rgb;
  using platf::vhf_gamepad::dispatch_rumble_poll;
  using platf::vhf_gamepad::make_input_state;
  using platf::vhf_gamepad::normalized_state_t;
  using platf::vhf_gamepad::observe_rumble_poll;
  using platf::vhf_gamepad::rumble_poll_outcome_e;
  using platf::vhf_gamepad::rumble_poll_summary_t;
  using platf::vhf_gamepad::rumble_rgb_t;
  using platf::vhf_gamepad::record_rumble_poll_summary;
  using platf::vhf_gamepad::record_rumble_poll_error;
  using platf::vhf_gamepad::clear_rumble_poll_summary;
  using platf::vhf_gamepad::select_automatic_backend;
  using platf::vhf_gamepad::select_automatic_profile;
  using platf::vhf_gamepad::select_desired_profile;
  using platf::vhf_gamepad::supported_button_mask;
  using platf::vhf_gamepad::to_milli_units;
  using platf::vhf_gamepad::to_normalized_touch;
  using platf::vhf_gamepad::to_protocol_battery_state;
  using platf::vhf_gamepad::to_protocol_motion_kind;
  using platf::vhf_gamepad::to_protocol_touch_event;
  using platf::vhf_profile_e;

  lvg::feedback_event make_rumble_event(
    const lvg::generic_rumble_rgb_feedback &payload,
    const lvg::feedback_type type = lvg::feedback_type::generic_rumble_rgb) {
    lvg::feedback_event event {};
    event.header.size = sizeof(event);
    event.header.version = lvg::k_protocol_version;
    event.controller_id = 3;
    event.type = type;
    event.payload_size = sizeof(payload);
    std::memcpy(event.payload, &payload, sizeof(payload));
    return event;
  }

  class VhfGamepadPolicyTest: public testing::Test {};

  TEST_F(VhfGamepadPolicyTest, AutomaticBackendFallsBackToVhfWhenVigemIsUnavailable) {
    EXPECT_EQ(select_automatic_backend(true, true), backend_e::vigem);
    EXPECT_EQ(select_automatic_backend(true, false), backend_e::vigem);
    EXPECT_EQ(select_automatic_backend(false, true), backend_e::vhf);
    EXPECT_EQ(select_automatic_backend(false, false), backend_e::unavailable);
  }

  TEST_F(VhfGamepadPolicyTest, AutomaticProfilePrefersXinputThenPlaystation) {
    const auto all_public =
      lvg::profile_bit(lvg::profile::xbox_series) |
      lvg::profile_bit(lvg::profile::dualsense) |
      lvg::profile_bit(lvg::profile::dualshock_4);
    EXPECT_EQ(select_automatic_profile(all_public), lvg::profile::xbox_series);
    EXPECT_EQ(
      select_automatic_profile(all_public & ~lvg::profile_bit(lvg::profile::xbox_series)),
      lvg::profile::dualsense);
    EXPECT_EQ(
      select_automatic_profile(lvg::profile_bit(lvg::profile::dualshock_4)),
      lvg::profile::dualshock_4);
  }

  TEST_F(VhfGamepadPolicyTest, AutomaticProfileNeverFallsBackToReservedGenericEnums) {
    const auto reserved_generics =
      lvg::profile_bit(lvg::profile::generic_pid) |
      lvg::profile_bit(lvg::profile::generic_hid);

    EXPECT_FALSE(select_automatic_profile(reserved_generics).has_value());
  }

  TEST_F(VhfGamepadPolicyTest, XboxClientTypeWinsOverMotionAndTouchpadPreferences) {
    // The Ally reports Xbox with motion sensors. Its extra capabilities must not change the
    // controller family under plain VHF or the Automatic setting's VHF fallback.
    for (const auto setting : {"vhf", "auto"}) {
      for (const auto capabilities : {0, LI_CCAP_ACCEL, LI_CCAP_GYRO, LI_CCAP_TOUCHPAD, LI_CCAP_ACCEL | LI_CCAP_GYRO | LI_CCAP_TOUCHPAD}) {
        SCOPED_TRACE(setting);
        SCOPED_TRACE(capabilities);
        EXPECT_EQ(select_desired_profile(setting, LI_CTYPE_XBOX, capabilities, true, true), vhf_profile_e::automatic);
      }
    }
  }

  TEST_F(VhfGamepadPolicyTest, XboxClientUsesTheXinputAutomaticProfile) {
    const auto desired = select_desired_profile("vhf", LI_CTYPE_XBOX, LI_CCAP_GYRO, true, true);
    ASSERT_EQ(desired, vhf_profile_e::automatic);
    const auto profiles = lvg::profile_bit(lvg::profile::xbox_series) | lvg::profile_bit(lvg::profile::dualsense);
    EXPECT_EQ(select_automatic_profile(profiles), lvg::profile::xbox_series);
  }

  TEST_F(VhfGamepadPolicyTest, KnownClientTypesSelectTheirConsoleProfiles) {
    for (const auto setting : {"vhf", "auto"}) {
      for (const bool preferences : {false, true}) {
        SCOPED_TRACE(setting);
        SCOPED_TRACE(preferences);
        EXPECT_EQ(select_desired_profile(setting, LI_CTYPE_PS, 0, preferences, preferences), vhf_profile_e::dualsense);
        EXPECT_EQ(select_desired_profile(setting, LI_CTYPE_NINTENDO, LI_CCAP_GYRO | LI_CCAP_TOUCHPAD, preferences, preferences), vhf_profile_e::switch_pro);
      }
    }
  }

  TEST_F(VhfGamepadPolicyTest, AutomaticPlaystationTouchSurvivesDisabledSelectionHeuristics) {
    for (const auto setting : {"vhf", "auto"}) {
      for (const bool motion : {false, true}) {
        for (const bool touchpad : {false, true}) {
          SCOPED_TRACE(setting);
          EXPECT_EQ(select_desired_profile(setting, LI_CTYPE_PS, 0, motion, touchpad), vhf_profile_e::dualsense);
          EXPECT_TRUE(platf::vhf_gamepad::supports_controller_touch(setting, false));
        }
      }
    }
  }

  TEST_F(VhfGamepadPolicyTest, FixedTouchlessProfilesDoNotAdvertiseControllerTouch) {
    for (const auto setting : {"x360", "vhf_xbox", "vhf_xbox_one", "vhf_switch"}) {
      SCOPED_TRACE(setting);
      EXPECT_FALSE(platf::vhf_gamepad::supports_controller_touch(setting, false));
      EXPECT_TRUE(platf::vhf_gamepad::supports_controller_touch(setting, true));
    }
  }

  TEST_F(VhfGamepadPolicyTest, ExplicitPlaystationProfilesAdvertiseControllerTouch) {
    for (const auto setting : {"ds4", "vhf_ds4", "vhf_ds5", "usbip_ds5"}) {
      SCOPED_TRACE(setting);
      EXPECT_TRUE(platf::vhf_gamepad::supports_controller_touch(setting, false));
    }
  }

  TEST_F(VhfGamepadPolicyTest, UnknownClientUsesEnabledMotionPreference) {
    for (const auto capabilities : {LI_CCAP_ACCEL, LI_CCAP_GYRO}) {
      SCOPED_TRACE(capabilities);
      EXPECT_EQ(select_desired_profile("vhf", LI_CTYPE_UNKNOWN, capabilities, true, false), vhf_profile_e::dualsense);
      EXPECT_EQ(select_desired_profile("vhf", LI_CTYPE_UNKNOWN, capabilities, false, true), vhf_profile_e::automatic);
    }
  }

  TEST_F(VhfGamepadPolicyTest, UnknownClientUsesEnabledTouchpadPreference) {
    EXPECT_EQ(select_desired_profile("vhf", LI_CTYPE_UNKNOWN, LI_CCAP_TOUCHPAD, false, true), vhf_profile_e::dualsense);
    EXPECT_EQ(select_desired_profile("vhf", LI_CTYPE_UNKNOWN, LI_CCAP_TOUCHPAD, true, false), vhf_profile_e::automatic);
    EXPECT_EQ(select_desired_profile("vhf", LI_CTYPE_UNKNOWN, 0, true, true), vhf_profile_e::automatic);
  }

  TEST_F(VhfGamepadPolicyTest, ExplicitProfilesOverrideClientMetadata) {
    const std::pair<std::string_view, vhf_profile_e> overrides[] {
      {"vhf_xbox", vhf_profile_e::xbox_series},
      {"vhf_xbox_one", vhf_profile_e::xbox_one},
      {"vhf_ds4", vhf_profile_e::dualshock4},
      {"vhf_ds5", vhf_profile_e::dualsense},
      {"vhf_switch", vhf_profile_e::switch_pro}
    };
    for (const auto &[setting, expected] : overrides) {
      for (const auto type : {LI_CTYPE_XBOX, LI_CTYPE_PS, LI_CTYPE_NINTENDO, LI_CTYPE_UNKNOWN}) {
        SCOPED_TRACE(setting);
        SCOPED_TRACE(type);
        EXPECT_EQ(select_desired_profile(setting, type, LI_CCAP_ACCEL | LI_CCAP_GYRO | LI_CCAP_TOUCHPAD, true, true), expected);
      }
    }
  }

  TEST_F(VhfGamepadPolicyTest, InputStateCarriesAValidProtocolHeader) {
    const auto request = make_input_state(5, normalized_state_t {});

    EXPECT_EQ(request.header.size, sizeof(lvg::input_state_request));
    EXPECT_EQ(request.header.version, lvg::k_protocol_version);
    EXPECT_EQ(request.header.reserved, 0);
    EXPECT_EQ(request.reserved, 0);
    EXPECT_EQ(request.controller_id, 5u);

    // The driver and client both reject a request that fails this predicate.
    EXPECT_TRUE(lvg::valid_request(&request, sizeof(request)));
  }

  TEST_F(VhfGamepadPolicyTest, ButtonsAreCopiedWithoutRemapping) {
    normalized_state_t state {};
    state.button_flags = supported_button_mask;

    EXPECT_EQ(make_input_state(0, state).buttons, supported_button_mask);
  }

  TEST_F(VhfGamepadPolicyTest, UnknownButtonBitsAreDropped) {
    normalized_state_t state {};
    state.button_flags = lvg::button_mask::south | 0x00000800u | 0x80000000u;

    EXPECT_EQ(make_input_state(0, state).buttons, static_cast<std::uint32_t>(lvg::button_mask::south));
  }

  TEST_F(VhfGamepadPolicyTest, HorizontalAxesAndTriggersPassThrough) {
    normalized_state_t state {};
    state.left_x = -12000;
    state.right_x = 24000;
    state.left_trigger = 17;
    state.right_trigger = 255;

    const auto request = make_input_state(0, state);
    EXPECT_EQ(request.left_x, -12000);
    EXPECT_EQ(request.right_x, 24000);
    EXPECT_EQ(request.left_trigger, 17);
    EXPECT_EQ(request.right_trigger, 255);
  }

  TEST_F(VhfGamepadPolicyTest, VerticalAxesGoOverTheWireUnchanged) {
    // The wire contract is Vibepollo's normalized state, positive-up. Each driver profile
    // converts to its own device's convention, so converting here too inverted both sticks.
    normalized_state_t state {};
    state.left_y = 20000;
    state.right_y = -8000;

    const auto request = make_input_state(0, state);
    EXPECT_EQ(request.left_y, 20000);
    EXPECT_EQ(request.right_y, -8000);
  }

  TEST_F(VhfGamepadPolicyTest, ExtremeAxisValuesSurviveUnchanged) {
    normalized_state_t state {};
    state.left_y = std::numeric_limits<std::int16_t>::min();
    state.right_y = std::numeric_limits<std::int16_t>::max();

    const auto request = make_input_state(0, state);
    EXPECT_EQ(request.left_y, std::numeric_limits<std::int16_t>::min());
    EXPECT_EQ(request.right_y, std::numeric_limits<std::int16_t>::max());
  }

  TEST_F(VhfGamepadPolicyTest, RumbleAndRgbFeedbackIsDecoded) {
    const lvg::generic_rumble_rgb_feedback payload {0xAB00, 0x1200, 0x10, 0x20, 0x30, 0};

    rumble_rgb_t feedback {};
    ASSERT_TRUE(decode_rumble_rgb(make_rumble_event(payload), feedback));
    EXPECT_EQ(feedback.low_frequency, 0xAB00);
    EXPECT_EQ(feedback.high_frequency, 0x1200);
    EXPECT_EQ(feedback.red, 0x10);
    EXPECT_EQ(feedback.green, 0x20);
    EXPECT_EQ(feedback.blue, 0x30);
  }

  TEST_F(VhfGamepadPolicyTest, RumbleAndRgbFeedbackReportsAnLed) {
    const lvg::generic_rumble_rgb_feedback payload {1, 2, 3, 4, 5, 0};

    rumble_rgb_t feedback {};
    ASSERT_TRUE(decode_rumble_rgb(make_rumble_event(payload), feedback));
    EXPECT_TRUE(feedback.has_rgb);
  }

  TEST_F(VhfGamepadPolicyTest, ForceFeedbackRumbleCarriesNoLed) {
    // A DirectInput effect says nothing about a light. If this reported an LED, the zeroed
    // colour channels would switch off the light on the client's real controller.
    const lvg::generic_rumble_rgb_feedback payload {0x4000, 0x8000, 0, 0, 0, 0};

    rumble_rgb_t feedback {};
    ASSERT_TRUE(decode_rumble_rgb(
      make_rumble_event(payload, lvg::feedback_type::generic_rumble), feedback));
    EXPECT_EQ(feedback.low_frequency, 0x4000);
    EXPECT_EQ(feedback.high_frequency, 0x8000);
    EXPECT_FALSE(feedback.has_rgb);
  }

  TEST_F(VhfGamepadPolicyTest, ForceFeedbackRumbleIgnoresStrayColourBytes) {
    // The payload struct still has colour channels; a rumble-only event must not surface them
    // whatever they happen to contain.
    const lvg::generic_rumble_rgb_feedback payload {1, 2, 0xAA, 0xBB, 0xCC, 0};

    rumble_rgb_t feedback {};
    ASSERT_TRUE(decode_rumble_rgb(
      make_rumble_event(payload, lvg::feedback_type::generic_rumble), feedback));
    EXPECT_FALSE(feedback.has_rgb);
    EXPECT_EQ(feedback.red, 0);
    EXPECT_EQ(feedback.green, 0);
    EXPECT_EQ(feedback.blue, 0);
  }

  TEST_F(VhfGamepadPolicyTest, XboxRumbleCarriesAllFourActuators) {
    // The driver keeps one pending feedback slot, so body and trigger rumble arrive together;
    // splitting them across events would let coalescing drop one.
    const lvg::xbox_rumble_feedback payload {0x1000, 0x2000, 0x3000, 0x4000};

    lvg::feedback_event event {};
    event.header.size = sizeof(event);
    event.header.version = lvg::k_protocol_version;
    event.controller_id = 3;
    event.type = lvg::feedback_type::xbox_rumble;
    event.payload_size = sizeof(payload);
    std::memcpy(event.payload, &payload, sizeof(payload));

    rumble_rgb_t feedback {};
    ASSERT_TRUE(decode_rumble_rgb(event, feedback));
    EXPECT_EQ(feedback.low_frequency, 0x1000);
    EXPECT_EQ(feedback.high_frequency, 0x2000);
    EXPECT_EQ(feedback.left_trigger, 0x3000);
    EXPECT_EQ(feedback.right_trigger, 0x4000);
    EXPECT_TRUE(feedback.has_triggers);
    EXPECT_FALSE(feedback.has_rgb);
  }

  TEST_F(VhfGamepadPolicyTest, XboxRumbleWithAWrongSizedPayloadIsRejected) {
    lvg::feedback_event event {};
    event.header.size = sizeof(event);
    event.header.version = lvg::k_protocol_version;
    event.controller_id = 3;
    event.type = lvg::feedback_type::xbox_rumble;
    event.payload_size = sizeof(lvg::xbox_rumble_feedback) - 1;

    rumble_rgb_t feedback {};
    EXPECT_FALSE(decode_rumble_rgb(event, feedback));
  }

  TEST_F(VhfGamepadPolicyTest, GenericFeedbackReportsNoTriggerRumble) {
    const lvg::generic_rumble_rgb_feedback payload {1, 2, 3, 4, 5, 0};

    rumble_rgb_t feedback {};
    ASSERT_TRUE(decode_rumble_rgb(make_rumble_event(payload), feedback));
    EXPECT_FALSE(feedback.has_triggers);
    EXPECT_EQ(feedback.left_trigger, 0);
    EXPECT_EQ(feedback.right_trigger, 0);
  }

  TEST_F(VhfGamepadPolicyTest, PlaystationOutputCarriesRumbleLightbarAndTriggers) {
    lvg::playstation_output_feedback payload {};
    payload.low_frequency = 0x1100;
    payload.high_frequency = 0x2200;
    payload.red = 0xAA;
    payload.green = 0xBB;
    payload.blue = 0xCC;
    payload.valid = lvg::ps_output_lightbar_valid | lvg::ps_output_triggers_valid;
    payload.left_trigger.mode = static_cast<std::uint8_t>(lvg::trigger_effect_mode::weapon);
    payload.left_trigger.parameters[0] = 0x42;
    payload.right_trigger.mode = static_cast<std::uint8_t>(lvg::trigger_effect_mode::feedback);

    lvg::feedback_event event {};
    event.header.size = sizeof(event);
    event.header.version = lvg::k_protocol_version;
    event.controller_id = 1;
    event.type = lvg::feedback_type::playstation_output;
    event.payload_size = sizeof(payload);
    std::memcpy(event.payload, &payload, sizeof(payload));

    rumble_rgb_t feedback {};
    ASSERT_TRUE(decode_rumble_rgb(event, feedback));
    EXPECT_EQ(feedback.low_frequency, 0x1100);
    EXPECT_EQ(feedback.high_frequency, 0x2200);
    EXPECT_TRUE(feedback.has_rgb);
    EXPECT_EQ(feedback.red, 0xAA);
    EXPECT_TRUE(feedback.has_trigger_effects);
    // The client masks these bits when enabling the physical trigger programs.
    EXPECT_EQ(feedback.trigger_event_flags & DS_EFFECT_LEFT_TRIGGER, DS_EFFECT_LEFT_TRIGGER);
    EXPECT_EQ(feedback.trigger_event_flags & DS_EFFECT_RIGHT_TRIGGER, DS_EFFECT_RIGHT_TRIGGER);
    EXPECT_EQ(feedback.trigger_event_flags, 0x0C);
    EXPECT_EQ(feedback.left_effect.mode, static_cast<std::uint8_t>(lvg::trigger_effect_mode::weapon));
    EXPECT_EQ(feedback.left_effect.parameters[0], 0x42);
    EXPECT_EQ(feedback.right_effect.mode, static_cast<std::uint8_t>(lvg::trigger_effect_mode::feedback));
    // A PlayStation pad has no impulse triggers, so the Xbox rumble fields stay clear.
    EXPECT_FALSE(feedback.has_triggers);
  }

  TEST_F(VhfGamepadPolicyTest, RumblePollObservationDrivesTheOrdinaryRumbleQueue) {
    lvg::playstation_output_feedback payload {};

    lvg::feedback_event event {};
    event.header.size = sizeof(event);
    event.header.version = lvg::k_protocol_version;
    event.controller_id = 7;  // This driver handle is not part of the observation.
    event.type = lvg::feedback_type::playstation_output;
    event.payload_size = sizeof(payload);
    std::memcpy(event.payload, &payload, sizeof(payload));

    rumble_rgb_t feedback {};
    ASSERT_TRUE(decode_rumble_rgb(event, feedback));
    EXPECT_EQ(feedback.low_frequency, 0);
    EXPECT_EQ(feedback.high_frequency, 0);

    auto feedback_mail = std::make_shared<safe::mail_raw_t>();
    feedback_queue_t queue = feedback_mail->queue<gamepad_feedback_msg_t>("vhf-policy-rumble");
    const auto make_rumble = [](const auto client_index, const auto low, const auto high) {
      return gamepad_feedback_msg_t::make_rumble(client_index, low, high);
    };

    // The first decoded sample queues the baseline even when both motors are zero.
    const auto initial_zero = observe_rumble_poll(event, 3, 1, feedback, true, false, {});
    EXPECT_EQ(initial_zero.driver_slot, 3u);
    EXPECT_EQ(initial_zero.event_type, static_cast<std::uint32_t>(lvg::feedback_type::playstation_output));
    EXPECT_EQ(initial_zero.payload_size, sizeof(payload));
    EXPECT_EQ(initial_zero.client_relative_index, 1);
    EXPECT_EQ(initial_zero.low_frequency, 0);
    EXPECT_EQ(initial_zero.high_frequency, 0);
    EXPECT_EQ(initial_zero.outcome, rumble_poll_outcome_e::raise_attempted);
    EXPECT_TRUE(dispatch_rumble_poll(initial_zero, queue, make_rumble));
    ASSERT_EQ(queue->unsafe().size(), 1u);
    EXPECT_EQ(queue->unsafe().front().type, gamepad_feedback_e::rumble);
    EXPECT_EQ(queue->unsafe().front().id, 1);
    EXPECT_EQ(queue->unsafe().front().data.rumble.lowfreq, 0);
    EXPECT_EQ(queue->unsafe().front().data.rumble.highfreq, 0);
    queue->reset();

    // A motor transition queues the decoded values with the client-relative ID.
    const auto zero_feedback = feedback;
    payload.low_frequency = 0x1234;
    payload.high_frequency = 0xABCD;
    std::memcpy(event.payload, &payload, sizeof(payload));
    ASSERT_TRUE(decode_rumble_rgb(event, feedback));
    const auto changed = observe_rumble_poll(event, 3, 1, feedback, true, true, zero_feedback);
    EXPECT_EQ(changed.outcome, rumble_poll_outcome_e::raise_attempted);
    EXPECT_TRUE(dispatch_rumble_poll(changed, queue, make_rumble));
    ASSERT_EQ(queue->unsafe().size(), 1u);
    EXPECT_EQ(queue->unsafe().front().id, 1);
    EXPECT_EQ(queue->unsafe().front().data.rumble.lowfreq, 0x1234);
    EXPECT_EQ(queue->unsafe().front().data.rumble.highfreq, 0xABCD);
    queue->reset();

    // Repeated motor values and lightbar-only changes must not queue ordinary rumble.
    auto previous = feedback;
    auto deduplicated = observe_rumble_poll(event, 3, 1, feedback, true, true, previous);
    EXPECT_EQ(deduplicated.outcome, rumble_poll_outcome_e::deduplicated);
    EXPECT_FALSE(dispatch_rumble_poll(deduplicated, queue, make_rumble));
    EXPECT_TRUE(queue->unsafe().empty());

    previous.red = 0x55;
    previous.has_rgb = true;
    deduplicated = observe_rumble_poll(event, 3, 1, feedback, true, true, previous);
    EXPECT_EQ(deduplicated.outcome, rumble_poll_outcome_e::deduplicated);
    EXPECT_FALSE(dispatch_rumble_poll(deduplicated, queue, make_rumble));
    EXPECT_TRUE(queue->unsafe().empty());

    // Queue presence does not imply that it is running: raise() is void and drops after stop.
    queue->stop();
    const auto stopped_attempt = observe_rumble_poll(event, 3, 1, feedback, true, false, {});
    EXPECT_EQ(stopped_attempt.outcome, rumble_poll_outcome_e::raise_attempted);
    EXPECT_TRUE(dispatch_rumble_poll(stopped_attempt, queue, make_rumble));
    EXPECT_TRUE(queue->unsafe().empty());
    feedback_queue_t missing_queue;
    EXPECT_FALSE(dispatch_rumble_poll(stopped_attempt, missing_queue, make_rumble));

    const auto unavailable = observe_rumble_poll(event, 3, 1, feedback, false, false, {});
    EXPECT_EQ(unavailable.outcome, rumble_poll_outcome_e::unavailable);
    EXPECT_FALSE(dispatch_rumble_poll(unavailable, queue, make_rumble));
    EXPECT_TRUE(queue->unsafe().empty());
  }

  TEST_F(VhfGamepadPolicyTest, RumblePollSummariesAreBoundedAndKeepOutcomeCounts) {
    using namespace std::chrono_literals;
    using clock = std::chrono::steady_clock;
    const auto start = clock::time_point {};
    rumble_poll_summary_t summary {};

    auto observation = platf::vhf_gamepad::rumble_poll_observation_t {
      2,
      7,
      8,
      1,
      100,
      200,
      rumble_poll_outcome_e::raise_attempted
    };
    EXPECT_TRUE(record_rumble_poll_summary(summary, observation, start));
    EXPECT_EQ(summary.decoded_events, 1u);
    EXPECT_EQ(summary.raise_attempts, 1u);
    EXPECT_EQ(summary.latest.driver_slot, 2u);
    clear_rumble_poll_summary(summary);

    // Reports within the five-second window update counters without scheduling another log line.
    observation.outcome = rumble_poll_outcome_e::deduplicated;
    EXPECT_FALSE(record_rumble_poll_summary(summary, observation, start + 1s));
    observation.outcome = rumble_poll_outcome_e::unavailable;
    EXPECT_FALSE(record_rumble_poll_summary(summary, observation, start + 2s));
    observation.outcome = rumble_poll_outcome_e::raise_attempted;
    EXPECT_FALSE(record_rumble_poll_summary(summary, observation, start + 4999ms));
    EXPECT_FALSE(record_rumble_poll_error(summary, 2, 0x1234, start + 4999ms));
    EXPECT_TRUE(record_rumble_poll_summary(summary, observation, start + 5s));

    EXPECT_EQ(summary.decoded_events, 4u);
    EXPECT_EQ(summary.raise_attempts, 2u);
    EXPECT_EQ(summary.deduplicated_events, 1u);
    EXPECT_EQ(summary.unavailable_events, 1u);
    EXPECT_EQ(summary.poll_errors, 1u);
    EXPECT_EQ(summary.latest_poll_error_status, 0x1234u);
    EXPECT_EQ(summary.latest.outcome, rumble_poll_outcome_e::raise_attempted);

    // A cleared summary retains the window boundary, so sustained input remains rate-limited.
    clear_rumble_poll_summary(summary);
    EXPECT_FALSE(record_rumble_poll_summary(summary, observation, start + 9s));
    EXPECT_TRUE(record_rumble_poll_summary(summary, observation, start + 10s));
  }

  TEST_F(VhfGamepadPolicyTest, RepeatedDriverPollErrorsAreRateLimited) {
    using namespace std::chrono_literals;
    const auto start = std::chrono::steady_clock::time_point {};
    rumble_poll_summary_t summary {};

    EXPECT_TRUE(record_rumble_poll_error(summary, 4, 0x00000005, start));
    clear_rumble_poll_summary(summary);

    // A persistent error at the feedback loop's 8 ms cadence accumulates into one five-second
    // summary instead of producing up to 125 debug records per second for this slot.
    for (int poll = 1; poll < 625; ++poll) {
      EXPECT_FALSE(record_rumble_poll_error(summary, 4, 0x00000005, start + poll * 8ms));
    }
    EXPECT_TRUE(record_rumble_poll_error(summary, 4, 0x00000005, start + 5s));
    EXPECT_EQ(summary.poll_errors, 625u);
    EXPECT_EQ(summary.latest_poll_error_status, 0x00000005u);

    clear_rumble_poll_summary(summary);
    EXPECT_FALSE(record_rumble_poll_error(summary, 4, 0x00000005, start + 5008ms));
  }

  TEST_F(VhfGamepadPolicyTest, PlaystationOutputWithoutLightbarLeavesTheLedAlone) {
    lvg::playstation_output_feedback payload {};
    payload.low_frequency = 0x0500;
    payload.red = 0xFF;  // Present in the struct but not claimed by the valid mask.

    lvg::feedback_event event {};
    event.header.size = sizeof(event);
    event.header.version = lvg::k_protocol_version;
    event.type = lvg::feedback_type::playstation_output;
    event.payload_size = sizeof(payload);
    std::memcpy(event.payload, &payload, sizeof(payload));

    rumble_rgb_t feedback {};
    ASSERT_TRUE(decode_rumble_rgb(event, feedback));
    EXPECT_FALSE(feedback.has_rgb);
    EXPECT_EQ(feedback.red, 0);
    EXPECT_FALSE(feedback.has_trigger_effects);
    EXPECT_EQ(feedback.trigger_event_flags, 0);
  }

  TEST_F(VhfGamepadPolicyTest, PlaystationTriggerReleaseStillEnablesBothEffectUpdates) {
    lvg::playstation_output_feedback payload {};
    payload.valid = lvg::ps_output_triggers_valid;

    lvg::feedback_event event {};
    event.type = lvg::feedback_type::playstation_output;
    event.payload_size = sizeof(payload);
    std::memcpy(event.payload, &payload, sizeof(payload));

    rumble_rgb_t feedback {};
    ASSERT_TRUE(decode_rumble_rgb(event, feedback));
    EXPECT_TRUE(feedback.has_trigger_effects);
    // Off is a program too: clearing the validity bits would leave the old effect active.
    EXPECT_EQ(feedback.trigger_event_flags, DS_EFFECT_LEFT_TRIGGER | DS_EFFECT_RIGHT_TRIGGER);
    EXPECT_EQ(feedback.left_effect.mode, 0);
    EXPECT_EQ(feedback.right_effect.mode, 0);
  }

  TEST_F(VhfGamepadPolicyTest, AdaptiveTriggerChangesAreNotDuplicateFeedback) {
    rumble_rgb_t previous {};
    previous.has_trigger_effects = true;
    previous.trigger_event_flags = DS_EFFECT_LEFT_TRIGGER | DS_EFFECT_RIGHT_TRIGGER;
    previous.left_effect.mode = static_cast<std::uint8_t>(lvg::trigger_effect_mode::weapon);
    previous.right_effect.mode = static_cast<std::uint8_t>(lvg::trigger_effect_mode::feedback);

    EXPECT_EQ(previous, previous);
    // raise_feedback discards equal reports before inspecting adaptive effects.
    auto changed = previous;
    changed.left_effect.mode = 0;
    EXPECT_FALSE(previous == changed);
    changed = previous;
    changed.right_effect.mode = 0;
    EXPECT_FALSE(previous == changed);
    changed = previous;
    changed.left_effect.parameters.back() = 0x42;
    EXPECT_FALSE(previous == changed);
    changed = previous;
    changed.right_effect.parameters.back() = 0x24;
    EXPECT_FALSE(previous == changed);
    changed = previous;
    changed.trigger_event_flags = DS_EFFECT_LEFT_TRIGGER;
    EXPECT_FALSE(previous == changed);
    changed = previous;
    changed.has_trigger_effects = false;
    EXPECT_FALSE(previous == changed);
  }

  TEST_F(VhfGamepadPolicyTest, TouchEventTypesMapToTheProtocol) {
    EXPECT_EQ(to_protocol_touch_event(LI_TOUCH_EVENT_DOWN),
              static_cast<std::uint8_t>(lvg::touch_event::down));
    EXPECT_EQ(to_protocol_touch_event(LI_TOUCH_EVENT_MOVE),
              static_cast<std::uint8_t>(lvg::touch_event::move));
    EXPECT_EQ(to_protocol_touch_event(LI_TOUCH_EVENT_UP),
              static_cast<std::uint8_t>(lvg::touch_event::up));
    // An unmapped event releases everything rather than inventing a contact.
    EXPECT_EQ(to_protocol_touch_event(0xEE),
              static_cast<std::uint8_t>(lvg::touch_event::cancel_all));
  }

  TEST_F(VhfGamepadPolicyTest, MotionAndBatteryMapToTheProtocol) {
    EXPECT_EQ(to_protocol_motion_kind(LI_MOTION_TYPE_ACCEL),
              static_cast<std::uint8_t>(lvg::motion_kind::accelerometer));
    EXPECT_EQ(to_protocol_motion_kind(LI_MOTION_TYPE_GYRO),
              static_cast<std::uint8_t>(lvg::motion_kind::gyroscope));
    EXPECT_EQ(to_protocol_motion_kind(0x7F), 0);

    EXPECT_EQ(to_protocol_battery_state(LI_BATTERY_STATE_CHARGING),
              static_cast<std::uint8_t>(lvg::battery_state::charging));
    EXPECT_EQ(to_protocol_battery_state(LI_BATTERY_STATE_FULL),
              static_cast<std::uint8_t>(lvg::battery_state::full));
  }

  TEST_F(VhfGamepadPolicyTest, TouchCoordinatesNormalizeAndClamp) {
    EXPECT_EQ(to_normalized_touch(0.0f), 0);
    EXPECT_EQ(to_normalized_touch(1.0f), 65535);
    EXPECT_EQ(to_normalized_touch(2.0f), 65535);
    EXPECT_EQ(to_normalized_touch(-1.0f), 0);
    // NaN must not become an arbitrary coordinate.
    EXPECT_EQ(to_normalized_touch(std::nanf("")), 0);
    const std::uint16_t middle = to_normalized_touch(0.5f);
    EXPECT_GT(middle, 32000);
    EXPECT_LT(middle, 33500);
  }

  TEST_F(VhfGamepadPolicyTest, MotionScalesToMilliUnits) {
    EXPECT_EQ(to_milli_units(1.0f), 1000);
    EXPECT_EQ(to_milli_units(-9.80665f), -9806);
    EXPECT_EQ(to_milli_units(std::nanf("")), 0);
    // Saturates instead of wrapping.
    EXPECT_EQ(to_milli_units(1e12f), 2147483000);
    EXPECT_EQ(to_milli_units(-1e12f), -2147483000);
  }

  TEST_F(VhfGamepadPolicyTest, NonRumbleFeedbackIsRejected) {
    auto event = make_rumble_event({});
    event.type = lvg::feedback_type::raw_output_report;

    rumble_rgb_t feedback {};
    EXPECT_FALSE(decode_rumble_rgb(event, feedback));
  }

  TEST_F(VhfGamepadPolicyTest, TruncatedFeedbackPayloadIsRejected) {
    auto event = make_rumble_event({});
    event.payload_size = sizeof(lvg::generic_rumble_rgb_feedback) - 1;

    rumble_rgb_t feedback {};
    EXPECT_FALSE(decode_rumble_rgb(event, feedback));
  }

}  // namespace
