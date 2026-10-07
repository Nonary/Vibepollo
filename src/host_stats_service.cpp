/**
 * @file src/host_stats_service.cpp
 * @brief Platform-neutral host-statistics sampler implementation.
 */
#include "host_stats_service.h"

#include <algorithm>
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
           diagnostic_t diagnostic,
           consumer_grace_provider_t consumer_grace_provider):
        provider_factory(std::move(provider_factory)),
        enabled_provider(std::move(enabled_provider)),
        interval_provider(std::move(interval_provider)),
        diagnostic(std::move(diagnostic)),
        consumer_grace_provider(std::move(consumer_grace_provider)) {}

    provider_factory_t provider_factory;
    enabled_provider_t enabled_provider;
    interval_provider_t interval_provider;
    diagnostic_t diagnostic;
    consumer_grace_provider_t consumer_grace_provider;

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
    bool streaming = false;
    std::chrono::steady_clock::time_point web_demand_until {};
    std::chrono::steady_clock::time_point next_sample_at {};
    bool rate_baselines_current = false;
    bool fresh_sample_pending = false;
    std::uint64_t sample_generation = 0;
    std::uint64_t state_revision = 0;

    void report(std::string_view message) const {
      if (diagnostic) {
        diagnostic(message);
      }
    }

    bool enabled() const {
      return !enabled_provider || enabled_provider();
    }

    std::chrono::milliseconds interval() const {
      return std::max(
        interval_provider ? interval_provider() : std::chrono::seconds(2),
        std::chrono::milliseconds(1)
      );
    }

    std::chrono::milliseconds consumer_grace() const {
      return std::max(
        consumer_grace_provider ? consumer_grace_provider() : std::chrono::seconds(12),
        std::chrono::milliseconds(1)
      );
    }

    void sample_once() {
      auto value = provider->sample();
      const std::lock_guard lock(snapshot_mutex);
      latest = value;
    }

    void run() {
      bool was_enabled = enabled();
      std::unique_lock lock(wait_mutex);
      while (!stop.load(std::memory_order_acquire)) {
        const bool is_enabled = enabled();
        if (!is_enabled) {
          if (was_enabled) {
            lock.unlock();
            {
              const std::lock_guard snapshot_lock(snapshot_mutex);
              latest = {};
            }
            lock.lock();
          }
          was_enabled = false;
          rate_baselines_current = false;
          fresh_sample_pending = false;
          wait_cv.notify_all();
          const auto revision = state_revision;
          wait_cv.wait(lock, [this, revision] {
            return stop.load(std::memory_order_acquire) || state_revision != revision;
          });
          continue;
        }
        was_enabled = true;

        const auto now = std::chrono::steady_clock::now();
        if (!streaming && now >= web_demand_until) {
          rate_baselines_current = false;
          fresh_sample_pending = false;
          const auto revision = state_revision;
          wait_cv.wait(lock, [this, revision] {
            return stop.load(std::memory_order_acquire) || state_revision != revision;
          });
          continue;
        }

        if (rate_baselines_current && !fresh_sample_pending && now < next_sample_at) {
          const auto wake_at = streaming ? next_sample_at : std::min(next_sample_at, web_demand_until);
          const auto revision = state_revision;
          wait_cv.wait_until(lock, wake_at, [this, revision] {
            return stop.load(std::memory_order_acquire) || state_revision != revision;
          });
          continue;
        }

        lock.unlock();
        if (!rate_baselines_current) {
          try {
            provider->reset_rate_baselines();
          } catch (...) {
            report("provider baseline reset failed");
          }
        }
        try {
          sample_once();
        } catch (...) {
          report("provider sample failed");
        }
        lock.lock();
        rate_baselines_current = true;
        fresh_sample_pending = false;
        ++sample_generation;
        next_sample_at = std::chrono::steady_clock::now() + interval();
        wait_cv.notify_all();

        const auto wake_at = streaming ? next_sample_at : std::min(next_sample_at, web_demand_until);
        const auto revision = state_revision;
        wait_cv.wait_until(lock, wake_at, [this, revision] {
          return stop.load(std::memory_order_acquire) || state_revision != revision;
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
                       diagnostic_t diagnostic,
                       consumer_grace_provider_t consumer_grace_provider):
      _impl(std::make_unique<impl_t>(
        std::move(provider_factory),
        std::move(enabled_provider),
        std::move(interval_provider),
        std::move(diagnostic),
        std::move(consumer_grace_provider)
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

  platf::host_stats_t service_t::latest_for_consumer() {
    if (!_impl->enabled()) {
      return {};
    }

    {
      const std::lock_guard state_lock(_impl->state_mutex);
      if (!_impl->provider) {
        return {};
      }
    }

    std::unique_lock lock(_impl->wait_mutex);
    const auto now = std::chrono::steady_clock::now();
    const bool demand_was_active = _impl->streaming || now < _impl->web_demand_until;
    _impl->web_demand_until = std::max(
      _impl->web_demand_until,
      now + _impl->consumer_grace()
    );
    if (!demand_was_active) {
      _impl->fresh_sample_pending = true;
    }
    const auto requested_generation = _impl->sample_generation + (_impl->fresh_sample_pending ? 1 : 0);
    ++_impl->state_revision;
    _impl->wait_cv.notify_all();
    _impl->wait_cv.wait_for(lock, std::chrono::seconds(2), [this, requested_generation] {
      return _impl->stop.load(std::memory_order_acquire) ||
             _impl->sample_generation >= requested_generation ||
             !_impl->fresh_sample_pending;
    });
    lock.unlock();
    return latest();
  }

  const platf::host_info_t &service_t::info() const {
    return _impl->info;
  }

  bool service_t::is_running() const {
    const std::lock_guard lock(_impl->state_mutex);
    return static_cast<bool>(_impl->provider);
  }

  void service_t::set_streaming(bool active) {
    const std::lock_guard lock(_impl->wait_mutex);
    if (_impl->streaming == active) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    const bool demand_was_active = _impl->streaming || now < _impl->web_demand_until;
    _impl->streaming = active;
    if (active && !demand_was_active) {
      _impl->fresh_sample_pending = true;
    }
    ++_impl->state_revision;
    _impl->wait_cv.notify_all();
  }

  void service_t::configuration_changed() {
    const std::lock_guard lock(_impl->wait_mutex);
    _impl->next_sample_at = std::chrono::steady_clock::now();
    ++_impl->state_revision;
    _impl->wait_cv.notify_all();
  }

  void service_t::release(std::uint64_t generation) {
    std::unique_lock lock(_impl->state_mutex);
    if (_impl->owner_generation != generation) {
      return;
    }
    _impl->stop.store(true, std::memory_order_release);
    {
      const std::lock_guard wait_lock(_impl->wait_mutex);
      ++_impl->state_revision;
      _impl->wait_cv.notify_all();
    }
    if (_impl->thread.joinable()) {
      _impl->thread.join();
    }
    _impl->provider.reset();
    _impl->owner_generation = 0;
  }
}  // namespace host_stats
