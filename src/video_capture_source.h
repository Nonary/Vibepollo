#pragma once

#include "thread_safe.h"

#include <cstdint>
#include <functional>
#include <utility>

namespace video {
  enum class capture_source_e : std::uint8_t {
    active_output,
    exact_output,
    synthetic_black,
  };

  /**
   * Normal game transports mirror one capture producer. Remote Monitor and
   * Remote Input retain private producers so they cannot inherit that output.
   * Keep the shared lease until the encoder has released its display/images;
   * only its final consumer may stop capture (including WGC helper handshakes).
   */
  template<class Context>
  class capture_source_lease_t {
  public:
    template<class Start, class Stop>
    capture_source_lease_t(capture_source_e source, safe::shared_t<Context> &shared, Start start, Stop stop):
        stop_private_ {std::move(stop)} {
      if (source == capture_source_e::active_output) {
        shared_ = shared.ref();
      } else {
        private_started_ = start(private_) == 0;
      }
    }

    capture_source_lease_t(const capture_source_lease_t &) = delete;
    capture_source_lease_t &operator=(const capture_source_lease_t &) = delete;

    ~capture_source_lease_t() {
      if (private_started_) {
        stop_private_(private_);
      }
    }

    Context *get() {
      if (shared_) {
        return shared_.get();
      }
      return private_started_ ? &private_ : nullptr;
    }

  private:
    typename safe::shared_t<Context>::ptr_t shared_;
    Context private_ {};
    bool private_started_ = false;
    std::function<void(Context &)> stop_private_;
  };
}  // namespace video
