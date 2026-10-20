/**
 * @file src/hardware_metrics_config.h
 * @brief Completed-configuration publication for paired-client metrics readers.
 */
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace config {
  struct hardware_metrics_settings_t {
    bool enabled = false;
    int interval_ms = 1000;
  };

  // Readers must not take the apply gate: apply can perform blocking platform
  // work or need session locks. One lock-free word keeps this pair coherent,
  // and stays independent of sunshine's reset/parse intermediate state.
  class hardware_metrics_config_t {
  public:
    hardware_metrics_settings_t snapshot() const {
      const auto value = completed.load(std::memory_order_acquire);
      return {bool(value & 1u), static_cast<int>(value >> 1)};
    }

    void publish(hardware_metrics_settings_t settings) {
      const auto interval = static_cast<std::uint32_t>(std::clamp(settings.interval_ms, 500, 10000));
      completed.store((interval << 1) | static_cast<std::uint32_t>(settings.enabled), std::memory_order_release);
    }

  private:
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    std::atomic<std::uint32_t> completed {1000u << 1};
  };
}  // namespace config
