/**
 * @file tests/unit/platform/windows/test_vhf_gamepad_policy.cpp
 * @brief Test the translation between normalized gamepad state and the VHF driver protocol.
 */
#include "../../../tests_common.h"

#include <cstring>
#include <limits>

#include <src/platform/windows/vhf_gamepad_policy.h>

namespace {

  using platf::vhf_gamepad::decode_rumble_rgb;
  using platf::vhf_gamepad::make_input_state;
  using platf::vhf_gamepad::normalized_state_t;
  using platf::vhf_gamepad::rumble_rgb_t;
  using platf::vhf_gamepad::supported_button_mask;
  using platf::vhf_gamepad::to_hid_vertical_axis;

  lvg::feedback_event make_rumble_event(const lvg::generic_rumble_rgb_feedback &payload) {
    lvg::feedback_event event {};
    event.header.size = sizeof(event);
    event.header.version = lvg::k_protocol_version;
    event.controller_id = 3;
    event.type = lvg::feedback_type::generic_rumble_rgb;
    event.payload_size = sizeof(payload);
    std::memcpy(event.payload, &payload, sizeof(payload));
    return event;
  }

  class VhfGamepadPolicyTest: public testing::Test {};

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

  TEST_F(VhfGamepadPolicyTest, VerticalAxesFlipToTheHidConvention) {
    // Vibeshine reports stick-up as positive; HID Y and Ry are positive-down, so pushing a stick
    // up has to reach the driver as a negative value or every game reads the stick inverted.
    normalized_state_t state {};
    state.left_y = 20000;
    state.right_y = -8000;

    const auto request = make_input_state(0, state);
    EXPECT_EQ(request.left_y, -20000);
    EXPECT_EQ(request.right_y, 8000);
  }

  TEST_F(VhfGamepadPolicyTest, VerticalAxisSaturatesInsteadOfWrapping) {
    constexpr auto min_axis = std::numeric_limits<std::int16_t>::min();
    constexpr auto max_axis = std::numeric_limits<std::int16_t>::max();

    EXPECT_EQ(to_hid_vertical_axis(min_axis), max_axis);
    EXPECT_EQ(to_hid_vertical_axis(max_axis), static_cast<std::int16_t>(-max_axis));
    EXPECT_EQ(to_hid_vertical_axis(0), 0);
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
