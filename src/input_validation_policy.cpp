/**
 * @file src/input_validation_policy.cpp
 * @brief Definitions for portable input packet and touch-coordinate policies.
 */
#include "input_validation_policy.h"

extern "C" {
#include <moonlight-common-c/src/Input.h>
}

namespace input::validation {
  namespace {
    std::optional<std::uint32_t> read_u32_be(const std::vector<std::uint8_t> &input_data, const std::size_t offset) {
      if (offset > input_data.size() || input_data.size() - offset < sizeof(std::uint32_t)) {
        return std::nullopt;
      }

      return (static_cast<std::uint32_t>(input_data[offset]) << 24) |
             (static_cast<std::uint32_t>(input_data[offset + 1]) << 16) |
             (static_cast<std::uint32_t>(input_data[offset + 2]) << 8) |
             static_cast<std::uint32_t>(input_data[offset + 3]);
    }

    std::optional<std::uint32_t> read_u32_le(const std::vector<std::uint8_t> &input_data, const std::size_t offset) {
      if (offset > input_data.size() || input_data.size() - offset < sizeof(std::uint32_t)) {
        return std::nullopt;
      }

      return static_cast<std::uint32_t>(input_data[offset]) |
             (static_cast<std::uint32_t>(input_data[offset + 1]) << 8) |
             (static_cast<std::uint32_t>(input_data[offset + 2]) << 16) |
             (static_cast<std::uint32_t>(input_data[offset + 3]) << 24);
    }

    std::optional<packet_size_bounds_t> packet_size_bounds(const std::uint32_t magic) {
      switch (magic) {
        case MOUSE_MOVE_REL_MAGIC_GEN5:
          return packet_size_bounds_t {sizeof(NV_REL_MOUSE_MOVE_PACKET), sizeof(NV_REL_MOUSE_MOVE_PACKET)};
        case MOUSE_MOVE_ABS_MAGIC:
          return packet_size_bounds_t {sizeof(NV_ABS_MOUSE_MOVE_PACKET), sizeof(NV_ABS_MOUSE_MOVE_PACKET)};
        case MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5:
        case MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5:
          return packet_size_bounds_t {sizeof(NV_MOUSE_BUTTON_PACKET), sizeof(NV_MOUSE_BUTTON_PACKET)};
        case SCROLL_MAGIC_GEN5:
          return packet_size_bounds_t {sizeof(NV_SCROLL_PACKET), sizeof(NV_SCROLL_PACKET)};
        case SS_HSCROLL_MAGIC:
          return packet_size_bounds_t {sizeof(SS_HSCROLL_PACKET), sizeof(SS_HSCROLL_PACKET)};
        case KEY_DOWN_EVENT_MAGIC:
        case KEY_UP_EVENT_MAGIC:
          return packet_size_bounds_t {sizeof(NV_KEYBOARD_PACKET), sizeof(NV_KEYBOARD_PACKET)};
        case UTF8_TEXT_EVENT_MAGIC:
          return packet_size_bounds_t {sizeof(NV_INPUT_HEADER), sizeof(NV_UNICODE_PACKET)};
        case MULTI_CONTROLLER_MAGIC_GEN5:
          return packet_size_bounds_t {sizeof(NV_MULTI_CONTROLLER_PACKET), sizeof(NV_MULTI_CONTROLLER_PACKET)};
        case SS_TOUCH_MAGIC:
          return packet_size_bounds_t {sizeof(SS_TOUCH_PACKET), sizeof(SS_TOUCH_PACKET)};
        case SS_PEN_MAGIC:
          return packet_size_bounds_t {sizeof(SS_PEN_PACKET), sizeof(SS_PEN_PACKET)};
        case SS_CONTROLLER_ARRIVAL_MAGIC:
          return packet_size_bounds_t {sizeof(SS_CONTROLLER_ARRIVAL_PACKET), sizeof(SS_CONTROLLER_ARRIVAL_PACKET)};
        case SS_CONTROLLER_TOUCH_MAGIC:
          return packet_size_bounds_t {sizeof(SS_CONTROLLER_TOUCH_PACKET), sizeof(SS_CONTROLLER_TOUCH_PACKET)};
        case SS_CONTROLLER_MOTION_MAGIC:
          return packet_size_bounds_t {sizeof(SS_CONTROLLER_MOTION_PACKET), sizeof(SS_CONTROLLER_MOTION_PACKET)};
        case SS_CONTROLLER_BATTERY_MAGIC:
          return packet_size_bounds_t {sizeof(SS_CONTROLLER_BATTERY_PACKET), sizeof(SS_CONTROLLER_BATTERY_PACKET)};
        default:
          return std::nullopt;
      }
    }
  }  // namespace

  packet_validation_result_t validate_packet(const std::vector<std::uint8_t> &input_data) {
    const auto declared_size = read_u32_be(input_data, 0);
    const auto magic = read_u32_le(input_data, sizeof(std::uint32_t));
    if (!declared_size || !magic) {
      return {.error = packet_validation_error_e::short_header};
    }

    const auto total_size = static_cast<std::size_t>(*declared_size) + sizeof(std::uint32_t);
    if (total_size != input_data.size()) {
      return {
        .error = packet_validation_error_e::declared_size_mismatch,
        .magic = *magic,
        .total_size = total_size,
      };
    }

    const auto bounds = packet_size_bounds(*magic);
    if (!bounds) {
      return {
        .error = packet_validation_error_e::unknown_magic,
        .magic = *magic,
        .total_size = total_size,
      };
    }

    if (total_size < bounds->min_total_size || total_size > bounds->max_total_size) {
      return {
        .error = packet_validation_error_e::invalid_size,
        .magic = *magic,
        .total_size = total_size,
        .expected_size = *bounds,
      };
    }

    return {
      .packet = validated_packet_t {*magic, total_size},
      .magic = *magic,
      .total_size = total_size,
    };
  }

  std::optional<normalized_touch_port_t> normalize_touch_port(const touch_port_t &touch_port, std::pair<float, float> &coords) {
    const float monitor_logical_w = (touch_port.width * touch_port.scalar_inv) / touch_port.scalar_tpcoords;
    const float monitor_logical_h = (touch_port.height * touch_port.scalar_inv) / touch_port.scalar_tpcoords;
    if (monitor_logical_w <= 0.0f || monitor_logical_h <= 0.0f) {
      return std::nullopt;
    }

    // Linux client_to_touchport() returns desktop-relative coordinates for
    // inputtino. Windows keeps monitor-local coordinates here and applies the
    // monitor offset when injecting the pointer.
#ifdef __linux__
    coords.first = (coords.first - touch_port.offset_x) / monitor_logical_w;
    coords.second = (coords.second - touch_port.offset_y) / monitor_logical_h;
#else
    coords.first = coords.first / monitor_logical_w;
    coords.second = coords.second / monitor_logical_h;
#endif

    return normalized_touch_port_t {
      touch_port.offset_x,
      touch_port.offset_y,
      static_cast<int>(monitor_logical_w),
      static_cast<int>(monitor_logical_h)
    };
  }
}  // namespace input::validation
