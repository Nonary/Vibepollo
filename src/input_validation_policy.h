/**
 * @file src/input_validation_policy.h
 * @brief Portable input packet and touch-coordinate validation policies.
 */
#pragma once

// standard includes
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace input::validation {
  struct validated_packet_t {
    std::uint32_t magic = 0;
    std::size_t total_size = 0;
  };

  struct packet_size_bounds_t {
    std::size_t min_total_size = 0;
    std::size_t max_total_size = 0;
  };

  enum class packet_validation_error_e {
    none,
    short_header,
    declared_size_mismatch,
    unknown_magic,
    invalid_size,
  };

  struct packet_validation_result_t {
    std::optional<validated_packet_t> packet;
    packet_validation_error_e error = packet_validation_error_e::none;
    std::uint32_t magic = 0;
    std::size_t total_size = 0;
    packet_size_bounds_t expected_size;

    explicit operator bool() const {
      return packet.has_value();
    }
  };

  /**
   * @brief Validate an input packet's declared size, magic, and packet-size bounds.
   * @param input_data Complete input packet bytes.
   * @return Validation details, including the parsed packet when valid.
   */
  packet_validation_result_t validate_packet(const std::vector<std::uint8_t> &input_data);

  struct touch_port_t {
    int offset_x = 0;
    int offset_y = 0;
    int width = 0;
    int height = 0;
    float scalar_inv = 0.0f;
    float scalar_tpcoords = 0.0f;
  };

  struct normalized_touch_port_t {
    int offset_x = 0;
    int offset_y = 0;
    int width = 0;
    int height = 0;
  };

  /**
   * @brief Normalize coordinates to monitor-local logical touch dimensions.
   * @param touch_port Portable touch-port metadata.
   * @param coords The in/out coordinate pair to normalize.
   * @return Monitor-local dimensions, or std::nullopt if dimensions are invalid.
   */
  std::optional<normalized_touch_port_t> normalize_touch_port(const touch_port_t &touch_port, std::pair<float, float> &coords);
}  // namespace input::validation
