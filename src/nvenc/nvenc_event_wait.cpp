/**
 * @file src/nvenc/nvenc_event_wait.cpp
 * @brief Preserve and describe NVENC completion-event wait outcomes.
 */
#include "nvenc_event_wait.h"

namespace nvenc::event_wait {
  result_t wait(void *event, std::uint32_t timeout_ms, api_t &api) {
    const auto code = api.wait(event, timeout_ms);
    // Capture the thread-local error before formatting, logging or another API
    // call can replace it. Other wait results do not define a last error.
    const auto error = code == failed ? api.last_error() : 0;
    return {code, error};
  }

  std::string describe(const result_t &result, std::uint32_t timeout_ms) {
    const auto details = " (wait_result=" + std::to_string(result.code);
    if (result.code == timed_out) {
      return "encode completion wait timed out" + details + ", timeout_ms=" + std::to_string(timeout_ms) + ")";
    }
    if (result.code == failed) {
      return "encode completion wait failed" + details + ", winerr=" + std::to_string(result.error) + ")";
    }
    if (result.succeeded()) {
      return "encode completion wait succeeded" + details + ")";
    }
    return "encode completion wait returned an unexpected result" + details + ")";
  }
}  // namespace nvenc::event_wait
