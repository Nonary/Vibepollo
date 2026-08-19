/**
 * @file src/platform/windows/vhf_gamepad_policy.cpp
 * @brief Definitions for translating normalized gamepad state into the VHF driver protocol.
 */
// local includes
#include "vhf_gamepad_policy.h"

// standard includes
#include <cstring>
#include <limits>

namespace platf::vhf_gamepad {

  std::int16_t to_hid_vertical_axis(const std::int16_t value) noexcept {
    if (value == std::numeric_limits<std::int16_t>::min()) {
      return std::numeric_limits<std::int16_t>::max();
    }
    return static_cast<std::int16_t>(-value);
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

    request.left_x = state.left_x;
    request.left_y = to_hid_vertical_axis(state.left_y);
    request.right_x = state.right_x;
    request.right_y = to_hid_vertical_axis(state.right_y);
    request.left_trigger = state.left_trigger;
    request.right_trigger = state.right_trigger;
    return request;
  }

  bool decode_rumble_rgb(const lvg::feedback_event &event, rumble_rgb_t &feedback) noexcept {
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
