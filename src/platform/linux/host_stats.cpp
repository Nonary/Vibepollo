/**
 * @file src/platform/linux/host_stats.cpp
 * @brief Stub Linux implementation of @ref platf::host_stats_provider_t.
 *
 * Placeholder until Phase 18 lands the real /proc + /sys + NVML
 * implementation. Returns sentinel-filled samples so the API/UI render
 * "N/A" everywhere.
 */
#include "src/platform/common.h"

namespace {

  class linux_host_stats_t: public platf::host_stats_provider_t {
  public:
    platf::host_stats_t
      sample() override {
      return {};
    }

    platf::host_info_t
      info() override {
      return {};
    }
  };

}  // namespace

namespace platf {

  std::unique_ptr<host_stats_provider_t>
    create_host_stats_provider() {
    return std::make_unique<linux_host_stats_t>();
  }

}  // namespace platf
