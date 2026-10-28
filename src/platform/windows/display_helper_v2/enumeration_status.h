#pragma once

#include <algorithm>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/uuid/name_generator_sha1.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace display_helper::v2::enumeration_status {
  inline bool is_complete_path_mapping(
    const bool monitor_path_observed,
    const bool device_id_observed,
    const bool display_name_observed
  ) {
    return monitor_path_observed && device_id_observed && display_name_observed;
  }

  inline bool is_complete_observation(
    const bool query_available,
    const bool query_has_available_paths,
    const bool path_mapping_incomplete,
    const bool devices_empty
  ) {
    return query_available && !path_mapping_incomplete && !(query_has_available_paths && devices_empty);
  }

  inline bool all_available_path_ids_were_enumerated(
    const std::vector<std::string> &available_path_ids,
    const std::vector<std::string> &enumerated_device_ids
  ) {
    return std::all_of(available_path_ids.begin(), available_path_ids.end(), [&](const auto &path_id) {
      return !path_id.empty() && std::any_of(
                                   enumerated_device_ids.begin(),
                                   enumerated_device_ids.end(),
                                   [&](const auto &device_id) {
                                     return boost::iequals(path_id, device_id);
                                   }
                                 );
    });
  }

  inline std::string hashed_device_id(const void *data, const std::size_t size) {
    if (data == nullptr || size == 0) {
      return {};
    }

    static constexpr boost::uuids::uuid namespace_id {};
    const auto id = boost::uuids::name_generator_sha1 {namespace_id}(data, size);
    return "{" + boost::uuids::to_string(id) + "}";
  }

  inline std::string path_fallback_device_id(const std::u16string_view monitor_path) {
    if (monitor_path.empty()) {
      return {};
    }

    // Keep in sync with libdisplaydevice::WinApiLayer::getDeviceId(), which
    // hashes the UTF-16 monitor path when SetupAPI identity lookup fails.
    return hashed_device_id(monitor_path.data(), monitor_path.size() * sizeof(char16_t));
  }

  inline bool is_path_fallback_device_id(
    const std::u16string_view monitor_path,
    const std::string_view device_id
  ) {
    return !device_id.empty() && device_id == path_fallback_device_id(monitor_path);
  }
}  // namespace display_helper::v2::enumeration_status
