/**
 * @file src/platform/linux/process_group.h
 * @brief Observe Linux process groups without consuming a child's exit status.
 */
#pragma once

#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
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

  /** Recheck negative observations so new groups are never cached as missing. */
  class group_observer_t {
  public:
    template<typename Observe>
    bool running(const pid_t group, const std::chrono::steady_clock::time_point now, Observe observe) {
      if (group <= 0) {
        return false;
      }
      if (group == _group && now < _positive_until) {
        return true;
      }
      const bool live = observe(group);
      _group = live ? group : 0;
      _positive_until = now + std::chrono::milliseconds {250};
      return live;
    }

  private:
    pid_t _group {0};
    std::chrono::steady_clock::time_point _positive_until {};
  };

  inline bool cached_group_running(const pid_t group) {
    // The control thread may poll once per input packet. Bound its procfs
    // scans without sharing mutable cache state between polling threads.
    thread_local group_observer_t observer;
    return observer.running(group, std::chrono::steady_clock::now(), group_running);
  }
}  // namespace platf::linux_process
