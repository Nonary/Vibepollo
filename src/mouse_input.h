/**
 * @file src/mouse_input.h
 * @brief Platform-neutral mouse input contract.
 */
#pragma once

namespace mouse_input {
  struct point_t {
    int x;
    int y;

    bool operator==(const point_t &) const = default;
  };

  struct viewport_t {
    int offset_x;
    int offset_y;
    int width;
    int height;
  };

  struct absolute_position_t {
    float x;
    float y;

    bool operator==(const absolute_position_t &) const = default;
  };

  // Platforms that address the whole desktop with a fixed normalized grid, rather than
  // in pixels, use this many steps per axis.
  constexpr int VIRTUAL_DESKTOP_GRID = 65535;

  /**
   * @brief Map a monitor-local position onto the normalized virtual-desktop grid.
   * @details The position is relative to the streamed monitor, so the monitor's own
   * origin within the desktop has to be folded in before scaling. A viewport with no
   * area maps to the origin rather than dividing by zero.
   * @param viewport The desktop origin of the streamed monitor and the desktop's size.
   * @param position The monitor-local position, in the viewport's own units.
   */
  point_t to_virtual_desktop(const viewport_t &viewport, absolute_position_t position);

  class backend_t {
  public:
    virtual ~backend_t() = default;
    virtual point_t position() const = 0;
    virtual void move_relative(point_t delta) = 0;
    virtual void move_absolute(const viewport_t &viewport, absolute_position_t position) = 0;
  };

  class controller_t {
  public:
    explicit controller_t(backend_t &backend);

    point_t position() const;
    void move_relative(point_t delta);
    void move_absolute(const viewport_t &viewport, absolute_position_t position);

  private:
    backend_t &_backend;
  };
}  // namespace mouse_input
