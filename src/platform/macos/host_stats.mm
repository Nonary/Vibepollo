/**
 * @file src/platform/macos/host_stats.mm
 * @brief Stub macOS implementation of @ref platf::host_stats_provider_t.
 *
 * Placeholder until Phase 19 lands the real mach + IOReport
 * implementation. Returns sentinel-filled samples so the API/UI render
 * "N/A" everywhere.
 */
#include "src/platform/common.h"

namespace {

  class macos_host_stats_t: public platf::host_stats_provider_t {
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
    return std::make_unique<macos_host_stats_t>();
  }

}  // namespace platf
