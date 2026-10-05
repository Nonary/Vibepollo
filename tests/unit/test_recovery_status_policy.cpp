#include "src/platform/windows/recovery_status.h"

#include <gtest/gtest.h>
#include <cstdint>

TEST(RecoveryStatusPolicy, TracksFailureParkingAndPowerEdges) {
  using display_helper::recovery_status::policy;
  using display_helper::recovery_status::status;

  policy recovery;
  recovery.begin(41, 7, 12);
  ASSERT_TRUE(recovery.query(41, 7, 12, 7, 12, false, false, true) == status::active);
  ASSERT_TRUE(!recovery.parked());
  ASSERT_TRUE(!recovery.publish(status::failed, 40, 7, 12));
  ASSERT_TRUE(!recovery.publish(status::failed, 41, 6, 12));
  ASSERT_TRUE(recovery.publish(status::failed, 41, 7, 12));

  // A stale ticket/epoch/generation cannot authorize cleanup.
  ASSERT_TRUE(recovery.query(40, 7, 12, 7, 12, false, false, true) == status::unknown);
  ASSERT_TRUE(recovery.query(41, 7, 11, 7, 12, false, false, true) == status::unknown);
  ASSERT_TRUE(recovery.query(41, 7, 12, 8, 12, false, false, true) == status::unknown);
  ASSERT_TRUE(!recovery.parked());

  // A failed result stays unparked while a worker or queued operation exists.
  ASSERT_TRUE(recovery.query(41, 7, 12, 7, 12, true, false, true) == status::active);
  ASSERT_TRUE(!recovery.parked());
  ASSERT_TRUE(recovery.query(41, 7, 12, 7, 12, false, true, true) == status::active);
  ASSERT_TRUE(!recovery.parked());
  ASSERT_TRUE(recovery.query(41, 7, 12, 7, 12, false, false, true) == status::failed);
  ASSERT_TRUE(recovery.parked());

  const auto first_revision = recovery.observe_event();
  ASSERT_TRUE(first_revision == 1);
  ASSERT_TRUE(recovery.event_revision() == 1);
  recovery.begin(42, 8, 13);
  ASSERT_TRUE(!recovery.parked());
  ASSERT_TRUE(recovery.value() == status::active);
  ASSERT_TRUE(recovery.event_revision() == 0);
  ASSERT_TRUE(!recovery.publish(status::restored, 41, 7, 12));
  ASSERT_TRUE(recovery.publish(status::restored, 42, 8, 13));
  ASSERT_TRUE(recovery.observe_event() == 2);
  ASSERT_TRUE(recovery.event_revision() == 1);
  recovery.supersede();
  ASSERT_TRUE(recovery.query(42, 8, 13, 8, 13, false, false, true) == status::unknown);

  display_helper::recovery_status::monitor_power_edge_policy power;
  ASSERT_TRUE(!power.observe(true, true));   // Initial On only seeds state.
  ASSERT_TRUE(!power.observe(true, true));   // Stable On is not a wake edge.
  ASSERT_TRUE(!power.observe(false, false)); // Unknown observation resets the edge baseline.
  ASSERT_TRUE(!power.observe(true, true));   // First known state after uncertainty is not evidence.
  ASSERT_TRUE(!power.observe(true, false));  // Off arms the wake edge.
  ASSERT_TRUE(power.observe(true, true));    // Only known Off -> On is a wake.
  ASSERT_TRUE(!power.observe(true, true));
  ASSERT_TRUE(!power.observe(true, false));
  ASSERT_TRUE(power.observe(true, true));

}
