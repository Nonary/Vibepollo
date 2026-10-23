/**
 * @file src/audio_display_transition.h
 * @brief Lazy request hold spanning configuration, display commands and rollback.
 */
#pragma once
#include <functional>
#include <memory>
#include <utility>

namespace audio {
  class display_change_hold_t {
  public:
    using capture_f = std::function<std::shared_ptr<void>()>;

    explicit display_change_hold_t(capture_f capture):
        capture_ {std::move(capture)} {}

    display_change_hold_t(const display_change_hold_t &) = delete;
    display_change_hold_t &operator=(const display_change_hold_t &) = delete;

    // Constructing the request scope does nothing. Only an already-authorized
    // caller's configuration/display boundary requests capture.
    void operator()() noexcept {
      if (hold_ || !capture_) {
        return;
      }
      try {
        hold_ = capture_();
      } catch (...) {
        // Audio snapshot failure cannot veto configuration or display policy.
      }
    }

    template<class Operation>
    void before(Operation &&operation) {
      (*this)();
      std::forward<Operation>(operation)();
    }

    void attach(std::shared_ptr<void> &pending_hold) {
      if (!pending_hold) {
        pending_hold = hold_;
      }
      if (!hold_) {
        hold_ = pending_hold;
      }
      // Keep our copy through later metadata destruction/config rollback.
    }

    void capture_if_empty_and_attach(std::shared_ptr<void> &pending_hold) noexcept {
      // Adopt an existing metadata hold before considering another acquisition.
      // A failed earlier attempt may be retried here, without vetoing preparation.
      attach(pending_hold);
      (*this)();
      attach(pending_hold);
    }

  private:
    capture_f capture_;
    std::shared_ptr<void> hold_;
  };
}  // namespace audio
