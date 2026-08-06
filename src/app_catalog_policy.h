/**
 * @file src/app_catalog_policy.h
 * @brief Pure application artwork and stable-ID catalog policy.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <vector>

namespace proc::catalog {
  using byte_buffer_t = std::vector<std::uint8_t>;
  using image_reader_t = std::function<std::optional<byte_buffer_t>(const std::string &)>;

  bool has_png_signature(std::span<const std::uint8_t> bytes);
  std::string asset_path(const std::string &assets_root, const std::string &relative_path);
  std::string validate_image_path(
    std::string image_path,
    const std::string &assets_root,
    const std::string &default_image,
    const image_reader_t &read_image);

  std::tuple<std::string, std::string> calculate_ids(
    const std::string &app_name,
    const std::string &app_uuid,
    const std::string &legacy_image_identity,
    int index);
  std::tuple<std::string, std::string> calculate_versioned_ids(
    const std::string &app_uuid,
    const std::string &cover_fingerprint,
    int index);

  struct alias_state_t {
    std::string current_id;
    std::string cover_fingerprint;
    std::set<std::string> aliases;
  };

  struct app_identity_t {
    std::string name;
    std::string uuid;
    std::string legacy_image_identity;
    std::string art_version;
    std::string id;
    std::vector<std::string> aliases;
  };

  void assign_compatible_id(
    app_identity_t &app,
    int index,
    std::set<std::string> &occupied_ids,
    std::map<std::string, alias_state_t> &persisted,
    std::set<std::string> &active_uuids,
    bool &state_changed);
  void prune_and_filter_aliases(
    std::vector<app_identity_t> &apps,
    std::map<std::string, alias_state_t> &persisted,
    const std::set<std::string> &active_uuids,
    bool &state_changed);
  std::optional<std::size_t> resolve_app(
    const std::vector<app_identity_t> &apps,
    std::string app_id,
    const std::string &app_uuid = {});
}  // namespace proc::catalog
