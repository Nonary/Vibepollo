/**
 * @file src/host_stats_service.cpp
 * @brief Platform-neutral host-statistics sampler implementation.
 */
#include "host_stats_service.h"

#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

namespace host_stats {
  class service_t::impl_t {
  public:
    impl_t(provider_factory_t provider_factory,
           enabled_provider_t enabled_provider,
           interval_provider_t interval_provider,
           diagnostic_t diagnostic):
        provider_factory(std::move(provider_factory)),
        enabled_provider(std::move(enabled_provider)),
        interval_provider(std::move(interval_provider)),
        diagnostic(std::move(diagnostic)) {}

    provider_factory_t provider_factory;
    enabled_provider_t enabled_provider;
    interval_provider_t interval_provider;
    diagnostic_t diagnostic;

    mutable std::mutex state_mutex;
    mutable std::mutex snapshot_mutex;
    std::mutex wait_mutex;
    std::condition_variable wait_cv;
    std::unique_ptr<platf::host_stats_provider_t> provider;
    std::thread thread;
    std::atomic<bool> stop {false};
    platf::host_stats_t latest;
    platf::host_info_t info;
    std::uint64_t owner_generation = 0;
    std::uint64_t next_generation = 0;

    void report(std::string_view message) const {
      if (diagnostic) {
        diagnostic(message);
      }
    }

    bool enabled() const {
      return !enabled_provider || enabled_provider();
    }

    std::chrono::milliseconds interval() const {
      return interval_provider ? interval_provider() : std::chrono::seconds(2);
    }

    void sample_once() {
      auto value = provider->sample();
      const std::lock_guard lock(snapshot_mutex);
      latest = value;
    }

    void run() {
      bool was_enabled = true;
      while (!stop.load(std::memory_order_acquire)) {
        const bool is_enabled = enabled();
        if (is_enabled) {
          try {
            sample_once();
          } catch (...) {
            report("provider sample failed");
          }
        } else if (was_enabled) {
          const std::lock_guard lock(snapshot_mutex);
          latest = {};
        }
        was_enabled = is_enabled;

        std::unique_lock lock(wait_mutex);
        wait_cv.wait_for(lock, interval(), [this] {
          return stop.load(std::memory_order_acquire);
        });
      }
    }
  };

  service_t::guard_t::guard_t(service_t &service, bool owns_sampler, std::uint64_t generation):
      _service(&service),
      _owns_sampler(owns_sampler),
      _generation(generation) {}

  service_t::guard_t::~guard_t() {
    if (_service && _owns_sampler) {
      _service->release(_generation);
    }
  }

  service_t::service_t(provider_factory_t provider_factory,
                       enabled_provider_t enabled_provider,
                       interval_provider_t interval_provider,
                       diagnostic_t diagnostic):
      _impl(std::make_unique<impl_t>(
        std::move(provider_factory),
        std::move(enabled_provider),
        std::move(interval_provider),
        std::move(diagnostic)
      )) {}

  service_t::~service_t() {
    std::uint64_t generation = 0;
    {
      const std::lock_guard lock(_impl->state_mutex);
      generation = _impl->owner_generation;
    }
    if (generation != 0) {
      release(generation);
    }
  }

  std::unique_ptr<service_t::guard_t> service_t::start() {
    const std::lock_guard lock(_impl->state_mutex);
    if (_impl->provider) {
      _impl->report("start called while already running");
      return std::unique_ptr<guard_t>(new guard_t(*this, false, 0));
    }

    try {
      _impl->provider = _impl->provider_factory ? _impl->provider_factory() : nullptr;
    } catch (...) {
      _impl->report("provider factory failed");
      return {};
    }
    if (!_impl->provider) {
      _impl->report("no provider available");
      return {};
    }

    _impl->stop.store(false, std::memory_order_release);
    try {
      const auto info = _impl->provider->info();
      const std::lock_guard snapshot_lock(_impl->snapshot_mutex);
      _impl->info = info;
    } catch (...) {
      _impl->report("provider info failed");
    }

    if (_impl->enabled()) {
      try {
        _impl->sample_once();
      } catch (...) {
        _impl->report("initial sample failed");
      }
    }

    _impl->owner_generation = ++_impl->next_generation;
    _impl->thread = std::thread([impl = _impl.get()] {
      impl->run();
    });
    return std::unique_ptr<guard_t>(new guard_t(*this, true, _impl->owner_generation));
  }

  platf::host_stats_t service_t::latest() const {
    const std::lock_guard lock(_impl->snapshot_mutex);
    return _impl->latest;
  }

  const platf::host_info_t &service_t::info() const {
    return _impl->info;
  }

  bool service_t::is_running() const {
    const std::lock_guard lock(_impl->state_mutex);
    return static_cast<bool>(_impl->provider);
  }

  void service_t::release(std::uint64_t generation) {
    std::unique_lock lock(_impl->state_mutex);
    if (_impl->owner_generation != generation) {
      return;
    }
    _impl->stop.store(true, std::memory_order_release);
    _impl->wait_cv.notify_all();
    if (_impl->thread.joinable()) {
      _impl->thread.join();
    }
    _impl->provider.reset();
    _impl->owner_generation = 0;
  }
}  // namespace host_stats
