#ifdef _WIN32

  #include "src/platform/windows/ipc/ipc_session.h"
  #include "src/platform/windows/ipc/pipes.h"
  #include "src/platform/windows/wgc_damage_tracker.h"

  #include <array>
  #include <atomic>
  #include <gtest/gtest.h>
  #include <memory>
  #include <thread>
  #include <vector>

namespace {
  using platf::dxgi::release_wgc_texture_slot;
  using platf::dxgi::transition_wgc_texture_slot;
  using platf::dxgi::wgc_texture_slot_metadata_t;
  using platf::dxgi::wgc_texture_slot_state;
  using platf::dxgi::wgc_texture_slot_state_e;

  class slot_release_recorder_t final: public platf::dxgi::wgc_texture_slot_release_sink_t {
  public:
    void release_wgc_texture_slot(size_t slot, LONG64 frame_id) override {
      releases.emplace_back(slot, frame_id);
    }

    std::vector<std::pair<size_t, LONG64>> releases;
  };

  struct aliasing_slot_owner_t {
    aliasing_slot_owner_t(platf::dxgi::wgc_texture_slot_lease_t slot_lease):
        lease(std::move(slot_lease)) {}

    int image = 0;
    platf::dxgi::wgc_texture_slot_lease_t lease;
  };

  void publish_slot(wgc_texture_slot_metadata_t &slot, LONG64 frame_id) {
    slot.frame_id = frame_id;
    slot.frame_qpc = frame_id * 100;
    ASSERT_TRUE(transition_wgc_texture_slot(slot, wgc_texture_slot_state_e::writing, wgc_texture_slot_state_e::ready));
  }
}  // namespace

TEST(WgcTextureRing, LeasedSlotRequiresMatchingGenerationToRelease) {
  wgc_texture_slot_metadata_t slot {};

  ASSERT_TRUE(transition_wgc_texture_slot(slot, wgc_texture_slot_state_e::free, wgc_texture_slot_state_e::writing));
  publish_slot(slot, 42);
  ASSERT_TRUE(transition_wgc_texture_slot(slot, wgc_texture_slot_state_e::ready, wgc_texture_slot_state_e::leased));

  EXPECT_FALSE(release_wgc_texture_slot(slot, 41));
  EXPECT_EQ(wgc_texture_slot_state(slot), static_cast<LONG>(wgc_texture_slot_state_e::leased));
  EXPECT_TRUE(release_wgc_texture_slot(slot, 42));
  EXPECT_EQ(wgc_texture_slot_state(slot), static_cast<LONG>(wgc_texture_slot_state_e::free));
}

TEST(WgcTextureRing, ProducerCannotReuseLeasedSlots) {
  std::array<wgc_texture_slot_metadata_t, platf::dxgi::WGC_IPC_TEXTURE_SLOT_COUNT> slots {};
  for (size_t index = 0; index < slots.size(); ++index) {
    ASSERT_TRUE(transition_wgc_texture_slot(slots[index], wgc_texture_slot_state_e::free, wgc_texture_slot_state_e::writing));
    publish_slot(slots[index], static_cast<LONG64>(index + 1));
    ASSERT_TRUE(transition_wgc_texture_slot(slots[index], wgc_texture_slot_state_e::ready, wgc_texture_slot_state_e::leased));

    EXPECT_FALSE(transition_wgc_texture_slot(slots[index], wgc_texture_slot_state_e::free, wgc_texture_slot_state_e::writing));
    EXPECT_FALSE(transition_wgc_texture_slot(slots[index], wgc_texture_slot_state_e::ready, wgc_texture_slot_state_e::writing));
  }
}

TEST(WgcTextureRing, ProducerMayReclaimAnUnclaimedReadySlot) {
  wgc_texture_slot_metadata_t slot {};
  ASSERT_TRUE(transition_wgc_texture_slot(slot, wgc_texture_slot_state_e::free, wgc_texture_slot_state_e::writing));
  publish_slot(slot, 10);

  EXPECT_TRUE(transition_wgc_texture_slot(slot, wgc_texture_slot_state_e::ready, wgc_texture_slot_state_e::writing));
  EXPECT_EQ(wgc_texture_slot_state(slot), static_cast<LONG>(wgc_texture_slot_state_e::writing));
}

TEST(WgcTextureRing, OnlyOneConsumerCanClaimReadySlot) {
  wgc_texture_slot_metadata_t slot {};
  ASSERT_TRUE(transition_wgc_texture_slot(slot, wgc_texture_slot_state_e::free, wgc_texture_slot_state_e::writing));
  publish_slot(slot, 7);

  std::atomic<int> claims = 0;
  std::vector<std::thread> consumers;
  consumers.reserve(8);
  for (int index = 0; index < 8; ++index) {
    consumers.emplace_back([&]() {
      if (transition_wgc_texture_slot(slot, wgc_texture_slot_state_e::ready, wgc_texture_slot_state_e::leased)) {
        claims.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (auto &consumer : consumers) {
    consumer.join();
  }

  EXPECT_EQ(claims.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(wgc_texture_slot_state(slot), static_cast<LONG>(wgc_texture_slot_state_e::leased));
}

TEST(WgcTextureRing, AliasingOwnerReleasesSlotAfterFinalConsumer) {
  auto recorder = std::make_shared<slot_release_recorder_t>();
  auto owner = std::make_shared<aliasing_slot_owner_t>(
    platf::dxgi::wgc_texture_slot_lease_t {recorder, 1, 73}
  );

  std::shared_ptr<int> first_consumer {owner, &owner->image};
  auto second_consumer = first_consumer;
  owner.reset();
  first_consumer.reset();
  EXPECT_TRUE(recorder->releases.empty());

  second_consumer.reset();
  ASSERT_EQ(recorder->releases.size(), 1);
  EXPECT_EQ(recorder->releases.front().first, 1U);
  EXPECT_EQ(recorder->releases.front().second, 73);
}

TEST(WgcTextureRing, MovedLeaseReleasesItsGenerationOnlyOnce) {
  auto recorder = std::make_shared<slot_release_recorder_t>();
  {
    platf::dxgi::wgc_texture_slot_lease_t first {recorder, 2, 99};
    auto second = std::move(first);
  }

  ASSERT_EQ(recorder->releases.size(), 1);
  EXPECT_EQ(recorder->releases.front().first, 2U);
  EXPECT_EQ(recorder->releases.front().second, 99);
}

TEST(WgcDamageTracker, StartsWithFullCopyThenSkipsValidNoChangeFrames) {
  platf::dxgi::wgc_texture_ring_damage_tracker_t tracker {100, 100};

  EXPECT_EQ(tracker.plan_for_slot(0).kind, platf::dxgi::wgc_damage_copy_kind_e::full);
  tracker.mark_copied(0);
  EXPECT_EQ(tracker.plan_for_slot(0).kind, platf::dxgi::wgc_damage_copy_kind_e::skip);
}

TEST(WgcDamageTracker, AccumulatesDamageAcrossFramesBeforeSlotIsReused) {
  platf::dxgi::wgc_texture_ring_damage_tracker_t tracker {100, 100};
  tracker.mark_copied(0);

  const std::array first {platf::dxgi::wgc_damage_rect_t {.x = 2, .y = 3, .width = 10, .height = 10}};
  const std::array second {platf::dxgi::wgc_damage_rect_t {.x = 8, .y = 9, .width = 12, .height = 6}};
  EXPECT_TRUE(tracker.accumulate(first));
  EXPECT_TRUE(tracker.accumulate(second));

  const auto plan = tracker.plan_for_slot(0);
  EXPECT_EQ(plan.kind, platf::dxgi::wgc_damage_copy_kind_e::partial);
  EXPECT_EQ(plan.rect.x, 2);
  EXPECT_EQ(plan.rect.y, 3);
  EXPECT_EQ(plan.rect.width, 18);
  EXPECT_EQ(plan.rect.height, 12);
}

TEST(WgcDamageTracker, RetainsDamageForSlotsLeasedWhileAnotherSlotUpdates) {
  platf::dxgi::wgc_texture_ring_damage_tracker_t tracker {100, 100};
  for (size_t slot = 0; slot < platf::dxgi::WGC_IPC_TEXTURE_SLOT_COUNT; ++slot) {
    tracker.mark_copied(slot);
  }

  const std::array damage {platf::dxgi::wgc_damage_rect_t {.x = 10, .y = 20, .width = 15, .height = 10}};
  ASSERT_TRUE(tracker.accumulate(damage));
  tracker.mark_copied(0);

  EXPECT_EQ(tracker.plan_for_slot(0).kind, platf::dxgi::wgc_damage_copy_kind_e::skip);
  const auto leased_slot_plan = tracker.plan_for_slot(1);
  EXPECT_EQ(leased_slot_plan.kind, platf::dxgi::wgc_damage_copy_kind_e::partial);
  EXPECT_EQ(leased_slot_plan.rect.x, 10);
  EXPECT_EQ(leased_slot_plan.rect.y, 20);
}

TEST(WgcDamageTracker, InvalidDamageForcesConservativeFullCopies) {
  platf::dxgi::wgc_texture_ring_damage_tracker_t tracker {100, 100};
  tracker.mark_copied(0);
  tracker.mark_copied(1);

  const std::array invalid {platf::dxgi::wgc_damage_rect_t {.x = 95, .y = 0, .width = 6, .height = 1}};
  EXPECT_FALSE(tracker.accumulate(invalid));
  EXPECT_EQ(tracker.plan_for_slot(0).kind, platf::dxgi::wgc_damage_copy_kind_e::full);
  EXPECT_EQ(tracker.plan_for_slot(1).kind, platf::dxgi::wgc_damage_copy_kind_e::full);
}

TEST(WgcDamageTracker, UsesFullCopyAtSeventyFivePercentDamage) {
  platf::dxgi::wgc_texture_ring_damage_tracker_t tracker {100, 100};
  tracker.mark_copied(0);

  const std::array damage {platf::dxgi::wgc_damage_rect_t {.x = 0, .y = 0, .width = 75, .height = 100}};
  ASSERT_TRUE(tracker.accumulate(damage));
  EXPECT_EQ(tracker.plan_for_slot(0).kind, platf::dxgi::wgc_damage_copy_kind_e::full);
}

#endif
