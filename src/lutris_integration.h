/** @file src/lutris_integration.h
 * Local Lutris catalog discovery and launch helpers.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace platf::lutris {
  struct game_t {
    std::int64_t id = 0;
    std::string stable_id;  // lutris:<id>, or steam:<appid> for Steam-backed records
    std::string name;
    std::string slug;
    std::string runner;
    std::string platform;
    std::filesystem::path directory;
    std::string config_path;
    std::string service;
    std::string service_id;
    std::filesystem::path image_path;
    std::int64_t last_played = 0;
    double playtime_seconds = 0.0;

    [[nodiscard]] bool steam_backed() const;
  };

  std::filesystem::path default_database_path();
  std::vector<game_t> discover(const std::filesystem::path &database = {});
  std::uint64_t source_fingerprint(const std::vector<game_t> &games);

  std::string launch_uri(std::int64_t id);
  bool launch(std::int64_t id);
  bool executable_available();
}  // namespace platf::lutris
