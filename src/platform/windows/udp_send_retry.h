#pragma once

#include <chrono>
#include <cstddef>

#include <WinSock2.h>

namespace platf::udp_send_retry {
  constexpr auto retry_budget = std::chrono::milliseconds {2};

  struct result_t {
    int error = 0;
    bool cancelled = false;
    bool timed_out = false;
    unsigned retries = 0;
  };

  inline bool pressure_error(int error) {
    return error == WSAENOBUFS || error == WSAEWOULDBLOCK;
  }

  inline bool offload_fallback_error(int error, bool segmented, std::size_t datagram_size, bool ipv6) {
    if (!segmented) {
      return false;
    }
    if (error == WSAEINVAL || error == WSAEOPNOTSUPP || error == WSAENOPROTOOPT) {
      return true;
    }
    // A large segmented message can be rejected even when each UDP packet fits.
    return error == WSAEMSGSIZE && datagram_size <= (ipv6 ? 65527u : 65507u);
  }

  /**
   * Retry the same atomic datagram/message only after temporary buffer pressure.
   * send() and wait(error, remaining) return zero on success, or a Winsock error.
   * wait requests at most remaining; cancellation is checked before each send.
   * Scheduler delays can exceed that request, so the budget bounds retries,
   * not the wall-clock duration of a suspended thread or kernel operation.
   * No send is retried after the deadline, including when readiness is immediate.
   */
  template<class Send, class Wait, class Cancelled, class Now>
  result_t send(Send &&attempt, Wait &&wait, Cancelled &&cancelled, Now &&now) {
    const auto deadline = now() + retry_budget;
    result_t result;
    for (;;) {
      if (cancelled()) {
        result.error = WSAEINTR;
        result.cancelled = true;
        return result;
      }
      if (result.retries != 0 && now() >= deadline) {
        result.timed_out = true;
        return result;
      }
      result.error = attempt();
      if (!pressure_error(result.error)) {
        return result;
      }
      const auto remaining = deadline - now();
      if (remaining <= decltype(remaining)::zero()) {
        result.timed_out = true;
        return result;
      }
      const auto wait_error = wait(result.error, remaining);
      if (wait_error != 0) {
        result.error = wait_error;
        return result;
      }
      ++result.retries;
    }
  }
}  // namespace platf::udp_send_retry
