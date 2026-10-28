#pragma once

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

namespace managed_app_focus {
  struct policy {
    int attempts = 3;
    int timeout_secs = 15;
    bool exit_on_first = false;

    bool enabled() const {
      return attempts > 0 && timeout_secs > 0;
    }
  };

  // Old Playnite keys are read as aliases; explicit shared keys take precedence.
  policy parse_policy(std::unordered_map<std::string, std::string> &vars);
  extern policy settings;
  std::string launcher_arguments(policy options);

  // Discovery and focus have separate bounds. Wait for eligible window identities
  // for at most max(180s, timeout); a newly observed identity arms the configured
  // focus window without moving that hard cap or replenishing confirmations.
  // Adapted from Jonathan Malmaud's Vibepollo PR573 (026775b930ce).
  class budget {
  public:
    using clock = std::chrono::steady_clock;

    budget(policy options, clock::time_point start):
        options_(options),
        hard_deadline_(start + std::max(std::chrono::seconds(180), std::chrono::seconds(options.timeout_secs))),
        deadline_(start) {}

    // Continue cheap discovery after a focus window expires: a replacement game
    // window may still appear. No stale window may be activated during that wait.
    bool active(clock::time_point now) const {
      return options_.enabled() && successes_ < options_.attempts && now < hard_deadline_;
    }

    bool can_focus(clock::time_point now) const {
      return active(now) && !seen_.empty() && now < deadline_;
    }

    bool can_focus(const std::string &identity, clock::time_point now) const {
      const auto found = seen_.find(identity);
      return active(now) && found != seen_.end() && now < found->second;
    }

    bool observe(const std::string &identity, clock::time_point now) {
      if (!active(now) || identity.empty() || seen_.contains(identity)) {
        return false;
      }
      deadline_ = std::min(hard_deadline_, now + std::chrono::seconds(options_.timeout_secs));
      seen_[identity] = deadline_;
      return true;
    }

    // A status update may extend a still-active allowance. It cannot revive an
    // expired splash, move the hard cap, or reset the confirmed-success count.
    void refresh(clock::time_point now) {
      if (active(now) && !seen_.empty()) {
        const auto extended = std::min(hard_deadline_, now + std::chrono::seconds(options_.timeout_secs));
        for (auto &[identity, deadline] : seen_) {
          if (now < deadline) {
            deadline = extended;
            deadline_ = std::max(deadline_, extended);
          }
        }
      }
    }

    clock::time_point deadline() const {
      return deadline_;
    }

    void confirmed() {
      successes_ += options_.exit_on_first ? options_.attempts : 1;
    }

  private:
    policy options_;
    clock::time_point hard_deadline_;
    clock::time_point deadline_;
    std::unordered_map<std::string, clock::time_point> seen_;
    int successes_ = 0;
  };

  struct target {
    std::string provider;
    std::string id;
    std::filesystem::path install_dir;
  };

  // Owned by the application session: destruction cancels pending focus work.
  class session {
  public:
    virtual ~session() = default;
  };

  std::unique_ptr<session> start(const target &app, policy options);
}  // namespace managed_app_focus
