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

#include <src/platform/windows/vhf_gamepad_policy.h>

namespace {

  using platf::vhf_gamepad::decode_rumble_rgb;
  using platf::vhf_gamepad::make_input_state;
  using platf::vhf_gamepad::normalized_state_t;
  using platf::vhf_gamepad::rumble_rgb_t;
  using platf::vhf_gamepad::select_automatic_profile;
  using platf::vhf_gamepad::supported_button_mask;
  using platf::vhf_gamepad::to_milli_units;
  using platf::vhf_gamepad::to_normalized_touch;
  using platf::vhf_gamepad::to_protocol_battery_state;
  using platf::vhf_gamepad::to_protocol_motion_kind;
  using platf::vhf_gamepad::to_protocol_touch_event;

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
    // The wire contract is Vibeshine's normalized state, positive-up. Each driver profile
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
    EXPECT_EQ(feedback.left_effect.mode, static_cast<std::uint8_t>(lvg::trigger_effect_mode::weapon));
    EXPECT_EQ(feedback.left_effect.parameters[0], 0x42);
    EXPECT_EQ(feedback.right_effect.mode, static_cast<std::uint8_t>(lvg::trigger_effect_mode::feedback));
    // A PlayStation pad has no impulse triggers, so the Xbox rumble fields stay clear.
    EXPECT_FALSE(feedback.has_triggers);
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
