#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace playnite_launcher::lossless::policy {

  struct restart_state {
    std::size_t running_process_count = 0;
    bool stopped = false;
  };

  [[nodiscard]] bool should_launch_new_instance(restart_state state, bool force_launch);
  [[nodiscard]] bool should_accept_focus_candidate(bool has_filter, bool path_matches, bool has_main_window);
  [[nodiscard]] std::optional<std::wstring> select_launch_executable(
    const std::optional<std::wstring> &explicit_executable,
    const std::optional<std::wstring> &discovered_executable
  );
  [[nodiscard]] std::wstring build_executable_filter(const std::vector<std::wstring> &executable_names);

}  // namespace playnite_launcher::lossless::policy
