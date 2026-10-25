/**
 * @file tests/unit/platform/windows/test_vhf_gamepad_policy.cpp
 * @brief Test the translation between normalized gamepad state and the VHF driver protocol.
 */
#include "../../../tests_common.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <cstring>
#include <limits>
#include <string_view>
#include <utility>

#include <src/platform/common.h>
#include <src/platform/windows/vhf_gamepad_policy.h>
#include <src/steam_controller_protocol.h>
#include <src/thread_safe.h>

namespace {

  using platf::gamepad_feedback_e;
  using platf::gamepad_feedback_msg_t;
  using platf::feedback_queue_t;
  using platf::raise_gamepad_feedback;
  using platf::vhf_gamepad::backend_e;
  using platf::vhf_gamepad::decode_rumble_rgb;
  using platf::vhf_gamepad::decode_steam_haptic;
  using platf::vhf_gamepad::decode_steam_settings;
  using platf::vhf_gamepad::accepts_steam_attributes;
  using platf::vhf_gamepad::describe_steam_attributes;
  using platf::vhf_gamepad::format_unix_time_utc;
  using platf::vhf_gamepad::make_steam_attributes_request;
  using platf::vhf_gamepad::steam_attributes_driver_mask;
  using platf::vhf_gamepad::steam_attributes_request_t;
  using platf::vhf_gamepad::steam_attributes_t;
  using platf::vhf_gamepad::steam_settings_t;
  using platf::vhf_gamepad::steam_haptic_t;
  using platf::vhf_gamepad::synthesize_steam_rumble;
  using platf::vhf_gamepad::synthesized_rumble_t;
  using platf::vhf_gamepad::STEAM_HAPTIC_CLICK_HOLD_MS;
  using platf::vhf_gamepad::STEAM_HAPTIC_RUMBLE_TIMEOUT_MS;
  using platf::vhf_gamepad::STEAM_HAPTIC_LEFT;
  using platf::vhf_gamepad::STEAM_HAPTIC_MIN_HOLD_MS;
  using platf::vhf_gamepad::STEAM_HAPTIC_RIGHT;
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
  using platf::vhf_profile_e;
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

  TEST_F(VhfGamepadPolicyTest, AutomaticBackendFallsBackToVhfWhenVigemIsUnavailable) {
    EXPECT_EQ(select_automatic_backend(true, true), backend_e::vigem);
    EXPECT_EQ(select_automatic_backend(true, false), backend_e::vigem);
    EXPECT_EQ(select_automatic_backend(false, true), backend_e::vhf);
    EXPECT_EQ(select_automatic_backend(false, false), backend_e::unavailable);
  }

  TEST_F(VhfGamepadPolicyTest, UnsupportedTouchEventsAreDroppedNotCancelAll) {
    using platf::vhf_gamepad::PROTOCOL_TOUCH_EVENT_UNSUPPORTED;
    EXPECT_EQ(to_protocol_touch_event(LI_TOUCH_EVENT_HOVER_LEAVE), PROTOCOL_TOUCH_EVENT_UNSUPPORTED);
    EXPECT_EQ(to_protocol_touch_event(LI_TOUCH_EVENT_BUTTON_ONLY), PROTOCOL_TOUCH_EVENT_UNSUPPORTED);
    EXPECT_EQ(to_protocol_touch_event(0x42), PROTOCOL_TOUCH_EVENT_UNSUPPORTED);
    EXPECT_NE(to_protocol_touch_event(LI_TOUCH_EVENT_CANCEL_ALL), PROTOCOL_TOUCH_EVENT_UNSUPPORTED);
    EXPECT_NE(to_protocol_touch_event(LI_TOUCH_EVENT_CANCEL), to_protocol_touch_event(LI_TOUCH_EVENT_CANCEL_ALL));
    for (std::uint8_t event : {LI_TOUCH_EVENT_HOVER, LI_TOUCH_EVENT_DOWN, LI_TOUCH_EVENT_UP, LI_TOUCH_EVENT_MOVE, LI_TOUCH_EVENT_CANCEL}) {
      EXPECT_NE(to_protocol_touch_event(event), PROTOCOL_TOUCH_EVENT_UNSUPPORTED);
      EXPECT_NE(to_protocol_touch_event(event), to_protocol_touch_event(LI_TOUCH_EVENT_CANCEL_ALL));
    }
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
        // A client that identifies a Steam Controller (the Steam type with a Steam extension) gets
        // the real thing, whatever the DS4 preferences say.
        EXPECT_EQ(select_desired_profile(setting, LI_CTYPE_STEAM, LI_CCAP_GYRO | LI_CCAP_TOUCHPAD | LI_CCAP_GRIP_SENSE, preferences, preferences), vhf_profile_e::steam_controller);
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

  TEST_F(VhfGamepadPolicyTest, SteamControllerSelectionsAdvertiseControllerTouch) {
    // Both Steam Controller pads are trackpads: the explicit profile, and a plain `vhf` that
    // selects it for a Steam client, keep controller touch on whatever the DS4 preferences say.
    EXPECT_TRUE(platf::vhf_gamepad::supports_controller_touch("vhf_steam", false));
    for (const bool preferences : {false, true}) {
      SCOPED_TRACE(preferences);
      EXPECT_EQ(select_desired_profile("vhf", LI_CTYPE_STEAM, LI_CCAP_TOUCHPAD | LI_CCAP_SC26_BUTTONS, preferences, preferences), vhf_profile_e::steam_controller);
    }
    EXPECT_TRUE(platf::vhf_gamepad::supports_controller_touch("vhf", false));
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
      for (const auto type : {LI_CTYPE_XBOX, LI_CTYPE_PS, LI_CTYPE_NINTENDO, LI_CTYPE_UNKNOWN, LI_CTYPE_STEAM}) {
        SCOPED_TRACE(setting);
        SCOPED_TRACE(type);
        EXPECT_EQ(select_desired_profile(setting, type, LI_CCAP_ACCEL | LI_CCAP_GYRO | LI_CCAP_TOUCHPAD, true, true), expected);
      }
    }
  }

  TEST_F(VhfGamepadPolicyTest, SteamProfileNeedsASteamControllerClient) {
    using platf::vhf_gamepad::is_steam_controller_client;
    const std::uint16_t generic = LI_CCAP_ACCEL | LI_CCAP_GYRO | LI_CCAP_TOUCHPAD;
    // Any one of the Steam extensions with the Steam type is the real controller's client driver.
    for (const int caps : {LI_CCAP_GRIP_SENSE, LI_CCAP_STEAM_HAPTIC, LI_CCAP_SC26_BUTTONS}) {
      SCOPED_TRACE(caps);
      EXPECT_TRUE(is_steam_controller_client(LI_CTYPE_STEAM, static_cast<std::uint16_t>(caps)));
      EXPECT_FALSE(is_steam_controller_client(LI_CTYPE_XBOX, static_cast<std::uint16_t>(caps)));
      EXPECT_EQ(select_desired_profile("vhf_steam", LI_CTYPE_STEAM, static_cast<std::uint16_t>(caps), false, false), vhf_profile_e::steam_controller);
      EXPECT_EQ(select_desired_profile("auto", LI_CTYPE_STEAM, static_cast<std::uint16_t>(caps), false, false), vhf_profile_e::steam_controller);
    }
    // A Steam Deck's built-in controls through SDL: the Steam type with generic capabilities only.
    EXPECT_FALSE(is_steam_controller_client(LI_CTYPE_STEAM, generic));
    EXPECT_EQ(select_desired_profile("auto", LI_CTYPE_STEAM, generic, true, false), vhf_profile_e::dualsense);
    EXPECT_EQ(select_desired_profile("auto", LI_CTYPE_STEAM, generic, false, false), vhf_profile_e::automatic);
    // vhf_steam forced: the other controllers a client forwards get the automatic choice, not a
    // second Steam Controller.
    EXPECT_EQ(select_desired_profile("vhf_steam", LI_CTYPE_XBOX, generic, false, false), vhf_profile_e::automatic);
    EXPECT_EQ(select_desired_profile("vhf_steam", LI_CTYPE_PS, generic, false, false), vhf_profile_e::dualsense);
    EXPECT_EQ(select_desired_profile("vhf_steam", LI_CTYPE_UNKNOWN, 0, false, false), vhf_profile_e::automatic);
    EXPECT_EQ(select_desired_profile("vhf_steam", LI_CTYPE_STEAM, generic, false, false), vhf_profile_e::automatic);
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

  TEST_F(VhfGamepadPolicyTest, GripSenseBitsSurviveTheMask) {
    // Moonlight's LI_CCAP_GRIP_SENSE bits ride in the button word; the Steam Controller
    // profile follows the client's grips for good once it has seen one of them.
    normalized_state_t state {};
    state.button_flags = lvg::button_mask::left_grip_touch | lvg::button_mask::right_grip_touch;

    EXPECT_EQ(make_input_state(0, state).buttons,
              lvg::button_mask::left_grip_touch | lvg::button_mask::right_grip_touch);
  }

  TEST_F(VhfGamepadPolicyTest, StickTouchBitsSurviveTheMask) {
    normalized_state_t state {};
    state.button_flags = lvg::button_mask::left_stick_touch | lvg::button_mask::right_stick_touch;

    EXPECT_EQ(make_input_state(0, state).buttons,
              lvg::button_mask::left_stick_touch | lvg::button_mask::right_stick_touch);
  }

  TEST_F(VhfGamepadPolicyTest, ExplicitSc26ClicksRequireCapabilityAndProfile) {
    normalized_state_t state {};
    state.button_flags = lvg::button_mask::left_pad_click | lvg::button_mask::right_trigger_click |
                         lvg::button_mask::sc26_buttons_explicit;
    const auto clicks = lvg::button_mask::left_pad_click | lvg::button_mask::right_trigger_click;
    EXPECT_EQ(make_input_state(0, state).buttons, clicks);
    EXPECT_EQ(make_input_state(0, state, lvg::profile::steam_controller, 0).buttons, clicks);
    EXPECT_EQ(make_input_state(0, state, lvg::profile::dualsense, 0x1000).buttons, clicks);
    EXPECT_EQ(make_input_state(0, state, lvg::profile::steam_controller, 0x1000).buttons,
              clicks | lvg::button_mask::sc26_buttons_explicit);
    state.button_flags = 0;
    EXPECT_EQ(make_input_state(0, state, lvg::profile::steam_controller, 0x1000).buttons,
              static_cast<std::uint32_t>(lvg::button_mask::sc26_buttons_explicit));
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
    const auto admit = [](auto &target, gamepad_feedback_msg_t msg) {
      return raise_gamepad_feedback(target, std::move(msg));
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
    EXPECT_TRUE(dispatch_rumble_poll(initial_zero, queue, make_rumble, admit));
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
    EXPECT_TRUE(dispatch_rumble_poll(changed, queue, make_rumble, admit));
    ASSERT_EQ(queue->unsafe().size(), 1u);
    EXPECT_EQ(queue->unsafe().front().id, 1);
    EXPECT_EQ(queue->unsafe().front().data.rumble.lowfreq, 0x1234);
    EXPECT_EQ(queue->unsafe().front().data.rumble.highfreq, 0xABCD);
    queue->reset();

    // Repeated motor values and lightbar-only changes must not queue ordinary rumble.
    auto previous = feedback;
    auto deduplicated = observe_rumble_poll(event, 3, 1, feedback, true, true, previous);
    EXPECT_EQ(deduplicated.outcome, rumble_poll_outcome_e::deduplicated);
    EXPECT_FALSE(dispatch_rumble_poll(deduplicated, queue, make_rumble, admit));
    EXPECT_TRUE(queue->unsafe().empty());

    previous.red = 0x55;
    previous.has_rgb = true;
    deduplicated = observe_rumble_poll(event, 3, 1, feedback, true, true, previous);
    EXPECT_EQ(deduplicated.outcome, rumble_poll_outcome_e::deduplicated);
    EXPECT_FALSE(dispatch_rumble_poll(deduplicated, queue, make_rumble, admit));
    EXPECT_TRUE(queue->unsafe().empty());

    // Queue presence does not imply that it is running: a stopped queue rejects the message,
    // and the caller must not record the feedback as delivered.
    queue->stop();
    const auto stopped_attempt = observe_rumble_poll(event, 3, 1, feedback, true, false, {});
    EXPECT_EQ(stopped_attempt.outcome, rumble_poll_outcome_e::raise_attempted);
    EXPECT_FALSE(dispatch_rumble_poll(stopped_attempt, queue, make_rumble, admit));
    EXPECT_TRUE(queue->unsafe().empty());
    feedback_queue_t missing_queue;
    EXPECT_FALSE(dispatch_rumble_poll(stopped_attempt, missing_queue, make_rumble, admit));

    const auto unavailable = observe_rumble_poll(event, 3, 1, feedback, false, false, {});
    EXPECT_EQ(unavailable.outcome, rumble_poll_outcome_e::unavailable);
    EXPECT_FALSE(dispatch_rumble_poll(unavailable, queue, make_rumble, admit));
    EXPECT_TRUE(queue->unsafe().empty());
  }

  TEST_F(VhfGamepadPolicyTest, RumblePollStopSurvivesAFullFeedbackMailbox) {
    // The stream mailbox: bounded, and it drops the newest plain raise() when full.
    auto feedback_mail = std::make_shared<safe::mail_raw_t>();
    feedback_queue_t queue = feedback_mail->queue<gamepad_feedback_msg_t>(
      "vhf-policy-full-mailbox", platf::GAMEPAD_FEEDBACK_QUEUE_CAPACITY, false);
    const auto make_rumble = [](const auto client_index, const auto low, const auto high) {
      return gamepad_feedback_msg_t::make_rumble(client_index, low, high);
    };
    const auto admit = [](auto &target, gamepad_feedback_msg_t msg) {
      return raise_gamepad_feedback(target, std::move(msg));
    };

    // Every controller's protected state (15 keys each) plus the reserved settings chunks fill
    // the mailbox to capacity with items plain raise() cannot evict.
    for (std::uint16_t id = 0; id < 16; ++id) {
      ASSERT_TRUE(raise_gamepad_feedback(*queue, gamepad_feedback_msg_t::make_rumble(id, 0x100, 0x200)));
      ASSERT_TRUE(raise_gamepad_feedback(*queue, gamepad_feedback_msg_t::make_rumble_triggers(id, 1, 2)));
      ASSERT_TRUE(raise_gamepad_feedback(*queue, gamepad_feedback_msg_t::make_rgb_led(id, 1, 2, 3)));
      for (std::uint8_t motion : {1, 2}) {
        ASSERT_TRUE(raise_gamepad_feedback(*queue, gamepad_feedback_msg_t::make_motion_event_state(id, motion, 100)));
      }
      for (std::uint8_t flags : {4, 8, 12}) {
        ASSERT_TRUE(raise_gamepad_feedback(*queue, gamepad_feedback_msg_t::make_adaptive_triggers(id, flags, 0, 0, {}, {})));
      }
      ASSERT_TRUE(raise_gamepad_feedback(*queue, gamepad_feedback_msg_t::make_steam_haptic(id, 12, {0x80})));
      for (std::uint8_t type : {0x81, 0x82}) {
        for (std::uint8_t side = 0; side < 3; ++side) {
          ASSERT_TRUE(raise_gamepad_feedback(*queue, gamepad_feedback_msg_t::make_steam_haptic(id, 12, {type, side})));
        }
      }
    }
    for (std::uint8_t chunk = 0; chunk < 16; ++chunk) {
      ASSERT_TRUE(raise_gamepad_feedback(*queue, gamepad_feedback_msg_t::make_steam_settings(chunk, 0x87, 1, {chunk, 1, 0})));
    }
    ASSERT_EQ(queue->unsafe().size(), platf::GAMEPAD_FEEDBACK_QUEUE_CAPACITY);
    // The plain path the dispatcher used before would drop the stop here.
    EXPECT_FALSE(queue->try_raise(gamepad_feedback_msg_t::make_rumble(1, 0, 0)));

    // Controller 1 goes from active motors to a stop while the mailbox is full.
    rumble_rgb_t previous {};
    previous.low_frequency = 0x100;
    previous.high_frequency = 0x200;
    const rumble_rgb_t stop {};
    const auto observation = observe_rumble_poll(lvg::feedback_event {}, 3, 1, stop, true, true, previous);
    ASSERT_EQ(observation.outcome, rumble_poll_outcome_e::raise_attempted);
    EXPECT_TRUE(dispatch_rumble_poll(observation, queue, make_rumble, admit));

    const auto &pending = queue->unsafe();
    EXPECT_EQ(pending.size(), platf::GAMEPAD_FEEDBACK_QUEUE_CAPACITY);
    const auto stops = std::count_if(pending.begin(), pending.end(), [](const gamepad_feedback_msg_t &msg) {
      return msg.type == gamepad_feedback_e::rumble && msg.id == 1 &&
             msg.data.rumble.lowfreq == 0 && msg.data.rumble.highfreq == 0;
    });
    EXPECT_EQ(stops, 1);
    const auto stale = std::count_if(pending.begin(), pending.end(), [](const gamepad_feedback_msg_t &msg) {
      return msg.type == gamepad_feedback_e::rumble && msg.id == 1 && msg.data.rumble.lowfreq == 0x100;
    });
    EXPECT_EQ(stale, 0);
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
    // Unmapped events are ignored; only explicit cancel-all releases every contact.
    EXPECT_EQ(to_protocol_touch_event(0xEE),
              platf::vhf_gamepad::PROTOCOL_TOUCH_EVENT_UNSUPPORTED);
    EXPECT_EQ(to_protocol_touch_event(LI_TOUCH_EVENT_CANCEL_ALL),
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

  lvg::feedback_event make_steam_haptic_event(std::initializer_list<std::uint8_t> report) {
    lvg::steam_haptic_feedback payload {};
    payload.length = static_cast<std::uint8_t>(report.size());
    std::copy(report.begin(), report.end(), payload.report);

    lvg::feedback_event event {};
    event.header.size = sizeof(event);
    event.header.version = lvg::k_protocol_version;
    event.controller_id = 3;
    event.type = lvg::feedback_type::steam_haptic;
    event.payload_size = sizeof(payload);
    std::memcpy(event.payload, &payload, sizeof(payload));
    return event;
  }

  steam_haptic_t make_steam_haptic(std::initializer_list<std::uint8_t> report) {
    steam_haptic_t haptic {};
    haptic.length = static_cast<std::uint8_t>(report.size());
    std::copy(report.begin(), report.end(), haptic.report.begin());
    return haptic;
  }

  lvg::feedback_event make_steam_settings_event(const std::uint8_t command, const std::uint8_t count, std::initializer_list<std::uint8_t> triples) {
    lvg::steam_settings_feedback payload {};
    payload.command = command;
    payload.count = count;
    std::copy(triples.begin(), triples.end(), payload.triples);
    lvg::feedback_event event {};
    event.header.size = sizeof(event);
    event.header.version = lvg::k_protocol_version;
    event.type = lvg::feedback_type::steam_settings;
    event.payload_size = sizeof(payload);
    std::memcpy(event.payload, &payload, sizeof(payload));
    return event;
  }

  TEST_F(VhfGamepadPolicyTest, SteamSettingsWriteIsDecodedVerbatim) {
    steam_settings_t settings {};
    ASSERT_TRUE(decode_steam_settings(
      make_steam_settings_event(STEAM_SETTINGS_SET_VALUES, 2, {48, 0x18, 0x00, 7, 0x07, 0x00}), settings));
    EXPECT_EQ(settings.command, STEAM_SETTINGS_SET_VALUES);
    EXPECT_EQ(settings.count, 2);
    EXPECT_EQ(settings.triples[0], 48);
    EXPECT_EQ(settings.triples[1], 0x18);
    EXPECT_EQ(settings.triples[3], 7);
    EXPECT_EQ(settings.triples[4], 0x07);
    EXPECT_EQ(settings.triples[6], 0);

    ASSERT_TRUE(decode_steam_settings(make_steam_settings_event(STEAM_SETTINGS_LOAD_DEFAULTS, 0, {}), settings));
    EXPECT_EQ(settings.command, STEAM_SETTINGS_LOAD_DEFAULTS);
    EXPECT_EQ(settings.count, 0);

    // A settings event is nothing else, and a haptic event is not a settings write.
    steam_haptic_t haptic {};
    EXPECT_FALSE(decode_steam_haptic(make_steam_settings_event(STEAM_SETTINGS_SET_VALUES, 1, {9, 0, 0}), haptic));
    rumble_rgb_t rumble {};
    EXPECT_FALSE(decode_rumble_rgb(make_steam_settings_event(STEAM_SETTINGS_SET_VALUES, 1, {9, 0, 0}), rumble));
    EXPECT_FALSE(decode_steam_settings(make_steam_haptic_event({0x82, 0x01, 0x02, 0xf2}), settings));
  }

  TEST_F(VhfGamepadPolicyTest, SteamSettingsWriteWithAnImpossibleShapeIsRefused) {
    steam_settings_t settings {};
    EXPECT_FALSE(decode_steam_settings(make_steam_settings_event(STEAM_SETTINGS_SET_VALUES, 0, {}), settings));
    EXPECT_FALSE(decode_steam_settings(
      make_steam_settings_event(STEAM_SETTINGS_SET_VALUES, STEAM_SETTINGS_TRIPLES_MAX + 1, {}), settings));
    EXPECT_FALSE(decode_steam_settings(make_steam_settings_event(STEAM_SETTINGS_LOAD_DEFAULTS, 1, {9, 0, 0}), settings));
    EXPECT_FALSE(decode_steam_settings(make_steam_settings_event(0x89, 1, {9, 0, 0}), settings));
    auto truncated = make_steam_settings_event(STEAM_SETTINGS_SET_VALUES, 1, {9, 0, 0});
    truncated.payload_size = sizeof(lvg::steam_settings_feedback) - 1;
    EXPECT_FALSE(decode_steam_settings(truncated, settings));
  }

  TEST_F(VhfGamepadPolicyTest, SteamHapticEventIsDecodedVerbatim) {
    steam_haptic_t haptic {};
    ASSERT_TRUE(decode_steam_haptic(make_steam_haptic_event({0x82, 0x01, 0x02, 0xf2}), haptic));
    EXPECT_EQ(haptic.length, 4);
    EXPECT_EQ(haptic.report[0], 0x82);
    EXPECT_EQ(haptic.report[1], 0x01);
    EXPECT_EQ(haptic.report[2], 0x02);
    EXPECT_EQ(haptic.report[3], 0xf2);

    rumble_rgb_t rumble {};
    EXPECT_FALSE(decode_rumble_rgb(make_steam_haptic_event({0x82, 0x01, 0x02, 0xf2}), rumble));

    auto truncated = make_steam_haptic_event({0x82, 0x01, 0x02, 0xf2});
    truncated.payload_size = sizeof(lvg::steam_haptic_feedback) - 1;
    EXPECT_FALSE(decode_steam_haptic(truncated, haptic));
    EXPECT_FALSE(decode_steam_haptic(make_steam_haptic_event({0x82}), haptic));
  }

  TEST_F(VhfGamepadPolicyTest, SteamHapticRumbleReportBecomesMotorSpeeds) {
    synthesized_rumble_t rumble {};
    EXPECT_EQ(synthesize_steam_rumble(
                make_steam_haptic({0x80, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0xff, 0x3f, 0x00}), rumble),
              STEAM_HAPTIC_LEFT | STEAM_HAPTIC_RIGHT);
    EXPECT_EQ(rumble.left, 0x8000);
    EXPECT_EQ(rumble.right, 0x3fff);
    EXPECT_EQ(rumble.left_hold_ms, STEAM_HAPTIC_RUMBLE_TIMEOUT_MS);
    EXPECT_EQ(rumble.right_hold_ms, STEAM_HAPTIC_RUMBLE_TIMEOUT_MS);
    EXPECT_EQ(synthesize_steam_rumble(make_steam_haptic({0x80, 0x00, 0x00}), rumble), 0);
  }

  TEST_F(VhfGamepadPolicyTest, SteamHapticPulseBecomesDutyCycleForTheTrainsLength) {
    synthesized_rumble_t rumble {};
    // 10 ms on, 10 ms off, ten times, on the left pad: half magnitude for 200 ms.
    EXPECT_EQ(synthesize_steam_rumble(make_steam_haptic({0x81, 0x01, 0x10, 0x27, 0x10, 0x27, 0x0a, 0x00}), rumble),
              STEAM_HAPTIC_LEFT);
    EXPECT_EQ(rumble.left, 32767);
    EXPECT_EQ(rumble.left_hold_ms, 200u);
    EXPECT_EQ(rumble.right, 0);

    // Steam's UI click: one 400 us pulse at full magnitude, held long enough to be felt.
    EXPECT_EQ(synthesize_steam_rumble(make_steam_haptic({0x81, 0x00, 0x90, 0x01, 0x00, 0x00, 0x01, 0x00}), rumble),
              STEAM_HAPTIC_RIGHT);
    EXPECT_EQ(rumble.right, 65535);
    EXPECT_EQ(rumble.right_hold_ms, STEAM_HAPTIC_MIN_HOLD_MS);
    EXPECT_EQ(rumble.left, 32767);

    // A long train does not overflow 32-bit microseconds and is capped.
    EXPECT_EQ(synthesize_steam_rumble(make_steam_haptic({0x81, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}), rumble),
              STEAM_HAPTIC_RIGHT);
    EXPECT_EQ(rumble.right_hold_ms, 10'000u);

    // A zero-repeat pulse is Steam's per-side stop.
    EXPECT_EQ(synthesize_steam_rumble(make_steam_haptic({0x81, 0x01, 0, 0, 0, 0, 0, 0}), rumble), STEAM_HAPTIC_LEFT);
    EXPECT_EQ(rumble.left, 0);
    EXPECT_EQ(rumble.left_hold_ms, 0u);
    EXPECT_EQ(rumble.right, 32767);
  }

  TEST_F(VhfGamepadPolicyTest, SteamHapticClickBecomesTimedRumbleFromItsGain) {
    synthesized_rumble_t rumble {};
    // Steam's test screen: a click at -14 dB on 0x82 side 0 (the left pad), every 100 ms.
    EXPECT_EQ(synthesize_steam_rumble(make_steam_haptic({0x82, 0x00, 0x02, 0xf2}), rumble), STEAM_HAPTIC_LEFT);
    EXPECT_NEAR(rumble.left, 65535 * 0.1995, 64);
    EXPECT_EQ(rumble.left_hold_ms, STEAM_HAPTIC_CLICK_HOLD_MS);
    EXPECT_EQ(rumble.right, 0);

    // A tone at +3 dB clips to full scale and runs until its off.
    EXPECT_EQ(synthesize_steam_rumble(make_steam_haptic({0x82, 0x01, 0x03, 0x03}), rumble), STEAM_HAPTIC_RIGHT);
    EXPECT_EQ(rumble.right, 65535);
    EXPECT_EQ(rumble.right_hold_ms, 0u);
    EXPECT_EQ(synthesize_steam_rumble(make_steam_haptic({0x82, 0x01, 0x00, 0x00}), rumble), STEAM_HAPTIC_RIGHT);
    EXPECT_EQ(rumble.right, 0);

    EXPECT_EQ(synthesize_steam_rumble(make_steam_haptic({0x82, 0x00}), rumble), 0);
  }

  // The two report families number their sides the opposite way round (measured on the real
  // unit): a 0x81 pulse's side 1 is the left pad, a 0x82 command's side 0 is. Side 2 is both.
  TEST_F(VhfGamepadPolicyTest, SteamHapticPulseSideByteIsOneForLeftZeroForRightTwoForBoth) {
    synthesized_rumble_t rumble {};
    // Side 1: left only.
    EXPECT_EQ(synthesize_steam_rumble(make_steam_haptic({0x81, 0x01, 0x10, 0x27, 0x10, 0x27, 0x0a, 0x00}), rumble),
              STEAM_HAPTIC_LEFT);
    EXPECT_EQ(rumble.left, 32767);
    EXPECT_EQ(rumble.left_hold_ms, 200u);
    EXPECT_EQ(rumble.right, 0);
    EXPECT_EQ(rumble.right_hold_ms, 0u);

    // Side 0: right only; the left side keeps what it had.
    rumble = {};
    EXPECT_EQ(synthesize_steam_rumble(make_steam_haptic({0x81, 0x00, 0x10, 0x27, 0x10, 0x27, 0x0a, 0x00}), rumble),
              STEAM_HAPTIC_RIGHT);
    EXPECT_EQ(rumble.right, 32767);
    EXPECT_EQ(rumble.right_hold_ms, 200u);
    EXPECT_EQ(rumble.left, 0);
    EXPECT_EQ(rumble.left_hold_ms, 0u);

    // Side 2: both pads, the mask the stop key in common.h derives for it.
    rumble = {};
    EXPECT_EQ(synthesize_steam_rumble(make_steam_haptic({0x81, 0x02, 0x10, 0x27, 0x10, 0x27, 0x0a, 0x00}), rumble),
              STEAM_HAPTIC_LEFT | STEAM_HAPTIC_RIGHT);
    EXPECT_EQ(rumble.left, 32767);
    EXPECT_EQ(rumble.right, 32767);
    EXPECT_EQ(rumble.left_hold_ms, 200u);
    EXPECT_EQ(rumble.right_hold_ms, 200u);

    // A zero-repeat pulse on side 2 stops both.
    EXPECT_EQ(synthesize_steam_rumble(make_steam_haptic({0x81, 0x02, 0, 0, 0, 0, 0, 0}), rumble),
              STEAM_HAPTIC_LEFT | STEAM_HAPTIC_RIGHT);
    EXPECT_EQ(rumble.left, 0);
    EXPECT_EQ(rumble.right, 0);

    // A side the firmware has no pad for renders nothing.
    rumble = {};
    EXPECT_EQ(synthesize_steam_rumble(make_steam_haptic({0x81, 0x03, 0x10, 0x27, 0x10, 0x27, 0x0a, 0x00}), rumble), 0);
    EXPECT_EQ(rumble.left, 0);
    EXPECT_EQ(rumble.right, 0);
  }

  TEST_F(VhfGamepadPolicyTest, SteamHapticCommandSideByteIsZeroForLeftOneForRightTwoForBoth) {
    synthesized_rumble_t rumble {};
    // Steam's tick (82 <side> 01 FD): side 0 is the LEFT pad.
    EXPECT_EQ(synthesize_steam_rumble(make_steam_haptic({0x82, 0x00, 0x01, 0xfd}), rumble), STEAM_HAPTIC_LEFT);
    EXPECT_GT(rumble.left, 0);
    EXPECT_EQ(rumble.left_hold_ms, STEAM_HAPTIC_CLICK_HOLD_MS);
    EXPECT_EQ(rumble.right, 0);
    EXPECT_EQ(rumble.right_hold_ms, 0u);

    // Steam's click (82 <side> 02 F2): side 1 is the RIGHT pad.
    rumble = {};
    EXPECT_EQ(synthesize_steam_rumble(make_steam_haptic({0x82, 0x01, 0x02, 0xf2}), rumble), STEAM_HAPTIC_RIGHT);
    EXPECT_NEAR(rumble.right, 65535 * 0.1995, 64);
    EXPECT_EQ(rumble.right_hold_ms, STEAM_HAPTIC_CLICK_HOLD_MS);
    EXPECT_EQ(rumble.left, 0);
    EXPECT_EQ(rumble.left_hold_ms, 0u);

    // Side 2: both pads.
    rumble = {};
    EXPECT_EQ(synthesize_steam_rumble(make_steam_haptic({0x82, 0x02, 0x02, 0xf2}), rumble),
              STEAM_HAPTIC_LEFT | STEAM_HAPTIC_RIGHT);
    EXPECT_NEAR(rumble.left, 65535 * 0.1995, 64);
    EXPECT_NEAR(rumble.right, 65535 * 0.1995, 64);
    EXPECT_EQ(rumble.left_hold_ms, STEAM_HAPTIC_CLICK_HOLD_MS);
    EXPECT_EQ(rumble.right_hold_ms, STEAM_HAPTIC_CLICK_HOLD_MS);

    // An off on side 2 silences both.
    EXPECT_EQ(synthesize_steam_rumble(make_steam_haptic({0x82, 0x02, 0x00, 0x00}), rumble),
              STEAM_HAPTIC_LEFT | STEAM_HAPTIC_RIGHT);
    EXPECT_EQ(rumble.left, 0);
    EXPECT_EQ(rumble.right, 0);

    // A side the firmware has no pad for renders nothing.
    rumble = {};
    EXPECT_EQ(synthesize_steam_rumble(make_steam_haptic({0x82, 0x03, 0x02, 0xf2}), rumble), 0);
    EXPECT_EQ(rumble.left, 0);
    EXPECT_EQ(rumble.right, 0);
  }

  TEST_F(VhfGamepadPolicyTest, SteamHapticEffectsWithoutARumbleEquivalentAreNotRendered) {
    synthesized_rumble_t rumble {};
    EXPECT_EQ(synthesize_steam_rumble(make_steam_haptic({0x83, 0, 0, 0, 0, 0, 0, 0, 0, 0}), rumble), 0);
    EXPECT_EQ(synthesize_steam_rumble(make_steam_haptic({0x84, 0, 0, 0, 0, 0, 0, 0, 0}), rumble), 0);
    EXPECT_EQ(synthesize_steam_rumble(make_steam_haptic({0x85, 0, 0, 0}), rumble), 0);
    EXPECT_EQ(rumble.left, 0);
    EXPECT_EQ(rumble.right, 0);
  }


  TEST_F(VhfGamepadPolicyTest, TouchPointerKeysSeparatePadsSharingAPointerId) {
    using platf::vhf_gamepad::touch_pointer_key;
    EXPECT_NE(touch_pointer_key(0, 0), touch_pointer_key(1, 0));
    EXPECT_EQ(touch_pointer_key(0, 5), 5u);
    EXPECT_NE(touch_pointer_key(1, 0), touch_pointer_key(0, 1));
  }

  TEST_F(VhfGamepadPolicyTest, SteamSplitPadsKeepExactlyOneOwnerPerHalf) {
    std::map<std::uint64_t, std::uint8_t> pointers;
    const auto send = [&](lvg::touch_event event, std::uint32_t pointer, float x) {
      return platf::vhf_gamepad::map_steam_touch(pointers, static_cast<std::uint8_t>(event), pointer, x);
    };
    EXPECT_EQ(send(lvg::touch_event::down, 10, .2f), 0);
    EXPECT_EQ(send(lvg::touch_event::down, 20, .8f), 1);
    for (std::uint32_t i = 100; i < 10000; ++i) {
      EXPECT_FALSE(send(lvg::touch_event::down, i, .1f));
      EXPECT_FALSE(send(lvg::touch_event::up, i, 0));
    }
    EXPECT_EQ(pointers.size(), 2);
    EXPECT_EQ(send(lvg::touch_event::move, 10, .9f), 0);
    EXPECT_EQ(send(lvg::touch_event::down, 10, .9f), 0);
    EXPECT_EQ(send(lvg::touch_event::up, 20, 0), 1);
    EXPECT_FALSE(send(lvg::touch_event::move, 20, 0));
    EXPECT_EQ(pointers.size(), 1);
    EXPECT_EQ(send(lvg::touch_event::down, 30, .9f), 1);
    EXPECT_EQ(send(lvg::touch_event::cancel_all, 0, 0), 0);
    EXPECT_TRUE(pointers.empty());
    EXPECT_FALSE(send(lvg::touch_event::down, 1, std::numeric_limits<float>::quiet_NaN()));
  }

  TEST_F(VhfGamepadPolicyTest, FeedbackCountersAreNotableOnlyWhenSomethingWasDropped) {
    using platf::vhf_gamepad::feedback_counters_notable;
    const auto counters = [](std::uint8_t haptic, std::uint8_t settings, std::uint8_t transitions, std::uint8_t unknown) {
      return lvg::feedback_counters {haptic, settings, transitions, unknown};
    };
    EXPECT_FALSE(feedback_counters_notable(counters(0, 0, 0, 0)));
    EXPECT_TRUE(feedback_counters_notable(counters(1, 0, 0, 0)));
    EXPECT_TRUE(feedback_counters_notable(counters(0, 1, 0, 0)));
    EXPECT_TRUE(feedback_counters_notable(counters(0, 0, 1, 0)));
    EXPECT_TRUE(feedback_counters_notable(counters(0, 0, 0, 1)));
  }

  TEST_F(VhfGamepadPolicyTest, FeedbackCountersDescribeEveryFieldAndMarkWireSaturation) {
    using platf::vhf_gamepad::describe_feedback_counters;
    EXPECT_EQ(
      describe_feedback_counters(lvg::feedback_counters {0, 0, 0, 0}),
      "haptic_evicted=0 settings_evicted=0 transitions_dropped=0 unknown_commands=0"
    );
    EXPECT_EQ(
      describe_feedback_counters(lvg::feedback_counters {3, 0xFF, 254, 1}),
      "haptic_evicted=3 settings_evicted=255+ transitions_dropped=254 unknown_commands=1"
    );
  }

  // The attributes one wired unit reported on 2026-10-05 (sc26_usb.h captures the same values as
  // the virtual device's defaults, bar the build id the capture left as a placeholder).
  steam_attributes_t captured_steam_attributes(std::uint8_t valid_mask = 0xFF) {
    steam_attributes_t attributes {};
    attributes.transport = LI_SCATTR_TRANSPORT_USB;
    attributes.valid_mask = valid_mask;
    attributes.firmware_build_time = 0x6A4D85E3;
    attributes.bootloader_build_time = 0x68D2F92E;
    attributes.board_revision = 0x4A;
    attributes.capabilities = 0x0003;
    attributes.radio_firmware_build_time = 0x6A4D0000;
    attributes.dongle_firmware_build_time = 0x6A4E0000;
    std::strcpy(attributes.build_id.data(), "26e1f7ef1955");
    std::strcpy(attributes.unit_serial.data(), "FXA1234567890");
    std::strcpy(attributes.board_serial.data(), "MXA0987654321");
    return attributes;
  }

  TEST_F(VhfGamepadPolicyTest, SteamAttributesPacketKeepsTheClientForksLayout) {
    EXPECT_EQ(sizeof(SS_STEAM_CONTROLLER_ATTRIBUTES_PACKET), 84u);
    EXPECT_EQ(sizeof(LI_STEAM_CONTROLLER_ATTRIBUTES), 76u);
    EXPECT_EQ(steam_attributes_driver_mask, 0xCFu);
  }

  TEST_F(VhfGamepadPolicyTest, SteamAttributesAreAcceptedOnlyBySteamSlotsWhoseClientSendsThem) {
    EXPECT_TRUE(accepts_steam_attributes(lvg::profile::steam_controller, LI_CCAP_STEAM_ATTRIBUTES));
    EXPECT_TRUE(accepts_steam_attributes(lvg::profile::steam_controller, LI_CCAP_STEAM_ATTRIBUTES | LI_CCAP_STEAM_SETTINGS | LI_CCAP_GYRO));
    EXPECT_FALSE(accepts_steam_attributes(lvg::profile::steam_controller, 0));
    EXPECT_FALSE(accepts_steam_attributes(lvg::profile::steam_controller, LI_CCAP_STEAM_SETTINGS | LI_CCAP_STEAM_HAPTIC));
    EXPECT_FALSE(accepts_steam_attributes(lvg::profile::dualsense, LI_CCAP_STEAM_ATTRIBUTES));
    EXPECT_FALSE(accepts_steam_attributes(lvg::profile::xbox_series, LI_CCAP_STEAM_ATTRIBUTES));
  }

  TEST_F(VhfGamepadPolicyTest, SteamAttributesRequestPassesTheDriverFieldsFlagsAndAllThrough) {
    const auto request = make_steam_attributes_request(captured_steam_attributes());
    // Radio and dongle build times are logged, never sent: the virtual device is wired.
    EXPECT_EQ(request.flags, 0xCFu);
    EXPECT_EQ(request.firmware_build_time, 0x6A4D85E3u);
    EXPECT_EQ(request.bootloader_build_time, 0x68D2F92Eu);
    EXPECT_EQ(request.board_revision, 0x4Au);
    EXPECT_EQ(request.capabilities, 0x0003u);
    EXPECT_STREQ(request.build_id.data(), "26e1f7ef1955");
    EXPECT_STREQ(request.unit_serial.data(), "FXA1234567890");
    EXPECT_STREQ(request.board_serial.data(), "MXA0987654321");
  }

  TEST_F(VhfGamepadPolicyTest, SteamAttributesRequestZeroesEveryFieldTheClientDidNotRead) {
    const auto request = make_steam_attributes_request(
      captured_steam_attributes(LI_SCATTR_FIRMWARE_BUILD_TIME | LI_SCATTR_RADIO_FW_BUILD_TIME));
    steam_attributes_request_t expected {};
    expected.flags = LI_SCATTR_FIRMWARE_BUILD_TIME;
    expected.firmware_build_time = 0x6A4D85E3;
    EXPECT_EQ(request, expected);

    // The serials flag without a readable serial reaches the driver as an empty string, which it
    // answers by keeping its synthetic per-slot serial; the build id stays out entirely.
    auto attributes = captured_steam_attributes(LI_SCATTR_SERIALS);
    attributes.unit_serial.fill('\0');
    attributes.board_serial.fill('\0');
    const auto serials_only = make_steam_attributes_request(attributes);
    EXPECT_EQ(serials_only.flags, static_cast<std::uint32_t>(LI_SCATTR_SERIALS));
    EXPECT_EQ(serials_only.unit_serial[0], '\0');
    EXPECT_EQ(serials_only.board_serial[0], '\0');
    EXPECT_EQ(serials_only.build_id[0], '\0');
    EXPECT_EQ(serials_only.firmware_build_time, 0u);
  }

  TEST_F(VhfGamepadPolicyTest, SteamAttributesStringsKeepFifteenPrintableAsciiCharacters) {
    auto attributes = captured_steam_attributes();
    // No terminator in the whole field: the request still ends in one.
    std::memcpy(attributes.build_id.data(), "0123456789abcdef", 16);
    // Control, DEL and high bytes become '?', the terminator is honoured.
    const char odd[16] = {'F', 'X', '\t', 'A', static_cast<char>(0x7F), static_cast<char>(0xC3), 'Z', '\0', 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x'};
    std::memcpy(attributes.unit_serial.data(), odd, 16);
    attributes.board_serial.fill('\0');

    const auto request = make_steam_attributes_request(attributes);
    EXPECT_EQ(std::string(request.build_id.data()), "0123456789abcde");
    EXPECT_EQ(request.build_id[15], '\0');
    EXPECT_EQ(std::string(request.unit_serial.data()), "FX?A??Z");
    EXPECT_EQ(request.board_serial[0], '\0');
  }

  TEST_F(VhfGamepadPolicyTest, UnixBuildTimesFormatAsUtcWithoutALocale) {
    EXPECT_EQ(format_unix_time_utc(0x6A4D85E3), "2026-07-07 23:04:03 UTC");
    EXPECT_EQ(format_unix_time_utc(0x68D2F92E), "2025-09-23 19:46:54 UTC");
    EXPECT_EQ(format_unix_time_utc(0), "1970-01-01 00:00:00 UTC");
    EXPECT_EQ(format_unix_time_utc(1000000000), "2001-09-09 01:46:40 UTC");
    EXPECT_EQ(format_unix_time_utc(0xFFFFFFFF), "2106-02-07 06:28:15 UTC");
  }

  TEST_F(VhfGamepadPolicyTest, SteamAttributesDescribeWhatSteamWillSee) {
    EXPECT_EQ(
      describe_steam_attributes(captured_steam_attributes()),
      "real controller firmware build 2026-07-07 23:04:03 UTC (0x6a4d85e3), bootloader 2025-09-23 19:46:54 UTC (0x68d2f92e), "
      "board rev 74, build id 26e1f7ef1955, serial FXA1234567890, via usb, "
      "radio firmware build 2026-07-07 13:32:48 UTC (0x6a4d0000), dongle firmware build 2026-07-08 07:45:04 UTC (0x6a4e0000)"
    );

    auto partial = captured_steam_attributes(LI_SCATTR_FIRMWARE_BUILD_TIME | LI_SCATTR_BUILD_ID);
    partial.transport = LI_SCATTR_TRANSPORT_BLE;
    EXPECT_EQ(
      describe_steam_attributes(partial),
      "real controller firmware build 2026-07-07 23:04:03 UTC (0x6a4d85e3), bootloader unknown, "
      "board rev unknown, build id 26e1f7ef1955, serial unknown, via ble"
    );

    auto empty = captured_steam_attributes(0);
    empty.transport = LI_SCATTR_TRANSPORT_PUCK;
    EXPECT_EQ(
      describe_steam_attributes(empty),
      "real controller firmware build unknown, bootloader unknown, board rev unknown, build id unknown, serial unknown, via puck"
    );

    // A flagged but empty serial reads as unknown; an unknown transport is shown by number.
    auto blank_serial = captured_steam_attributes(LI_SCATTR_SERIALS);
    blank_serial.unit_serial.fill('\0');
    blank_serial.transport = 7;
    EXPECT_EQ(
      describe_steam_attributes(blank_serial),
      "real controller firmware build unknown, bootloader unknown, board rev unknown, build id unknown, serial unknown, via transport 7"
    );
  }
}  // namespace
