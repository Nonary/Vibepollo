/** @file Narrow machine greeter-to-desktop transfer of pending physical restore intent. */
#pragma once

#include "private_display_restore_policy.h"
#include "private_display_snapshot_policy.h"

#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>

namespace platf::linux_private_display::handoff_policy {
  struct context_t {
    bool machine_host {false};
    std::string boot_id;
    std::string role;
    std::uint64_t generation {0};
  };

  /** Connector retirement is not successful restoration. An adopted pending
   * baseline survives an all-disabled desktop even after its orphan vanishes. */
  template<typename Json, typename Persist>
  bool capture(std::optional<Json> &snapshot, const Json &current,
               const std::set<std::string> &private_names, const bool reserved,
               const bool pending_handoff, Persist persist) {
    if (pending_handoff && snapshot) {
      if (!snapshot_policy::valid(current)) return false;
      const bool physical_active = std::ranges::any_of(current["outputs"], [](const auto &output) {
        return output.value("connected", false) && output.value("enabled", false) &&
               !mode_policy::managed_connector_name(output.value("name", std::string {}));
      });
      if (!physical_active) return true;
    }
    return snapshot_policy::capture(snapshot, current, private_names, reserved, std::move(persist));
  }

  template<typename Json>
  bool physical_identities_match(const Json &snapshot, const Json &current,
                                const std::map<std::string, std::string> &saved_identities,
                                const std::map<std::string, std::string> &current_identities) {
    bool guard = false;
    for (const auto &saved : snapshot["outputs"]) {
      const auto name = saved.value("name", std::string {});
      if (!saved.value("enabled", false) || mode_policy::managed_connector_name(name)) continue;
      const auto present = std::ranges::find_if(current["outputs"], [&](const auto &output) {
        return output.value("name", std::string {}) == name && output.value("connected", false);
      });
      if (present == current["outputs"].end()) continue;
      const auto expected = saved_identities.find(name);
      const auto observed = current_identities.find(name);
      if (expected == saved_identities.end() || observed == current_identities.end() ||
          expected->second.empty() || expected->second != observed->second ||
          !restore_policy::select_restore_mode(saved, &*present)) return false;
      guard = true;
    }
    return guard;
  }

  /** A controller rebind to the same greeter may advance its pending record
   * by exactly one generation, without replacing pre-mutation identities. */
  template<typename Json>
  std::optional<Json> refresh_greeter_generation(
    const std::string &contents, const std::string &owner, const context_t &context,
    const Json &current, const std::map<std::string, std::string> &current_identities
  ) {
    if (!context.machine_host || context.role != "greeter" || !owner.ends_with(":greeter") ||
        context.boot_id.empty() || context.generation == 0 || !snapshot_policy::valid(current)) return std::nullopt;
    try {
      bool pending = false;
      const auto snapshot = snapshot_policy::decode<Json>(contents, owner, &pending);
      if (!snapshot || !pending) return std::nullopt;
      auto record = Json::parse(contents);
      auto &handoff = record.at("machine_handoff");
      if (handoff.value("role", std::string {}) != "greeter" ||
          handoff.value("boot_id", std::string {}) != context.boot_id ||
          !handoff.at("generation").is_number_unsigned()) return std::nullopt;
      const auto previous_generation = handoff.at("generation").template get<std::uint64_t>();
      if (previous_generation == 0 || previous_generation == std::numeric_limits<std::uint64_t>::max() ||
          previous_generation + 1 != context.generation) return std::nullopt;
      const auto identities = handoff.at("physical_identities").template get<std::map<std::string, std::string>>();
      if (!physical_identities_match(*snapshot, current, identities, current_identities)) return std::nullopt;
      handoff["generation"] = context.generation;
      return record;
    } catch (...) {
      return std::nullopt;
    }
  }

  /** Ordinary owner decoding stays strict. Only the immediately succeeding
   * machine desktop may adopt a greeter's unfinished, same-boot mutation. */
  template<typename Json>
  std::optional<Json> adopt(const std::string &contents, const std::string &owner,
                            const context_t &context, const Json &current,
                            const std::map<std::string, std::string> &current_identities) {
    if (!context.machine_host || context.role != "desktop" || context.boot_id.empty() ||
        context.generation == 0 || !snapshot_policy::valid(current)) return std::nullopt;
    try {
      if (contents.size() > 1024 * 1024) return std::nullopt;
      const auto record = Json::parse(contents);
      const auto previous_owner = record.value("owner", std::string {});
      if (previous_owner == owner || !owner.ends_with(":desktop") || !previous_owner.ends_with(":greeter") ||
          !record.contains("restore_pending") || !record.value("restore_pending", false)) return std::nullopt;
      bool pending = false;
      const auto snapshot = snapshot_policy::decode<Json>(contents, previous_owner, &pending);
      if (!snapshot || !pending) return std::nullopt;
      const auto &handoff = record.at("machine_handoff");
      if (!handoff.is_object() || handoff.value("role", std::string {}) != "greeter" ||
          handoff.value("boot_id", std::string {}) != context.boot_id ||
          !handoff.at("generation").is_number_unsigned()) return std::nullopt;
      const auto previous_generation = handoff.at("generation").template get<std::uint64_t>();
      if (previous_generation == 0 || previous_generation == std::numeric_limits<std::uint64_t>::max() ||
          previous_generation + 1 != context.generation) return std::nullopt;
      for (const auto &output : current["outputs"]) {
        const auto name = output.value("name", std::string {});
        if (!output.value("connected", false)) continue;
        if (!mode_policy::managed_connector_name(name) && output.value("enabled", false)) return std::nullopt;
      }
      // The compositor may retire its virtual connector before the desktop
      // process reads the pending greeter record. The same-boot, adjacent
      // generation and connected physical identity checks below still bind
      // this transfer to the machine handoff; do not discard restore intent
      // merely because there is no longer a managed orphan to observe.
      const auto identities = handoff.at("physical_identities").template get<std::map<std::string, std::string>>();
      return physical_identities_match(*snapshot, current, identities, current_identities) ? snapshot : std::nullopt;
    } catch (...) {
      return std::nullopt;
    }
  }
}  // namespace platf::linux_private_display::handoff_policy
