/**
 * @file src/platform/macos/touchscreen.h
 * @brief The host's side of the macOS 27 virtual touchscreen.
 * @details Fingers go to a virtual HID touchscreen on the streamed display, so macOS sees direct
 *          touches. The device is owned by a separately signed helper (see
 *          tools/macos_touch_helper), which the host starts on demand. When the touchscreen can't
 *          be used, the caller keeps using the touch emulation.
 */
#pragma once

// standard includes
#include <cstdint>

namespace platf::macos_touchscreen {
  using owner_t = const void *;
  using display_t = std::uint32_t;

  /**
   * @brief Starts a gesture on the touchscreen of a display.
   * @details Call this when a client's first finger lands. It never blocks: if the helper or the
   *          device isn't ready, it starts them in the background and returns true while they
   *          start, so the gesture's reports are queued; it returns false when the touchscreen
   *          can't be used, so the caller emulates the gesture instead.
   * @param owner The client.
   * @param display The display the client's touches belong to.
   * @return Whether this gesture goes to the touchscreen.
   */
  bool begin_gesture(owner_t owner, display_t display);

  /**
   * @brief Gets the touchscreen of a display ready before a client's first touch.
   * @details macOS needs a moment after a device appears before it accepts reports, so making it
   *          when the client connects keeps the first touch from waiting. It never blocks, does
   *          nothing when the touchscreen can't be used, and the client's release() undoes it.
   */
  void prepare(owner_t owner, display_t display);

  /**
   * @brief Puts a finger down or moves it, in global screen points.
   * @return False when the touchscreen went away; the gesture is over.
   */
  bool touch(owner_t owner, display_t display, std::uint32_t pointer, double global_x, double global_y);

  /// Lifts a finger. Returns false when the touchscreen went away.
  bool lift(owner_t owner, display_t display, std::uint32_t pointer);

  /**
   * @brief Lifts every finger a client has down, on any display.
   * @details Used for LI_TOUCH_EVENT_CANCEL_ALL. It never starts the helper or makes a device.
   */
  void lift_owner(owner_t owner);

  /**
   * @brief Forgets a client: lifts its fingers and, when it was the last client on a display,
   *        destroys that display's touchscreen.
   */
  void release(owner_t owner);

  /// Whether touches can go to the touchscreen on this Mac at all (macOS 27 and the setting on).
  bool supported();
}  // namespace platf::macos_touchscreen
