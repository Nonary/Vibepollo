#include "../tests_common.h"

#include "src/rtsp_pending_policy.h"

#include <limits>
#include <set>

TEST(RtspPendingPolicy, ValidatesAndNormalizesClientCaptureFramerate) {
  using rtsp_stream::pending_policy::normalize_requested_framerate;
  using rtsp_stream::pending_policy::parse_requested_framerate;
  using normalized_t = rtsp_stream::pending_policy::normalized_framerate_t;

  EXPECT_FALSE(normalize_requested_framerate(0));
  EXPECT_FALSE(normalize_requested_framerate(-1));
  EXPECT_FALSE(normalize_requested_framerate(std::numeric_limits<std::int64_t>::max()));
  EXPECT_EQ(normalize_requested_framerate(60), (normalized_t {.capture_framerate = 60, .encoding_framerate = 60000}));
  EXPECT_EQ(normalize_requested_framerate(59940), (normalized_t {.capture_framerate = 60, .encoding_framerate = 59940}));
  EXPECT_EQ(normalize_requested_framerate(4000), (normalized_t {.capture_framerate = 4000, .encoding_framerate = 4000}));
  EXPECT_EQ(normalize_requested_framerate(4000000), (normalized_t {.capture_framerate = 4000, .encoding_framerate = 4000000}));
  EXPECT_FALSE(normalize_requested_framerate(4001000));
  EXPECT_EQ(parse_requested_framerate("120"), (normalized_t {.capture_framerate = 120, .encoding_framerate = 120000}));
  EXPECT_FALSE(parse_requested_framerate(""));
  EXPECT_FALSE(parse_requested_framerate("60fps"));
  EXPECT_FALSE(parse_requested_framerate("999999999999999999999999999999999999"));
}

TEST(RtspPendingPolicy, MixedNatEncryptedFramingSelectsAuthenticatedRoute) {
  const std::array<std::uint8_t, 4> encrypted_word {0x80, 0x00, 0x00, 0x10};
  EXPECT_EQ(
    rtsp_stream::pending_policy::choose_initial_route(true, true, encrypted_word),
    rtsp_stream::pending_policy::initial_route_e::encrypted
  );
}

TEST(RtspPendingPolicy, ProcesslessRemoteRolesKeepControlServerAlive) {
  using remote_session::role_e;
  EXPECT_FALSE(rtsp_stream::pending_policy::game_session_requires_shutdown(false, role_e::monitor));
  EXPECT_FALSE(rtsp_stream::pending_policy::game_session_requires_shutdown(false, role_e::input));
  EXPECT_TRUE(rtsp_stream::pending_policy::control_server_should_remain_alive(false, true, false));
}

TEST(RtspPendingPolicy, EndedGameStopsWithoutTakingDownProcesslessRemoteRoles) {
  using remote_session::role_e;
  EXPECT_FALSE(rtsp_stream::pending_policy::game_session_requires_shutdown(true, role_e::game));
  EXPECT_TRUE(rtsp_stream::pending_policy::game_session_requires_shutdown(false, role_e::game));

  // A connected game cannot own its own lifetime after the app ends.
  EXPECT_FALSE(rtsp_stream::pending_policy::control_server_should_remain_alive(false, false, false));

  // A processless peer keeps the shared server alive while the ended game is
  // removed selectively. A game awaiting its peer or draining after a local
  // stop keeps one final control iteration for graceful cleanup.
  EXPECT_TRUE(rtsp_stream::pending_policy::control_server_should_remain_alive(false, true, false));
  EXPECT_TRUE(rtsp_stream::pending_policy::control_server_should_remain_alive(false, false, true));
  EXPECT_TRUE(rtsp_stream::pending_policy::control_server_should_remain_alive(true, false, false));
}

TEST(RtspPendingPolicy, EndStreamSelectsEveryGameTransportButNoRemoteRole) {
  using remote_session::role_e;
  EXPECT_TRUE(rtsp_stream::pending_policy::disconnect_scope_matches(role_e::game, role_e::game, false, true));
  EXPECT_TRUE(rtsp_stream::pending_policy::disconnect_scope_matches(role_e::game, role_e::game, true, false));
  EXPECT_FALSE(rtsp_stream::pending_policy::disconnect_scope_matches(role_e::monitor, role_e::game, true, true));
  EXPECT_FALSE(rtsp_stream::pending_policy::disconnect_scope_matches(role_e::game, role_e::monitor, false, false));
}

TEST(RtspPendingPolicy, PendingExpiryForgetsOnlyRemoteInputGeneration) {
  const std::vector<rtsp_stream::pending_policy::pending_owner_t> expired {
    {.role = remote_session::role_e::input, .client_uuid = "input", .generation = 7},
    {.role = remote_session::role_e::monitor, .client_uuid = "monitor", .generation = 9},
    {.role = remote_session::role_e::game, .client_uuid = "game", .generation = 11},
  };
  const auto forgotten = rtsp_stream::pending_policy::expired_remote_input_owners(expired);
  ASSERT_EQ(forgotten.size(), 1);
  EXPECT_EQ(forgotten[0].client_uuid, "input");
  EXPECT_EQ(forgotten[0].generation, 7u);
}

TEST(RtspPendingPolicy, DisconnectCleanupDoesNotSelectPostRemovalInputGeneration) {
  const std::vector<rtsp_stream::pending_policy::pending_owner_t> removed {{.role = remote_session::role_e::input, .client_uuid = "client", .generation = 7}};
  const auto cleanup = rtsp_stream::pending_policy::disconnect_input_owners_to_forget(removed);
  ASSERT_EQ(cleanup.size(), 1);
  EXPECT_EQ(cleanup[0].generation, 7u);
  std::set<std::uint64_t> remembered_generations {7, 8};
  for (const auto &owner : cleanup) remembered_generations.erase(owner.generation);
  EXPECT_FALSE(remembered_generations.contains(7));
  EXPECT_TRUE(remembered_generations.contains(8));
}

TEST(RtspPendingPolicy, ApolloCadenceKeepsValidationFractionalRatesAndOptOut) {
  using namespace rtsp_stream::pending_policy;
  const auto warp = *parse_requested_framerate("240");
  EXPECT_EQ(select_encoding_framerate(warp, 60000, true), 60000);
  EXPECT_EQ(select_encoding_framerate(warp, 60, true), 60000);
  EXPECT_EQ(select_encoding_framerate(warp, 60000, false), 240000);
  EXPECT_EQ(select_encoding_framerate(*parse_requested_framerate("59940"), 59940, true), 59940);
  for (int invalid : {0, -1, std::numeric_limits<int>::max()}) {
    EXPECT_EQ(select_encoding_framerate(warp, invalid, true), 240000);
  }
}

TEST(RtspPendingPolicy, HostCeilingAppliesAfterWarpAndKeepsOriginalRequestedBudget) {
  using namespace rtsp_stream::pending_policy;
  const auto normal = negotiate_bitrate(100000, 100000, 20000, {60, 60000}, true, 20, 2, true);
  EXPECT_EQ(normal.requested_kbps, 100000);
  EXPECT_EQ(normal.encoder_kbps, 14988);
  const auto warp = negotiate_bitrate(100000, 100000, 20000, {240, 60000}, true, 20, 2, true);
  EXPECT_EQ(warp.encoder_kbps, normal.encoder_kbps);
  EXPECT_EQ(warp.requested_kbps, 100000);
  EXPECT_EQ(negotiate_bitrate(10000, 10000, 0, {240, 60000}, true, 20, 2, true).encoder_kbps, 30988);
  EXPECT_EQ(negotiate_bitrate(10000, 10000, 0, {240, 240000}, false, 20, 2, true).encoder_kbps, 6988);
}

TEST(RtspPendingPolicy, BitratePolicyPreservesUncappedAndLegacyNegotiation) {
  using namespace rtsp_stream::pending_policy;
  EXPECT_EQ(negotiate_bitrate(100000, 100000, 0, {60, 60000}, true, 20, 2, true).encoder_kbps, 78988);
  EXPECT_EQ(negotiate_bitrate(10000, 10000, 0, {60, 59940}, true, 20, 2, true).encoder_kbps, 6988);
  EXPECT_EQ(negotiate_bitrate(0, 100000, 0, {60, 60000}, true, 20, 2, true).encoder_kbps, 100000);
  EXPECT_EQ(negotiate_bitrate(0, 100000, 20000, {60, 60000}, true, 20, 2, true).encoder_kbps, 20000);
  for (const auto request : {1LL, 20000LL, std::numeric_limits<long long>::max()}) {
    const auto rate = negotiate_bitrate(request, 100000, 20000, {4000, 1}, true, 80, 8, true);
    EXPECT_GT(rate.encoder_kbps, 0);
    EXPECT_LE(rate.encoder_kbps, 20000);
  }
  const auto huge = negotiate_bitrate(std::numeric_limits<std::int64_t>::max(), 100000, 0, {4000, 1}, true, 0, 0, false);
  EXPECT_LE(huge.encoder_kbps, std::numeric_limits<int>::max() / 1000);
}
