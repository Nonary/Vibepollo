/**
 * @file src/hardware_metrics.cpp
 * @brief Shared request-only sampling and hardware metrics v1 responses.
 */
#include "hardware_metrics.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

namespace hardware_metrics {
  class service_t::impl_t {
  public:
    explicit impl_t(factory_t factory):
        factory(std::move(factory)) {}

    factory_t factory;
    std::mutex mutex;
    std::condition_variable cv;
    std::thread worker;
    bool stop = false;
    bool pending = false;
    std::uint64_t generation = 0;
    std::optional<sample_t> cached;
    std::chrono::steady_clock::time_point completed_at {};

    void run() {
      std::unique_ptr<platf::host_stats_provider_t> provider;
      std::unique_lock lock(mutex);
      while (true) {
        cv.wait(lock, [this] {
          return stop || pending;
        });
        if (stop) {
          return;
        }
        lock.unlock();
        std::optional<sample_t> result;
        try {
          if (!provider && factory) {
            provider = factory();
            if (provider) {
              provider->reset_rate_baselines();
            }
          }
          if (provider) {
            auto stats = provider->sample();
            const auto now = std::chrono::system_clock::now().time_since_epoch();
            result = sample_t {std::move(stats), static_cast<std::uint64_t>(std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(now).count()))};
          }
        } catch (...) {
          // Never retain an old sample after a failed provider read.
        }
        lock.lock();
        cached = std::move(result);
        completed_at = std::chrono::steady_clock::now();
        pending = false;
        ++generation;
        cv.notify_all();
      }
    }
  };

  service_t::service_t(factory_t factory):
      impl(std::make_unique<impl_t>(std::move(factory))) {}

  service_t::~service_t() {
    {
      const std::lock_guard lock(impl->mutex);
      impl->stop = true;
      impl->cv.notify_all();
    }
    if (impl->worker.joinable()) {
      impl->worker.join();
    }
  }

  std::optional<sample_t> service_t::request(std::chrono::milliseconds interval) {
    std::unique_lock lock(impl->mutex);
    interval = std::clamp(interval, std::chrono::milliseconds(500), std::chrono::milliseconds(10000));
    if (impl->generation > 0 && std::chrono::steady_clock::now() - impl->completed_at < interval) {
      return impl->cached;
    }
    if (!impl->worker.joinable()) {
      impl->worker = std::thread([this] {
        impl->run();
      });
    }
    const auto requested_generation = impl->generation + 1;
    impl->pending = true;
    impl->cv.notify_all();
    if (!impl->cv.wait_for(lock, std::chrono::seconds(2), [this, requested_generation] {
          return impl->stop || impl->generation >= requested_generation;
        }) ||
        impl->stop) {
      return {};
    }
    return impl->cached;
  }

  bool advertised(access_t access) {
    return access.enabled && access.authenticated_viewer;
  }

  nlohmann::json serialize(const sample_t &sample, int interval_ms) {
    using nlohmann::json;
    auto cpu = json::object();
    auto add = [](json &object, const char *key, double value, double max) {
      if (std::isfinite(value) && value >= 0 && value <= max) {
        object[key] = value;
      }
    };
    add(cpu, "cpu_usage_percent", sample.stats.cpu_percent, 100);
    auto gpus = json::array();
    for (const auto &gpu : sample.stats.client_gpus) {
      auto metrics = json::object();
      add(metrics, "gpu_usage_percent", gpu.usage_percent, 100);
      add(metrics, "gpu_encoder_percent", gpu.encoder_percent, 100);
      add(metrics, "gpu_temperature_c", gpu.temperature_c, 200);
      if (gpu.vram_used_bytes && (!gpu.vram_total_bytes || *gpu.vram_total_bytes == 0 || *gpu.vram_used_bytes <= *gpu.vram_total_bytes)) {
        metrics["gpu_vram_used_mb"] = static_cast<double>(*gpu.vram_used_bytes) / (1024.0 * 1024.0);
        if (gpu.vram_total_bytes && *gpu.vram_total_bytes > 0 && *gpu.vram_used_bytes <= *gpu.vram_total_bytes) {
          metrics["gpu_vram_usage_percent"] = static_cast<double>(*gpu.vram_used_bytes) * 100.0 / *gpu.vram_total_bytes;
        }
      }
      if (!metrics.empty()) {
        gpus.push_back({{"id", gpu.id}, {"name", gpu.name}, {"metrics", std::move(metrics)}});
      }
    }
    return {{"version", 1}, {"sampled_at_unix_ms", sample.sampled_at_unix_ms}, {"interval_ms", interval_ms}, {"cpu", std::move(cpu)}, {"gpus", std::move(gpus)}};
  }

  response_t query(service_t &service, const std::function<access_t()> &access, const std::function<bool()> &own_running_video) {
    auto check = [&](access_t current) -> std::optional<response_t> {
      if (!current.enabled) {
        return response_t {403, {{"error", "Hardware metrics are disabled"}}};
      }
      if (!current.authenticated_viewer) {
        return response_t {403, {{"error", "An authenticated viewer is required"}}};
      }
      if (!own_running_video()) {
        return response_t {409, {{"error", "An active video stream is required"}}};
      }
      return {};
    };
    const auto initial = access();
    if (auto rejected = check(initial)) {
      return *rejected;
    }
    auto sample = service.request(std::chrono::milliseconds(initial.interval_ms));
    // No config gate or client/session lock is held across the provider wait.
    const auto completed = access();
    if (auto rejected = check(completed)) {
      return *rejected;
    }
    if (!sample) {
      return {503, {{"error", "Hardware metrics are unavailable"}}};
    }
    return {200, serialize(*sample, std::clamp(completed.interval_ms, 500, 10000))};
  }
}  // namespace hardware_metrics
