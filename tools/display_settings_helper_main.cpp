/**
 * @file tools/display_settings_helper_main.cpp
 * @brief Entry point for sunshine_display_helper.exe: dispatches to the v2 engine
 *        (default) or the legacy engine based on --engine= / persisted state.
 *
 * One exe hosts both engines so the boot-restore scheduled task, the installer
 * payload, and Sunshine's helper launch path never depend on which engine is
 * selected. Boot --restore tasks read the persisted engine choice from
 * vibeshine_state.json so a restore registered under one engine completes
 * correctly after the user toggles the other.
 */
#ifdef _WIN32

  #include <cstring>
  #include <filesystem>
  #include <fstream>
  #include <optional>
  #include <string>

  #include <nlohmann/json.hpp>

  #include "tools/display_helper_paths.h"

int run_legacy_helper(int argc, char *argv[]);
int run_v2_helper(int argc, char *argv[]);

namespace {
  std::optional<std::string> engine_from_state_files() {
    for (const auto &root : display_helper_paths::snapshot_search_roots()) {
      const auto state_file = root / L"vibeshine_state.json";
      std::error_code ec;
      if (!std::filesystem::exists(state_file, ec) || ec) {
        continue;
      }
      try {
        std::ifstream file(state_file, std::ios::binary);
        if (!file) {
          continue;
        }
        auto j = nlohmann::json::parse(file, nullptr, false);
        if (!j.is_object() || !j.contains("root") || !j["root"].is_object()) {
          continue;
        }
        const auto &state_root = j["root"];
        if (state_root.contains("display_helper_engine") && state_root["display_helper_engine"].is_string()) {
          auto engine = state_root["display_helper_engine"].get<std::string>();
          if (!engine.empty()) {
            return engine;
          }
        }
      } catch (...) {
      }
    }
    return std::nullopt;
  }
}  // namespace

int main(int argc, char *argv[]) {
  std::string engine;
  constexpr const char *kEnginePrefix = "--engine=";
  for (int i = 1; i < argc; ++i) {
    if (std::strncmp(argv[i], kEnginePrefix, std::strlen(kEnginePrefix)) == 0) {
      engine = argv[i] + std::strlen(kEnginePrefix);
    }
  }

  if (engine.empty()) {
    engine = engine_from_state_files().value_or("v2");
  }

  if (engine == "legacy") {
    return run_legacy_helper(argc, argv);
  }
  return run_v2_helper(argc, argv);
}

#else
int main() {
  return 0;
}
#endif
