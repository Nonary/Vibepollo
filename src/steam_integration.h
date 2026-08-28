/**
 * @file src/steam_integration.h
 * @brief Local Steam library discovery and launch helpers.
 *
 * Steam is intentionally treated as a provider: this module does not own the
 * applications catalog or configuration.  Callers can reconcile the returned
 * records with their other providers using stable_id.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace platf::steam {

  struct vdf_node {
    std::string value;
    std::vector<std::pair<std::string, vdf_node>> children;

    const vdf_node *find(const std::string &key) const;
  };

  // Parse Valve's quoted-key/quoted-value format. Comments and escaped quotes
  // are accepted; malformed trailing input is ignored after valid entries.
  vdf_node parse_vdf(const std::string &contents);

  struct game_t {
    std::uint32_t app_id = 0;
    std::string stable_id;  // "steam:<appid>"
    std::string name;
    std::filesystem::path install_dir;
    std::filesystem::path library_path;
    std::filesystem::path icon_path;
    std::filesystem::path header_path;
    // Best available local artwork source. Steam commonly stores this as a
    // JPEG/WebP; callers must inspect artwork_format before using it as a
    // client catalog image (it is not converted by this provider).
    std::filesystem::path portrait_path;
    // Alias used by catalog/API consumers that call portrait art "box art".
    std::filesystem::path boxart_path;
    std::filesystem::path artwork_path;
    // Managed PNG generated from artwork_path for catalog clients that do
    // not accept Steam's native JPEG/WebP files.
    std::filesystem::path artwork_client_path;
    std::string artwork_format;
    std::string app_type;
    // Resolved Steam launch metadata used to start the game without routing
    // through an already-running Steam client's immutable environment.
    std::filesystem::path launch_executable;
    std::filesystem::path launch_working_dir;
    std::filesystem::path proton_path;
    std::filesystem::path compatdata_path;
    std::string launch_arguments;
    std::string launch_options;
    std::string launch_os;
    std::uint32_t state_flags = 0;
    std::uint64_t last_updated = 0;
  };

  // Discover installed games from the supplied Steam roots. A root may be a
  // Steam installation directory or a steamapps directory. Duplicate app IDs
  // are collapsed deterministically. If roots is empty, common OS locations
  // are searched.
  std::vector<game_t> discover(const std::vector<std::filesystem::path> &roots = {});
  std::vector<std::filesystem::path> default_library_roots();

  // Return a URI/argv-safe launch target after validating the app ID.
  std::string launch_uri(std::uint32_t app_id);
  std::string launch_command(std::uint32_t app_id);
  // Build a direct Linux launch that preserves Steam's user launch options
  // while placing Vibepollo's game-process wrapper at %command%. Falls back
  // to the Steam broker when local metadata is incomplete.
  std::string launch_command(const game_t &game);
  bool launch(std::uint32_t app_id);

}  // namespace platf::steam
