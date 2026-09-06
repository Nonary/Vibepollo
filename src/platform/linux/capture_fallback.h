/**
 * @file src/platform/linux/capture_fallback.h
 * @brief Lazy capture attempts for an existing compositor scene.
 */
#pragma once

#include <string>
#include <utility>

namespace platf::linux_capture {
  // Enumerate only when needed, and use names belonging to this capture API.
  // A saved desktop connector or the logical "gamescope" target is not a
  // valid output identifier for an unrelated capture backend.
  template<class Enumerate, class Capture, class Eligible>
  auto try_outputs(Enumerate enumerate, Capture capture, Eligible eligible, bool hdr_required) {
    using result_t = decltype(capture(std::declval<const std::string &>()));
    for (const auto &name : enumerate()) {
      if (!eligible(name)) {
        continue;
      }
      auto display = capture(name);
      if (display && (!hdr_required || display->is_hdr())) {
        return display;
      }
    }
    return result_t {};
  }
}  // namespace platf::linux_capture
