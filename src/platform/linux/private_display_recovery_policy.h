/** @file Incident and readiness gates for passive Linux display recovery. */
#pragma once

#include <cstdint>
#include <exception>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace platf::linux_private_display::recovery_policy {
  struct connector_state_t {
    bool known {false};
    bool connected {false};
    bool enabled_known {false};
    bool enabled {false};
    bool dpms_known {false};
    bool dpms_on {false};

    bool ready() const {
      return known && connected && enabled_known && enabled;
    }

    bool present() const {
      return known && connected;
    }
  };

  /** Real DPMS transitions and connector topology edges are recovery evidence.
   * Enabled scanout alone is deliberately not treated as a wake. */
  class wake_event_tracker_t {
  public:
    struct event_t {
      bool topology_changed {false};
      bool dpms_woke {false};
      explicit operator bool() const { return topology_changed || dpms_woke; }
    };

    void prime(const std::string &name, const connector_state_t &sample) {
      if (name.empty()) return;
      if (sample.known) previous_[name] = sample;
      else previous_.erase(name);
    }

    event_t observe(const std::string &name, const connector_state_t &sample) {
      if (name.empty()) return {};
      if (!sample.known) {
        previous_.erase(name);
        return {};
      }
      const auto previous = previous_.find(name);
      event_t event;
      // Enumeration can be incomplete. The first known sample establishes a
      // baseline; only an observed edge is evidence. KScreen separately
      // supplies verified topology changes, including newly added outputs.
      if (previous != previous_.end()) {
        event.topology_changed = previous->second.connected != sample.connected;
        event.dpms_woke = previous->second.dpms_known && sample.dpms_known &&
                          !previous->second.dpms_on && sample.dpms_on;
      }
      previous_[name] = sample;
      return event;
    }

  private:
    std::map<std::string, connector_state_t> previous_;
  };

  template<typename Topology>
  class topology_change_tracker_t {
  public:
    void prime(const Topology &sample) { previous_ = sample; }

    bool observe(const std::optional<Topology> &sample) {
      if (!sample) {
        previous_.reset();
        return false;
      }
      const bool changed = previous_ && *previous_ != *sample;
      previous_ = *sample;
      return changed;
    }

  private:
    std::optional<Topology> previous_;
  };

  struct incident_t {
    std::uint64_t cleanup_generation {0};
    std::string session_owner;
    std::string snapshot_identity;
    bool restore_callback_claimed {false};
    bool transaction_failed {false};
    bool helper_completion_known {false};
    bool attempted {false};
  };

  /** run_delayed_restore increments its generation before invoking restore. */
  inline bool failed_claim_is_current(
    const std::uint64_t claimed_generation,
    const std::uint64_t current_generation
  ) {
    return claimed_generation != 0 && claimed_generation == current_generation;
  }

  /** Do not turn an admission timeout, unknown helper completion, or stale
   * incident into a recovery attempt. A monitor gets one guarded opportunity. */
  inline bool may_start(
    const incident_t &incident,
    const std::uint64_t current_generation,
    const std::string &current_owner,
    const std::string &current_snapshot_identity,
    const bool helper_completion_known,
    const bool has_capture_owner,
    const bool has_retained_remote_monitor,
    const bool shutting_down
  ) {
    return incident.restore_callback_claimed && incident.transaction_failed &&
           incident.helper_completion_known && helper_completion_known &&
           !incident.attempted && incident.cleanup_generation == current_generation &&
           incident.session_owner == current_owner &&
           incident.snapshot_identity == current_snapshot_identity &&
           !has_capture_owner && !has_retained_remote_monitor && !shutting_down;
  }

  enum class target_policy_e { saved_baseline, preserve_live_physical_layout, automatic_recovery };
  enum class target_e { saved_baseline, live_physical_layout };

  inline target_e select_target(
    const target_policy_e policy,
    const bool has_connected_enabled_physical_output
  ) {
    return policy == target_policy_e::preserve_live_physical_layout ||
                   (policy == target_policy_e::automatic_recovery && has_connected_enabled_physical_output)
             ? target_e::live_physical_layout
             : target_e::saved_baseline;
  }

  inline bool solely_virtual_cleanup_failed(const bool connector_disconnect_failed,
                                            const bool connector_disappearance_failed) {
    return connector_disconnect_failed || connector_disappearance_failed;
  }

  template<typename Topology>
  bool topology_still_matches(const Topology &verified_before_retirement,
                              const Topology &fresh_topology) {
    return verified_before_retirement == fresh_topology;
  }

  inline bool managed_connector_retired(const bool exact_identity_still_present,
                                        const bool kernel_status_known,
                                        const bool kernel_disconnected,
                                        const bool compositor_inactive) {
    return !exact_identity_still_present ||
           (kernel_status_known && kernel_disconnected && compositor_inactive);
  }

  /** Contain malformed compositor data or filesystem failures in the passive
   * waiter; a monitor exception parks recovery without escaping the jthread. */
  template<typename Work, typename Failure>
  void run_monitor_safely(Work &&work, Failure &&failure) noexcept {
    try {
      std::forward<Work>(work)();
    } catch (const std::exception &error) {
      try {
        std::forward<Failure>(failure)(error.what());
      } catch (...) {}
    } catch (...) {
      try {
        std::forward<Failure>(failure)(std::string_view {});
      } catch (...) {}
    }
  }
}  // namespace platf::linux_private_display::recovery_policy
