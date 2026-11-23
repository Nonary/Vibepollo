// SPDX-License-Identifier: GPL-3.0-or-later
#include "src/dualsense_controls_protocol.h"
#if defined(_WIN32) || defined(DUALSENSE_CONTROLS_TEST_WITH_WIN_STUBS)
  #include "src/platform/windows/dualsense_usbip_gamepad_policy.h"
#endif

#include <gtest/gtest.h>

#include <array>
#include <cstring>

#if defined(_WIN32) || defined(DUALSENSE_CONTROLS_TEST_WITH_WIN_STUBS)
namespace {
  std::array<std::uint8_t, sizeof(lvg::driver::ds5_output_report)> make_report(
    const lvg::driver::ds5_output_report &report
  ) {
    std::array<std::uint8_t, sizeof(lvg::driver::ds5_output_report)> bytes {};
    std::memcpy(bytes.data(), &report, sizeof(report));
    return bytes;
  }
}  // namespace

TEST(DualSenseControls, OutputStateExtractsAllExtendedControlsAndDeduplicates) {
  platf::dualsense_usbip_gamepad::output_state state;

  lvg::driver::ds5_output_report report {};
  report.report_id = lvg::driver::k_ds5_output_report_id;
  report.valid_flag0 = 0x10 | 0x20;  // headphone + speaker volume
  report.valid_flag1 = lvg::driver::k_ds5_flag1_mic_mute_led | lvg::driver::k_ds5_flag1_player_indicator;
  report.valid_flag2 = 0x01;  // LED brightness
  report.reserved0[0] = 0x5A;  // headphone volume
  report.reserved0[1] = 0x4B;  // speaker volume
  report.mute_button_led = 2;  // pulsing
  report.led_brightness = 1;  // medium
  report.player_leds = 0x20 | 0x15;  // instant bit + 5-LED mask 0x15

  const auto first = state.apply(make_report(report));
  ASSERT_TRUE(first.accepted);
  EXPECT_EQ(first.ds_controls.valid_mask, dualsense_controls::out_all_mask);
  EXPECT_EQ(first.ds_controls.player_leds, 0x15);
  EXPECT_EQ(first.ds_controls.led_brightness, 1);
  EXPECT_EQ(first.ds_controls.mic_led, 2);
  EXPECT_EQ(first.ds_controls.speaker_volume, 0x4B);
  EXPECT_EQ(first.ds_controls.headphone_volume, 0x5A);

  // Repeating identical values produces no redundant update.
  const auto repeat = state.apply(make_report(report));
  ASSERT_TRUE(repeat.accepted);
  EXPECT_EQ(repeat.ds_controls.valid_mask, 0);

  // Changing only mic LED emits only the mic LED valid bit while keeping sticky state.
  lvg::driver::ds5_output_report mic_only {};
  mic_only.report_id = lvg::driver::k_ds5_output_report_id;
  mic_only.valid_flag1 = lvg::driver::k_ds5_flag1_mic_mute_led;
  mic_only.mute_button_led = 0;
  const auto second = state.apply(make_report(mic_only));
  ASSERT_TRUE(second.accepted);
  EXPECT_EQ(second.ds_controls.valid_mask, dualsense_controls::out_mic_led);
  EXPECT_EQ(second.ds_controls.mic_led, 0);

  const auto sticky = state.current_ds_controls();
  EXPECT_EQ(sticky.valid_mask, dualsense_controls::out_all_mask);
  EXPECT_EQ(sticky.player_leds, 0x15);
  EXPECT_EQ(sticky.led_brightness, 1);
  EXPECT_EQ(sticky.mic_led, 0);
  EXPECT_EQ(sticky.speaker_volume, 0x4B);
  EXPECT_EQ(sticky.headphone_volume, 0x5A);
}
#endif

TEST(DualSenseControls, EncodeOutputRequiresFeatureFlagAndValidFields) {
  dualsense_controls::output_payload payload {};
  payload.valid_mask = dualsense_controls::out_all_mask;
  payload.player_leds = 0x04;
  payload.led_brightness = 2;
  payload.mic_led = 1;
  payload.speaker_volume = 80;
  payload.headphone_volume = 100;

  // Missing session feature flag -> fail closed.
  EXPECT_FALSE(dualsense_controls::encode_output(0x0F, 0, payload));
  // Valid session feature flag -> 16-byte frame (4-byte header + 12-byte payload).
  const auto wire = dualsense_controls::encode_output(dualsense_controls::session_feature, 3, payload);
  ASSERT_TRUE(wire);
  EXPECT_EQ(wire->size(), 16u);
  EXPECT_EQ((*wire)[0], 0x06);
  EXPECT_EQ((*wire)[1], 0x55);
  EXPECT_EQ((*wire)[2], 12);
  EXPECT_EQ((*wire)[3], 0);
  EXPECT_EQ((*wire)[4], 1);
  EXPECT_EQ((*wire)[6], 3);
  EXPECT_EQ((*wire)[8], 0x1F);
  EXPECT_EQ((*wire)[10], 0x04);
  EXPECT_EQ((*wire)[11], 2);
  EXPECT_EQ((*wire)[12], 1);
  EXPECT_EQ((*wire)[13], 80);
  EXPECT_EQ((*wire)[14], 100);

  // Out-of-range fields rejected.
  payload.headphone_volume = 0x80;
  EXPECT_FALSE(dualsense_controls::encode_output(dualsense_controls::session_feature, 0, payload));
  payload.headphone_volume = 100;
  payload.led_brightness = 3;
  EXPECT_FALSE(dualsense_controls::encode_output(dualsense_controls::session_feature, 0, payload));
}
