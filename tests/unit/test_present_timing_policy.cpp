/**
 * @file tests/unit/test_present_timing_policy.cpp
 * @brief Present-cadence refinement of composition-quantized frame times.
 */
#include "../tests_common.h"

#include <src/platform/windows/present_timing_policy.h>
#include <src/platform/windows/wgc_capture_policy.h>

#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {
  using platf::dxgi::present_timing::refiner_t;

  // Microsecond clock; a 116 FPS stream on the 4x (464 Hz) virtual display.
  constexpr std::int64_t grid = 2155;
  constexpr std::int64_t minimum_step = 12;
  constexpr std::int64_t stale = 100000;

  std::int64_t compose(const std::int64_t ready) {
    return (ready + grid - 1) / grid * grid;
  }

  struct game_t {
    std::vector<std::int64_t> presents;
    std::vector<std::int64_t> ready;
  };

  // Uneven game pacing (8-11 ms frames) with a fixed present-to-ready latency.
  game_t make_game(const int frames, const std::int64_t latency) {
    game_t game;
    std::int64_t present = 1000000;
    for (int k = 0; k < frames; ++k) {
      present += 8000 + (k * 37 % 3000);
      game.presents.push_back(present);
      game.ready.push_back(present + latency);
    }
    return game;
  }

  struct errors_t {
    std::int64_t refined_total {0};
    std::int64_t centre_total {0};
    int frames {0};
    bool inside_refresh {true};
    bool increasing {true};
  };

  errors_t run(const game_t &game, const int warmup) {
    refiner_t refiner;
    errors_t errors;
    std::int64_t previous = 0;
    for (std::size_t k = 0; k < game.ready.size(); ++k) {
      const auto composition = compose(game.ready[k]);
      const auto result = refiner.refine(composition, grid, game.presents, minimum_step, stale);
      errors.inside_refresh = errors.inside_refresh && result.stamp > composition - grid && result.stamp <= composition;
      errors.increasing = errors.increasing && (k == 0 || result.stamp > previous);
      previous = result.stamp;
      if (static_cast<int>(k) >= warmup) {
        errors.refined_total += std::llabs(result.stamp - game.ready[k]);
        errors.centre_total += std::llabs(composition - grid / 2 - game.ready[k]);
        ++errors.frames;
      }
    }
    return errors;
  }
}  // namespace

TEST(PresentTimingPolicy, UnknownGridKeepsCompositionTime) {
  refiner_t refiner;
  const std::vector<std::int64_t> presents {1000, 9000};
  EXPECT_EQ(refiner.refine(12000, 0, presents, minimum_step, stale).stamp, 12000);
  EXPECT_EQ(refiner.refine(21000, 0, presents, minimum_step, stale).stamp, 21000);
}

TEST(PresentTimingPolicy, UnmatchedFrameUsesRefreshCentre) {
  refiner_t refiner;
  const auto result = refiner.refine(10 * grid, grid, {}, minimum_step, stale);
  EXPECT_FALSE(result.matched);
  EXPECT_EQ(result.stamp, 10 * grid - grid / 2);
}

TEST(PresentTimingPolicy, SteadyLatencyRemovesRefreshQuantization) {
  const auto errors = run(make_game(600, 3000), 32);
  ASSERT_GT(errors.frames, 0);
  EXPECT_TRUE(errors.inside_refresh);
  EXPECT_TRUE(errors.increasing);
  // The refresh centre is off by a quarter refresh on average; present
  // cadence should place frames far closer to when they became ready.
  EXPECT_LT(errors.refined_total / errors.frames, 200);
  EXPECT_LT(errors.refined_total * 3, errors.centre_total);
}

TEST(PresentTimingPolicy, PresentStillRenderingBelongsToLaterComposition) {
  // With 7 ms of GPU work the next present is often issued before this frame
  // is composed. Matching it would stamp the frame a whole game frame late.
  const auto errors = run(make_game(600, 7000), 32);
  ASSERT_GT(errors.frames, 0);
  EXPECT_TRUE(errors.inside_refresh);
  EXPECT_LT(errors.refined_total / errors.frames, 200);
}

TEST(PresentTimingPolicy, UnrelatedPresentsStayInsideTheRefresh) {
  refiner_t refiner;
  std::vector<std::int64_t> presents;
  for (std::int64_t t = 0; t < 2000000; t += 3331) {
    presents.push_back(t);
  }
  std::int64_t previous = 0;
  for (int k = 1; k < 400; ++k) {
    const std::int64_t composition = (k * 4 + (k % 3)) * grid;
    const auto stamp = refiner.refine(composition, grid, presents, minimum_step, stale).stamp;
    EXPECT_GT(stamp, composition - grid);
    EXPECT_LE(stamp, composition);
    EXPECT_GT(stamp, previous);
    previous = stamp;
  }
}

TEST(PresentTimingPolicy, CompositionsFasterThanTheGridDisableRefinement) {
  refiner_t refiner;
  std::int64_t composition = 100000;
  for (int k = 0; k < refiner_t::grid_violation_limit + 1; ++k) {
    composition += grid / 2;
    refiner.refine(composition, grid, {}, minimum_step, stale);
  }
  EXPECT_TRUE(refiner.grid_disabled());
  composition += grid;
  EXPECT_EQ(refiner.refine(composition, grid, {}, minimum_step, stale).stamp, composition);
}

TEST(PresentTimingPolicy, SourceResetKeepsStampsMonotonic) {
  refiner_t refiner;
  const auto first = refiner.refine(50 * grid, grid, {}, minimum_step, stale).stamp;
  refiner.reset_source();
  const auto second = refiner.refine(50 * grid + grid, grid, {}, minimum_step, stale).stamp;
  EXPECT_GT(second, first);
}

TEST(WgcCapturePolicy, SystemRelativeTimeConvertsToPerformanceCounterTicks) {
  using platf::dxgi::wgc_policy::system_relative_time_to_qpc;
  EXPECT_EQ(system_relative_time_to_qpc(12345678, 10000000), 12345678u);
  EXPECT_EQ(system_relative_time_to_qpc(10000000, 24000000), 24000000u);
  EXPECT_EQ(system_relative_time_to_qpc(15000000, 24000000), 36000000u);
  EXPECT_EQ(system_relative_time_to_qpc(0, 24000000), 0u);
  // Large uptimes must not overflow the intermediate product.
  EXPECT_EQ(system_relative_time_to_qpc(864000000000000, 24000000), 2073600000000000u);
}
