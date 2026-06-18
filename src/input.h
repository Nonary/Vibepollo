/**
 * @file src/input.h
 * @brief Declarations for gamepad, keyboard, and mouse input handling.
 */
#pragma once

// standard includes
#include <functional>
#include <optional>
#include <utility>
#include <vector>

// local includes
#include "crypto.h"
#include "platform/common.h"
#include "thread_safe.h"

namespace input {
  struct input_t;
  struct touch_port_t;

  void print(void *input);
  void reset(std::shared_ptr<input_t> &input);
  void passthrough(std::shared_ptr<input_t> &input, std::vector<std::uint8_t> &&input_data, const crypto::PERM &permission);

#ifdef SUNSHINE_TESTS
  bool validate_packet_for_tests(const std::vector<std::uint8_t> &input_data);
#endif

#ifdef SUNSHINE_TESTS
  bool validate_packet_for_tests(const std::vector<std::uint8_t> &input_data);
  std::optional<platf::touch_port_t> monitor_touch_port_for_tests(const input::touch_port_t &touch_port, std::pair<float, float> &coords);
#endif

  [[nodiscard]] std::unique_ptr<platf::deinit_t> init();

  bool probe_gamepads();

  std::shared_ptr<input_t> alloc(safe::mail_t mail);

  struct touch_port_t: public platf::touch_port_t {
    int env_width;
    int env_height;

    // Offset x and y coordinates of the client
    float client_offsetX;
    float client_offsetY;

    float scalar_inv;
    float scalar_tpcoords;

    int env_logical_width;
    int env_logical_height;

    explicit operator bool() const {
      return width != 0 && height != 0 && env_width != 0 && env_height != 0;
    }
  };

  /**
   * @brief Scale the ellipse axes according to the provided size.
   * @param val The major and minor axis pair.
   * @param rotation The rotation value from the touch/pen event.
   * @param scalar The scalar cartesian coordinate pair.
   * @return The major and minor axis pair.
   */
  std::pair<float, float> scale_client_contact_area(const std::pair<float, float> &val, uint16_t rotation, const std::pair<float, float> &scalar);
}  // namespace input
