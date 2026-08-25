/** @file src/steam_auto_sync_policy.cpp */
#include "steam_auto_sync_policy.h"

#include <filesystem>
#include <string_view>

namespace {
  constexpr std::string_view separator {"\x1f", 1};

  void mix(std::uint64_t &value, std::string_view text) {
    // FNV-1a gives deterministic, cheap mixing while retaining path and
    // metadata changes for the polling watcher.
    for (const auto c : text) {
      value ^= static_cast<unsigned char>(c);
      value *= 1099511628211ULL;
    }
  }

  template<typename T>
  void mix_number(std::uint64_t &value, T number) {
    mix(value, std::to_string(number));
    mix(value, separator);
  }

  void mix_path(std::uint64_t &value, const std::filesystem::path &path) {
    mix(value, path.generic_string());
    mix(value, separator);
    if (path.empty()) {
      return;
    }
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (!ec) {
      mix_number(value, size);
    } else {
      mix(value, "missing");
    }
    const auto timestamp = std::filesystem::last_write_time(path, ec);
    if (!ec) {
      mix_number(value, timestamp.time_since_epoch().count());
    } else {
      mix(value, "unstatable");
    }
  }
}

namespace platf::steam::autosync {
  std::uint64_t source_fingerprint(const std::vector<game_t> &games) {
    std::uint64_t result = 1469598103934665603ULL;
    mix_number(result, games.size());
    for (const auto &game : games) {
      mix_number(result, game.app_id);
      mix(result, game.stable_id);
      mix(result, separator);
      mix(result, game.name);
      mix(result, separator);
      mix_path(result, game.install_dir);
      mix_path(result, game.library_path);
      mix_path(result, game.icon_path);
      mix_path(result, game.header_path);
      mix_path(result, game.portrait_path);
      mix_path(result, game.boxart_path);
      mix_path(result, game.artwork_path);
      mix(result, game.artwork_format);
      mix(result, separator);
      mix_number(result, game.state_flags);
      mix_number(result, game.last_updated);
      mix(result, game.app_type);
      mix(result, separator);
    }
    return result;
  }
}  // namespace platf::steam::autosync
