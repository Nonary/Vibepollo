/** @file src/steam_artwork.h
 *  @brief Steam artwork conversion and managed PNG cache.
 */
#pragma once

#include "steam_integration.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace platf::steam::artwork {

  // Scanner-side conversion; only the dropped session UID opens the source.
  std::optional<std::vector<std::uint8_t>> export_png(const std::filesystem::path &source);

  // Import bounded PNG bytes into a caller-selected service cache path.
  bool import_png(const std::vector<std::uint8_t> &bytes, const std::filesystem::path &output);
  using session_fetcher_t = std::function<std::optional<std::vector<std::uint8_t>>(const std::string &request)>;
  std::filesystem::path session_cover(std::string_view provider, std::uint64_t id,
                                     const std::string &revision, const std::filesystem::path &output,
                                     session_fetcher_t fetcher = {});

  struct source_fingerprint_t {
    std::string path;
    std::uintmax_t size = 0;
    std::intmax_t mtime = 0;

    bool operator==(const source_fingerprint_t &) const = default;
  };

  struct sync_result_t {
    // The output is usable by the catalog only when client_path is non-empty.
    std::filesystem::path client_path;
    source_fingerprint_t fingerprint;
    bool converted = false;
    bool reused = false;
  };

  // The official Steam CDN is intentionally the only production source for a
  // missing/half-resolution portrait.  A byte-returning seam keeps network
  // behavior deterministic in component tests and leaves room for another
  // provider to be added later without exposing arbitrary URLs to callers.
  using remote_fetcher_t = std::function<std::optional<std::vector<std::uint8_t>>(const std::string &url)>;

  std::string remote_portrait_url(std::uint32_t app_id);
  std::filesystem::path remote_cache_path(const std::filesystem::path &appdata, std::uint32_t app_id);

  // Return the stable managed path used for Steam's client-compatible cover.
  std::filesystem::path cache_path(const std::filesystem::path &appdata, std::uint32_t app_id);

  // Convert a local provider image to a caller-owned managed PNG path. This
  // exposes the format-safe cache primitive to other local integrations while
  // keeping Steam-specific source selection and downloads in prepare().
  sync_result_t sync_to(const std::filesystem::path &source,
                        const std::filesystem::path &output);

  // Convert a Steam image to PNG in appdata/covers. This function never calls
  // an external command. A matching sidecar fingerprint reuses an existing
  // valid PNG; source changes cause an atomic replacement. Missing or invalid
  // sources return an empty client_path and leave user-owned covers alone.
  sync_result_t sync(std::uint32_t app_id, const std::filesystem::path &source,
                     const std::filesystem::path &appdata);

  // Prepare discovered records for catalog reconciliation. The source path
  // remains in artwork_path, while successful conversion is exposed through
  // artwork_client_path. The source artwork_format is intentionally retained
  // for diagnostics and UI metadata.
  void prepare(std::vector<game_t> &games, const std::filesystem::path &appdata,
               remote_fetcher_t fetcher = {});

}  // namespace platf::steam::artwork
