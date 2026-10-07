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

  struct launch_executables {
    std::optional<std::wstring> lossless_scaling;
    // The game executable is a focus/profile target, never a launch fallback.
    std::optional<std::wstring> game;
  };

  [[nodiscard]] constexpr bool should_enable_runtime(bool scaling_enabled, bool frame_generation_enabled) noexcept {
    return scaling_enabled || frame_generation_enabled;
  }

  [[nodiscard]] bool should_launch_new_instance(restart_state state, bool force_launch);
  [[nodiscard]] bool should_accept_focus_candidate(bool has_filter, bool path_matches, bool has_main_window);
  [[nodiscard]] std::optional<std::wstring> select_launch_executable(const launch_executables &executables);
  [[nodiscard]] std::wstring build_executable_filter(const std::vector<std::wstring> &executable_names);

}  // namespace playnite_launcher::lossless::policy
