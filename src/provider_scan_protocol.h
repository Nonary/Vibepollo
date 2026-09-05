/**
 * @file src/provider_scan_protocol.h
 * @brief Bounded protocol for unprivileged Linux provider discovery.
 */
#pragma once

#include "lutris_integration.h"
#include "steam_integration.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace platf::provider_scan {
  inline constexpr std::string_view schema_name = "vibeshine.provider-catalog";
  inline constexpr unsigned int schema_version = 1;
  inline constexpr std::size_t max_games = 4096;
  inline constexpr std::size_t max_payload_bytes = 16U * 1024U * 1024U;
  inline constexpr auto command_timeout = std::chrono::seconds {15};

  struct steam_catalog_t {
    bool available = false;
    std::vector<steam::game_t> games;
  };

  struct lutris_catalog_t {
    bool database_available = false;
    bool executable_available = false;
    std::vector<lutris::game_t> games;
  };

  // The wire catalog intentionally excludes every user-derived filesystem
  // path and Steam direct-launch command field. Only an opaque artwork revision
  // crosses in the catalog; separate ID-only requests return bounded PNG bytes. The machine
  // host must never open or interpolate desktop-user-controlled provider
  // content while holding CAP_SYS_ADMIN.
  std::optional<std::string> encode_steam_catalog(const steam_catalog_t &catalog);
  std::optional<std::string> encode_lutris_catalog(const lutris_catalog_t &catalog);
  std::optional<steam_catalog_t> decode_steam_catalog(std::string_view payload);
  std::optional<lutris_catalog_t> decode_lutris_catalog(std::string_view payload);

#if defined(__linux__)
  namespace detail {
    struct capture_limits_t {
      std::chrono::milliseconds timeout;
      std::size_t maximum_bytes;
    };

    // Exposed for focused tests. Production callers use the fixed wrapper
    // below, never a caller-controlled executable path.
    std::optional<std::string> capture_command(
      const std::filesystem::path &executable,
      std::string_view verb,
      capture_limits_t limits
    );
  }  // namespace detail

  std::optional<steam_catalog_t> scan_steam_session();
  std::optional<lutris_catalog_t> scan_lutris_session();
#endif
}  // namespace platf::provider_scan
