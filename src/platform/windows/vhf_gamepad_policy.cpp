/**
 * @file src/platform/windows/vhf_gamepad_policy.cpp
 * @brief Definitions for translating normalized gamepad state into the VHF driver protocol.
 */
// local includes
#include "vhf_gamepad_policy.h"

// standard includes
#include <cmath>
#include <cstring>
#include <limits>

extern "C" {
#include <moonlight-common-c/src/Limelight.h>
}

namespace platf::vhf_gamepad {

  std::optional<lvg::profile> select_automatic_profile(
    const lvg::profile_mask_t available_profiles
  ) noexcept {
    // Xbox Series stays first because it is the only automatic candidate on
    // the XInput path. Generic profiles are intentionally excluded until they
    // have an accepted public USB PID.
    for (const auto candidate : {
           lvg::profile::xbox_series,
           lvg::profile::dualsense,
           lvg::profile::dualshock_4}) {
      if ((available_profiles & lvg::profile_bit(candidate)) != 0) {
        return candidate;
      }
    }
    return std::nullopt;
  }

  lvg::input_state_request make_input_state(
    const std::uint32_t controller_id,
    const normalized_state_t &state
  ) noexcept {
    lvg::input_state_request request {};
    request.header.size = sizeof(request);
    request.header.version = lvg::k_protocol_version;
    request.controller_id = controller_id;

    // The protocol's button bits are defined to match Vibeshine's normalized flags, so the
    // supported bits are a straight copy and the driver owns the HID button/hat encoding.
    request.buttons = state.button_flags & supported_button_mask;

    // Axes go over the wire as Vibeshine's normalized state, positive-up. Each
    // driver profile converts to its own device's convention, and they disagree:
    // HID sticks are positive-down while a DualShock 4's are unsigned. Flipping
    // here as well double-inverted both sticks on every profile that does its own
    // conversion, which is all of them.
    request.left_x = state.left_x;
    request.left_y = state.left_y;
    request.right_x = state.right_x;
    request.right_y = state.right_y;
    request.left_trigger = state.left_trigger;
    request.right_trigger = state.right_trigger;
    return request;
  }

  std::uint8_t to_protocol_touch_event(const std::uint8_t event_type) noexcept {
    switch (event_type) {
      case LI_TOUCH_EVENT_HOVER:
        return static_cast<std::uint8_t>(lvg::touch_event::hover);
      case LI_TOUCH_EVENT_DOWN:
        return static_cast<std::uint8_t>(lvg::touch_event::down);
      case LI_TOUCH_EVENT_UP:
        return static_cast<std::uint8_t>(lvg::touch_event::up);
      case LI_TOUCH_EVENT_MOVE:
        return static_cast<std::uint8_t>(lvg::touch_event::move);
      case LI_TOUCH_EVENT_CANCEL:
        return static_cast<std::uint8_t>(lvg::touch_event::cancel);
      default:
        // An unmapped event is safer read as "release everything" than as a
        // contact the client never reported.
        return static_cast<std::uint8_t>(lvg::touch_event::cancel_all);
    }
  }

  std::uint8_t to_protocol_motion_kind(const std::uint8_t motion_type) noexcept {
    switch (motion_type) {
      case LI_MOTION_TYPE_ACCEL:
        return static_cast<std::uint8_t>(lvg::motion_kind::accelerometer);
      case LI_MOTION_TYPE_GYRO:
        return static_cast<std::uint8_t>(lvg::motion_kind::gyroscope);
      default:
        return 0;
    }
  }

  std::uint8_t to_protocol_battery_state(const std::uint8_t state) noexcept {
    switch (state) {
      case LI_BATTERY_STATE_NOT_PRESENT:
        return static_cast<std::uint8_t>(lvg::battery_state::not_present);
      case LI_BATTERY_STATE_DISCHARGING:
        return static_cast<std::uint8_t>(lvg::battery_state::discharging);
      case LI_BATTERY_STATE_CHARGING:
        return static_cast<std::uint8_t>(lvg::battery_state::charging);
      case LI_BATTERY_STATE_FULL:
        return static_cast<std::uint8_t>(lvg::battery_state::full);
      case LI_BATTERY_STATE_NOT_CHARGING:
        return static_cast<std::uint8_t>(lvg::battery_state::not_charging);
      default:
        return static_cast<std::uint8_t>(lvg::battery_state::unknown);
    }
  }

  std::uint16_t to_normalized_touch(const float value) noexcept {
    if (!(value > 0.0f)) {  // Also catches NaN.
      return 0;
    }
    if (value >= 1.0f) {
      return 65535;
    }
    return static_cast<std::uint16_t>(value * 65535.0f);
  }

  std::int32_t to_milli_units(const float value) noexcept {
    if (std::isnan(value)) {
      return 0;
    }
    const float scaled = value * 1000.0f;
    if (scaled >= 2147483000.0f) {
      return 2147483000;
    }
    if (scaled <= -2147483000.0f) {
      return -2147483000;
    }
    return static_cast<std::int32_t>(scaled);
  }

  bool decode_rumble_rgb(const lvg::feedback_event &event, rumble_rgb_t &feedback) noexcept {
    if (event.type == lvg::feedback_type::playstation_output) {
      if (event.payload_size != sizeof(lvg::playstation_output_feedback)) {
        return false;
      }

      lvg::playstation_output_feedback payload {};
      std::memcpy(&payload, event.payload, sizeof(payload));

      feedback = {};
      feedback.low_frequency = payload.low_frequency;
      feedback.high_frequency = payload.high_frequency;

      if (payload.valid & lvg::ps_output_lightbar_valid) {
        feedback.red = payload.red;
        feedback.green = payload.green;
        feedback.blue = payload.blue;
        feedback.has_rgb = true;
      }

      if (payload.valid & lvg::ps_output_triggers_valid) {
        feedback.left_effect.mode = payload.left_trigger.mode;
        feedback.right_effect.mode = payload.right_trigger.mode;
        std::memcpy(feedback.left_effect.parameters.data(), payload.left_trigger.parameters,
                    feedback.left_effect.parameters.size());
        std::memcpy(feedback.right_effect.parameters.data(), payload.right_trigger.parameters,
                    feedback.right_effect.parameters.size());
        // Both triggers travel in one report, so both are always current.
        feedback.trigger_event_flags = 0x03;
        feedback.has_trigger_effects = true;
      }
      return true;
    }

    if (event.type == lvg::feedback_type::xbox_rumble) {
      if (event.payload_size != sizeof(lvg::xbox_rumble_feedback)) {
        return false;
      }

      lvg::xbox_rumble_feedback payload {};
      std::memcpy(&payload, event.payload, sizeof(payload));

      feedback.low_frequency = payload.low_frequency;
      feedback.high_frequency = payload.high_frequency;
      feedback.left_trigger = payload.left_trigger;
      feedback.right_trigger = payload.right_trigger;
      feedback.has_triggers = true;
      feedback.has_rgb = false;
      feedback.red = 0;
      feedback.green = 0;
      feedback.blue = 0;
      return true;
    }

    const bool has_rgb = event.type == lvg::feedback_type::generic_rumble_rgb;
    const bool rumble_only = event.type == lvg::feedback_type::generic_rumble;
    if ((!has_rgb && !rumble_only) ||
        event.payload_size != sizeof(lvg::generic_rumble_rgb_feedback)) {
      return false;
    }

    lvg::generic_rumble_rgb_feedback payload {};
    std::memcpy(&payload, event.payload, sizeof(payload));

    feedback.low_frequency = payload.low_frequency;
    feedback.high_frequency = payload.high_frequency;
    feedback.red = has_rgb ? payload.red : 0;
    feedback.green = has_rgb ? payload.green : 0;
    feedback.blue = has_rgb ? payload.blue : 0;
    feedback.has_rgb = has_rgb;
    feedback.left_trigger = 0;
    feedback.right_trigger = 0;
    feedback.has_triggers = false;
    return true;
  }

}  // namespace platf::vhf_gamepad
