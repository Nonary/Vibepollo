/** @file Portable policy for restoring a dropped legacy display event. */
#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <utility>

namespace display_helper::legacy_restore_event_policy {
  enum class retry_action_t { none, join_open_window, open_bounded_event_window };

  inline retry_action_t retry_action(const bool returned_required_device, const bool current_window_active) {
    if (!returned_required_device) return retry_action_t::none;
    return current_window_active ? retry_action_t::join_open_window : retry_action_t::open_bounded_event_window;
  }

  inline bool hint_retry_admitted(const std::int64_t deadline_ms, const std::int64_t now_ms) {
    return deadline_ms != 0 && now_ms <= deadline_ms;
  }

  /**
   * Preserve a display event received during a restore stage, but only let it
   * create another retry when that stage failed and a required physical
   * baseline device became present during the stage.
   *
   * Callers supply normalized IDs and only physical, non-excluded devices.
   */
  class deferred_physical_return_t {
  public:
    void begin_observation(const std::uint64_t generation) {
      generation_ = generation;
      required_baseline_.clear();
      present_before_.clear();
      active_ = true;
      observations_known_ = false;
      event_seen_ = false;
      pending_ = false;
      retry_admitted_ = false;
    }

    bool install_observations(
      const std::uint64_t generation,
      std::set<std::string> required_baseline,
      std::set<std::string> present_before,
      const bool observations_known
    ) {
      if (!active_ || generation != generation_) return false;
      required_baseline_ = std::move(required_baseline);
      present_before_ = std::move(present_before);
      observations_known_ = observations_known;
      return true;
    }

    void begin_attempt(
      const std::uint64_t generation,
      std::set<std::string> required_baseline,
      std::set<std::string> present_before,
      const bool observations_known = true
    ) {
      // A new ordinary retry supersedes any pending hint: its saved-layout
      // attempt itself is the retry opportunity for the newly present device.
      generation_ = generation;
      required_baseline_ = std::move(required_baseline);
      present_before_ = std::move(present_before);
      active_ = true;
      observations_known_ = observations_known;
      event_seen_ = false;
      pending_ = false;
      retry_admitted_ = false;
    }

    bool note_display_event(const std::uint64_t generation) {
      if (generation != generation_ || (!active_ && !pending_ && !retry_admitted_)) {
        return false;
      }
      if (pending_ || retry_admitted_) return true;
      // During preparation this is only a retained hint. The failed-stage
      // transition below still requires known before/after observations.
      event_seen_ = true;
      return true;
    }

    bool finish_attempt(const std::uint64_t generation, const bool failed) {
      if (!active_ || generation != generation_) {
        return false;
      }
      const bool observations_known = observations_known_;
      active_ = false;
      observations_known_ = false;
      pending_ = failed && observations_known && event_seen_;
      return pending_;
    }

    bool reconcile(
      const std::uint64_t generation,
      const std::uint64_t current_generation,
      const std::set<std::string> &present_now
    ) {
      if (!pending_ || generation != generation_ || generation != current_generation) {
        return false;
      }
      pending_ = false;
      for (const auto &id : required_baseline_) {
        if (!present_before_.contains(id) && present_now.contains(id)) {
          retry_admitted_ = true;
          return true;
        }
      }
      return false;
    }

    void discard_pending(const std::uint64_t generation) {
      if (pending_ && generation == generation_) {
        pending_ = false;
      }
    }

    void cancel_admission(const std::uint64_t generation) {
      if (generation == generation_) retry_admitted_ = false;
    }

    void clear() {
      generation_ = 0;
      required_baseline_.clear();
      present_before_.clear();
      active_ = false;
      observations_known_ = false;
      event_seen_ = false;
      pending_ = false;
      retry_admitted_ = false;
    }

    bool active() const { return active_; }
    bool pending() const { return pending_; }

  private:
    std::uint64_t generation_ {0};
    std::set<std::string> required_baseline_;
    std::set<std::string> present_before_;
    bool active_ {false};
    bool observations_known_ {false};
    bool event_seen_ {false};
    bool pending_ {false};
    bool retry_admitted_ {false};
  };
}  // namespace display_helper::legacy_restore_event_policy
