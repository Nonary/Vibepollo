/**
 * @file src/platform/windows/vhf_gamepad_policy.h
 * @brief Declarations for translating normalized gamepad state into the VHF driver protocol.
 */
#pragma once

// standard includes
#include <cstdint>

// lib includes
#include <libvirtualgamepad/protocol.h>

namespace platf::vhf_gamepad {

  /**
   * @brief Vibeshine's normalized controller state, copied field-for-field out of `gamepad_state_t`.
   * @details Keeping this struct free of platform headers lets the translation be tested on its own.
   */
  struct normalized_state_t {
    std::uint32_t button_flags {};
    std::uint8_t left_trigger {};
    std::uint8_t right_trigger {};
    std::int16_t left_x {};
    std::int16_t left_y {};
    std::int16_t right_x {};
    std::int16_t right_y {};
  };

  /**
   * @brief A decoded rumble feedback report from the driver.
   * @details `has_rgb` is false for force-feedback events. A DirectInput effect says nothing
   *          about a light, so forwarding its zeroed colour channels would switch off the LED on
   *          the client's real controller.
   */
  struct rumble_rgb_t {
    std::uint16_t low_frequency {};
    std::uint16_t high_frequency {};
    std::uint8_t red {};
    std::uint8_t green {};
    std::uint8_t blue {};
    bool has_rgb {};

    // Xbox controllers have impulse triggers. The driver sends all four
    // actuators in one event because it keeps a single pending feedback slot,
    // so splitting them would let coalescing drop one.
    std::uint16_t left_trigger {};
    std::uint16_t right_trigger {};
    bool has_triggers {};

    bool operator==(const rumble_rgb_t &other) const noexcept {
      return low_frequency == other.low_frequency &&
             high_frequency == other.high_frequency &&
             red == other.red && green == other.green && blue == other.blue &&
             has_rgb == other.has_rgb &&
             left_trigger == other.left_trigger &&
             right_trigger == other.right_trigger &&
             has_triggers == other.has_triggers;
    }
  };

  /**
   * @brief Every button the protocol can carry.
   * @details Bits outside this mask are dropped instead of being sent as unspecified wire state.
   */
  inline constexpr std::uint32_t supported_button_mask =
    lvg::button_mask::dpad_up | lvg::button_mask::dpad_down |
    lvg::button_mask::dpad_left | lvg::button_mask::dpad_right |
    lvg::button_mask::start | lvg::button_mask::back |
    lvg::button_mask::left_stick | lvg::button_mask::right_stick |
    lvg::button_mask::left_shoulder | lvg::button_mask::right_shoulder |
    lvg::button_mask::home |
    lvg::button_mask::south | lvg::button_mask::east |
    lvg::button_mask::west | lvg::button_mask::north |
    lvg::button_mask::paddle_1 | lvg::button_mask::paddle_2 |
    lvg::button_mask::paddle_3 | lvg::button_mask::paddle_4 |
    lvg::button_mask::touchpad | lvg::button_mask::misc;

  /**
   * @brief Converts a vertical stick axis to the HID sign convention.
   * @details Vibeshine's normalized state is positive-up (the XInput convention Moonlight sends),
   *          while HID Generic Desktop Y and Ry are positive-down. `INT16_MIN` has no positive
   *          counterpart, so it saturates instead of wrapping back to itself.
   * @param value The normalized axis value.
   * @return The axis value in HID orientation.
   */
  [[nodiscard]] std::int16_t to_hid_vertical_axis(std::int16_t value) noexcept;

  /**
   * @brief Builds a protocol input report for a controller.
   * @param controller_id The driver-side controller slot.
   * @param state The normalized controller state.
   * @return A fully populated request the client can submit as-is.
   */
  [[nodiscard]] lvg::input_state_request make_input_state(
    std::uint32_t controller_id,
    const normalized_state_t &state
  ) noexcept;

  /**
   * @brief Decodes a driver feedback event into rumble and RGB values.
   * @details Accepts both the rumble/RGB report a vendor output report produces and the
   *          rumble-only report a DirectInput force-feedback effect produces.
   * @param event The event returned by the driver.
   * @param feedback Receives the decoded values when the event carries rumble.
   * @return `true` when `feedback` was populated.
   */
  [[nodiscard]] bool decode_rumble_rgb(const lvg::feedback_event &event, rumble_rgb_t &feedback) noexcept;

}  // namespace platf::vhf_gamepad
