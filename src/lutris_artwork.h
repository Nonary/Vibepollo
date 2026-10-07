/** @file src/lutris_artwork.h
 *  @brief Lutris cover-art preparation for catalog clients.
 */
#pragma once

#include "lutris_integration.h"

#include <filesystem>
#include <vector>

namespace platf::lutris::artwork {
  std::filesystem::path cache_path(const std::filesystem::path &appdata, std::int64_t id);

  // Prefer Lutris portrait cover art, falling back to its square icon only
  // when no cover exists, and produce the PNG required by catalog clients.
  void prepare(std::vector<game_t> &games, const std::filesystem::path &appdata);
}  // namespace platf::lutris::artwork
