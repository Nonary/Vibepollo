/**
 * @file src/host_stats_types.h
 * @brief Platform-neutral host statistics contracts.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace platf {
  // Only measurements explicitly tied to this provider-local device belong here.
  // The legacy flat GPU fields can aggregate different adapters/fallbacks.
  struct client_gpu_stats_t {
    std::int32_t id = 0;
    std::string name;
    float usage_percent = -1.f;
    float encoder_percent = -1.f;
    float temperature_c = -1.f;
    std::optional<std::uint64_t> vram_used_bytes;
    std::optional<std::uint64_t> vram_total_bytes;
  };

  struct host_stats_t {
    float cpu_percent = -1.f;
    float cpu_temp_c = -1.f;
    std::uint64_t ram_used_bytes = 0;
    std::uint64_t ram_total_bytes = 0;
    float gpu_percent = -1.f;
    float gpu_encoder_percent = -1.f;
    float gpu_temp_c = -1.f;
    std::uint64_t vram_used_bytes = 0;
    std::uint64_t vram_total_bytes = 0;
    double net_rx_bps = -1.0;
    double net_tx_bps = -1.0;
    std::vector<client_gpu_stats_t> client_gpus;
  };

  struct host_info_t {
    std::string cpu_model;
    std::string gpu_model;
    int cpu_logical_cores = 0;
    std::uint64_t ram_total_bytes = 0;
    std::uint64_t vram_total_bytes = 0;
    std::string net_interface;
    std::uint64_t net_link_speed_mbps = 0;
  };

  class host_stats_provider_t {
  public:
    virtual ~host_stats_provider_t() = default;
    virtual host_stats_t sample() = 0;
    virtual host_info_t info() = 0;
    /// Discard rate-counter state before sampling resumes after an idle gap.
    virtual void reset_rate_baselines() {}
  };
}  // namespace platf
