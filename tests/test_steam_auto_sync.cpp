#include "src/steam_auto_sync_policy.h"

#include <gtest/gtest.h>

namespace {
  TEST(SteamAutoSyncPolicy, FingerprintIsStableAndTracksMetadata) {
    platf::steam::game_t game;
    game.app_id = 2679460;
    game.stable_id = "steam:2679460";
    game.name = "Metaphor: ReFantazio";
    game.app_type = "game";

    const std::vector<platf::steam::game_t> games {game};
    EXPECT_EQ(platf::steam::autosync::source_fingerprint(games),
              platf::steam::autosync::source_fingerprint(games));

    auto changed = games;
    changed.front().name = "Renamed";
    EXPECT_NE(platf::steam::autosync::source_fingerprint(games),
              platf::steam::autosync::source_fingerprint(changed));
  }

  TEST(SteamAutoSyncPolicy, FingerprintTracksArtworkSource) {
    platf::steam::game_t first;
    first.app_id = 42;
    first.artwork_format = "jpg";
    first.artwork_path = "/tmp/steam-cover.jpg";
    auto second = first;
    second.artwork_path = "/tmp/steam-cover-updated.jpg";

    EXPECT_NE(platf::steam::autosync::source_fingerprint({first}),
              platf::steam::autosync::source_fingerprint({second}));
  }
}  // namespace
