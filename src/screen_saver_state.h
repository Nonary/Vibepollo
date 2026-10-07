#pragma once

#include <cstdint>
#include <optional>
#include <unordered_set>

namespace platf {
  // Access is serialized by the Windows screen-saver mutex. Keep the baseline
  // until every pause worker that might change it has finished. Only an
  // inactive lifecycle may restore it, including after an older worker exits.
  class screen_saver_state_t {
  public:
    using token_t = std::uint64_t;

    std::optional<bool> value() const { return before_app_; }

    template<class Read>
    bool begin(Read read) {
      active_ = true;
      if (!before_app_) {
        before_app_ = read();
      }
      return before_app_.has_value();
    }

    token_t defer() {
      const auto token = ++next_token_;
      pending_.insert(token);
      return token;
    }

    template<class Write>
    bool restore(Write write) {
      active_ = false;
      return restore_if_inactive(write);
    }

    template<class Write>
    bool finish(token_t token, Write write) {
      if (pending_.erase(token) == 0) {
        return true;
      }
      return restore_if_inactive(write);
    }

  private:
    template<class Write>
    bool restore_if_inactive(Write write) {
      if (active_ || !before_app_) {
        return true;
      }
      if (!write(*before_app_)) {
        return false;
      }
      if (pending_.empty()) {
        before_app_.reset();
      }
      return true;
    }

    bool active_ = false;
    std::uint64_t next_token_ = 0;
    std::unordered_set<token_t> pending_;
    std::optional<bool> before_app_;
  };
}  // namespace platf
