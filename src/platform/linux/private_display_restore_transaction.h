/**
 * @file src/platform/linux/private_display_restore_transaction.h
 * @brief Guarded desktop restoration shared by production and sequencing tests.
 */
#pragma once

#include "private_display_restore_policy.h"

#include <algorithm>
#include <iterator>
#include <set>
#include <string>
#include <vector>

namespace platf::linux_private_display::restore_transaction {
  enum class result_e {
    restored,
    cancelled,
    no_guard,
    guard_configuration_failed,
    guard_activation_failed,
    guard_activation_lost,
    guard_capture_failed,
    topology_configuration_failed,
    topology_activation_failed,
    guard_capture_lost,
    connector_disconnect_failed,
    connector_disappearance_failed,
    settled_query_failed,
    guard_recovery_configuration_failed,
    guard_recovery_activation_failed,
    guard_recovery_capture_failed,
    final_configuration_failed,
    final_verification_failed,
    final_guard_capture_failed,
  };

  constexpr const char *result_name(result_e result) {
    switch (result) {
      case result_e::restored: return "restored";
      case result_e::cancelled: return "cancelled";
      case result_e::no_guard: return "no connected saved restore guard";
      case result_e::guard_configuration_failed: return "guard configuration failed";
      case result_e::guard_activation_failed: return "guard activation verification failed";
      case result_e::guard_activation_lost: return "guard activation lost before retirement";
      case result_e::guard_capture_failed: return "guard capture publication failed";
      case result_e::topology_configuration_failed: return "pre-retirement output deactivation failed";
      case result_e::topology_activation_failed: return "saved topology activation verification failed";
      case result_e::guard_capture_lost: return "guard capture publication lost before retirement";
      case result_e::connector_disconnect_failed: return "private connector disconnect failed";
      case result_e::connector_disappearance_failed: return "private connector disappearance verification failed";
      case result_e::settled_query_failed: return "post-retirement topology query failed";
      case result_e::guard_recovery_configuration_failed: return "post-retirement guard configuration failed";
      case result_e::guard_recovery_activation_failed: return "post-retirement guard activation verification failed";
      case result_e::guard_recovery_capture_failed: return "post-retirement guard capture publication failed";
      case result_e::final_configuration_failed: return "post-retirement topology repair failed";
      case result_e::final_verification_failed: return "complete saved topology verification failed";
      case result_e::final_guard_capture_failed: return "final restored guard capture publication failed";
    }
    return "unknown restore result";
  }

  /**
   * Keep retiring scanouts until a distinct saved output is verified and
   * capture-ready. Retire private outputs before activating the full baseline,
   * which may already occupy every compositor output slot. The caller retains
   * its baseline and ownership on every failure, and clears them only when
   * this returns restored.
   * Operations supply the production planner and external compositor/driver
   * boundaries; this function owns their ordering and cancellation checks.
   */
  template <typename Configuration, typename Operations>
  result_e perform_guarded_restore(
    const Configuration &snapshot,
    const Configuration &current,
    const std::set<std::string> &retiring,
    Operations &operations
  ) {
    if (!operations.allowed()) return result_e::cancelled;
    const auto arguments = operations.plan(snapshot, current, retiring);
    if (!arguments.guard_output) return result_e::no_guard;
    auto guard_output = *arguments.guard_output;
    const auto snapshot_for_guard = [&](const std::string &name) {
      auto result = Configuration::object();
      result["outputs"] = Configuration::array();
      for (const auto &saved : snapshot["outputs"]) {
        if (saved.value("name", std::string {}) == name) {
          result["outputs"].push_back(saved);
          break;
        }
      }
      return result;
    };
    auto guard_snapshot = snapshot_for_guard(guard_output);
    if (guard_snapshot["outputs"].empty()) return result_e::no_guard;
    if (!operations.allowed()) return result_e::cancelled;
    if (!operations.configure(arguments.guard_activate, "topology restore guard activation")) {
      return result_e::guard_configuration_failed;
    }
    if (!operations.wait_snapshot(guard_snapshot, false)) return result_e::guard_activation_failed;
    if (!operations.wait_capture(guard_output)) return result_e::guard_capture_failed;

    std::vector<std::string> deactivate;
    auto guard_prefix = "output." + guard_output + ".";
    std::ranges::copy_if(arguments.deactivate, std::back_inserter(deactivate), [&](const auto &argument) {
      if (argument.starts_with(guard_prefix)) return false;
      return std::ranges::none_of(retiring, [&](const auto &name) {
        return argument.starts_with("output." + name + ".");
      });
    });
    // Free unwanted non-retiring outputs, while preserving the verified guard
    // and every private scanout until the guard is rechecked below. Enabling
    // the rest of the desktop here can exceed the compositor's active limit.
    if (!operations.allowed()) return result_e::cancelled;
    if (!operations.configure(deactivate, "topology restore deactivation")) return result_e::topology_configuration_failed;
    for (const auto &name : retiring) {
      if (!operations.wait_snapshot(guard_snapshot, false)) return result_e::guard_activation_lost;
      if (!operations.wait_capture(guard_output)) return result_e::guard_capture_lost;
      if (!operations.allowed()) return result_e::cancelled;
      if (!operations.disconnect(name)) return result_e::connector_disconnect_failed;
      // A hotplug can load a different remembered desktop. Observe its removal
      // before checking the guard for the next connector retirement.
      if (!operations.wait_disconnected(std::set<std::string> {name})) return result_e::connector_disappearance_failed;
    }
    const auto settled = operations.query();
    if (!settled) return result_e::settled_query_failed;
    const auto final_arguments = operations.plan(snapshot, *settled, retiring);
    // KWin can switch to a remembered output set on the last hot-unplug. If
    // that disturbed the guard, restore and verify a saved guard before any
    // final transaction can disable a currently active desktop output.
    if (!restore_policy::snapshot_matches(guard_snapshot, *settled) || !operations.wait_capture(guard_output)) {
      if (!final_arguments.guard_output) return result_e::no_guard;
      guard_output = *final_arguments.guard_output;
      guard_prefix = "output." + guard_output + ".";
      guard_snapshot = snapshot_for_guard(guard_output);
      if (guard_snapshot["outputs"].empty()) return result_e::no_guard;
      if (!operations.allowed()) return result_e::cancelled;
      if (!operations.configure(final_arguments.guard_activate, "post-disconnect restore guard activation")) {
        return result_e::guard_recovery_configuration_failed;
      }
      if (!operations.wait_snapshot(guard_snapshot, false)) return result_e::guard_recovery_activation_failed;
      if (!operations.wait_capture(guard_output)) return result_e::guard_recovery_capture_failed;
    }
    if (!restore_policy::snapshot_matches(snapshot, *settled, true)) {
      std::vector<std::string> final_restore;
      std::ranges::copy_if(final_arguments.deactivate, std::back_inserter(final_restore), [&](const auto &argument) {
        return !argument.starts_with(guard_prefix);
      });
      std::ranges::copy_if(final_arguments.activate, std::back_inserter(final_restore), [&](const auto &argument) {
        // Keep the verified guard live without retraining its mode/color link.
        // Saved priority is reapplied after the private output slots disappear.
        return !argument.starts_with(guard_prefix) || argument.starts_with(guard_prefix + "priority.");
      });
      if (!operations.allowed()) return result_e::cancelled;
      if (!operations.configure(final_restore, "post-disconnect topology restore")) {
        return result_e::final_configuration_failed;
      }
    }
    const auto activation_snapshot = restore_policy::connected_activation_snapshot(snapshot, *settled);
    if (!operations.wait_snapshot(activation_snapshot, false)) return result_e::topology_activation_failed;
    if (!operations.wait_snapshot(snapshot, true)) return result_e::final_verification_failed;
    if (!operations.wait_capture(guard_output)) return result_e::final_guard_capture_failed;
    return operations.allowed() ? result_e::restored : result_e::cancelled;
  }
}  // namespace platf::linux_private_display::restore_transaction
