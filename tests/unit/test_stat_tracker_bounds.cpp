#include "../tests_common.h"
#include "src/stat_trackers.h"

TEST(StatTrackerBounds, NegativeFloatingValuesHaveNegativeMaximum) {
  stat_trackers::min_max_avg_tracker<double> tracker;
  int callbacks = 0;
  const auto callback = [&](double minimum, double maximum, double average) {
    ++callbacks;
    EXPECT_DOUBLE_EQ(minimum, -3.5);
    EXPECT_DOUBLE_EQ(maximum, -1.5);
    EXPECT_DOUBLE_EQ(average, -2.5);
  };
  tracker.collect_and_callback_on_interval(-3.5, callback, std::chrono::seconds(60));
  tracker.collect_and_callback_on_interval(-2.5, callback, std::chrono::seconds(60));
  tracker.collect_and_callback_on_interval(-1.5, callback, std::chrono::seconds(60));
  EXPECT_EQ(callbacks, 0);
  tracker.collect_and_callback_on_interval(0., callback, std::chrono::seconds(-1));
  EXPECT_EQ(callbacks, 1);
  tracker.reset();
  tracker.collect_and_callback_on_interval(-4., callback, std::chrono::seconds(-1));
  EXPECT_EQ(callbacks, 1);
}
