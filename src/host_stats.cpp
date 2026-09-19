/**
 * @file src/host_stats.cpp
 * @brief Production adapter for the platform-neutral host statistics service.
 */
#include "host_stats.h"

#include "config.h"
#include "host_stats_service.h"
#include "logging.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <string_view>

namespace host_stats {
  namespace {
    std::mutex streaming_mutex;
    std::size_t rtsp_sessions = 0;
    std::size_t webrtc_sessions = 0;

    class deinit_t: public platf::deinit_t {
    public:
      explicit deinit_t(std::unique_ptr<service_t::guard_t> guard):
          _guard(std::move(guard)) {}

    private:
      std::unique_ptr<service_t::guard_t> _guard;
    };

    service_t &service() {
      static service_t instance {
        [] {
          return platf::create_host_stats_provider();
        },
        [] {
          return config::sunshine.realtime_stats_enabled;
        },
        [] {
          if (!config::sunshine.realtime_stats_enabled) {
            return std::chrono::milliseconds(2000);
          }
          return std::chrono::milliseconds(std::clamp(config::sunshine.realtime_stats_poll_interval_ms, 250, 60000));
        },
        [](std::string_view message) {
          BOOST_LOG(warning) << "host_stats: " << message;
        },
        [] {
          const auto interval = std::chrono::milliseconds(
            std::clamp(config::sunshine.realtime_stats_poll_interval_ms, 250, 60000)
          );
          return std::max(std::chrono::milliseconds(std::chrono::seconds(12)), interval + std::chrono::seconds(2));
        }
      };
      return instance;
    }

    void update_streaming_demand() {
      service().set_streaming(rtsp_sessions > 0 || webrtc_sessions > 0);
    }
  }  // namespace

  std::unique_ptr<platf::deinit_t> start() {
    auto guard = service().start();
    if (!guard) {
      return {};
    }
    return std::make_unique<deinit_t>(std::move(guard));
  }

  platf::host_stats_t latest() {
    return service().latest();
  }

  platf::host_stats_t latest_for_consumer() {
    return service().latest_for_consumer();
  }

  const platf::host_info_t &info() {
    return service().info();
  }

  void rtsp_session_started() {
    const std::lock_guard lock(streaming_mutex);
    ++rtsp_sessions;
    update_streaming_demand();
  }

  void rtsp_session_ended() {
    const std::lock_guard lock(streaming_mutex);
    if (rtsp_sessions > 0) {
      --rtsp_sessions;
    }
    update_streaming_demand();
  }

  void webrtc_session_started() {
    const std::lock_guard lock(streaming_mutex);
    ++webrtc_sessions;
    update_streaming_demand();
  }

  void webrtc_session_ended() {
    const std::lock_guard lock(streaming_mutex);
    if (webrtc_sessions > 0) {
      --webrtc_sessions;
    }
    update_streaming_demand();
  }

  void configuration_changed() {
    service().configuration_changed();
  }

#ifdef SUNSHINE_TESTS
  bool is_running_for_tests() {
    return service().is_running();
  }
#endif
}  // namespace host_stats
