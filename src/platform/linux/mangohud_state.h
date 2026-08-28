/**
 * @file src/platform/linux/mangohud_state.h
 * @brief Runtime handoff used by the Steam last-mile MangoHud wrapper.
 */
#pragma once

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

#include <unistd.h>

namespace platf::mangohud {

  inline std::filesystem::path state_directory() {
    if (const char *override_dir = std::getenv("VIBEPOLLO_MANGOHUD_STATE_DIR");
        override_dir && *override_dir) {
      return override_dir;
    }
    if (const char *runtime_dir = std::getenv("XDG_RUNTIME_DIR"); runtime_dir && *runtime_dir) {
      return std::filesystem::path(runtime_dir) / "vibepollo" / "mangohud";
    }
    if (const char *config_home = std::getenv("XDG_CONFIG_HOME"); config_home && *config_home) {
      return std::filesystem::path(config_home) / "vibepollo" / "mangohud-runtime";
    }
    if (const char *home = std::getenv("HOME"); home && *home) {
      return std::filesystem::path(home) / ".config" / "vibepollo" / "mangohud-runtime";
    }
    return {};
  }

  inline bool valid_steam_app_id(std::string_view app_id) {
    if (app_id.empty()) {
      return false;
    }
    for (const char ch : app_id) {
      if (ch < '0' || ch > '9') {
        return false;
      }
    }
    return app_id != "0";
  }

  inline std::filesystem::path state_path(std::string_view app_id) {
    const auto directory = state_directory();
    if (directory.empty() || !valid_steam_app_id(app_id)) {
      return {};
    }
    return directory / (std::string(app_id) + ".state");
  }

  inline std::string serialize_state(
    std::string_view limit,
    std::string_view preset,
    bool always_show_graph,
    std::chrono::system_clock::time_point expires_at
  ) {
    const bool standard_preset = preset == "1" || preset == "2" || preset == "3" || preset == "4";
    const auto expires = std::chrono::duration_cast<std::chrono::seconds>(
      expires_at.time_since_epoch()
    ).count();
    return "version=1\nlimit=" + std::string(limit) +
           "\npreset=" + (standard_preset ? std::string(preset) : "custom") +
           "\nalways_show_graph=" + (always_show_graph ? "1" : "0") +
           "\nowner_pid=" + std::to_string(static_cast<unsigned long>(getpid())) +
           "\nexpires=" + std::to_string(expires) + "\n";
  }

  inline std::filesystem::path write_state(
    std::string_view app_id,
    std::string_view limit,
    std::string_view preset,
    bool always_show_graph
  ) {
    const auto path = state_path(app_id);
    if (path.empty()) {
      return {};
    }

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
      return {};
    }
    std::filesystem::permissions(
      path.parent_path(),
      std::filesystem::perms::owner_all,
      std::filesystem::perm_options::replace,
      ec
    );
    if (ec) {
      return {};
    }

    auto temporary = path;
    temporary += ".tmp-" + std::to_string(static_cast<unsigned long>(getpid()));
    {
      std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
      if (!output) {
        return {};
      }
      output << serialize_state(
        limit,
        preset,
        always_show_graph,
        std::chrono::system_clock::now() + std::chrono::hours(1)
      );
      if (!output) {
        output.close();
        std::filesystem::remove(temporary, ec);
        return {};
      }
    }
    std::filesystem::permissions(
      temporary,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
      std::filesystem::perm_options::replace,
      ec
    );
    if (ec) {
      std::filesystem::remove(temporary, ec);
      return {};
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
      std::filesystem::remove(temporary, ec);
      return {};
    }
    return path;
  }

  inline void remove_state(std::string_view app_id) {
    const auto path = state_path(app_id);
    if (path.empty()) {
      return;
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }

}  // namespace platf::mangohud
