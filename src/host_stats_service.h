/**
 * @file src/host_stats_service.h
 * @brief Deterministic host-statistics sampler independent of platform factories.
 */
#pragma once

#include "host_stats_types.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string_view>

namespace host_stats {
  class service_t {
  public:
    using provider_factory_t = std::function<std::unique_ptr<platf::host_stats_provider_t>()>;
    using enabled_provider_t = std::function<bool()>;
    using interval_provider_t = std::function<std::chrono::milliseconds()>;
    using diagnostic_t = std::function<void(std::string_view)>;

    class guard_t {
    public:
      ~guard_t();
      guard_t(const guard_t &) = delete;
      guard_t &operator=(const guard_t &) = delete;

    private:
      friend class service_t;
      guard_t(service_t &service, bool owns_sampler, std::uint64_t generation);

      service_t *_service;
      bool _owns_sampler;
      std::uint64_t _generation;
    };

    service_t(provider_factory_t provider_factory,
              enabled_provider_t enabled_provider,
              interval_provider_t interval_provider,
              diagnostic_t diagnostic = {});
    ~service_t();

    service_t(const service_t &) = delete;
    service_t &operator=(const service_t &) = delete;

    std::unique_ptr<guard_t> start();
    platf::host_stats_t latest() const;
    const platf::host_info_t &info() const;
    bool is_running() const;

  private:
    class impl_t;
    std::unique_ptr<impl_t> _impl;
    void release(std::uint64_t generation);
  };
}  // namespace host_stats
