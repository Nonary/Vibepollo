/**
 * @file src/nvhttp_pending_peer.h
 * @brief Liveness of an HTTPS request waiting for a stream handoff.
 */
#pragma once

#include <chrono>
#include <functional>
#include <memory>

namespace nvhttp {
  enum class pending_peer_end_e { disconnected, expired };

  class pending_peer_t {
  public:
    virtual ~pending_peer_t() = default;
    virtual void start(std::chrono::steady_clock::time_point deadline, std::function<void(pending_peer_end_e)> on_end) = 0;
    // Report readiness while continuing to monitor until the worker claims it.
    virtual void prepare(std::function<void(bool)> on_ready) = 0;
    // Called after prepare, immediately before the worker starts. A successful
    // claim stops TLS probing and leaves the transport ready for the response.
    virtual bool connected() = 0;
    virtual void stop() = 0;
  };

  using pending_peer_factory_t = std::function<std::shared_ptr<pending_peer_t>()>;
}  // namespace nvhttp
