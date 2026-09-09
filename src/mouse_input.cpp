/**
 * @file src/mouse_input.cpp
 * @brief Platform-neutral mouse input contract implementation.
 */
#include "mouse_input.h"

#include <cmath>

namespace mouse_input {
  point_t to_virtual_desktop(const viewport_t &viewport, const absolute_position_t position) {
    if (viewport.width <= 0 || viewport.height <= 0) {
      return {0, 0};
    }

    const auto scale = [](const float value, const int span) {
      return static_cast<int>(std::lround(value * (static_cast<float>(VIRTUAL_DESKTOP_GRID) / static_cast<float>(span))));
    };

    return {
      scale(position.x + static_cast<float>(viewport.offset_x), viewport.width),
      scale(position.y + static_cast<float>(viewport.offset_y), viewport.height)
    };
  }

  controller_t::controller_t(backend_t &backend):
      _backend(backend) {}

  point_t controller_t::position() const {
    return _backend.position();
  }

  void controller_t::move_relative(point_t delta) {
    _backend.move_relative(delta);
  }

  void controller_t::move_absolute(const viewport_t &viewport, absolute_position_t position) {
    _backend.move_absolute(viewport, position);
  }
}  // namespace mouse_input
