#include "../tests_common.h"

#include "src/rtsp_pending_policy.h"

TEST(RtspPendingPolicy, MixedNatEncryptedFramingSelectsAuthenticatedRoute) {
  const std::array<std::uint8_t, 4> encrypted_word {0x80, 0x00, 0x00, 0x10};
  EXPECT_EQ(
    rtsp_stream::pending_policy::choose_initial_route(true, true, encrypted_word),
    rtsp_stream::pending_policy::initial_route_e::encrypted
  );
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
