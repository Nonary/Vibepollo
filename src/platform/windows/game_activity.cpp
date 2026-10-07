/**
 * @file src/platform/windows/game_activity.cpp
 */

#include "game_activity.h"

#include "fullscreen_detector.h"
#include "playnite_integration.h"
#include "src/logging.h"
#include "src/platform/windows/ipc/display_settings_client.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace std::chrono_literals;

namespace platf::game_activity {
  namespace {
    constexpr auto POLL_INTERVAL = 100ms;
    constexpr auto FOREGROUND_CACHE_LIFETIME = 250ms;
    constexpr auto HEURISTIC_PROMOTION_DELAY = 300ms;
    // Long enough to outlast an alt-tab round trip. Every demotion is a real mode
    // set on the virtual display, and a mode set that lands while the display stack
    // is already churning can stall for seconds.
    constexpr auto DEMOTION_DELAY = 1500ms;
    // Repeated switching costs far more than running at the wrong rate for a few
    // extra seconds, so once the state has flipped this often inside the window,
    // every further change (including the otherwise-immediate promotion) has to
    // prove itself first.
    constexpr auto FLAP_WINDOW = 30s;
    constexpr int FLAP_THRESHOLD = 3;
    constexpr auto FLAP_EXTRA_DELAY = 4s;
    constexpr auto AMBIGUOUS_SAMPLE_GRACE = 5s;
    constexpr auto DISPLAY_TRANSITION_MINIMUM_HOLD = 500ms;
    constexpr auto DISPLAY_TRANSITION_SETTLE_TIME = 1s;
    constexpr auto RETRY_DELAY = 2s;
    constexpr auto EXPECTED_TRANSITION_LIFETIME = 5s;

    struct cached_foreground_t {
      RECT rect {};
      foreground_app::state_t state;
      foreground_app::state_t last_confirmed;
      std::chrono::steady_clock::time_point sampled_at {};
    };

    std::mutex g_foreground_cache_mutex;
    std::vector<cached_foreground_t> g_foreground_cache;

    // Process-wide because the consumers (capture/encode) never see the per-display
    // refresh target that owns the transition.
    std::atomic<int> g_mode_changes_in_flight {0};
    std::atomic<long long> g_mode_change_settled_at_ms {0};

    std::mutex g_refresh_targets_mutex;
    std::unordered_map<std::string, std::weak_ptr<refresh_target_t>> g_refresh_targets;

    std::string refresh_target_key(std::string device_id) {
      std::ranges::transform(device_id, device_id.begin(), [](const unsigned char value) {
        return static_cast<char>(std::tolower(value));
      });
      return device_id;
    }

    long long steady_now_ms() {
      return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()
      )
        .count();
    }

    bool same_rect(const RECT &lhs, const RECT &rhs) {
      return lhs.left == rhs.left && lhs.top == rhs.top &&
             lhs.right == rhs.right && lhs.bottom == rhs.bottom;
    }

    void publish_foreground(
      const RECT &rect,
      const foreground_app::state_t &state,
      const bool update_last_confirmed = true
    ) {
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
        if (update_last_confirmed &&
            state.fullscreen_on_capture_display &&
            state.source != "desktop-visible" &&
            state.source != "visibility-unknown") {
          existing->last_confirmed = state;
        }
        existing->sampled_at = now;
        return;
      }
      cached_foreground_t entry {
        .rect = rect,
        .state = state,
        .sampled_at = now,
      };
      if (update_last_confirmed &&
          state.fullscreen_on_capture_display &&
          state.source != "desktop-visible" &&
          state.source != "visibility-unknown") {
        entry.last_confirmed = state;
      }
      g_foreground_cache.push_back(std::move(entry));
    }

    signal_t foreground_signal(const foreground_app::state_t &foreground) {
      if (foreground.source == "desktop-visible" ||
          foreground.source == "visibility-unknown") {
        return {};
      }
      if (foreground.source == "playnite-status" ||
          foreground.source == "playnite-visible") {
        return {signal_source_e::playnite, true, foreground.foreground_pid, foreground.foreground_exe};
      }
      if (foreground.matches_active_app &&
          (foreground.source == "process" ||
           foreground.source == "process-visible" ||
           foreground.source == "playnite-cache")) {
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
      if (foreground.source == "desktop-visible" ||
          foreground.source == "visibility-unknown") {
        return {};
      }
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

    bool process_is_running(const DWORD pid) {
      if (pid == 0) {
        return false;
      }
      const auto process = OpenProcess(SYNCHRONIZE, FALSE, pid);
      if (!process) {
        return false;
      }
      const auto wait_result = WaitForSingleObject(process, 0);
      CloseHandle(process);
      return wait_result == WAIT_TIMEOUT;
    }

    bool overlay_preserves_confirmed_game(
      const fullscreen_detector::result_t &detection,
      const foreground_app::state_t &sample,
      const foreground_app::state_t &last_confirmed
    ) {
      return detection.verdict == fullscreen_detector::verdict_e::fullscreen &&
             detection.source == fullscreen_detector::source_e::overlay_preserved &&
             sample.matching_game_fullscreen &&
             last_confirmed.fullscreen_on_capture_display &&
             detection.pid != 0 &&
             detection.pid == last_confirmed.foreground_pid &&
             process_is_running(last_confirmed.foreground_pid);
    }

    foreground_app::state_t desktop_effective_foreground(
      const foreground_app::state_t &sample,
      const fullscreen_detector::result_t &detection
    ) {
      auto desktop = sample;
      desktop.valid_window = sample.blocker_present;
      desktop.shell_window = sample.blocker_desktop_ui;
      desktop.fullscreen_on_capture_display = false;
      desktop.matches_active_app = false;
      desktop.foreground_pid =
        sample.blocker_pid != 0 ? sample.blocker_pid : detection.pid;
      desktop.foreground_exe = sample.blocker_exe;
      desktop.source = "desktop-visible";
      return desktop;
    }
  }  // namespace

  state_t reduce_signals(std::span<const signal_t> signals) {
    return game_activity_policy::reduce_signals(signals);
  }

  const char *source_name(const signal_source_e source) {
    switch (source) {
      case signal_source_e::playnite:
        return "playnite";
      case signal_source_e::tracked_process:
        return "tracked-process";
      case signal_source_e::fullscreen_foreground:
        return "fullscreen-foreground";
      case signal_source_e::shell_fullscreen:
        return "shell-fullscreen";
      default:
        return "none";
    }
  }

  bool preserve_confirmed_game_during_display_transition(
    const foreground_app::state_t &sample,
    const foreground_app::state_t &last_confirmed,
    const bool transition_settling,
    const bool minimum_hold_active
  ) {
    const auto policy_sample = game_activity_policy::foreground_sample_t {
      .fullscreen_on_capture_display = sample.fullscreen_on_capture_display,
      .matching_window_seen = sample.matching_window_seen,
      .source = sample.source,
    };
    const auto policy_last_confirmed = game_activity_policy::foreground_sample_t {
      .fullscreen_on_capture_display = last_confirmed.fullscreen_on_capture_display,
      .matching_window_seen = last_confirmed.matching_window_seen,
      .source = last_confirmed.source,
    };
    return game_activity_policy::preserve_confirmed_game_during_display_transition(
      policy_sample,
      policy_last_confirmed,
      transition_settling,
      minimum_hold_active
    );
  }

  foreground_app::state_t foreground_snapshot(const std::optional<RECT> &capture_rect) {
    if (capture_rect) {
      const auto now = std::chrono::steady_clock::now();
      foreground_app::state_t last_confirmed;
      {
        std::scoped_lock lock {g_foreground_cache_mutex};
        const auto cached = std::find_if(g_foreground_cache.begin(), g_foreground_cache.end(), [&](const auto &entry) {
          return same_rect(entry.rect, *capture_rect);
        });
        if (cached != g_foreground_cache.end()) {
          if (now - cached->sampled_at <= FOREGROUND_CACHE_LIFETIME) {
            return cached->state;
          }
          last_confirmed = cached->last_confirmed;
        }
      }
      auto sample = foreground_app::snapshot(
        capture_rect,
        last_confirmed.foreground_pid,
        last_confirmed.foreground_exe
      );
      const auto detection = fullscreen_detector::detect(sample, *capture_rect);
      const bool overlay_preserved =
        overlay_preserves_confirmed_game(detection, sample, last_confirmed);
      if (overlay_preserved) {
        sample = last_confirmed;
      } else if (detection.verdict == fullscreen_detector::verdict_e::desktop) {
        sample = desktop_effective_foreground(sample, detection);
      }
      const bool overlay_sample =
        detection.source == fullscreen_detector::source_e::overlay_preserved;
      publish_foreground(
        *capture_rect,
        sample,
        !overlay_sample && !overlay_preserved
      );
      return sample;
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
      foreground_app::state_t last_confirmed_foreground;
      std::string previous_visibility_signature;
      auto display_transition_minimum_hold_until = std::chrono::steady_clock::time_point {};
      auto display_transition_settle_until = std::chrono::steady_clock::time_point {};
      auto retry_after = std::chrono::steady_clock::time_point {};
      auto hold_until = std::chrono::steady_clock::time_point {};
      auto detection_unknown_since = std::chrono::steady_clock::time_point {};
      std::string previous_detector_signature;

      while (!stop_token.stop_requested()) {
        const auto now = std::chrono::steady_clock::now();
        const auto raw_foreground = foreground_app::snapshot(
          options.capture_rect,
          last_confirmed_foreground.foreground_pid,
          last_confirmed_foreground.foreground_exe
        );
        const auto detection = fullscreen_detector::detect(raw_foreground, options.capture_rect);
        if (detection.verdict == fullscreen_detector::verdict_e::unknown) {
          if (detection_unknown_since == std::chrono::steady_clock::time_point {}) {
            detection_unknown_since = now;
          }
        } else {
          detection_unknown_since = {};
        }
        auto foreground = raw_foreground;
        const bool preserve_through_own_transition =
          preserve_confirmed_game_during_display_transition(
            raw_foreground,
            last_confirmed_foreground,
            now < display_transition_settle_until,
            now < display_transition_minimum_hold_until
          );
        const bool preserve_through_overlay =
          !preserve_through_own_transition &&
          overlay_preserves_confirmed_game(
            detection,
            raw_foreground,
            last_confirmed_foreground
          );
        if (preserve_through_own_transition) {
          foreground = last_confirmed_foreground;
        } else if (preserve_through_overlay) {
          foreground = last_confirmed_foreground;
        } else if (detection.verdict == fullscreen_detector::verdict_e::desktop) {
          foreground = desktop_effective_foreground(raw_foreground, detection);
        }
        const bool overlay_sample =
          detection.source == fullscreen_detector::source_e::overlay_preserved;
        publish_foreground(
          options.capture_rect,
          foreground,
          !overlay_sample &&
            !preserve_through_own_transition &&
            !preserve_through_overlay
        );

        std::vector<signal_t> signals;
        signals.reserve(3);
        const auto playnite_games = platf::playnite::get_active_game_statuses();
        signals.push_back(playnite_foreground_signal(foreground, playnite_games));
        signals.push_back(foreground_signal(foreground));

        auto resolved = game_activity_policy::reduce_signals(signals);
        if (detection.verdict == fullscreen_detector::verdict_e::fullscreen) {
          if (resolved.source == signal_source_e::none) {
            resolved = {
              .active = true,
              .source = signal_source_e::shell_fullscreen,
              .pid = detection.pid,
            };
          }
        } else if (detection.verdict == fullscreen_detector::verdict_e::desktop) {
          resolved = {};
        }
        const auto detector_signature =
          std::string(fullscreen_detector::verdict_name(detection.verdict)) + "|" +
          fullscreen_detector::source_name(detection.source) + "|" +
          std::to_string(detection.pid);
        if (detector_signature != previous_detector_signature) {
          BOOST_LOG(debug) << "Fullscreen detector: display='" << options.display_name
                           << "' verdict=" << fullscreen_detector::verdict_name(detection.verdict)
                           << " provider=" << fullscreen_detector::source_name(detection.source)
                           << " pid=" << detection.pid;
          previous_detector_signature = detector_signature;
        }

        const auto visibility_signature =
          raw_foreground.source + "|" +
          raw_foreground.blocker_reason + "|" +
          std::to_string(raw_foreground.blocker_pid) + "|" +
          raw_foreground.blocker_exe + "|" +
          raw_foreground.blocker_classification + "|" +
          std::to_string(raw_foreground.blocker_coverage_percent) + "|" +
          (raw_foreground.definite_desktop_blocker_present ? "definite-desktop|" : "no-definite-desktop|") +
          (preserve_through_own_transition ?
             "transition-held" :
             (preserve_through_overlay ?
                "overlay-held" :
                "effective"));
        if (visibility_signature != previous_visibility_signature) {
          if (raw_foreground.blocker_present ||
              raw_foreground.source == "desktop-visible" ||
              raw_foreground.source == "visibility-unknown") {
            BOOST_LOG(debug) << "Game visibility: display='" << options.display_name
                             << "' classification=" << raw_foreground.source
                             << " effective="
                             << (preserve_through_own_transition ?
                                   "game-transition" :
                                   (preserve_through_overlay ?
                                      "game-overlay" :
                                      foreground.source))
                             << " reason=" << raw_foreground.blocker_reason
                             << " overlay_classification=" << raw_foreground.blocker_classification
                             << " blocker_pid=" << raw_foreground.blocker_pid
                             << " blocker_exe='" << raw_foreground.blocker_exe << "'"
                             << " blocker_class='" << raw_foreground.blocker_class << "'"
                             << " blocker_title='" << raw_foreground.blocker_title << "'"
                             << " blocker_opaque=" << (raw_foreground.blocker_opaque ? "true" : "false")
                             << " blocker_framed=" << (raw_foreground.blocker_framed ? "true" : "false")
                             << " blocker_passive_overlay=" << (raw_foreground.blocker_passive_overlay ? "true" : "false")
                             << " blocker_desktop_ui=" << (raw_foreground.blocker_desktop_ui ? "true" : "false")
                             << " blocker_coverage=" << raw_foreground.blocker_coverage_percent << '%'
                             << " definite_desktop_blocker="
                             << (raw_foreground.definite_desktop_blocker_present ? "true" : "false")
                             << " blocker_rect=[" << raw_foreground.blocker_rect.left << ','
                             << raw_foreground.blocker_rect.top << ','
                             << raw_foreground.blocker_rect.right << ','
                             << raw_foreground.blocker_rect.bottom << ']'
                             << " matching_game_window_seen=" << (raw_foreground.matching_window_seen ? "true" : "false")
                             << " matching_game_fullscreen=" << (raw_foreground.matching_game_fullscreen ? "true" : "false");
          }
          previous_visibility_signature = visibility_signature;
        }
        if (resolved.active != previous_state.active || resolved.source != previous_state.source ||
            resolved.pid != previous_state.pid || resolved.executable != previous_state.executable) {
          BOOST_LOG(debug) << "Game activity: display='" << options.display_name
                           << "' active=" << (resolved.active ? "1" : "0")
                           << " source=" << source_name(resolved.source)
                           << " detector=" << fullscreen_detector::source_name(detection.source)
                           << " visibility=" << foreground.source
                           << " ignored_passive_windows=" << foreground.ignored_passive_window_count
                           << " pid=" << resolved.pid
                           << " exe='" << resolved.executable << "'";
          previous_state = resolved;
        }
        if (resolved.active &&
            !preserve_through_own_transition &&
            !overlay_sample &&
            !preserve_through_overlay &&
            foreground.fullscreen_on_capture_display &&
            foreground.source != "desktop-visible" &&
            foreground.source != "visibility-unknown") {
          last_confirmed_foreground = foreground;
        }

        if (now < hold_until) {
          // Our own mode-set drops games out of fullscreen for a moment. Ignore samples
          // taken during the transition instead of chasing them into a second mode-set.
          candidate_high = applied_high;
          candidate_since = now;
        } else if (detection.verdict == fullscreen_detector::verdict_e::unknown &&
                   now - detection_unknown_since < AMBIGUOUS_SAMPLE_GRACE) {
          // Briefly bridge indeterminate samples, but do not let them latch the
          // high-refresh candidate forever after confirmed identity disappears.
        } else if (resolved.active != candidate_high) {
          candidate_high = resolved.active;
          candidate_since = now;
        }

        if (now - flap_window_start > FLAP_WINDOW) {
          flap_window_start = now;
          flap_count = 0;
        }
        const auto base_delay =
          candidate_high && resolved.source >= signal_source_e::playnite ?
            0ms :
            (candidate_high ? HEURISTIC_PROMOTION_DELAY : DEMOTION_DELAY);
        const auto required_delay =
          flap_count >= FLAP_THRESHOLD ?
            std::chrono::duration_cast<std::chrono::milliseconds>(base_delay + FLAP_EXTRA_DELAY) :
            std::chrono::duration_cast<std::chrono::milliseconds>(base_delay);
        if (candidate_high != applied_high &&
            now - candidate_since >= required_delay &&
            now >= retry_after) {
          const auto numerator = candidate_high ? options.high_refresh_numerator : options.base_refresh_numerator;
          const auto denominator = candidate_high ? options.high_refresh_denominator : options.base_refresh_denominator;
          const bool changes_display_mode = !options.apply_activity_state;
          if (changes_display_mode) {
            begin_mode_change();
          }
          const bool applied = options.apply_activity_state ?
                                 options.apply_activity_state(candidate_high) :
                                 display_helper_client::send_refresh_rate(options.device_id, numerator, denominator);
          if (changes_display_mode) {
            finish_mode_change();
          }
          if (applied) {
            applied_high = candidate_high;
            const auto applied_at = std::chrono::steady_clock::now();
            ++flap_count;
            if (changes_display_mode) {
              hold_until = applied_at + DISPLAY_TRANSITION_SETTLE_TIME;
              if (candidate_high) {
                display_transition_minimum_hold_until = applied_at + DISPLAY_TRANSITION_MINIMUM_HOLD;
                display_transition_settle_until = applied_at + DISPLAY_TRANSITION_SETTLE_TIME;
              }
            }
            BOOST_LOG(info) << (changes_display_mode ? "Virtual display refresh: display='" : "WGC activity admission: display='")
                            << options.display_name
                            << "' source=" << source_name(resolved.source)
                            << " detector=" << fullscreen_detector::source_name(detection.source)
                            << " visibility=" << foreground.source
                            << " rate=" << numerator << '/' << denominator;
          } else {
            retry_after = now + RETRY_DELAY;
            if (changes_display_mode) {
              BOOST_LOG(warning) << "Virtual display refresh: failed to apply " << numerator << '/' << denominator
                                 << " to device='" << options.device_id << "'";
            } else {
              BOOST_LOG(warning) << "WGC activity admission: failed to apply " << numerator << '/' << denominator;
            }
          }
        }

        std::this_thread::sleep_for(POLL_INTERVAL);
      }
    }

    void begin_mode_change() {
      g_mode_changes_in_flight.fetch_add(1, std::memory_order_acq_rel);
    }

    void finish_mode_change() {
      g_mode_change_settled_at_ms.store(
        steady_now_ms() +
          std::chrono::duration_cast<std::chrono::milliseconds>(DISPLAY_TRANSITION_SETTLE_TIME).count(),
        std::memory_order_release
      );
      g_mode_changes_in_flight.fetch_sub(1, std::memory_order_acq_rel);
    }

    refresh_target_options_t options;
    bool applied_high {false};
    bool candidate_high {false};
    std::chrono::steady_clock::time_point candidate_since {};
    std::chrono::steady_clock::time_point flap_window_start {std::chrono::steady_clock::now()};
    int flap_count {0};

    std::jthread worker;
  };

  refresh_target_t::refresh_target_t(refresh_target_options_t options):
      impl_ {std::make_unique<impl_t>(std::move(options))} {
  }

  refresh_target_t::~refresh_target_t() = default;

  bool display_mode_change_in_flight() {
    return g_mode_changes_in_flight.load(std::memory_order_acquire) > 0 ||
           steady_now_ms() < g_mode_change_settled_at_ms.load(std::memory_order_acquire);
  }

  std::shared_ptr<refresh_target_t> make_refresh_target(refresh_target_options_t options) {
    const bool session_bound_admission = static_cast<bool>(options.apply_activity_state);
    const auto target_identity = session_bound_admission ? options.display_name : options.device_id;
    if (target_identity.empty() || options.base_refresh_numerator == 0 ||
        options.base_refresh_denominator == 0 || options.high_refresh_numerator == 0 ||
        options.high_refresh_denominator == 0) {
      return {};
    }

    // WGC admission callbacks close over one ipc_session_t. Reusing a controller
    // by display name would retain the original session after a second capture
    // instance takes over that display, so keep those controllers session-bound.
    if (session_bound_admission) {
      return std::shared_ptr<refresh_target_t>(new refresh_target_t(std::move(options)));
    }

    const auto target_key = refresh_target_key(target_identity);
    std::scoped_lock lock {g_refresh_targets_mutex};
    std::erase_if(g_refresh_targets, [](const auto &entry) {
      return entry.second.expired();
    });
    if (const auto existing = g_refresh_targets.find(target_key);
        existing != g_refresh_targets.end()) {
      if (auto target = existing->second.lock()) {
        BOOST_LOG(debug) << "Virtual display refresh: display='" << options.display_name
                         << "' reusing active controller for target='" << target_identity
                         << "'; original stream retains refresh policy ownership";
        return target;
      }
    }

    auto target = std::shared_ptr<refresh_target_t>(new refresh_target_t(std::move(options)));
    g_refresh_targets[target_key] = target;
    return target;
  }

}  // namespace platf::game_activity
