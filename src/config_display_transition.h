/**
 * @file src/config_display_transition.h
 * @brief Effective configuration installation before optional display recovery.
 */
#pragma once
#include <functional>
#include <utility>

namespace config::display_transition {
  using before_mutation_f = std::function<void()>;

  struct state_t {
    bool dd_changed;
    bool runtime_overrides_empty;
    bool dd_was_enabled;
    bool dd_disabled_now;

    bool allow_disabled_recovery() const {
      return dd_was_enabled && dd_disabled_now;
    }
  };

  inline void capture_best_effort(const before_mutation_f &capture) noexcept {
    try {
      if (capture) {
        capture();
      }
    } catch (...) {
      // Optional capture must never veto the original display operation.
    }
  }

  // Installation includes parsing/effective settings and returns the original
  // DD-change inputs. Ownership is observed afterward, with the same short
  // circuit order as the original apply_config_now predicate.
  template<class Install, class ActiveOwners, class Revert>
  void apply(Install &&install, ActiveOwners &&active_owners, const before_mutation_f &capture, Revert &&revert) {
    const auto state = std::forward<Install>(install)();
    if (state.dd_changed && !std::forward<ActiveOwners>(active_owners)() && state.runtime_overrides_empty) {
      capture_best_effort(capture);
      std::forward<Revert>(revert)(state);
    }
  }
}  // namespace config::display_transition
