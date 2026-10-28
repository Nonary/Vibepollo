/**
 * @file src/hardware_metrics.h
 * @brief On-demand paired-client hardware metrics protocol v1.
 */
#pragma once

#include "host_stats_types.h"

#include <chrono>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>

namespace hardware_metrics {
  struct sample_t {
    platf::host_stats_t stats;
    std::uint64_t sampled_at_unix_ms = 0;
  };

  // One worker owns the provider. No timer or streaming demand collects samples.
  class service_t {
  public:
    using factory_t = std::function<std::unique_ptr<platf::host_stats_provider_t>()>;
    explicit service_t(factory_t factory);
    ~service_t();
    service_t(const service_t &) = delete;
    service_t &operator=(const service_t &) = delete;
    std::optional<sample_t> request(std::chrono::milliseconds interval);

  private:
    class impl_t;
    std::unique_ptr<impl_t> impl;
  };

  struct access_t {
    bool enabled = false;
    bool authenticated_viewer = false;
    int interval_ms = 1000;
  };

  struct response_t {
    int status;
    nlohmann::json body;
  };

  bool advertised(access_t access);
  nlohmann::json serialize(const sample_t &sample, int interval_ms);
  // Recheck live certificate/config/session authority after any sampling wait.
  response_t query(service_t &service, const std::function<access_t()> &access, const std::function<bool()> &own_running_video);
}  // namespace hardware_metrics
