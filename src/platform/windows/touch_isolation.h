/**
 * @file src/platform/windows/touch_isolation.h
 * @brief Keeps the desktop cursor out of a streamed virtual display's touch surface.
 *
 * While a scope is held, mouse events Windows promoted from an injected pen or touch
 * contact are swallowed before they reach any window, so the desktop cursor is neither
 * moved onto the touched pixel nor handed to legacy applications. Only events landing
 * inside a rectangle that matches one of our virtual displays are affected; physical
 * monitors and physical digitizers keep working normally.
 */
#pragma once

#ifdef _WIN32

  #include "touch_isolation_policy.h"

  #include <cstddef>
  #include <memory>

namespace platf::touch_isolation {
  /**
   * @brief Holds isolation active for one rectangle; releases it on destruction.
   */
  class scope_t {
  public:
    explicit scope_t(std::size_t slot);
    ~scope_t();

    scope_t(const scope_t &) = delete;
    scope_t &operator=(const scope_t &) = delete;

  private:
    std::size_t _slot;
  };

  /**
   * @brief Ask for a streamed display's rectangle to be isolated.
   * @details Whether the rectangle really belongs to a Vibepollo virtual display is
   * resolved off the caller's thread, because enumerating displays is far too slow for
   * the input path. Isolation therefore engages shortly after the first contact rather
   * than instantly.
   * @param streamed_rect The streamed display's rectangle, in true (signed)
   * virtual-screen coordinates.
   * @return A scope keeping the request alive, or nullptr when the feature is disabled
   * or no slot is free.
   */
  [[nodiscard]] std::shared_ptr<scope_t> acquire(const rect_t &streamed_rect);

  /**
   * @brief Restore the host cursor if Windows suppressed it for touch.
   * @details Blocking promotion stops the cursor from moving, but the pointer stack
   * decides visibility on its own. Costs one `GetCursorInfo` when the cursor is already
   * visible, and is throttled so it cannot flood the input queue.
   */
  void poke_cursor_visibility();
}  // namespace platf::touch_isolation

#endif
