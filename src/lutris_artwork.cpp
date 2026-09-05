/** @file src/lutris_artwork.cpp */
#include "lutris_artwork.h"

#include "steam_artwork.h"

namespace fs = std::filesystem;

namespace platf::lutris::artwork {
  fs::path cache_path(const fs::path &appdata, std::int64_t id) {
    return appdata / "covers" / ("lutris_" + std::to_string(id) + ".png");
  }

  void prepare(std::vector<game_t> &games, const fs::path &appdata) {
    for (auto &game : games) {
      game.image_path.clear();
      if (game.id <= 0) continue;
      const auto output = cache_path(appdata, game.id);
      if (!game.session_artwork_revision.empty()) {
        game.image_path = platf::steam::artwork::session_cover("lutris", game.id, game.session_artwork_revision, output);
        continue;
      }
      if (!game.artwork_path.empty()) {
        const auto result = platf::steam::artwork::sync_to(game.artwork_path, output);
        if (!result.client_path.empty()) game.image_path = result.client_path;
      }
      // A corrupt/unsupported cover must not regress below the square icon
      // behavior that predates portrait synchronization.
      if (game.image_path.empty() && !game.icon_path.empty()) {
        const auto result = platf::steam::artwork::sync_to(game.icon_path, output);
        if (!result.client_path.empty()) game.image_path = result.client_path;
      }
    }
  }
}  // namespace platf::lutris::artwork
