/**
 * @file src/platform/windows/wgc_screensaver_recovery.h
 * @brief Bounded capture recovery for positively observed screen saver episodes.
 */
#pragma once

#include <chrono>
#include <optional>

namespace platf::dxgi::wgc_screensaver {
  // Owned by the capture producer, across display/encoder recreation. An unknown
  // probe never starts or ends an episode, and never rearms an attempted fallback.
  class recovery_t {
  public:
    using clock = std::chrono::steady_clock;
    static constexpr auto probe_interval = std::chrono::milliseconds(250);
    static constexpr auto observation_lease = std::chrono::milliseconds(1500);
    static constexpr auto episode_cooldown = std::chrono::seconds(5);

    bool probe_due(clock::time_point now) const {
      return !last_probe_ || now - *last_probe_ >= probe_interval;
    }

    void observe(std::optional<bool> running, clock::time_point now, bool startup = false) {
      if (last_probe_ && now - *last_probe_ > observation_lease) {
        candidate_.reset();
      }
      last_probe_ = now;
      last_observation_ = running;
      if (!running) {
        candidate_.reset();
        return;
      }
      if (*running) {
        last_positive_ = now;
      }
      if (!candidate_ || candidate_->running != *running) {
        candidate_ = candidate_t {*running, now};
      }
      const bool confirmed = now - candidate_->since >= probe_interval || (startup && *running);
      if (!confirmed) {
        return;
      }
      if (*running && !episode_ && (!next_episode_ || now >= *next_episode_)) {
        episode_ = true;
        fallback_attempted_ = false;
        post_exit_refresh_ = false;
      } else if (!*running && episode_) {
        episode_ = false;
        post_exit_refresh_ = true;
        next_episode_ = now + episode_cooldown;
      }
    }

    bool prefer_dxgi(clock::time_point now) const {
      return episode_ && !fallback_attempted_ && last_observation_.value_or(false) &&
             last_positive_ && now - *last_positive_ <= observation_lease;
    }

    bool restart_wgc(clock::time_point now) const {
      // Also recover after dismissal when WGC kept returning black/stale content:
      // frame delivery alone cannot prove that the desktop image recovered.
      return prefer_dxgi(now) || post_exit_refresh_;
    }

    void starting_dxgi() {
      saver_dxgi_ = episode_;
      if (episode_) {
        fallback_attempted_ = true;
      }
    }

    void starting_wgc() {
      saver_dxgi_ = false;
      post_exit_refresh_ = false;
      if (episode_) {
        // A failed DXGI attempt, or a desktop change between requesting recovery
        // and creating the backend, must not cause repeated capture teardown.
        fallback_attempted_ = true;
      }
    }

    bool hold_dxgi(clock::time_point now) const {
      return saver_dxgi_ && episode_ && last_positive_ &&
             now - *last_positive_ <= observation_lease;
    }

    bool saver_dxgi() const {
      return saver_dxgi_;
    }

    void dxgi_failed() {
      saver_dxgi_ = false;
    }

  private:
    struct candidate_t {
      bool running;
      clock::time_point since;
    };

    std::optional<candidate_t> candidate_;
    std::optional<bool> last_observation_;
    std::optional<clock::time_point> last_probe_, last_positive_, next_episode_;
    bool episode_ = false;
    bool fallback_attempted_ = false;
    bool post_exit_refresh_ = false;
    bool saver_dxgi_ = false;
  };
}  // namespace platf::dxgi::wgc_screensaver
