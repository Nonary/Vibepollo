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
