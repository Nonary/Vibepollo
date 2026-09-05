/**
 * Unprivileged Steam/Lutris catalog scanner.
 *
 * This program is only executed by the session broker after it has entered
 * the controller-selected desktop user's identity and environment.
 */
#include "src/lutris_integration.h"
#include "src/provider_scan_protocol.h"
#include "src/steam_integration.h"

#include "src/steam_artwork.h"

#include <charconv>
#include <cstdlib>
#include <sys/resource.h>
#include <iostream>
#include <string_view>

namespace {
  int scan_steam() {
    const auto roots = platf::steam::default_library_roots();
    const platf::provider_scan::steam_catalog_t catalog {
      .available = !roots.empty(),
      .games = roots.empty() ? std::vector<platf::steam::game_t> {} : platf::steam::discover_catalog(roots),
    };
    const auto payload = platf::provider_scan::encode_steam_catalog(catalog);
    if (!payload) return 1;
    std::cout << *payload;
    return std::cout ? 0 : 1;
  }

  int scan_lutris() {
    const auto database = platf::lutris::default_database_path();
    const platf::provider_scan::lutris_catalog_t catalog {
      .database_available = !database.empty(),
      .executable_available = platf::lutris::executable_available(),
      .games = database.empty() ? std::vector<platf::lutris::game_t> {} : platf::lutris::discover(database),
    };
    const auto payload = platf::provider_scan::encode_lutris_catalog(catalog);
    if (!payload) return 1;
    std::cout << *payload;
    return std::cout ? 0 : 1;
  }
}  // namespace

int main(int argc, char **argv) {
  if (argc != 2 && argc != 3) return 2;
  // Defense in depth: provider discovery is always relative to the dropped
  // user's HOME/XDG environment, never the machine-host mode switch.
  unsetenv("VIBEPOLLO_MACHINE_HOST");
  const std::string_view provider {argv[1]};
  if (argc == 3) {
    // Bound image decoder memory and CPU in this capability-free worker.
    const rlimit memory {512U * 1024U * 1024U, 512U * 1024U * 1024U};
    const rlimit cpu {10, 10};
    if (setrlimit(RLIMIT_AS, &memory) || setrlimit(RLIMIT_CPU, &cpu)) return 1;
    std::uint64_t id = 0;
    const std::string_view value {argv[2]};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), id);
    if (parsed.ec != std::errc {} || parsed.ptr != value.data() + value.size() || id == 0) return 2;
    std::optional<std::vector<std::uint8_t>> png;
    if (provider == "steam-artwork" && id <= UINT32_MAX) {
      for (const auto &game : platf::steam::discover_catalog(platf::steam::default_library_roots())) {
        if (game.app_id != id) continue;
        for (const auto &path : {game.artwork_path, game.boxart_path, game.icon_path}) {
          png = platf::steam::artwork::export_png(path);
          if (png) break;
        }
        break;
      }
    } else if (provider == "lutris-artwork" && id <= INT64_MAX) {
      const auto database = platf::lutris::default_database_path();
      if (!database.empty()) for (const auto &game : platf::lutris::discover(database)) {
        if (game.id != static_cast<std::int64_t>(id)) continue;
        png = platf::steam::artwork::export_png(game.artwork_path);
        if (!png) png = platf::steam::artwork::export_png(game.icon_path);
        break;
      }
    } else return 2;
    if (!png) return 1;
    std::cout.write(reinterpret_cast<const char *>(png->data()), png->size());
    return std::cout ? 0 : 1;
  }
  if (provider == "steam") return scan_steam();
  if (provider == "lutris") return scan_lutris();
  return 2;
}
