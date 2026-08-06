/**
 * @file src/mouse_input.cpp
 * @brief Platform-neutral mouse input contract implementation.
 */
#include "mouse_input.h"

namespace mouse_input {
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
