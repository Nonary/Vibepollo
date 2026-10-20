/**
 * @file src/hardware_metrics_gpu.h
 * @brief Adapter-qualified utilization accumulation for paired-client samples.
 */
#pragma once
#include <algorithm>
#include <cmath>
#include <optional>
#include <string_view>

namespace hardware_metrics {
  class qualified_counter_t {
  public:
    explicit qualified_counter_t(std::wstring_view adapter):
        adapter(adapter) {}

    void add(std::wstring_view instance, double value) {
      if (!adapter.empty() && instance.find(adapter) != std::wstring_view::npos && std::isfinite(value) && value >= 0) {
        total = total.value_or(0) + value;
      }
    }

    float value(float maximum) const {
      if (!total) {
        return -1.f;
      }
      return static_cast<float>(maximum > 0 ? std::min(*total, static_cast<double>(maximum)) : *total);
    }

  private:
    std::wstring_view adapter;
    std::optional<double> total;
  };
}  // namespace hardware_metrics
