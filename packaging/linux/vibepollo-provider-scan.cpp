/**
 * Unprivileged Steam/Lutris catalog scanner.
 *
 * This program is only executed by the session broker after it has entered
 * the controller-selected desktop user's identity and environment.
 */
#include "src/lutris_integration.h"
#include "src/provider_scan_protocol.h"
#include "src/steam_integration.h"

#include <cstdlib>
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
  if (argc != 2) return 2;
  // Defense in depth: provider discovery is always relative to the dropped
  // user's HOME/XDG environment, never the machine-host mode switch.
  unsetenv("VIBEPOLLO_MACHINE_HOST");
  const std::string_view provider {argv[1]};
  if (provider == "steam") return scan_steam();
  if (provider == "lutris") return scan_lutris();
  return 2;
}
