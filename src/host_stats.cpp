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
#include <string_view>

namespace host_stats {
  namespace {
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
        }
      };
      return instance;
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

  const platf::host_info_t &info() {
    return service().info();
  }

#ifdef SUNSHINE_TESTS
  bool is_running_for_tests() {
    return service().is_running();
  }
#endif
}  // namespace host_stats
