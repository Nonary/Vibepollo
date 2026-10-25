/**
 * @file src/platform/macos/display_policy.h
 * @brief CoreGraphics-independent decisions for lid transitions, the exclusive layout, and capture recovery.
 * @details Kept free of Apple frameworks so the unit tests can check them without displays.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace platf::macos_display_policy {
  using display_id_t = std::uint32_t;
  using clock = std::chrono::steady_clock;

  enum class stream_role_e {
    app,  ///< A stream of the launched app, or of the desktop; ends when the app exits.
    remote,  ///< A Remote Monitor or Remote Input stream, which outlives the app on purpose.
  };

  struct lid_close_plan_t {
    bool quit_app = false;  ///< Quit the app, which ends its streams as when it exits.
    std::vector<std::size_t> end_streams;  ///< Indexes of the streams to end directly.
  };

  /**
   * @brief What closing the lid should do.
   * @param end_streams_on_lid_close The lid_close_ends_streams setting. When it's off, streams keep
   *        going for as long as the Mac stays awake, which is up to macOS or another utility.
   * @param streams The roles of the streams in progress.
   * @param app_running Whether a launched app is running.
   */
  lid_close_plan_t plan_lid_close(bool end_streams_on_lid_close, const std::vector<stream_role_e> &streams, bool app_running);

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
   * @param defer_untracked Leave displays this session hasn't turned off before alone for now, as while a
   *        Remote Monitor is being made: until it's published, its display looks like any other.
   */
  exclusive_plan_t plan_exclusive_layout(const std::vector<display_id_t> &active, const std::vector<display_id_t> &keep_on, const std::vector<display_id_t> &tracked, bool defer_untracked = false);

  /// The restore helper, which turns the displays back on if Vibepollo exits without doing so.
  struct restore_helper_t {
    int pid = -1;
    int fd = -1;  ///< Write end of the helper's stdin; EOF without "done" makes it restore.

    bool running() const {
      return pid > 0 && fd >= 0;
    }
  };

  /**
   * @brief How the safeguards and the displays are changed, so the order they change in can be tested.
   * @details The marker lists the displays the next launch turns back on; the helper turns them back
   *          on if Vibepollo dies.
   */
  struct safeguard_ops_t {
    std::function<bool(const std::vector<display_id_t> &ids)> write_marker;  ///< Returns whether the marker was written.
    std::function<void()> remove_marker;
    std::function<restore_helper_t(const std::vector<display_id_t> &ids)> start_helper;  ///< Not running() if it couldn't start.
    std::function<void(restore_helper_t &helper, bool displays_restored)> stop_helper;
    std::function<bool(const std::vector<display_id_t> &ids)> turn_off;
  };

  enum class cover_result_e {
    turned_off,  ///< The displays are off, and both safeguards cover them.
    safeguards_failed,  ///< Nothing was turned off; the safeguards are as they were.
    turn_off_failed,  ///< Nothing was turned off; the safeguards are as they were.
  };

  /**
   * @brief Point both safeguards at `covered`, then turn `turn_off` off.
   * @details Nothing is turned off until the marker is written and the new helper runs. The current
   *          helper is kept until the displays are off, so some helper always covers every display
   *          that's off, and it's kept for good if anything fails.
   * @param helper The current helper, covering `previously_covered`; replaced on success.
   * @param previously_covered The displays already off, which the safeguards already cover.
   * @param covered `previously_covered` and the displays about to be turned off.
   * @param turn_off The displays to turn off now.
   */
  cover_result_e cover_and_turn_off(const safeguard_ops_t &ops, restore_helper_t &helper, const std::vector<display_id_t> &previously_covered, const std::vector<display_id_t> &covered, const std::vector<display_id_t> &turn_off);

  /// A display's size: in points, which desktop coordinates use, and in pixels, which get captured.
  struct display_size_t {
    int width = 0;  ///< In points.
    int height = 0;
    int pixel_width = 0;
    int pixel_height = 0;
  };

  /**
   * @brief The size of a virtual display made for a requested mode.
   * @param hidpi Retina: modes are sized in points, and their backing store, which is captured, is 2x.
   */
  display_size_t virtual_display_size(int width, int height, bool hidpi);

  /// The display a stream's touch port names (its output name is the display ID), if it names one.
  std::optional<display_id_t> parse_display_identity(std::string_view output_name);

  /**
   * @brief The display a stream's pointer, touch, and pen input goes to.
   * @details A Remote Monitor stream's input belongs to its own display, wherever that display is now.
   *          Every other stream's goes to the streamed display, which can change as streams come and go.
   * @param stream_display The display the stream captures, from its touch port.
   * @param is_remote_display Whether a display is a Remote Monitor display that still exists.
   * @param streamed_display The game's virtual display, or else the configured display.
   */
  display_id_t pointer_display(std::optional<display_id_t> stream_display, const std::function<bool(display_id_t)> &is_remote_display, display_id_t streamed_display);

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
