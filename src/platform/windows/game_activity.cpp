/**
 * @file src/platform/windows/game_activity.cpp
 */

#include "game_activity.h"

#include "playnite_integration.h"
#include "src/logging.h"
#include "src/platform/windows/ipc/display_settings_client.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace platf::game_activity {
  namespace {
    constexpr auto POLL_INTERVAL = 100ms;
    constexpr auto FOREGROUND_CACHE_LIFETIME = 250ms;
    constexpr auto HEURISTIC_PROMOTION_DELAY = 300ms;
    // Briefly bridge the task-switcher/desktop foreground during a direct game-to-game
    // Alt+Tab, but return to the base desktop rate promptly when a non-game keeps focus.
    constexpr auto DEMOTION_DELAY = 750ms;
    constexpr auto RETRY_DELAY = 2s;
    constexpr auto EXPECTED_TRANSITION_LIFETIME = 5s;

    struct cached_foreground_t {
      RECT rect {};
      foreground_app::state_t state;
      std::chrono::steady_clock::time_point sampled_at {};
    };

    std::mutex g_foreground_cache_mutex;
    std::vector<cached_foreground_t> g_foreground_cache;

    bool same_rect(const RECT &lhs, const RECT &rhs) {
      return lhs.left == rhs.left && lhs.top == rhs.top &&
             lhs.right == rhs.right && lhs.bottom == rhs.bottom;
    }

    void publish_foreground(const RECT &rect, const foreground_app::state_t &state) {
      const auto now = std::chrono::steady_clock::now();
      std::scoped_lock lock {g_foreground_cache_mutex};
      std::erase_if(g_foreground_cache, [&](const auto &entry) {
        return now - entry.sampled_at > EXPECTED_TRANSITION_LIFETIME;
      });
      const auto existing = std::find_if(g_foreground_cache.begin(), g_foreground_cache.end(), [&](const auto &entry) {
        return same_rect(entry.rect, rect);
      });
      if (existing != g_foreground_cache.end()) {
        existing->state = state;
        existing->sampled_at = now;
        return;
      }
      g_foreground_cache.push_back({rect, state, now});
    }

    signal_t foreground_signal(const foreground_app::state_t &foreground) {
      if (foreground.source == "playnite-status") {
        return {signal_source_e::playnite, true, foreground.foreground_pid, foreground.foreground_exe};
      }
      if (foreground.matches_active_app &&
          (foreground.source == "process" || foreground.source == "playnite-cache")) {
        return {signal_source_e::tracked_process, true, foreground.foreground_pid, foreground.foreground_exe};
      }
      if (foreground.valid_window && foreground.fullscreen_on_capture_display) {
        return {signal_source_e::fullscreen_foreground, true, foreground.foreground_pid, foreground.foreground_exe};
      }
      return {};
    }

    signal_t playnite_foreground_signal(
      const foreground_app::state_t &foreground,
      const std::vector<platf::playnite::active_game_status_t> &active_games
    ) {
      if (!foreground.valid_window || foreground.foreground_exe.empty()) {
        return {};
      }

      // Walk newest-first so a recently started second game wins when Playnite has more
      // than one running-game claim. A background claim alone never promotes refresh.
      for (auto game = active_games.rbegin(); game != active_games.rend(); ++game) {
        if (game->active && foreground_app::playnite_foreground_matches_for_tests(
                              {},
                              game->id,
                              game->exe,
                              game->install_dir,
                              foreground.foreground_exe
                            )) {
          return {
            signal_source_e::playnite,
            true,
            foreground.foreground_pid,
            foreground.foreground_exe,
          };
        }
      }
      return {};
    }
  }  // namespace

  state_t reduce_signals(std::span<const signal_t> signals) {
    state_t result;
    for (const auto &signal : signals) {
      if (!signal.active || signal.source <= result.source) {
        continue;
      }
      result.active = true;
      result.source = signal.source;
      result.pid = signal.pid;
      result.executable = signal.executable;
    }
    return result;
  }

  const char *source_name(const signal_source_e source) {
    switch (source) {
      case signal_source_e::playnite:
        return "playnite";
      case signal_source_e::tracked_process:
        return "tracked-process";
      case signal_source_e::fullscreen_foreground:
        return "fullscreen-foreground";
      default:
        return "none";
    }
  }

  foreground_app::state_t foreground_snapshot(const std::optional<RECT> &capture_rect) {
    if (capture_rect) {
      const auto now = std::chrono::steady_clock::now();
      std::scoped_lock lock {g_foreground_cache_mutex};
      const auto cached = std::find_if(g_foreground_cache.begin(), g_foreground_cache.end(), [&](const auto &entry) {
        return same_rect(entry.rect, *capture_rect) && now - entry.sampled_at <= FOREGROUND_CACHE_LIFETIME;
      });
      if (cached != g_foreground_cache.end()) {
        return cached->state;
      }
    }
    return foreground_app::snapshot(capture_rect);
  }

  struct refresh_target_t::impl_t {
    explicit impl_t(refresh_target_options_t options):
        options {std::move(options)},
        applied_high {this->options.initial_high},
        candidate_high {this->options.initial_high},
        candidate_since {std::chrono::steady_clock::now()},
        worker {[this](std::stop_token stop_token) {
          run(stop_token);
        }} {
    }

    ~impl_t() {
      worker.request_stop();
      if (worker.joinable()) {
        worker.join();
      }
    }

    void run(std::stop_token stop_token) {
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
      state_t previous_state;
      auto retry_after = std::chrono::steady_clock::time_point {};

      while (!stop_token.stop_requested()) {
        const auto foreground = foreground_app::snapshot(options.capture_rect);
        publish_foreground(options.capture_rect, foreground);

        std::vector<signal_t> signals;
        signals.reserve(3);
        const auto playnite_games = platf::playnite::get_active_game_statuses();
        signals.push_back(playnite_foreground_signal(foreground, playnite_games));
        signals.push_back(foreground_signal(foreground));

        const auto resolved = reduce_signals(signals);
        if (resolved.active != previous_state.active || resolved.source != previous_state.source ||
            resolved.pid != previous_state.pid || resolved.executable != previous_state.executable) {
          BOOST_LOG(debug) << "Game activity: display='" << options.display_name
                           << "' active=" << (resolved.active ? "1" : "0")
                           << " source=" << source_name(resolved.source)
                           << " pid=" << resolved.pid
                           << " exe='" << resolved.executable << "'";
          previous_state = resolved;
        }

        const auto now = std::chrono::steady_clock::now();
        if (resolved.active != candidate_high) {
          candidate_high = resolved.active;
          candidate_since = now;
        }

        const auto required_delay = candidate_high && resolved.source >= signal_source_e::playnite ?
                                      0ms :
                                      (candidate_high ? HEURISTIC_PROMOTION_DELAY : DEMOTION_DELAY);
        if (candidate_high != applied_high && now - candidate_since >= required_delay && now >= retry_after) {
          const auto numerator = candidate_high ? options.high_refresh_numerator : options.base_refresh_numerator;
          const auto denominator = candidate_high ? options.high_refresh_denominator : options.base_refresh_denominator;
          begin_expected_transition();
          const bool applied = display_helper_client::send_refresh_rate(options.device_id, numerator, denominator);
          finish_expected_transition(applied);
          if (applied) {
            applied_high = candidate_high;
            BOOST_LOG(info) << "Virtual display refresh: display='" << options.display_name
                            << "' source=" << source_name(resolved.source)
                            << " rate=" << numerator << '/' << denominator;
          } else {
            retry_after = now + RETRY_DELAY;
            BOOST_LOG(warning) << "Virtual display refresh: failed to apply " << numerator << '/' << denominator
                               << " to device='" << options.device_id << "'";
          }
        }

        std::this_thread::sleep_for(POLL_INTERVAL);
      }
    }

    void begin_expected_transition() {
      std::scoped_lock lock {transition_mutex};
      transition_expected = true;
      transition_in_progress = true;
      transition_succeeded = false;
      transition_deadline = std::chrono::steady_clock::now() + EXPECTED_TRANSITION_LIFETIME;
    }

    void finish_expected_transition(const bool success) {
      {
        std::scoped_lock lock {transition_mutex};
        transition_in_progress = false;
        transition_succeeded = success;
        if (!success) {
          transition_expected = false;
        }
      }
      transition_cv.notify_all();
    }

    bool wait_for_expected_refresh_change(const std::chrono::milliseconds timeout) {
      std::unique_lock lock {transition_mutex};
      if (!transition_expected || std::chrono::steady_clock::now() > transition_deadline) {
        transition_expected = false;
        return false;
      }
      if (transition_in_progress) {
        transition_cv.wait_for(lock, timeout, [&] {
          return !transition_in_progress;
        });
      }
      const bool accepted = transition_expected && !transition_in_progress && transition_succeeded &&
                            std::chrono::steady_clock::now() <= transition_deadline;
      transition_expected = false;
      return accepted;
    }

    refresh_target_options_t options;
    bool applied_high {false};
    bool candidate_high {false};
    std::chrono::steady_clock::time_point candidate_since {};

    std::mutex transition_mutex;
    std::condition_variable transition_cv;
    bool transition_expected {false};
    bool transition_in_progress {false};
    bool transition_succeeded {false};
    std::chrono::steady_clock::time_point transition_deadline {};

    std::jthread worker;
  };

  refresh_target_t::refresh_target_t(refresh_target_options_t options):
      impl_ {std::make_unique<impl_t>(std::move(options))} {
  }

  refresh_target_t::~refresh_target_t() = default;

  bool refresh_target_t::wait_for_expected_refresh_change(const std::chrono::milliseconds timeout) {
    return impl_ && impl_->wait_for_expected_refresh_change(timeout);
  }

  std::shared_ptr<refresh_target_t> make_refresh_target(refresh_target_options_t options) {
    if (options.device_id.empty() || options.base_refresh_numerator == 0 ||
        options.base_refresh_denominator == 0 || options.high_refresh_numerator == 0 ||
        options.high_refresh_denominator == 0) {
      return {};
    }
    return std::shared_ptr<refresh_target_t>(new refresh_target_t(std::move(options)));
  }

}  // namespace platf::game_activity
