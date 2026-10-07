/**
 * @file tests/unit/test_mouse.cpp
 * @brief Deterministic mouse-controller contract tests.
 */
#include "../tests_common.h"

#include <src/mouse_input.h>

namespace {
  class fake_mouse_backend_t: public mouse_input::backend_t {
  public:
    mouse_input::point_t current {10, 20};
    mouse_input::viewport_t last_viewport {};
    mouse_input::absolute_position_t last_absolute {};
    int relative_calls = 0;
    int absolute_calls = 0;

    mouse_input::point_t position() const override {
      return current;
    }

    void move_relative(mouse_input::point_t delta) override {
      ++relative_calls;
      current.x += delta.x;
      current.y += delta.y;
    }

    void move_absolute(const mouse_input::viewport_t &viewport,
                       mouse_input::absolute_position_t position) override {
      ++absolute_calls;
      last_viewport = viewport;
      last_absolute = position;
      current = {static_cast<int>(position.x), static_cast<int>(position.y)};
    }
  };
}  // namespace

class MouseControllerTest: public testing::TestWithParam<mouse_input::point_t> {};

INSTANTIATE_TEST_SUITE_P(
  MouseInputs,
  MouseControllerTest,
  testing::Values(mouse_input::point_t {40, 40}, mouse_input::point_t {70, 150})
);

TEST_P(MouseControllerTest, RelativeMoveUpdatesBothCoordinatesExactlyOnce) {
  fake_mouse_backend_t backend;
  mouse_input::controller_t controller {backend};
  const auto old_position = controller.position();

  controller.move_relative(GetParam());

  const auto new_position = controller.position();
  EXPECT_EQ(new_position.x - old_position.x, GetParam().x);
  EXPECT_EQ(new_position.y - old_position.y, GetParam().y);
  EXPECT_EQ(backend.relative_calls, 1);
}

TEST_P(MouseControllerTest, AbsoluteMoveForwardsViewportAndCoordinates) {
  fake_mouse_backend_t backend;
  mouse_input::controller_t controller {backend};
  const mouse_input::viewport_t viewport {5, 7, 1920, 1080};
  const mouse_input::absolute_position_t requested {
    static_cast<float>(GetParam().x),
    static_cast<float>(GetParam().y)
  };

  controller.move_absolute(viewport, requested);

  EXPECT_EQ(controller.position(), GetParam());
  EXPECT_EQ(backend.last_viewport.width, 1920);
  EXPECT_EQ(backend.last_viewport.height, 1080);
  EXPECT_EQ(backend.last_absolute, requested);
  EXPECT_EQ(backend.absolute_calls, 1);
}
