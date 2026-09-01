/**
 * @file src/platform/linux/scoped_capability.h
 * @brief Verified, fail-closed effective-capability scopes for Linux.
 */
#pragma once

#include <cstdio>
#include <cstdlib>

#include <sys/capability.h>

namespace platf::linux_security {
  class scoped_effective_capability final {
  public:
    enum class state_e {
      unavailable,
      active,
      failed,
    };

    explicit scoped_effective_capability(cap_value_t capability) noexcept:
        capability_ {capability} {
      cap_t current = cap_get_proc();
      if (!current) {
        return;
      }

      cap_flag_value_t permitted = CAP_CLEAR;
      if (cap_get_flag(current, capability_, CAP_PERMITTED, &permitted) != 0) {
        cap_free(current);
        return;
      }
      if (permitted != CAP_SET) {
        state_ = state_e::unavailable;
        cap_free(current);
        return;
      }

      cap_t elevated = cap_dup(current);
      cap_free(current);
      if (!elevated) {
        return;
      }

      if (cap_clear_flag(elevated, CAP_EFFECTIVE) != 0 ||
          cap_set_flag(elevated, CAP_EFFECTIVE, 1, &capability_, CAP_SET) != 0) {
        cap_free(elevated);
        return;
      }

      if (cap_set_proc(elevated) != 0) {
        cap_free(elevated);
        if (!clear_effective_exact()) {
          fail_stop("clearing effective capabilities after a failed raise");
        }
        return;
      }

      cap_t verified = cap_get_proc();
      if (!verified) {
        cap_free(elevated);
        fail_stop_after_raise_verification("reading capabilities after a raise");
      }
      const int comparison = cap_compare(elevated, verified);
      cap_free(verified);
      cap_free(elevated);
      if (comparison != 0) {
        fail_stop_after_raise_verification("verifying the exact raised capability set");
      }

      state_ = state_e::active;
    }

    ~scoped_effective_capability() noexcept {
      if (state_ == state_e::active && !clear_effective_exact()) {
        fail_stop("clearing and verifying effective capabilities at scope exit");
      }
    }

    scoped_effective_capability(const scoped_effective_capability &) = delete;
    scoped_effective_capability &operator=(const scoped_effective_capability &) = delete;
    scoped_effective_capability(scoped_effective_capability &&) = delete;
    scoped_effective_capability &operator=(scoped_effective_capability &&) = delete;

    [[nodiscard]] state_e state() const noexcept {
      return state_;
    }

    [[nodiscard]] bool active() const noexcept {
      return state_ == state_e::active;
    }

    [[nodiscard]] bool unavailable() const noexcept {
      return state_ == state_e::unavailable;
    }

  private:
    [[nodiscard]] static bool clear_effective_exact() noexcept {
      cap_t current = cap_get_proc();
      if (!current) {
        return false;
      }

      cap_t cleared = cap_dup(current);
      cap_free(current);
      if (!cleared) {
        return false;
      }
      if (cap_clear_flag(cleared, CAP_EFFECTIVE) != 0 || cap_set_proc(cleared) != 0) {
        cap_free(cleared);
        return false;
      }

      cap_t verified = cap_get_proc();
      if (!verified) {
        cap_free(cleared);
        return false;
      }
      const int comparison = cap_compare(cleared, verified);
      cap_free(verified);
      cap_free(cleared);
      return comparison == 0;
    }

    [[noreturn]] static void fail_stop(const char *operation) noexcept {
      std::fprintf(stderr, "Vibepollo capability safety failure while %s; aborting.\n", operation);
      std::abort();
    }

    [[noreturn]] static void fail_stop_after_raise_verification(const char *operation) noexcept {
      (void) clear_effective_exact();
      fail_stop(operation);
    }

    cap_value_t capability_;
    state_e state_ {state_e::failed};
  };
}  // namespace platf::linux_security
