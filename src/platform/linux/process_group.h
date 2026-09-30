/**
 * @file src/platform/linux/process_group.h
 * @brief Observe Linux process groups without consuming a child's exit status.
 */
#pragma once

#include <charconv>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/types.h>

namespace platf::linux_process {
  inline bool group_running(const pid_t group) {
    if (group <= 0) {
      return false;
    }
    std::error_code error;
    for (std::filesystem::directory_iterator it {"/proc", error}, end;
         !error && it != end;
         it.increment(error)) {
      const auto name = it->path().filename().string();
      pid_t pid = 0;
      const auto parsed = std::from_chars(name.data(), name.data() + name.size(), pid);
      if (parsed.ec != std::errc {} || parsed.ptr != name.data() + name.size()) {
        continue;
      }
      std::ifstream stat {it->path() / "stat"};
      std::string text;
      if (!std::getline(stat, text)) {
        continue;  // A concurrently exiting process may vanish.
      }
      // comm may contain whitespace or ')'; the final ')' precedes the state.
      const auto close = text.rfind(')');
      if (close == std::string::npos) {
        continue;
      }
      std::istringstream fields {text.substr(close + 1)};
      char state = 0;
      pid_t parent = 0, process_group = 0;
      if (fields >> state >> parent >> process_group; fields && process_group == group && state != 'Z' && state != 'X') {
        return true;
      }
    }
    // An unavailable procfs cannot prove quiescence. Preserve the group until
    // it disappears, using the non-destructive signal probe as a fallback.
    return error && (::kill(-group, 0) == 0 || errno == EPERM);
  }
}  // namespace platf::linux_process
