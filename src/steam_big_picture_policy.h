#pragma once

#include "steam_process_tracker.h"
#include <charconv>
#include <set>
#include <string_view>

namespace platf::steam::lifecycle {

  inline std::uint32_t big_picture_app_id(std::string_view environment) {
    while (!environment.empty()) {
      const auto end = environment.find('\0');
      if (end == std::string_view::npos) break;
      const auto entry = environment.substr(0, end);
      if (entry.starts_with("SteamAppId=")) {
        const auto value = entry.substr(11);
        std::uint32_t id = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), id);
        return parsed.ec == std::errc {} && parsed.ptr == value.data() + value.size() ? id : 0;
      }
      environment.remove_prefix(end + 1);
    }
    return 0;
  }

  inline tracked_tree big_picture_tree(
    const process_snapshot &baseline,
    const process_snapshot &current
  ) {
    tracked_tree result;
    if (!baseline.complete) return result;
    std::set<std::uint32_t> existing_apps;
    for (const auto &[pid, process] : baseline.processes) {
      if (process.steam_app_id) existing_apps.insert(process.steam_app_id);
    }
    for (const auto &[pid, process] : current.processes) {
      // A game that predates Big Picture owns its later children too. Steam
      // and untagged processes are never selected, including shared runtimes.
      if (!baseline.processes.contains(pid) && process.start_time_ticks && process.steam_app_id &&
          !existing_apps.contains(process.steam_app_id) &&
          !is_protected_steam_process(process)) {
        result.processes.emplace(pid, tracked_process {process, false});
      }
    }
    return result;
  }
}
