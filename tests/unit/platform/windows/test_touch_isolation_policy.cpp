#ifdef _WIN32

  #include "src/platform/windows/touch_isolation_policy.h"

  #include <array>
  #include <gtest/gtest.h>

namespace {
  using namespace platf::touch_isolation;

  // A virtual display sitting to the right of a 1920x1080 primary.
  constexpr rect_t VIRTUAL_DISPLAY {1920, 0, 3840, 1080};
  constexpr rect_t PRIMARY {0, 0, 1920, 1080};

  // A promoted event tags its extra info with the signature plus a payload byte; the
  // payload also discriminates pen (bit 7 clear) from touch (bit 7 set).
  constexpr std::uint64_t PEN_EVENT = 0xFF515700ull;
  constexpr std::uint64_t TOUCH_EVENT = 0xFF515780ull;
  constexpr std::uint64_t TOUCH_EVENT_WITH_ID = 0xFF5157ABull;
  constexpr std::uint64_t REAL_MOUSE = 0ull;

  TEST(TouchIsolationPolicy, RecognizesPromotedPenAndTouchSignatures) {
    EXPECT_TRUE(is_promoted_from_pointer(PEN_EVENT));
    EXPECT_TRUE(is_promoted_from_pointer(TOUCH_EVENT));
    EXPECT_TRUE(is_promoted_from_pointer(TOUCH_EVENT_WITH_ID));
  }

  TEST(TouchIsolationPolicy, RejectsEverythingElse) {
    EXPECT_FALSE(is_promoted_from_pointer(REAL_MOUSE));
    EXPECT_FALSE(is_promoted_from_pointer(0xFF515600ull));
    EXPECT_FALSE(is_promoted_from_pointer(0xFE515700ull));
  }

  TEST(TouchIsolationPolicy, SwallowsPromotedEventsInsideTheRect) {
    EXPECT_TRUE(should_swallow(TOUCH_EVENT, 2880, 540, VIRTUAL_DISPLAY));
    EXPECT_TRUE(should_swallow(PEN_EVENT, 2880, 540, VIRTUAL_DISPLAY));
  }

  TEST(TouchIsolationPolicy, PassesRealMouseInsideTheRect) {
    EXPECT_FALSE(should_swallow(REAL_MOUSE, 2880, 540, VIRTUAL_DISPLAY));
  }

  TEST(TouchIsolationPolicy, PassesPromotedEventsOutsideTheRect) {
    // A physical touchscreen on the primary keeps driving the cursor.
    EXPECT_FALSE(should_swallow(TOUCH_EVENT, 960, 540, VIRTUAL_DISPLAY));
  }

  TEST(TouchIsolationPolicy, EmptyRectSwallowsNothing) {
    EXPECT_FALSE(should_swallow(TOUCH_EVENT, 0, 0, rect_t {}));
    EXPECT_FALSE(should_swallow(TOUCH_EVENT, 10, 10, rect_t {100, 100, 100, 200}));
  }

  TEST(TouchIsolationPolicy, RectEdgesAreHalfOpen) {
    EXPECT_TRUE(contains(VIRTUAL_DISPLAY, 1920, 0));
    EXPECT_FALSE(contains(VIRTUAL_DISPLAY, 3840, 0));
    EXPECT_FALSE(contains(VIRTUAL_DISPLAY, 1920, 1080));
    // Adjacent monitors must never both claim the same pixel.
    EXPECT_FALSE(contains(PRIMARY, 1920, 540));
  }

  TEST(TouchIsolationPolicy, HandlesNegativeVirtualScreenOrigin) {
    // The primary sits to the right, so the virtual display has a negative origin.
    constexpr rect_t left_of_primary {-1920, -600, 0, 480};
    EXPECT_TRUE(should_swallow(TOUCH_EVENT, -960, -60, left_of_primary));
    EXPECT_FALSE(should_swallow(TOUCH_EVENT, 10, 10, left_of_primary));
  }

  TEST(TouchIsolationPolicy, HandlesTheIsolatedLayoutOffset) {
    // extended_isolated parks the virtual display far away from every physical monitor.
    constexpr rect_t isolated {64000, 64000, 65920, 65080};
    EXPECT_TRUE(should_swallow(TOUCH_EVENT, 64960, 64540, isolated));
    EXPECT_FALSE(should_swallow(TOUCH_EVENT, 960, 540, isolated));
  }

  TEST(TouchIsolationPolicy, MatchesAVirtualDisplayExactly) {
    const std::array all {PRIMARY, VIRTUAL_DISPLAY};
    const std::array virtual_displays {VIRTUAL_DISPLAY};

    const auto matched = match_virtual_display_rect(VIRTUAL_DISPLAY, virtual_displays, all);
    ASSERT_TRUE(matched.has_value());
    EXPECT_EQ(*matched, VIRTUAL_DISPLAY);
  }

  TEST(TouchIsolationPolicy, RefusesAPhysicalMonitor) {
    const std::array all {PRIMARY, VIRTUAL_DISPLAY};
    const std::array virtual_displays {VIRTUAL_DISPLAY};

    EXPECT_FALSE(match_virtual_display_rect(PRIMARY, virtual_displays, all).has_value());
  }

  TEST(TouchIsolationPolicy, RefusesWhenNoVirtualDisplayIsPresent) {
    const std::array all {PRIMARY};
    EXPECT_FALSE(match_virtual_display_rect(PRIMARY, std::span<const rect_t> {}, all).has_value());
  }

  TEST(TouchIsolationPolicy, RefusesADuplicatedTopology) {
    // Mirroring makes the virtual display and a physical one indistinguishable, so
    // isolating would silently break the physical monitor.
    const std::array all {VIRTUAL_DISPLAY, VIRTUAL_DISPLAY};
    const std::array virtual_displays {VIRTUAL_DISPLAY};

    EXPECT_FALSE(match_virtual_display_rect(VIRTUAL_DISPLAY, virtual_displays, all).has_value());
  }

  TEST(TouchIsolationPolicy, RefusesAnEmptyStreamedRect) {
    const std::array all {rect_t {}};
    const std::array virtual_displays {rect_t {}};

    EXPECT_FALSE(match_virtual_display_rect(rect_t {}, virtual_displays, all).has_value());
  }
}  // namespace

#endif
