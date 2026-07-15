/**
 * @file src/platform/windows/rtx_hdr_runtime.cpp
 */

#include "rtx_hdr_runtime.h"

#include "foreground_app.h"
#include "game_activity.h"

#include "src/config.h"
#include "src/logging.h"

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace platf::rtx_hdr {
  namespace {
    constexpr auto FOREGROUND_REFRESH_INTERVAL = std::chrono::milliseconds(100);
    constexpr auto PROFILE_REFRESH_INTERVAL = std::chrono::seconds(5);
    constexpr auto PROFILE_REFRESH_SLOW_INTERVAL = std::chrono::seconds(15);
    constexpr auto PROFILE_REFRESH_MAX_INTERVAL = std::chrono::seconds(30);
    constexpr auto SLOW_PROFILE_LOOKUP_THRESHOLD = std::chrono::milliseconds(100);
    constexpr float SDR_BRIGHTNESS_NEUTRAL_WHITE_NITS = 100.0f;
    constexpr float SDR_BRIGHTNESS_MAX_WHITE_NITS = 200.0f;

    std::atomic<std::uint64_t> g_live_settings_generation {0};
    // High 32 bits are a change counter; low 32 bits are the peak represented by
    // the most recent frame state consumed by the TrueHDR conversion path.
    std::atomic<std::uint64_t> g_live_output_metadata_state {0};

    void publish_live_output_peak(const int peak_brightness) {
      const auto peak = static_cast<std::uint32_t>(std::clamp(peak_brightness, 400, 2000));
      auto current = g_live_output_metadata_state.load(std::memory_order_acquire);
      while (static_cast<std::uint32_t>(current) != peak) {
        const auto generation = static_cast<std::uint32_t>(current >> 32) + 1;
        const auto desired = (static_cast<std::uint64_t>(generation) << 32) | peak;
        if (g_live_output_metadata_state.compare_exchange_weak(
              current,
              desired,
              std::memory_order_acq_rel,
              std::memory_order_acquire
            )) {
          return;
        }
      }
    }

    runtime_values_t config_runtime_values() {
      runtime_values_t values;
      values.enabled = config::video.rtx_hdr.enabled;
      values.contrast = std::clamp(config::video.rtx_hdr.contrast + 100, 0, 200);
      values.saturation = std::clamp(config::video.rtx_hdr.saturation + 100, 0, 200);
      values.middle_gray = config::video.rtx_hdr.middle_gray;
      values.sdr_brightness = config::video.rtx_hdr.sdr_brightness;
      values.peak_brightness = config::video.rtx_hdr.peak_brightness;
      values.source = profile_source_e::config;
      return values;
    }

    runtime_values_t desktop_runtime_values() {
      runtime_values_t values;
      values.contrast = 100;
      values.saturation = 100;
      if (!config::runtime_config_override_enabled("rtx_hdr") || !config::video.rtx_hdr.enabled) {
        return values;
      }

      // Desktop/non-matching content should stay in the neutral SDR-to-PQ path.
      // Carry the configured SDR brightness boost for the encoder fallback,
      // but do not enable NVIDIA TrueHDR conversion for desktop frames.
      values.sdr_brightness = config::video.rtx_hdr.sdr_brightness;
      values.peak_brightness = config::video.rtx_hdr.peak_brightness;
      values.source = profile_source_e::config;
      return values;
    }

    std::string identity_key(const platf::foreground_app::state_t &foreground) {
      return foreground.active_app_exe + "\n" +
             foreground.foreground_exe + "\n" +
             foreground.active_app_name + "\n" +
             foreground.source;
    }

    std::string profile_executable(const platf::foreground_app::state_t &foreground) {
      return foreground.active_app_exe.empty() ? foreground.foreground_exe : foreground.active_app_exe;
    }

    void apply_values(frame_state_t &frame, const runtime_values_t &values) {
      frame.enabled = values.enabled;
      frame.contrast = values.contrast;
      frame.saturation = values.saturation;
      frame.middle_gray = values.middle_gray;
      frame.sdr_brightness = values.sdr_brightness;
      frame.peak_brightness = values.peak_brightness;
      frame.source = values.source;
    }

    void copy_foreground(frame_state_t &frame, const platf::foreground_app::state_t &foreground) {
      frame.has_active_app = foreground.has_active_app;
      frame.foreground_matches = foreground.matches_active_app;
      frame.foreground_exe = foreground.foreground_exe;
      frame.active_app_exe = foreground.active_app_exe;
      frame.foreground_source = foreground.source;
    }

    void set_low_priority_thread() {
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    }
  }  // namespace

  float sdr_brightness_to_white_nits(int brightness) {
    const auto clamped_brightness = static_cast<float>(std::clamp(brightness, 0, 100));
    const auto t = clamped_brightness / 100.0f;
    return SDR_BRIGHTNESS_NEUTRAL_WHITE_NITS +
           (SDR_BRIGHTNESS_MAX_WHITE_NITS - SDR_BRIGHTNESS_NEUTRAL_WHITE_NITS) * t;
  }

  struct runtime_t::backend_t {
    std::function<std::chrono::steady_clock::time_point()> now;
    std::function<platf::foreground_app::state_t(const std::optional<RECT> &capture_rect)> foreground_snapshot;
    std::function<resolved_profile_t(const std::string &executable)> resolve_profile;
    bool start_background_threads {true};
  };

  struct runtime_t::shared_state_t {
    struct profile_job_t {
      std::string identity_key;
      std::string executable;
      std::uint64_t generation {};
    };

    explicit shared_state_t(backend_t backend):
        backend {std::move(backend)} {
    }

    backend_t backend;
    mutable std::mutex mutex;
    std::condition_variable foreground_cv;
    std::condition_variable profile_cv;
    bool stopping {false};

    std::optional<RECT> latest_capture_rect;
    frame_state_t cached_frame_state;
    std::optional<resolved_profile_t> last_successful_profile;
    std::uint64_t cached_live_settings_generation {live_settings_generation()};

    std::string current_identity_key;
    std::uint64_t current_generation {0};
    std::chrono::steady_clock::time_point next_profile_refresh {};
    std::chrono::milliseconds profile_refresh_interval {PROFILE_REFRESH_INTERVAL};
    int consecutive_slow_or_failed_lookups {0};

    std::optional<profile_job_t> pending_profile_job;
    bool profile_lookup_in_flight {false};
    std::string in_flight_identity_key;
  };

  namespace {
    runtime_t::backend_t production_backend() {
      runtime_t::backend_t backend;
      backend.now = []() {
        return std::chrono::steady_clock::now();
      };
      backend.foreground_snapshot = [](const std::optional<RECT> &capture_rect) {
        return platf::game_activity::foreground_snapshot(capture_rect);
      };
      backend.resolve_profile = [](const std::string &executable) {
        return resolve_profile_for_executable(executable);
      };
      return backend;
    }

    void publish_initial_app_values_locked(runtime_t::shared_state_t &state, const platf::foreground_app::state_t &foreground) {
      frame_state_t frame;
      copy_foreground(frame, foreground);
      resolved_profile_t no_profile;
      apply_values(frame, materialize_runtime_values_for_tests(no_profile, config_runtime_values()));
      frame.lookup_available = false;
      state.cached_frame_state = std::move(frame);
    }

    void recompute_live_settings_locked(runtime_t::shared_state_t &state) {
      if (!state.cached_frame_state.has_active_app || !state.cached_frame_state.foreground_matches) {
        apply_values(state.cached_frame_state, desktop_runtime_values());
        return;
      }

      resolved_profile_t no_profile;
      const auto values = materialize_live_values(
        state.last_successful_profile.value_or(no_profile),
        config_runtime_values()
      );
      apply_values(state.cached_frame_state, values);
    }

    void enqueue_profile_lookup_locked(
      runtime_t::shared_state_t &state,
      const std::string &key,
      const std::string &executable,
      std::uint64_t generation,
      std::chrono::steady_clock::time_point now
    ) {
      if (state.pending_profile_job && state.pending_profile_job->identity_key == key) {
        return;
      }
      if (state.profile_lookup_in_flight && state.in_flight_identity_key == key) {
        return;
      }

      state.pending_profile_job = runtime_t::shared_state_t::profile_job_t {
        key,
        executable,
        generation,
      };
      state.next_profile_refresh = now + state.profile_refresh_interval;
      state.profile_cv.notify_one();
    }

    void poll_foreground_once(const std::shared_ptr<runtime_t::shared_state_t> &state) {
      runtime_t::backend_t backend;
      std::optional<RECT> capture_rect;
      {
        std::scoped_lock lk(state->mutex);
        if (state->stopping) {
          return;
        }
        backend = state->backend;
        capture_rect = state->latest_capture_rect;
      }

      const auto now = backend.now();
      const auto foreground = backend.foreground_snapshot(capture_rect);

      std::unique_lock lk(state->mutex);
      if (state->stopping) {
        return;
      }

      if (foreground.source != state->cached_frame_state.foreground_source ||
          foreground.matches_active_app != state->cached_frame_state.foreground_matches) {
        BOOST_LOG(debug) << "RTX HDR: foreground '" << foreground.source << "'"
                         << " matches=" << (foreground.matches_active_app ? "1" : "0")
                         << " fg_exe='" << foreground.foreground_exe << "'"
                         << " active='" << foreground.active_app_name << "'"
                         << " active_exe='" << foreground.active_app_exe << "'"
                         << " playnite=" << (foreground.uses_playnite ? "1" : "0")
                         << " fullscreen=" << (foreground.fullscreen_on_capture_display ? "1" : "0");
      }

      if (!foreground.has_active_app || !foreground.matches_active_app) {
        if (!state->current_identity_key.empty()) {
          ++state->current_generation;
          state->current_identity_key.clear();
        }
        state->next_profile_refresh = {};
        state->profile_refresh_interval = PROFILE_REFRESH_INTERVAL;
        state->consecutive_slow_or_failed_lookups = 0;
        state->pending_profile_job.reset();
        state->last_successful_profile.reset();

        frame_state_t frame;
        copy_foreground(frame, foreground);
        apply_values(frame, desktop_runtime_values());
        frame.lookup_available = false;
        state->cached_frame_state = std::move(frame);
        return;
      }

      const auto key = identity_key(foreground);
      const auto executable = profile_executable(foreground);
      const bool identity_changed = key != state->current_identity_key;
      if (identity_changed) {
        state->current_identity_key = key;
        ++state->current_generation;
        state->last_successful_profile.reset();
        state->profile_refresh_interval = PROFILE_REFRESH_INTERVAL;
        state->consecutive_slow_or_failed_lookups = 0;
        state->next_profile_refresh = now;
        publish_initial_app_values_locked(*state, foreground);
      } else {
        auto frame = state->cached_frame_state;
        copy_foreground(frame, foreground);
        state->cached_frame_state = std::move(frame);
      }

      if (identity_changed || now >= state->next_profile_refresh) {
        enqueue_profile_lookup_locked(*state, key, executable, state->current_generation, now);
      }
    }

    bool run_pending_profile_lookup(const std::shared_ptr<runtime_t::shared_state_t> &state) {
      runtime_t::backend_t backend;
      runtime_t::shared_state_t::profile_job_t job;
      {
        std::unique_lock lk(state->mutex);
        if (!state->pending_profile_job || state->stopping) {
          return false;
        }

        backend = state->backend;
        job = std::move(*state->pending_profile_job);
        state->pending_profile_job.reset();
        state->profile_lookup_in_flight = true;
        state->in_flight_identity_key = job.identity_key;
      }

      const auto start = backend.now();
      auto resolved = backend.resolve_profile(job.executable);
      const auto finish = backend.now();
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(finish - start);
      const bool slow = elapsed > SLOW_PROFILE_LOOKUP_THRESHOLD;
      const bool failed = !resolved.lookup_available;

      std::unique_lock lk(state->mutex);
      if (state->profile_lookup_in_flight && state->in_flight_identity_key == job.identity_key) {
        state->profile_lookup_in_flight = false;
        state->in_flight_identity_key.clear();
      }

      if (state->stopping ||
          state->current_generation != job.generation ||
          state->current_identity_key != job.identity_key ||
          !state->cached_frame_state.foreground_matches) {
        return true;
      }

      if (slow) {
        BOOST_LOG(warning) << "RTX HDR: NVIDIA profile lookup for '" << job.executable
                           << "' took " << elapsed.count() << " ms";
      }

      if (slow || failed) {
        ++state->consecutive_slow_or_failed_lookups;
        state->profile_refresh_interval = state->profile_refresh_interval < PROFILE_REFRESH_SLOW_INTERVAL ?
                                            PROFILE_REFRESH_SLOW_INTERVAL :
                                            PROFILE_REFRESH_MAX_INTERVAL;
      } else {
        state->consecutive_slow_or_failed_lookups = 0;
        state->profile_refresh_interval = PROFILE_REFRESH_INTERVAL;
      }

      // A transient driver read failure (NvAPI unavailable for one refresh) should not flicker an
      // already-active conversion off and back on; keep the last-known-good state until a lookup
      // succeeds. This only applies to the lookup-unavailable path -- an empty profile on a
      // successful lookup is a deliberate "no activating profile -> bypass" decision.
      if (failed && state->cached_frame_state.enabled) {
        state->next_profile_refresh = finish + state->profile_refresh_interval;
        return true;
      }

      if (resolved.lookup_available) {
        state->last_successful_profile = resolved;
      }
      const auto values = materialize_runtime_values_for_tests(resolved, config_runtime_values());
      auto frame = state->cached_frame_state;
      apply_values(frame, values);
      frame.lookup_available = resolved.lookup_available;
      state->cached_frame_state = std::move(frame);
      state->next_profile_refresh = finish + state->profile_refresh_interval;
      return true;
    }

    void foreground_worker_proc(std::shared_ptr<runtime_t::shared_state_t> state) {
      set_low_priority_thread();
      for (;;) {
        poll_foreground_once(state);

        std::unique_lock lk(state->mutex);
        if (state->stopping) {
          return;
        }
        state->foreground_cv.wait_for(lk, FOREGROUND_REFRESH_INTERVAL, [&] {
          return state->stopping;
        });
        if (state->stopping) {
          return;
        }
      }
    }

    void profile_worker_proc(std::shared_ptr<runtime_t::shared_state_t> state) {
      set_low_priority_thread();
      for (;;) {
        {
          std::unique_lock lk(state->mutex);
          state->profile_cv.wait(lk, [&] {
            return state->stopping || state->pending_profile_job.has_value();
          });
          if (state->stopping) {
            return;
          }
        }

        run_pending_profile_lookup(state);
      }
    }
  }  // namespace

  runtime_t::runtime_t():
      runtime_t {production_backend()} {
  }

#ifdef SUNSHINE_TESTS
  runtime_t::runtime_t(runtime_test_hooks_t hooks):
      runtime_t {backend_t {
        std::move(hooks.now),
        std::move(hooks.foreground_snapshot),
        std::move(hooks.resolve_profile),
        hooks.start_background_threads,
      }} {
  }
#endif

  runtime_t::runtime_t(backend_t backend):
      state {std::make_shared<shared_state_t>(std::move(backend))} {
  }

  runtime_t::~runtime_t() {
    if (!state) {
      return;
    }

    {
      std::scoped_lock lk(state->mutex);
      state->stopping = true;
    }
    state->foreground_cv.notify_all();
    state->profile_cv.notify_all();

    // Profile resolution is intentionally off the frame path and may be stuck in
    // driver code. Do not let stream teardown wait behind that external call.
    if (foreground_worker.joinable()) {
      foreground_worker.detach();
    }
    if (profile_worker.joinable()) {
      profile_worker.detach();
    }
  }

  void notify_live_settings_changed() {
    g_live_settings_generation.fetch_add(1, std::memory_order_acq_rel);
  }

  std::uint64_t live_settings_generation() {
    return g_live_settings_generation.load(std::memory_order_acquire);
  }

  live_output_metadata_state_t live_output_metadata_state() {
    const auto state = g_live_output_metadata_state.load(std::memory_order_acquire);
    return {
      static_cast<std::uint32_t>(state >> 32),
      static_cast<int>(static_cast<std::uint32_t>(state)),
    };
  }

  frame_state_t runtime_t::update_for_frame(const std::optional<RECT> &capture_rect) {
    frame_state_t frame;
    {
      std::scoped_lock lk(state->mutex);
      state->latest_capture_rect = capture_rect;
      const auto settings_generation = live_settings_generation();
      if (state->cached_live_settings_generation != settings_generation) {
        state->cached_live_settings_generation = settings_generation;
        recompute_live_settings_locked(*state);
      }
      frame = state->cached_frame_state;
    }

    publish_live_output_peak(frame.peak_brightness);

    start_workers();

    state->foreground_cv.notify_one();
    return frame;
  }

  void runtime_t::start_workers() {
    std::call_once(start_once, [&] {
      if (!state->backend.start_background_threads) {
        return;
      }

      foreground_worker = std::thread(foreground_worker_proc, state);
      profile_worker = std::thread(profile_worker_proc, state);
    });
  }

#ifdef SUNSHINE_TESTS
  void runtime_t::poll_foreground_for_tests() {
    poll_foreground_once(state);
  }

  bool runtime_t::run_pending_profile_lookup_for_tests() {
    return run_pending_profile_lookup(state);
  }

  std::chrono::milliseconds runtime_t::profile_refresh_interval_for_tests() const {
    std::scoped_lock lk(state->mutex);
    return state->profile_refresh_interval;
  }
#endif

}  // namespace platf::rtx_hdr
