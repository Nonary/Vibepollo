#include "../tests_common.h"
#include "src/platform/windows/virtual_display_recovery_policy.h"

TEST(VirtualDisplayRecoveryTiming, BoundsInPlaceAttemptsAndKeepsDelayedFallback) {
  using namespace std::chrono_literals;
  VDISPLAY::policy::inactive_reactivation_budget_t budget;

  EXPECT_FALSE(budget.consume_if_due(749ms));
  EXPECT_TRUE(budget.consume_if_due(750ms));
  EXPECT_FALSE(budget.consume_if_due(1499ms));
  EXPECT_TRUE(budget.consume_if_due(1500ms));
  EXPECT_FALSE(budget.consume_if_due(2249ms));
  EXPECT_TRUE(budget.consume_if_due(2250ms));
  EXPECT_EQ(budget.attempts(), 3u);
  EXPECT_FALSE(budget.consume_if_due(11s));
  EXPECT_EQ(VDISPLAY::policy::inactive_reactivation_budget_t::fallback_grace, 12s);

  budget.reset();
  EXPECT_TRUE(budget.consume_if_due(750ms));
  EXPECT_EQ(budget.attempts(), 1u);
}
