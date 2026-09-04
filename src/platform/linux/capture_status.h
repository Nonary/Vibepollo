/** Passive observations of live managed KMS capture, never encoder probes. */
#pragma once

#include <atomic>

namespace platf::linux_capture_status {
  inline std::atomic<unsigned> managed_event_captures {0};

  class managed_capture_scope {
  public:
    managed_capture_scope() noexcept {
      managed_event_captures.fetch_add(1, std::memory_order_relaxed);
    }
    ~managed_capture_scope() {
      managed_event_captures.fetch_sub(1, std::memory_order_relaxed);
    }
    managed_capture_scope(const managed_capture_scope &) = delete;
    managed_capture_scope &operator=(const managed_capture_scope &) = delete;
  };

  inline bool managed_event_capture_active() noexcept {
    return managed_event_captures.load(std::memory_order_relaxed) != 0;
  }
}
