/**
 * @file src/platform/windows/present_timing_policy.h
 * @brief Places captured frames at the game's present cadence inside their composition refresh.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace platf::dxgi::present_timing {

  /**
   * A captured frame's composition time C lies on the captured display's
   * refresh grid: the frame became ready somewhere in (C - grid, C]. On the 4x
   * virtual display a 116 FPS stream composes on a 2.156 ms grid, so stamping C
   * sends clients up to one grid step of jitter they cannot tell apart from
   * real game pacing.
   *
   * The refiner matches each composition to the game present it shows, learns
   * the typical present-to-composition latency, and places the stamp at that
   * present plus the latency. The stamp is always clamped into (C - grid, C],
   * so a missing, late or wrong match is never worse than the grid it replaces.
   * All times share one clock (QPC ticks in production).
   */
  class refiner_t {
  public:
    static constexpr std::size_t latency_window = 64;
    static constexpr std::size_t minimum_latency_samples = 16;
    // Deltas this far below the grid mean the grid does not describe the display.
    static constexpr int grid_violation_limit = 3;

    struct result_t {
      std::int64_t stamp {0};
      bool matched {false};
      bool refined {false};
    };

    /**
     * @param composition Composition time of the captured frame.
     * @param grid Refresh interval of the captured display; 0 disables refinement.
     * @param presents Presents of the tracked swap chain, ascending.
     * @param minimum_step Smallest increment between consecutive stamps.
     * @param stale Presents older than `composition - stale` cannot be this frame.
     */
    result_t refine(
      const std::int64_t composition,
      std::int64_t grid,
      const std::span<const std::int64_t> presents,
      const std::int64_t minimum_step,
      const std::int64_t stale
    ) {
      if (grid > 0 && have_composition_ && composition > last_composition_ &&
          composition - last_composition_ < grid * 3 / 4) {
        ++grid_violations_;
      }
      have_composition_ = true;
      last_composition_ = composition;
      if (grid_violations_ >= grid_violation_limit) {
        grid = 0;
      }

      result_t result {composition, false, false};
      if (grid > 0) {
        // Without a matching present the unbiased estimate is the middle of
        // the refresh, which keeps matched and unmatched frames in one domain.
        result.stamp = composition - grid / 2;
        if (const auto *present = select(composition, grid, presents, stale)) {
          result.matched = true;
          consumed_ = *present;
          have_consumed_ = true;
          observe_latency(composition - *present, stale);
          if (latency_count_ >= minimum_latency_samples) {
            const auto ready = *present + latency() - grid / 2;
            result.stamp = std::clamp(ready, composition - grid + 1, composition);
            result.refined = true;
          }
        }
      }

      if (have_stamp_ && result.stamp < last_stamp_ + minimum_step) {
        result.stamp = last_stamp_ + minimum_step;
      }
      have_stamp_ = true;
      last_stamp_ = result.stamp;
      return result;
    }

    /**
     * @brief Forget the present match state, e.g. when the tracked swap chain changes.
     * Stamps stay monotonic across a reset.
     */
    void reset_source() {
      have_consumed_ = false;
      consumed_ = 0;
      latency_count_ = 0;
      latency_index_ = 0;
    }

    [[nodiscard]] bool grid_disabled() const {
      return grid_violations_ >= grid_violation_limit;
    }

    [[nodiscard]] std::int64_t latency() const {
      if (latency_count_ == 0) {
        return 0;
      }
      auto samples = latencies_;
      const auto count = static_cast<std::ptrdiff_t>(latency_count_);
      auto middle = samples.begin() + count / 2;
      std::nth_element(samples.begin(), middle, samples.begin() + count);
      return *middle;
    }

  private:
    const std::int64_t *select(
      const std::int64_t composition,
      const std::int64_t grid,
      const std::span<const std::int64_t> presents,
      const std::int64_t stale
    ) const {
      // Candidates: not yet shown, not stale, and presented by this composition.
      const auto oldest_excluded = std::max(have_consumed_ ? consumed_ : composition - stale - 1, composition - stale - 1);
      const auto first = std::upper_bound(presents.begin(), presents.end(), oldest_excluded);
      const auto last = std::upper_bound(first, presents.end(), composition);
      if (first == last) {
        return nullptr;
      }
      if (latency_count_ < minimum_latency_samples) {
        return &*first;
      }
      // The newest present whose typical ready time falls by this composition.
      // A present issued just before C whose GPU work is still running belongs
      // to a later composition; skipped older presents were never composed.
      const auto ready_lead = latency() - grid / 2;
      for (auto it = last; it != first;) {
        --it;
        if (*it + ready_lead <= composition) {
          return &*it;
        }
      }
      return &*first;
    }

    void observe_latency(const std::int64_t sample, const std::int64_t stale) {
      if (sample < 0 || sample > stale) {
        return;
      }
      latencies_[latency_index_] = sample;
      latency_index_ = (latency_index_ + 1) % latency_window;
      latency_count_ = std::min(latency_count_ + 1, latency_window);
    }

    std::array<std::int64_t, latency_window> latencies_ {};
    std::size_t latency_count_ {0};
    std::size_t latency_index_ {0};
    std::int64_t consumed_ {0};
    bool have_consumed_ {false};
    std::int64_t last_stamp_ {0};
    bool have_stamp_ {false};
    std::int64_t last_composition_ {0};
    bool have_composition_ {false};
    int grid_violations_ {0};
  };

}  // namespace platf::dxgi::present_timing
