/**
 * @file src/platform/linux/hdr_policy.h
 * @brief Pure HDR capability and metadata policy for Linux private displays.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace platf::linux_hdr {

  /**
   * KScreen can publish the requested HDR bit before KWin has committed the
   * matching output color pipeline. Require several consecutive observations
   * before treating an externally applied output state as capture-ready.
   */
  struct output_state_stabilizer_t {
    static constexpr std::size_t required_observations = 3;

    std::size_t matching_observations {};

    [[nodiscard]] constexpr bool observe(const bool matches) noexcept {
      matching_observations = matches ? matching_observations + 1 : 0;
      return matching_observations >= required_observations;
    }
  };

  /** A newly published connector may retain a stale true bit from its prior lease. */
  [[nodiscard]] constexpr bool requires_hdr_rearm(
    const std::optional<bool> command,
    const bool newly_connected
  ) noexcept {
    return newly_connected && command.value_or(false);
  }

  struct output_state_policy_t {
    // Empty means the display-device policy explicitly requested no HDR
    // change (dd_hdr_option=disabled). It must never be reinterpreted as the
    // client's raw HDR request.
    std::optional<bool> command;
    bool expected_enabled;
  };

  /** Only HDR activation needs a separate transaction after the modeset. */
  [[nodiscard]] constexpr bool requires_post_modeset_transaction(const std::optional<bool> command) noexcept {
    return command.value_or(false);
  }

  /** Rearming may share the modeset, but both results must precede HDR activation. */
  [[nodiscard]] constexpr bool ready_for_hdr_activation(
    const bool mode_matches,
    const bool rearm_required,
    const bool hdr_enabled
  ) noexcept {
    return mode_matches && (!rearm_required || !hdr_enabled);
  }

  /** Resolve a parsed display-device HDR action against connector capability. */
  [[nodiscard]] constexpr output_state_policy_t resolve_output_state(
    const std::optional<bool> parsed_state,
    const bool capable,
    const bool current_enabled
  ) noexcept {
    if (!parsed_state) {
      return {.command = std::nullopt, .expected_enabled = capable && current_enabled};
    }
    if (*parsed_state && capable) {
      return {.command = true, .expected_enabled = true};
    }
    if (!*parsed_state && capable) {
      return {.command = false, .expected_enabled = false};
    }
    return {.command = std::nullopt, .expected_enabled = false};
  }

  struct chromaticity_t {
    std::uint16_t x;
    std::uint16_t y;

    constexpr bool operator==(const chromaticity_t &) const = default;
  };

  struct mastering_metadata_t {
    std::array<chromaticity_t, 3> display_primaries;
    chromaticity_t white_point;
    std::uint16_t max_display_luminance;
    std::uint16_t min_display_luminance;
    std::uint16_t max_content_light_level;
    std::uint16_t max_frame_average_light_level;
    std::uint16_t max_full_frame_luminance;
  };

  // These values mirror Vibeshine's private-display CTA EDID: an approximately
  // 1000-nit desired peak and approximately 590-nit full-frame luminance.
  inline constexpr mastering_metadata_t private_display_mastering_metadata {
    .display_primaries = {{{35400, 14600}, {8500, 39850}, {6550, 2300}}},
    .white_point = {15635, 16450},
    .max_display_luminance = 1000,
    .min_display_luminance = 1,
    .max_content_light_level = 0,
    .max_frame_average_light_level = 0,
    .max_full_frame_luminance = 590,
  };

  // PipeWire currently exposes the transfer characteristics but not the
  // physical monitor's mastering metadata. Preserve the historical generic
  // fallback for non-private sources instead of attributing Vibeshine's EDID
  // luminance contract to every HDR monitor.
  inline constexpr mastering_metadata_t generic_pipewire_mastering_metadata {
    .display_primaries = {{{35400, 14600}, {8500, 39850}, {6550, 2300}}},
    .white_point = {15635, 16450},
    .max_display_luminance = 4000,
    .min_display_luminance = 1,
    .max_content_light_level = 0,
    .max_frame_average_light_level = 0,
    .max_full_frame_luminance = 0,
  };

  [[nodiscard]] constexpr const mastering_metadata_t &pipewire_mastering_metadata(
    const bool private_display
  ) noexcept {
    return private_display ? private_display_mastering_metadata : generic_pipewire_mastering_metadata;
  }

  /** KMS capture supports HDR10/PQ, not traditional-gamma HDR or HLG. */
  [[nodiscard]] constexpr bool is_hdr10_eotf(const std::uint8_t eotf) noexcept {
    return eotf == 2;  // HDMI_EOTF_SMPTE_ST2084
  }

  /** Return true when an EDID advertises PQ and static metadata type 1. */
  [[nodiscard]] constexpr bool edid_supports_hdr10(const std::span<const std::uint8_t> edid) {
    constexpr std::size_t block_size = 128;
    constexpr std::uint8_t cta_extension_tag = 0x02;
    constexpr std::uint8_t extended_data_block_tag = 0x07;
    constexpr std::uint8_t hdr_static_metadata_tag = 0x06;
    constexpr std::uint8_t pq_eotf = 1u << 2;
    constexpr std::uint8_t static_metadata_type_1 = 1u << 0;
    constexpr std::array<std::uint8_t, 8> edid_header {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};

    if (edid.size() < block_size || !std::ranges::equal(edid_header, edid.first(edid_header.size()))) {
      return false;
    }
    const auto checksum_valid = [](const std::span<const std::uint8_t> block) {
      std::uint8_t sum = 0;
      for (const auto value : block) {
        sum = static_cast<std::uint8_t>(sum + value);
      }
      return sum == 0;
    };
    if (!checksum_valid(edid.first(block_size))) {
      return false;
    }
    const auto extension_count = std::min<std::size_t>(edid[126], edid.size() / block_size - 1);
    for (std::size_t extension = 0; extension < extension_count; ++extension) {
      const auto offset = block_size * (extension + 1);
      const auto block = edid.subspan(offset, block_size);
      if (block[0] != cta_extension_tag || !checksum_valid(block)) {
        continue;
      }
      const auto data_block_end = block[2] == 0 ? 127u : std::min<unsigned int>(block[2], 127u);
      for (std::size_t cursor = 4; cursor < data_block_end;) {
        const auto header = block[cursor];
        const auto tag = header >> 5;
        const auto length = header & 0x1f;
        const auto next = cursor + 1 + length;
        if (next > data_block_end) {
          break;
        }
        if (tag == extended_data_block_tag && length >= 3 &&
            block[cursor + 1] == hdr_static_metadata_tag &&
            (block[cursor + 2] & pq_eotf) != 0 &&
            (block[cursor + 3] & static_metadata_type_1) != 0) {
          return true;
        }
        cursor = next;
      }
    }
    return false;
  }

}  // namespace platf::linux_hdr
