/**
 * @file src/platform/macos/display_policy.h
 * @brief CoreGraphics-independent decisions for lid transitions, the exclusive layout, stream teardown, and capture recovery.
 * @details Kept free of Apple frameworks so the unit tests can check them without displays.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace platf::macos_display_policy {
  using display_id_t = std::uint32_t;
  using clock = std::chrono::steady_clock;

  enum class lid_close_action_e {
    none,  ///< Keep streaming.
    end_streams,  ///< End the streams; no app is running.
    quit_app,  ///< Quit the app, which ends its streams as when it exits.
  };

  /**
   * @brief What closing the lid should do.
   * @param end_streams_on_lid_close The lid_close_ends_streams setting. When it's off, streams keep
   *        going for as long as the Mac stays awake, which is up to macOS or another utility.
   * @param sessions Streams in progress.
   * @param app_running Whether a launched app is running.
   */
  lid_close_action_e lid_close_action(bool end_streams_on_lid_close, std::size_t sessions, bool app_running);

  struct exclusive_plan_t {
    std::vector<display_id_t> turn_off;  ///< Displays that are on but shouldn't be.
    std::vector<display_id_t> newly_tracked;  ///< Of those, the ones this session hasn't turned off before.
  };

  /**
   * @brief Which displays to turn off so only the stream's displays stay on, as the exclusive layout asks.
   * @details Displays come back on by themselves: reopening a MacBook's lid brings back the built-in
   *          display, and one plugged in during the session turns on.
   * @param active The displays that are on.
   * @param keep_on The stream's virtual display and the Remote Monitor displays, which belong to clients.
   * @param tracked The displays this session already turned off, and will turn back on when it ends.
   */
  exclusive_plan_t plan_exclusive_layout(const std::vector<display_id_t> &active, const std::vector<display_id_t> &keep_on, const std::vector<display_id_t> &tracked);

  /// A display and the display it mirrors, if any.
  struct display_mirror_t {
    display_id_t id;
    std::optional<display_id_t> mirrors;
  };

  /**
   * @brief Which displays to mirror onto the stream's display, the exclusive layout's way of keeping the
   *        desktop off them when leave_displays_off_after_streaming is on.
   * @details Turning a display off takes it out of the desktop, but turning it back on when the stream
   *          ends lights its panel, even one another utility is holding dark. A mirrored display stays
   *          on, with its power left alone, and stops mirroring when the stream's display goes away.
   * @param online The displays that are online, with what each one mirrors.
   * @param stream_display The display everything else should mirror.
   * @param keep_separate The stream's display and the Remote Monitor displays, which belong to clients.
   */
  std::vector<display_id_t> plan_exclusive_mirror(const std::vector<display_mirror_t> &online, display_id_t stream_display, const std::vector<display_id_t> &keep_separate);

  struct teardown_plan_t {
    bool wake_displays;  ///< Wake displays that are asleep, so the ones turned off can come back.
    bool turn_on;  ///< Turn the displays the exclusive layout turned off back on.
  };

  /**
   * @brief What ending a stream's virtual display does to the physical displays.
   * @details Displays the exclusive layout turned off are always turned back on, so none is left
   *          disabled. With leave_displays_off_after_streaming, nothing wakes displays that are asleep
   *          or held dark: local input still does.
   * @param leave_displays_off The leave_displays_off_after_streaming setting, as it was when the display was made.
   * @param turned_off Whether the exclusive layout turned any display off.
   */
  teardown_plan_t plan_teardown(bool leave_displays_off, bool turned_off);

  /**
   * @brief Allows at most `limit` attempts within any `window`, to keep automatic fixes from looping.
   * @details Not thread-safe; callers hold their own lock.
   */
  class attempt_budget_t {
  public:
    attempt_budget_t(std::size_t limit, clock::duration window);

    /// Record an attempt at `now` if the budget allows one. Returns whether it did.
    bool try_spend(clock::time_point now);

    void reset();

  private:
    std::size_t limit;
    clock::duration window;
    std::deque<clock::time_point> attempts;
  };

  /**
   * @brief Notices when a captured display stays offline, as a MacBook's built-in display does with the lid closed.
   * @details A display can drop offline for a moment while macOS rearranges displays, so it only counts
   *          as lost after `grace`. Offline isn't asleep: an asleep display stays online.
   */
  class display_presence_t {
  public:
    explicit display_presence_t(clock::duration grace);

    /// Returns true once the display has been offline for at least the grace period.
    bool lost(bool online, clock::time_point now);

  private:
    clock::duration grace;
    std::optional<clock::time_point> offline_since;
  };
}  // namespace platf::macos_display_policy
