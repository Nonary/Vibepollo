#include "tools/playnite_launcher/lossless_scaling_policy.h"

#include <cwctype>

namespace playnite_launcher::lossless::policy {

  bool should_launch_new_instance(restart_state state, bool force_launch) {
    return force_launch || state.running_process_count == 0 || state.stopped;
  }

  bool should_accept_focus_candidate(bool has_filter, bool path_matches, bool has_main_window) {
    return has_main_window && (!has_filter || path_matches);
  }

  std::optional<std::wstring> select_launch_executable(
    const std::optional<std::wstring> &explicit_executable,
    const std::optional<std::wstring> &discovered_executable
  ) {
    return explicit_executable ? explicit_executable : discovered_executable;
  }

  std::wstring build_executable_filter(const std::vector<std::wstring> &executable_names) {
    std::wstring filter;
    for (const auto &name : executable_names) {
      std::wstring lowercase_name = name;
      for (auto &character : lowercase_name) {
        character = static_cast<wchar_t>(std::towlower(character));
      }
      if (lowercase_name.empty()) {
        continue;
      }
      if (!filter.empty()) {
        filter.push_back(L';');
      }
      filter.append(lowercase_name);
    }
    return filter;
  }

}  // namespace playnite_launcher::lossless::policy
