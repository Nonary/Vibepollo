#pragma once

#include <chrono>
#include <string>

namespace platf::touch {
  // Recheck even successful assignments: KWin or an output can be recreated
  // while the client retains its uinput device. A missing compositor/output
  // must not disable future attempts or hold up ordinary touch injection.
  struct mapping_retry_t {
    std::string output;
    std::chrono::steady_clock::time_point retry_after {};

    bool begin(const std::string &requested, bool wayland, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
      if (requested.empty() || !wayland) {
        output.clear();
        retry_after = {};
        return false;
      }
      if (requested == output && now < retry_after) {
        return false;
      }
      output = requested;
      retry_after = now + std::chrono::seconds(5);
      return true;
    }
  };
}  // namespace platf::touch
