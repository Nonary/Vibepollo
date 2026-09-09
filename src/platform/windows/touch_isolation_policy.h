/**
 * @file src/platform/windows/touch_isolation_policy.h
 * @brief Value-only decisions for suppressing touch-to-mouse promotion.
 *
 * Windows synthesizes legacy mouse messages from injected pointer contacts so that
 * windows without `WM_POINTER`/`WM_TOUCH` support still react to touch. Those
 * synthesized messages drag the desktop cursor onto the touched pixel, which defeats
 * the goal of treating a streamed virtual display as an independent touch panel.
 *
 * Deciding whether a low-level mouse event is one of those promoted messages, and
 * whether a streamed rectangle really belongs to one of our virtual displays, lives
 * here, free of Win32 types, so it can be unit tested.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <span>

namespace platf::touch_isolation {
  /**
   * @brief A desktop rectangle in true (signed) virtual-screen coordinates.
   * @details Right and bottom are exclusive, matching Win32 rectangles. An empty
   * rectangle means isolation is disabled.
   */
  struct rect_t {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    bool operator==(const rect_t &) const = default;
  };

  // Windows tags mouse messages promoted from pen or touch with this signature in the
  // event's extra info. See the Win32 docs, "Distinguishing Pen Input from Mouse and
  // Touch": the low byte is a payload (it also discriminates pen from touch) and must
  // be masked off before comparing. Both kinds are suppressed.
  constexpr std::uint64_t POINTER_SIGNATURE = 0xFF515700ull;
  constexpr std::uint64_t POINTER_SIGNATURE_MASK = 0xFFFFFF00ull;

  /**
   * @brief Whether the rectangle covers no pixels.
   */
  bool is_empty(const rect_t &rect);

  /**
   * @brief Whether a mouse event's extra info marks it as promoted from pen or touch.
   * @param extra_info The `dwExtraInfo` member of the low-level mouse event.
   */
  bool is_promoted_from_pointer(std::uint64_t extra_info);

  /**
   * @brief Whether a point falls inside the rectangle.
   * @details Left and top are inclusive, right and bottom exclusive, so two adjacent
   * monitors never both claim the same pixel.
   */
  bool contains(const rect_t &rect, int x, int y);

  /**
   * @brief Whether a low-level mouse event should be swallowed instead of delivered.
   * @param extra_info The `dwExtraInfo` member of the low-level mouse event.
   * @param x The event's virtual-screen X coordinate.
   * @param y The event's virtual-screen Y coordinate.
   * @param rect The isolated region, or an empty rectangle to swallow nothing.
   */
  bool should_swallow(std::uint64_t extra_info, int x, int y, const rect_t &rect);

  /**
   * @brief Resolve the rectangle to isolate for a streamed display.
   * @details Isolation must never touch a physical monitor, so the streamed rectangle
   * has to match one of our virtual displays exactly. A duplicated or mirrored
   * topology, where more than one monitor reports the same rectangle, is refused
   * because the two are then indistinguishable.
   * @param streamed The rectangle of the display being streamed.
   * @param virtual_display_rects Rectangles of the active Vibepollo virtual displays.
   * @param all_monitor_rects Rectangles of every monitor currently on the desktop.
   * @return The rectangle to isolate, or nullopt to leave the cursor alone.
   */
  std::optional<rect_t> match_virtual_display_rect(
    const rect_t &streamed,
    std::span<const rect_t> virtual_display_rects,
    std::span<const rect_t> all_monitor_rects
  );
}  // namespace platf::touch_isolation
