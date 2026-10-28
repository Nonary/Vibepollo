#pragma once
#include "managed_app_focus.h"
#include "steam_process_tracker.h"

#include <vector>

namespace managed_app_focus {
  // The platform enumerator supplies only processes owned by the desktop
  // user. Steam identity handles Proton wrappers whose executable is outside
  // the install directory; matching a title alone is never sufficient.
  inline std::vector<std::uint64_t> candidates(const target &app, const platf::steam::lifecycle::process_snapshot &snapshot) {
    std::vector<std::uint64_t> result;
    if (app.install_dir.empty() || app.install_dir == app.install_dir.root_path()) {
      return result;
    }
    for (const auto &[pid, process] : snapshot.processes) {
      if (platf::steam::lifecycle::is_protected_steam_process(process)) {
        continue;
      }
      const bool belongs = app.provider == "steam" ?
                             process.steam_app_id != 0 && std::to_string(process.steam_app_id) == app.id :
                             app.provider == "lutris" &&
                               (platf::steam::lifecycle::path_is_within(process.executable, app.install_dir) ||
                                platf::steam::lifecycle::path_is_within(process.cwd, app.install_dir));
      if (belongs) {
        result.push_back(pid);
      }
    }
    return result;
  }
}  // namespace managed_app_focus
