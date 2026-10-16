// SPDX-License-Identifier: GPL-3.0-or-later
#include "src/platform/windows/dualsense_usbip_gamepad_policy.h"

#include <gtest/gtest.h>

namespace {
  using namespace platf::dualsense_usbip_gamepad;
  using namespace std::chrono_literals;

  std::span<const std::uint8_t> bytes(const lvg::driver::ds5_output_report &report) {
    return {reinterpret_cast<const std::uint8_t *>(&report), sizeof(report)};
  }
}  // namespace

TEST(DualSenseUsbipGamepadPolicyTests, FeedbackHonorsValidityAndRetainsOmittedTriggerPrograms) {
  output_state state;
  lvg::driver::ds5_output_report report {};
  report.report_id = 2;
  report.motor_left = 100;
  report.lightbar_red = 50;
  auto update = state.apply(bytes(report));
  EXPECT_TRUE(update.accepted);
  EXPECT_FALSE(update.rumble);
  EXPECT_FALSE(update.rgb);
  EXPECT_EQ(update.triggers, 0);

  report.valid_flag0 = lvg::driver::k_ds5_flag0_left_trigger_effect;
  report.left_trigger.mode = 0x21;
  report.left_trigger.parameters[7] = 42;
  update = state.apply(bytes(report));
  EXPECT_EQ(update.triggers, lvg::driver::k_ds5_flag0_left_trigger_effect);
  EXPECT_EQ(update.state.left_trigger.parameters[7], 42);
  EXPECT_EQ(state.apply(bytes(report)).triggers, 0);

  report.valid_flag0 = lvg::driver::k_ds5_flag0_right_trigger_effect;
  report.right_trigger.mode = 0x26;
  report.right_trigger.parameters[0] = 88;
  // Invalid left bytes cannot overwrite the previously programmed left side.
  report.left_trigger = {};
  update = state.apply(bytes(report));
  EXPECT_EQ(update.triggers, lvg::driver::k_ds5_flag0_right_trigger_effect);
  EXPECT_EQ(update.state.left_trigger.mode, 0x21);
  EXPECT_EQ(update.state.left_trigger.parameters[7], 42);

  report.valid_flag0 = lvg::driver::k_ds5_flag0_left_trigger_effect;
  update = state.apply(bytes(report));
  EXPECT_EQ(update.triggers, lvg::driver::k_ds5_flag0_left_trigger_effect);
  EXPECT_EQ(update.state.left_trigger.mode, 0);
  EXPECT_EQ(update.state.right_trigger.mode, 0x26);

  report.valid_flag0 = 0;
  report.valid_flag2 = lvg::driver::k_ds5_flag2_compatible_vibration;
  report.valid_flag1 = lvg::driver::k_ds5_flag1_lightbar;
  update = state.apply(bytes(report));
  EXPECT_TRUE(update.rumble);
  EXPECT_EQ(update.state.low_frequency, 100 << 8);
  EXPECT_TRUE(update.rgb);
  EXPECT_EQ(update.state.red, 50);
  EXPECT_FALSE(state.apply(bytes(report)).rumble);
  EXPECT_FALSE(state.apply(bytes(report)).rgb);

  report.valid_flag2 = 0;
  report.valid_flag1 = 0;
  report.motor_left = 0;
  report.lightbar_red = 0;
  update = state.apply(bytes(report));
  EXPECT_FALSE(update.rumble);
  EXPECT_FALSE(update.rgb);
  EXPECT_EQ(update.state.low_frequency, 100 << 8);
  EXPECT_EQ(update.state.red, 50);
}

TEST(DualSenseUsbipGamepadPolicyTests, RejectsWrongReportAndTruncatedRgbPayload) {
  output_state state;
  lvg::driver::ds5_output_report report {};
  report.report_id = 2;
  report.valid_flag1 = lvg::driver::k_ds5_flag1_lightbar;
  EXPECT_FALSE(state.apply(bytes(report).first(47)).accepted);
  report.report_id = 0x31;
  EXPECT_FALSE(state.apply(bytes(report)).accepted);
}

TEST(DualSenseUsbipGamepadPolicyTests, PacketizerPreservesFrameBytesAcrossFragmentedUsbTransfers) {
  pcm_packetizer packetizer;
  std::vector<std::array<std::uint8_t, 960>> packets;
  const auto emit = [&](const auto &packet) {
    packets.push_back(packet);
  };
  std::array<std::uint8_t, 1920> samples;
  for (std::size_t i = 0; i < samples.size(); ++i) {
    samples[i] = static_cast<std::uint8_t>(i);
  }
  const auto now = std::chrono::steady_clock::now();
  packetizer.push(std::span {samples}.first(192), now, emit);
  EXPECT_TRUE(packets.empty());
  packetizer.push(std::span {samples}.subspan(192), now + 1ms, emit);
  ASSERT_EQ(packets.size(), 2);
  EXPECT_TRUE(std::equal(packets[0].begin(), packets[0].end(), samples.begin()));
  EXPECT_TRUE(std::equal(packets[1].begin(), packets[1].end(), samples.begin() + 960));
}

TEST(DualSenseUsbipGamepadPolicyTests, PacketizerDiscardsPartialFramesAfterStallAndReset) {
  pcm_packetizer packetizer;
  std::vector<std::array<std::uint8_t, 960>> packets;
  const auto emit = [&](const auto &packet) {
    packets.push_back(packet);
  };
  std::array<std::uint8_t, 960> old_samples;
  old_samples.fill(0x55);
  std::array<std::uint8_t, 960> fresh_samples;
  fresh_samples.fill(0xAA);
  const auto now = std::chrono::steady_clock::now();
  packetizer.push(std::span {old_samples}.first(192), now, emit);
  packetizer.push(fresh_samples, now + 51ms, emit);
  ASSERT_EQ(packets.size(), 1);
  EXPECT_EQ(packets[0], fresh_samples);
  packetizer.push(std::span {old_samples}.first(192), now + 52ms, emit);
  packetizer.reset();
  packetizer.push(fresh_samples, now + 53ms, emit);
  ASSERT_EQ(packets.size(), 2);
  EXPECT_EQ(packets[1], fresh_samples);
}

TEST(DualSenseUsbipGamepadPolicyTests, MalformedStereoPayloadCannotShiftChannelAlignment) {
  pcm_packetizer packetizer;
  std::vector<std::array<std::uint8_t, 960>> packets;
  const auto emit = [&](const auto &packet) {
    packets.push_back(packet);
  };
  std::array<std::uint8_t, 960> samples {};
  const auto now = std::chrono::steady_clock::now();
  packetizer.push(std::span {samples}.first(192), now, emit);
  packetizer.push(std::span {samples}.first(3), now + 1ms, emit);
  packetizer.push(std::span {samples}.first(768), now + 2ms, emit);
  EXPECT_TRUE(packets.empty());
  packetizer.push(std::span {samples}.first(192), now + 3ms, emit);
  ASSERT_EQ(packets.size(), 1);
  EXPECT_EQ(packets[0], samples);
}
