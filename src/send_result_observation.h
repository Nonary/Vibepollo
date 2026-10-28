#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>

namespace stream::detail {
  inline bool claim_first_send_result_log(const int result, std::atomic_bool &success_logged, std::atomic_bool &failure_logged) noexcept {
    auto &logged = result == 0 ? success_logged : failure_logged;
    return !logged.exchange(true, std::memory_order_relaxed);
  }

  inline std::string format_client_relative_controller_index_field(const std::uint16_t index) {
    return "client-relative controller index=" + std::to_string(index);
  }

  // Report the local send return value after the callable completes.
  template<typename Send, typename ObserveResult>
  int send_and_observe_result(Send &&send, ObserveResult &&observe_result) {
    const int result = std::forward<Send>(send)();
    std::forward<ObserveResult>(observe_result)(result);
    return result;
  }
}  // namespace stream::detail
