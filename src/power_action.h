/** Dedicated paired-client power controls; never a streaming application. */
#pragma once

#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace nvhttp::power_action {
  struct authority_t {
    std::string uuid;  // Resolved from the current paired TLS certificate only.
    bool allow_sleep {false};
  };

  struct activity_t {
    bool app {false};  // Includes a paused app retained for Resume.
    bool pending {false};
    bool rtsp {false};
    bool running {false};
    bool teardown {false};
    bool webrtc {false};
    bool capture {false};
    bool remote_owner {false};

    bool busy() const;
  };

  struct response_t {
    int status {200};
    std::string message;
    bool supported {false};
    bool allowed {false};
    bool available {false};
    std::string token;
  };

  struct request_t {
    std::string method;
    std::string token;
    bool valid_shape {false};  // Exactly one token, no body or Origin header.
  };

  struct hooks_t {
    std::function<authority_t()> authority;
    std::function<bool()> supported;
    std::function<activity_t()> activity;  // Called with the lifecycle gate held.
    // This boundary must revalidate the TLS authority and opt-in under the live
    // registry lock, keeping that lock through the fixed native invocation.
    std::function<bool()> suspend_if_authorized;
    // true means the complete response was flushed; false prevents suspend.
    std::function<bool(const response_t &)> respond;
  };

  enum class result_e {
    refused,
    response_failed,
    native_failed,
    requested
  };

  class service_t {
  public:
    using clock = std::chrono::steady_clock;
    static constexpr auto token_lifetime = std::chrono::seconds(30);
    static constexpr std::size_t max_tokens = 256;

    explicit service_t(std::mutex &lifecycle_gate);
    response_t capabilities(const hooks_t &hooks, clock::time_point now = clock::now());
    result_e sleep(const request_t &request, const hooks_t &hooks, clock::time_point now = clock::now());

  private:
    struct token_t {
      std::string value;
      clock::time_point expires;
    };

    std::mutex &lifecycle_gate;
    std::mutex token_mutex;
    std::unordered_map<std::string, token_t> tokens;
  };

  std::string response_xml(const response_t &response);
  bool native_sleep_supported();
  bool native_sleep();
}  // namespace nvhttp::power_action
