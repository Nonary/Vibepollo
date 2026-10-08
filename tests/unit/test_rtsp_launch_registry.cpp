#include "../tests_common.h"

#include "src/rtsp_launch_registry.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace {
  using namespace std::chrono_literals;

  struct fake_session_t {
    std::uint32_t id {};
    std::string unique_id;
    std::string rtsp_source_address;
    std::optional<int> rtsp_cipher;
  };

  using registry_t = rtsp_stream::launch_registry_t<fake_session_t>;
  using add_result_e = registry_t::add_result_e;

  constexpr auto ping_timeout = std::chrono::milliseconds {10s};
  constexpr std::size_t capacity = 8;

  std::shared_ptr<fake_session_t> plaintext(std::uint32_t id, std::string address = "10.0.0.2") {
    return std::make_shared<fake_session_t>(fake_session_t {
      .id = id,
      .unique_id = "launch-" + std::to_string(id),
      .rtsp_source_address = std::move(address),
      .rtsp_cipher = std::nullopt,
    });
  }

  std::shared_ptr<fake_session_t> encrypted(std::uint32_t id, std::string address = "10.0.0.2") {
    auto session = plaintext(id, std::move(address));
    session->rtsp_cipher = 1;
    return session;
  }
}  // namespace

TEST(RtspLaunchRegistry, ClaimedLaunchSurvivesSlowAnnounceUntilPlay) {
  // Mirrors the failing host logs: the client starts RTSP 9.5s after /launch
  // queues the launch, and ANNOUNCE's startup worker finishes after the
  // original 10s deadline.
  registry_t registry;
  const auto t0 = std::chrono::steady_clock::now();
  ASSERT_EQ(registry.add(plaintext(1), t0, ping_timeout, capacity), add_result_e::added);

  ASSERT_TRUE(registry.claim_startup(1, "launch-1", t0 + 9500ms));

  // The expiry timer fires while the worker waits for the lifecycle gate.
  EXPECT_TRUE(registry.expire(t0 + 10500ms).empty());
  EXPECT_TRUE(registry.contains_live(1, "launch-1", t0 + 10500ms));

  registry.finish_startup(1, "launch-1", true, t0 + 10900ms, ping_timeout);

  // PLAY arrives on a new connection and must still find its launch.
  EXPECT_TRUE(registry.contains_live(1, "launch-1", t0 + 11s));
  EXPECT_TRUE(registry.expire(t0 + 20s).empty());

  // Without a control connection to clear it, the fresh window still ends.
  const auto expired = registry.expire(t0 + 20900ms);
  ASSERT_EQ(expired.size(), 1u);
  EXPECT_EQ(expired.front()->id, 1u);
  EXPECT_TRUE(registry.empty());
}

TEST(RtspLaunchRegistry, UnclaimedLaunchExpiresAtItsDeadline) {
  registry_t registry;
  const auto t0 = std::chrono::steady_clock::now();
  ASSERT_EQ(registry.add(plaintext(1), t0, ping_timeout, capacity), add_result_e::added);

  EXPECT_TRUE(registry.expire(t0 + 9999ms).empty());
  EXPECT_FALSE(registry.contains_live(1, "launch-1", t0 + 10s));
  EXPECT_FALSE(registry.claim_startup(1, "launch-1", t0 + 10s));
  EXPECT_EQ(registry.expire(t0 + 10s).size(), 1u);
  EXPECT_TRUE(registry.empty());
}

TEST(RtspLaunchRegistry, FailedStartupKeepsOriginalDeadline) {
  registry_t registry;
  const auto t0 = std::chrono::steady_clock::now();
  ASSERT_EQ(registry.add(plaintext(1), t0, ping_timeout, capacity), add_result_e::added);
  ASSERT_TRUE(registry.claim_startup(1, "launch-1", t0 + 2s));

  registry.finish_startup(1, "launch-1", false, t0 + 4s, ping_timeout);
  EXPECT_TRUE(registry.expire(t0 + 9s).empty());
  EXPECT_EQ(registry.expire(t0 + 10s).size(), 1u);
}

TEST(RtspLaunchRegistry, FailedStartupPastDeadlineExpiresOnNextCheck) {
  registry_t registry;
  const auto t0 = std::chrono::steady_clock::now();
  ASSERT_EQ(registry.add(plaintext(1), t0, ping_timeout, capacity), add_result_e::added);
  ASSERT_TRUE(registry.claim_startup(1, "launch-1", t0 + 9s));

  registry.finish_startup(1, "launch-1", false, t0 + 12s, ping_timeout);
  EXPECT_EQ(registry.next_expiry(), t0 + 10s);
  EXPECT_EQ(registry.expire(t0 + 12s).size(), 1u);
}

TEST(RtspLaunchRegistry, StartupClaimIsExclusiveUntilReleased) {
  registry_t registry;
  const auto t0 = std::chrono::steady_clock::now();
  ASSERT_EQ(registry.add(plaintext(1), t0, ping_timeout, capacity), add_result_e::added);

  EXPECT_FALSE(registry.claim_startup(1, "other-launch", t0 + 1s));
  ASSERT_TRUE(registry.claim_startup(1, "launch-1", t0 + 1s));
  EXPECT_FALSE(registry.claim_startup(1, "launch-1", t0 + 2s));

  registry.release_startup(1, "launch-1");
  EXPECT_TRUE(registry.claim_startup(1, "launch-1", t0 + 3s));

  // A finished startup keeps its claim, so a repeated ANNOUNCE is rejected.
  registry.finish_startup(1, "launch-1", true, t0 + 4s, ping_timeout);
  EXPECT_FALSE(registry.claim_startup(1, "launch-1", t0 + 5s));
}

TEST(RtspLaunchRegistry, ReleasedClaimPastDeadlineExpires) {
  registry_t registry;
  const auto t0 = std::chrono::steady_clock::now();
  ASSERT_EQ(registry.add(plaintext(1), t0, ping_timeout, capacity), add_result_e::added);
  ASSERT_TRUE(registry.claim_startup(1, "launch-1", t0 + 9s));
  EXPECT_TRUE(registry.expire(t0 + 11s).empty());

  registry.release_startup(1, "launch-1");
  EXPECT_EQ(registry.expire(t0 + 11s).size(), 1u);
}

TEST(RtspLaunchRegistry, NextExpiryIgnoresRunningStartups) {
  registry_t registry;
  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_FALSE(registry.next_expiry());

  ASSERT_EQ(registry.add(plaintext(1, "10.0.0.2"), t0, ping_timeout, capacity), add_result_e::added);
  ASSERT_EQ(registry.add(plaintext(2, "10.0.0.3"), t0 + 2s, ping_timeout, capacity), add_result_e::added);
  EXPECT_EQ(registry.next_expiry(), t0 + 10s);

  // A running startup whose deadline passes must not make the timer fire
  // immediately and repeatedly.
  ASSERT_TRUE(registry.claim_startup(1, "launch-1", t0 + 9s));
  EXPECT_EQ(registry.next_expiry(), t0 + 12s);

  ASSERT_TRUE(registry.claim_startup(2, "launch-2", t0 + 11s));
  EXPECT_FALSE(registry.next_expiry());

  registry.finish_startup(1, "launch-1", true, t0 + 13s, ping_timeout);
  EXPECT_EQ(registry.next_expiry(), t0 + 23s);
}

TEST(RtspLaunchRegistry, AddRejectsCollisionsAndOverflow) {
  registry_t registry;
  const auto t0 = std::chrono::steady_clock::now();
  ASSERT_EQ(registry.add(plaintext(1, "10.0.0.2"), t0, ping_timeout, 2), add_result_e::added);

  EXPECT_EQ(registry.add(encrypted(1, "10.0.0.9"), t0, ping_timeout, 2), add_result_e::duplicate_id);
  EXPECT_EQ(registry.add(plaintext(2, "10.0.0.2"), t0, ping_timeout, 2), add_result_e::duplicate_plaintext_source);

  // Encrypted RTSP is authenticated per connection, not routed by address.
  ASSERT_EQ(registry.add(encrypted(3, "10.0.0.2"), t0, ping_timeout, 2), add_result_e::added);
  EXPECT_EQ(registry.add(plaintext(4, "10.0.0.4"), t0, ping_timeout, 2), add_result_e::full);
}

TEST(RtspLaunchRegistry, ExpiredPlaintextLaunchFreesItsSourceAddress) {
  registry_t registry;
  const auto t0 = std::chrono::steady_clock::now();
  ASSERT_EQ(registry.add(plaintext(1, "10.0.0.2"), t0, ping_timeout, capacity), add_result_e::added);

  EXPECT_EQ(registry.add(plaintext(2, "10.0.0.2"), t0 + 10s, ping_timeout, capacity), add_result_e::added);
}

TEST(RtspLaunchRegistry, RunningStartupStillBlocksItsSourceAddress) {
  registry_t registry;
  const auto t0 = std::chrono::steady_clock::now();
  ASSERT_EQ(registry.add(plaintext(1, "10.0.0.2"), t0, ping_timeout, capacity), add_result_e::added);
  ASSERT_TRUE(registry.claim_startup(1, "launch-1", t0 + 9s));

  EXPECT_EQ(registry.add(plaintext(2, "10.0.0.2"), t0 + 11s, ping_timeout, capacity), add_result_e::duplicate_plaintext_source);
}

TEST(RtspLaunchRegistry, RemoveIfAndClearReturnRemovedSessions) {
  registry_t registry;
  const auto t0 = std::chrono::steady_clock::now();
  ASSERT_EQ(registry.add(plaintext(1, "10.0.0.2"), t0, ping_timeout, capacity), add_result_e::added);
  ASSERT_EQ(registry.add(plaintext(2, "10.0.0.3"), t0, ping_timeout, capacity), add_result_e::added);
  ASSERT_EQ(registry.add(plaintext(3, "10.0.0.4"), t0, ping_timeout, capacity), add_result_e::added);

  // Removal ignores startup state: a cancel or disconnect wins over a running worker.
  ASSERT_TRUE(registry.claim_startup(2, "launch-2", t0 + 1s));
  const auto removed = registry.remove_if([](const fake_session_t &session) {
    return session.id == 2;
  });
  ASSERT_EQ(removed.size(), 1u);
  EXPECT_EQ(removed.front()->id, 2u);
  EXPECT_FALSE(registry.contains_live(2, "launch-2", t0 + 1s));
  EXPECT_EQ(registry.size(), 2u);

  EXPECT_EQ(registry.clear().size(), 2u);
  EXPECT_TRUE(registry.empty());
}
