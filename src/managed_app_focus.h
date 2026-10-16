#pragma once

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

  // Only confirmed foreground activations consume the budget. Window discovery
  // failures keep polling until the deadline, including slow launcher handoffs.
  class budget {
  public:
    using clock = std::chrono::steady_clock;

    budget(policy options, clock::time_point start):
        options_(options),
        deadline_(start + std::chrono::seconds(options.timeout_secs)) {}

    bool active(clock::time_point now) const {
      return options_.enabled() && successes_ < options_.attempts && now < deadline_;
    }

    void confirmed() {
      successes_ += options_.exit_on_first ? options_.attempts : 1;
    }

  private:
    policy options_;
    clock::time_point deadline_;
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
