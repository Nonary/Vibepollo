#pragma once

#ifdef _WIN32

#include <filesystem>
#include <optional>
#include <vector>

namespace lossless_paths {

  /**
   * Default Steam install location for Lossless Scaling.
   */
  std::filesystem::path default_steam_lossless_path();

  /**
   * If the candidate points to a valid Lossless Scaling executable (or a directory
   * containing one), return the normalized path. Otherwise, std::nullopt.
   */
  std::optional<std::filesystem::path> resolve_lossless_candidate(const std::filesystem::path &candidate);

  /**
   * Build a deduplicated list of plausible Lossless Scaling executable paths,
   * prioritizing override/configured/default hints and searching common install roots.
   */
  std::vector<std::filesystem::path> discover_lossless_candidates(
    const std::optional<std::filesystem::path> &configured,
    const std::optional<std::filesystem::path> &override_candidate,
    const std::optional<std::filesystem::path> &default_path);

}  // namespace lossless_paths

#endif  // _WIN32

