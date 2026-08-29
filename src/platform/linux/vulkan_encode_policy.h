/**
 * @file src/platform/linux/vulkan_encode_policy.h
 * @brief Pure safety policy helpers for Vulkan device-memory selection.
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace vk::policy {

  /**
   * @brief Select a memory type that is both advertised by Vulkan and usable by the caller.
   *
   * A missing preferred property may be tolerated for imported device memory, but a type bit
   * that Vulkan did not advertise is never selected. Host-mapped allocations must set
   * allow_compatible_fallback to false so HOST_VISIBLE/HOST_COHERENT remain mandatory.
   */
  [[nodiscard]] constexpr std::optional<std::uint32_t> select_memory_type(
    std::uint32_t compatible_type_bits,
    std::span<const std::uint32_t> property_flags,
    std::uint32_t preferred_properties,
    bool allow_compatible_fallback
  ) noexcept {
    const auto count = std::min<std::size_t>(property_flags.size(), 32);
    for (std::size_t index = 0; index < count; ++index) {
      const auto bit = std::uint32_t {1} << index;
      if ((compatible_type_bits & bit) != 0 &&
          (property_flags[index] & preferred_properties) == preferred_properties) {
        return static_cast<std::uint32_t>(index);
      }
    }

    if (allow_compatible_fallback) {
      for (std::size_t index = 0; index < count; ++index) {
        if ((compatible_type_bits & (std::uint32_t {1} << index)) != 0) {
          return static_cast<std::uint32_t>(index);
        }
      }
    }

    return std::nullopt;
  }

  /** Exact capture-GPU identities must never fall back to FFmpeg's default adapter. */
  [[nodiscard]] constexpr bool may_fallback_to_default_device(bool exact_capture_device_required) noexcept {
    return !exact_capture_device_required;
  }

}  // namespace vk::policy
