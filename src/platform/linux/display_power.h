#pragma once

#include <memory>
#include <map>
#include <optional>
#include <string>
#include <chrono>
#include <functional>

namespace platf::display_power {
  // Carried from a pending launch into its active capture. The last owner
  // closes the broker connection and releases the session inhibitor.
  // A null result means that a machine-host request could not become ready.
  std::shared_ptr<void> acquire();

  struct dpms_state_t {
    bool known {false};
    bool on {false};
  };

  /** Read the current session compositor's DPMS state without requesting a
   * mode change. Missing protocol/output support returns an unknown sample. */
  std::optional<std::map<std::string, dpms_state_t>> query_dpms_states(
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max());

  struct observation_t {
    enum class kind_e { baseline_ready, power, topology } kind;
    std::string output;
    dpms_state_t power;
  };

  // Subscribe once to the selected session. Initial samples seed a baseline;
  // subsequent callbacks come from compositor events, without changing power.
  void observe_events(const std::function<bool(const observation_t &)> &on_event,
                      const std::function<bool()> &keep_running);
}  // namespace platf::display_power
