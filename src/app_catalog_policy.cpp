/**
 * @file src/app_catalog_policy.cpp
 * @brief Pure application artwork and stable-ID catalog policy.
 */

#include "app_catalog_policy.h"

#include <algorithm>
#include <array>
#include <boost/crc.hpp>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <sstream>

namespace proc::catalog {
  namespace {
    std::string numeric_id(const std::vector<std::string> &parts, std::optional<int> index = std::nullopt) {
      boost::crc_32_type crc;
      for (const auto &part : parts) {
        crc.process_bytes(part.data(), part.size());
      }
      if (index) {
        const auto suffix = std::to_string(*index);
        crc.process_bytes(suffix.data(), suffix.size());
      }
      return std::to_string(std::abs(static_cast<std::int32_t>(crc.checksum())));
    }

    void remember_alias(alias_state_t &state, const std::string &alias) {
      if (!alias.empty() && alias != state.current_id) {
        state.aliases.insert(alias);
      }
    }

    std::string lowercase_extension(const std::string &path) {
      auto extension = std::filesystem::path(path).extension().string();
      std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
      });
      return extension;
    }
  }  // namespace

  bool has_png_signature(std::span<const std::uint8_t> bytes) {
    static constexpr std::array<std::uint8_t, 8> signature {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
    return bytes.size() >= signature.size() && std::equal(signature.begin(), signature.end(), bytes.begin());
  }

  std::string asset_path(const std::string &assets_root, const std::string &relative_path) {
    return (std::filesystem::path(assets_root) / relative_path).string();
  }

  std::string validate_image_path(
    std::string image_path,
    const std::string &assets_root,
    const std::string &default_image,
    const image_reader_t &read_image) {
    if (image_path.empty() || lowercase_extension(image_path) != ".png") {
      return default_image;
    }

    const auto asset_candidate = asset_path(assets_root, image_path);
    if (const auto bytes = read_image(asset_candidate)) {
      return has_png_signature(*bytes) ? asset_candidate : default_image;
    }
    if (image_path == "./assets/steam.png") {
      return asset_path(assets_root, "steam.png");
    }
    const auto bytes = read_image(image_path);
    return bytes && has_png_signature(*bytes) ? image_path : default_image;
  }

  std::tuple<std::string, std::string> calculate_ids(
    const std::string &app_name,
    const std::string &app_uuid,
    const std::string &legacy_image_identity,
    int index) {
    std::vector<std::string> parts;
    if (!app_uuid.empty()) {
      parts.push_back(app_uuid);
    } else {
      parts.push_back(app_name);
      if (!legacy_image_identity.empty()) {
        parts.push_back(legacy_image_identity);
      }
    }
    return {numeric_id(parts), numeric_id(parts, index)};
  }

  std::tuple<std::string, std::string> calculate_versioned_ids(
    const std::string &app_uuid,
    const std::string &cover_fingerprint,
    int index) {
    const std::vector<std::string> parts {app_uuid, "\n", cover_fingerprint};
    return {numeric_id(parts), numeric_id(parts, index)};
  }

  void assign_compatible_id(
    app_identity_t &app,
    int index,
    std::set<std::string> &occupied_ids,
    std::map<std::string, alias_state_t> &persisted,
    std::set<std::string> &active_uuids,
    bool &state_changed) {
    const auto legacy_ids = calculate_ids(app.name, app.uuid, app.legacy_image_identity, index);
    if (app.uuid.empty()) {
      app.id = occupied_ids.contains(std::get<0>(legacy_ids)) ? std::get<1>(legacy_ids) : std::get<0>(legacy_ids);
      occupied_ids.insert(app.id);
      return;
    }

    active_uuids.insert(app.uuid);
    auto [iter, inserted] = persisted.try_emplace(
      app.uuid,
      alias_state_t {std::get<0>(legacy_ids), app.art_version, {}});
    auto &state = iter->second;
    state_changed = state_changed || inserted;
    if (state.current_id.empty()) {
      state.current_id = std::get<0>(legacy_ids);
      state_changed = true;
    }
    if (state.cover_fingerprint.empty()) {
      state.cover_fingerprint = app.art_version;
      state_changed = true;
    }
    if (state.cover_fingerprint != app.art_version) {
      const auto previous = state.current_id;
      const auto versioned = calculate_versioned_ids(app.uuid, app.art_version, index);
      state.current_id = occupied_ids.contains(std::get<0>(versioned)) ? std::get<1>(versioned) : std::get<0>(versioned);
      state.cover_fingerprint = app.art_version;
      remember_alias(state, previous);
      remember_alias(state, std::get<0>(legacy_ids));
      state_changed = true;
    }
    if (occupied_ids.contains(state.current_id)) {
      remember_alias(state, state.current_id);
      const auto versioned = calculate_versioned_ids(app.uuid, app.art_version, index);
      state.current_id = occupied_ids.contains(std::get<1>(versioned)) ? std::get<1>(legacy_ids) : std::get<1>(versioned);
      state_changed = true;
    }
    app.id = state.current_id;
    app.aliases.assign(state.aliases.begin(), state.aliases.end());
    occupied_ids.insert(app.id);
  }

  void prune_and_filter_aliases(
    std::vector<app_identity_t> &apps,
    std::map<std::string, alias_state_t> &persisted,
    const std::set<std::string> &active_uuids,
    bool &state_changed) {
    for (auto it = persisted.begin(); it != persisted.end();) {
      if (!active_uuids.contains(it->first)) {
        it = persisted.erase(it);
        state_changed = true;
      } else {
        ++it;
      }
    }

    std::set<std::string> current_ids;
    std::map<std::string, int> alias_counts;
    for (const auto &app : apps) {
      current_ids.insert(app.id);
      for (const auto &alias : app.aliases) {
        ++alias_counts[alias];
      }
    }
    for (auto &app : apps) {
      if (app.uuid.empty()) {
        continue;
      }
      std::vector<std::string> filtered;
      std::set<std::string> seen;
      for (const auto &alias : app.aliases) {
        if (alias.empty() || alias == app.id || !seen.insert(alias).second || current_ids.contains(alias) || alias_counts[alias] > 1) {
          state_changed = true;
          continue;
        }
        filtered.push_back(alias);
      }
      app.aliases = std::move(filtered);
      auto &state = persisted[app.uuid];
      state.current_id = app.id;
      state.cover_fingerprint = app.art_version;
      state.aliases = {app.aliases.begin(), app.aliases.end()};
    }
  }

  std::optional<std::size_t> resolve_app(
    const std::vector<app_identity_t> &apps,
    std::string app_id,
    const std::string &app_uuid) {
    if (!app_uuid.empty()) {
      for (std::size_t i = 0; i < apps.size(); ++i) {
        if (apps[i].uuid == app_uuid) {
          return i;
        }
      }
    }
    app_id.erase(app_id.begin(), std::find_if(app_id.begin(), app_id.end(), [](unsigned char ch) { return !std::isspace(ch); }));
    app_id.erase(std::find_if(app_id.rbegin(), app_id.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), app_id.end());
    if (app_id.empty() || app_id == "0") {
      return std::nullopt;
    }
    for (std::size_t i = 0; i < apps.size(); ++i) {
      if (apps[i].id == app_id) {
        return i;
      }
    }
    std::optional<std::size_t> alias_match;
    for (std::size_t i = 0; i < apps.size(); ++i) {
      if (std::find(apps[i].aliases.begin(), apps[i].aliases.end(), app_id) == apps[i].aliases.end()) {
        continue;
      }
      if (alias_match) {
        return std::nullopt;
      }
      alias_match = i;
    }
    return alias_match;
  }
}  // namespace proc::catalog
