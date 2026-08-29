/**
 * @file tests/unit/platform/linux/test_vulkan_encode_policy.cpp
 * @brief Tests for fail-closed Vulkan import and device-selection policy.
 */
#include "../../../tests_common.h"

#include <array>

#include <src/platform/linux/vulkan_encode_policy.h>

TEST(VulkanEncodePolicy, SelectsPreferredCompatibleMemoryType) {
  constexpr std::array<std::uint32_t, 4> properties = {0x1, 0x3, 0x2, 0x7};

  EXPECT_EQ(vk::policy::select_memory_type(0b1110, properties, 0x3, true), 1U);
}

TEST(VulkanEncodePolicy, CompatibleFallbackNeverSelectsUnadvertisedType) {
  constexpr std::array<std::uint32_t, 4> properties = {0x4, 0x4, 0x4, 0x4};

  EXPECT_EQ(vk::policy::select_memory_type(0b0100, properties, 0x1, true), 2U);
  EXPECT_FALSE(vk::policy::select_memory_type(0, properties, 0x1, true).has_value());
}

TEST(VulkanEncodePolicy, StrictMemoryPropertiesDoNotFallback) {
  constexpr std::array<std::uint32_t, 3> properties = {0x1, 0x2, 0x4};

  EXPECT_FALSE(vk::policy::select_memory_type(0b111, properties, 0x3, false).has_value());
}

TEST(VulkanEncodePolicy, ExactCaptureDeviceForbidsDefaultGpuFallback) {
  EXPECT_FALSE(vk::policy::may_fallback_to_default_device(true));
  EXPECT_TRUE(vk::policy::may_fallback_to_default_device(false));
}
