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

namespace {
  using hold_t = VDISPLAY::policy::exclusive_layout_hold_t;
  using action_e = hold_t::action_e;

  const std::set<std::string> full_set {"portrait", "virtual", "wide"};
  const std::set<std::string> wide_gone {"portrait", "virtual"};

  // A hold that has watched a correct layout on `connected` for long enough.
  hold_t settled_hold(const std::set<std::string> &connected, hold_t::time_point &now) {
    hold_t hold;
    EXPECT_EQ(hold.observe(connected, true, false, now), action_e::none);
    now += hold_t::settle_time;
    EXPECT_EQ(hold.observe(connected, true, false, now), action_e::none);
    return hold;
  }
}  // namespace

TEST(ExclusiveLayoutHold, RepairsADisplayThatJoinsAfterTheConnectedSetChanges) {
  using namespace std::chrono_literals;
  hold_t::time_point now {};
  auto hold = settled_hold(full_set, now);

  now += 150ms;
  EXPECT_EQ(hold.observe(wide_gone, true, true, now), action_e::repair);
}

TEST(ExclusiveLayoutHold, LeavesADisplayTheUserSwitchedOnByHand) {
  using namespace std::chrono_literals;
  hold_t::time_point now {};
  auto hold = settled_hold(full_set, now);

  now += 150ms;
  EXPECT_EQ(hold.observe(full_set, true, true, now), action_e::none);
  now += 10s;
  EXPECT_EQ(hold.observe(full_set, true, true, now), action_e::none);
  EXPECT_EQ(hold.repairs(), 0u);
}

TEST(ExclusiveLayoutHold, KeepsAHandEnabledDisplayThroughALaterHotplug) {
  using namespace std::chrono_literals;
  hold_t::time_point now {};
  auto hold = settled_hold(full_set, now);

  now += 150ms;
  EXPECT_EQ(hold.observe(full_set, true, true, now), action_e::none);
  // Ten minutes later the wide monitor drops and returns; the display the
  // user switched on was already on, so it is not the hold's to remove.
  now += 10min;
  EXPECT_EQ(hold.observe(wide_gone, true, true, now), action_e::none);
  now += 1s;
  EXPECT_EQ(hold.observe(full_set, true, true, now), action_e::none);

  // Once the layout is stream-only again, the hold is back on duty.
  now += 1s;
  EXPECT_EQ(hold.observe(full_set, true, false, now), action_e::none);
  now += hold_t::settle_time;
  EXPECT_EQ(hold.observe(full_set, true, false, now), action_e::none);
  now += 150ms;
  EXPECT_EQ(hold.observe(wide_gone, true, true, now), action_e::repair);
}

TEST(ExclusiveLayoutHold, StartsOverAfterBeingSuspended) {
  using namespace std::chrono_literals;
  hold_t::time_point now {};
  auto hold = settled_hold(full_set, now);

  // A second stream display appeared; the layout is no longer meant to be
  // exclusive, and nothing learned before then may be acted on afterwards.
  hold.suspend();
  now += 150ms;
  EXPECT_EQ(hold.observe(wide_gone, true, true, now), action_e::none);
  EXPECT_EQ(hold.observe(full_set, true, false, now), action_e::none);
  now += hold_t::settle_time;
  EXPECT_EQ(hold.observe(full_set, true, false, now), action_e::none);
  now += 150ms;
  EXPECT_EQ(hold.observe(wide_gone, true, true, now), action_e::repair);
}

TEST(ExclusiveLayoutHold, DoesNotAdoptTheChangedSetBeforeWindowsLoadsItsLayout) {
  using namespace std::chrono_literals;
  hold_t::time_point now {};
  auto hold = settled_hold(full_set, now);

  // The set changes first; the stored layout arrives a moment later.
  now += 150ms;
  EXPECT_EQ(hold.observe(wide_gone, true, false, now), action_e::none);
  now += 150ms;
  EXPECT_EQ(hold.observe(wide_gone, true, false, now), action_e::none);
  now += 150ms;
  EXPECT_EQ(hold.observe(wide_gone, true, true, now), action_e::repair);
}

TEST(ExclusiveLayoutHold, NeedsASettledReferenceBeforeItActs) {
  using namespace std::chrono_literals;
  hold_t hold;
  hold_t::time_point now {};

  EXPECT_EQ(hold.observe(full_set, true, true, now), action_e::none);
  EXPECT_EQ(hold.observe(full_set, true, false, now), action_e::none);
  now += hold_t::settle_time - 1ms;
  EXPECT_EQ(hold.observe(wide_gone, true, true, now), action_e::none);
}

TEST(ExclusiveLayoutHold, AdoptsASetThatStaysCorrect) {
  using namespace std::chrono_literals;
  hold_t::time_point now {};
  auto hold = settled_hold(full_set, now);

  // The wide monitor is unplugged for good and the layout stays exclusive.
  now += 150ms;
  EXPECT_EQ(hold.observe(wide_gone, true, false, now), action_e::none);
  now += hold_t::settle_time;
  EXPECT_EQ(hold.observe(wide_gone, true, false, now), action_e::none);

  // With that set as the reference, plugging the monitor back in is a change.
  now += 150ms;
  EXPECT_EQ(hold.observe(full_set, true, true, now), action_e::repair);
}

TEST(ExclusiveLayoutHold, LeavesAnInactiveStreamDisplayToReactivation) {
  using namespace std::chrono_literals;
  hold_t::time_point now {};
  auto hold = settled_hold(full_set, now);

  now += 150ms;
  EXPECT_EQ(hold.observe(wide_gone, false, true, now), action_e::none);
  EXPECT_EQ(hold.repairs(), 0u);
}

TEST(ExclusiveLayoutHold, SpacesRepairsAndStopsAtTheBudget) {
  using namespace std::chrono_literals;
  hold_t::time_point now {};
  auto hold = settled_hold(full_set, now);

  now += 150ms;
  ASSERT_EQ(hold.observe(wide_gone, true, true, now), action_e::repair);
  hold.note_repair(now);
  now += hold_t::min_repair_interval - 1ms;
  EXPECT_EQ(hold.observe(wide_gone, true, true, now), action_e::none);
  now += 1ms;
  EXPECT_EQ(hold.observe(wide_gone, true, true, now), action_e::repair);

  while (!hold.exhausted()) {
    hold.note_repair(now);
    now += hold_t::min_repair_interval;
  }
  EXPECT_EQ(hold.repairs(), hold_t::max_repairs);
  EXPECT_EQ(hold.observe(wide_gone, true, true, now), action_e::none);

  // A stream-only layout that settles gives the next episode a full budget.
  now += 150ms;
  EXPECT_EQ(hold.observe(full_set, true, false, now), action_e::none);
  now += hold_t::settle_time;
  EXPECT_EQ(hold.observe(full_set, true, false, now), action_e::none);
  EXPECT_EQ(hold.repairs(), 0u);
  now += 150ms;
  EXPECT_EQ(hold.observe(wide_gone, true, true, now), action_e::repair);
}

TEST(ExclusiveLayoutHold, TriesAgainOnTheNextDropWhenARepairLandedLate) {
  using namespace std::chrono_literals;
  hold_t::time_point now {};
  auto hold = settled_hold(full_set, now);

  now += 150ms;
  ASSERT_EQ(hold.observe(wide_gone, true, true, now), action_e::repair);
  hold.note_repair(now);

  // The monitor is back before the repair is saved, so the full set settles again.
  now += 150ms;
  EXPECT_EQ(hold.observe(full_set, true, false, now), action_e::none);
  now += hold_t::settle_time;
  EXPECT_EQ(hold.observe(full_set, true, false, now), action_e::none);

  // Two minutes later it drops again and Windows loads the same stored layout.
  now += 2min;
  EXPECT_EQ(hold.observe(wide_gone, true, true, now), action_e::repair);
}
