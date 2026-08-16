/**
 * @file src/platform/windows/audio_visibility_recovery.h
 * @brief Pure decisions for crash-safe Steam audio endpoint visibility transitions.
 */
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace platf::audio::visibility_recovery {
  /**
   * @brief Upper bound for a readable marker file.
   *
   * Anything larger is not a marker this code wrote; its content is ignored,
   * although the file's presence still records an unfinished transition.
   */
  constexpr std::size_t max_marker_size = 2048;

  /** @brief Serialize the endpoint id recorded before hiding the endpoint. */
  inline std::string make_marker(std::string_view device_id_utf8) {
    std::string marker {device_id_utf8};
    marker.push_back('\n');
    return marker;
  }

  /**
   * @brief Parse a marker file back into the recorded endpoint id.
   * @return The recorded id, or std::nullopt when the content is unusable.
   *
   * The id is only a hint for the healing pass; a corrupt marker still means
   * a transition was interrupted and healing must fall back to name matching.
   */
  inline std::optional<std::string> parse_marker(std::string_view content) {
    if (content.empty() || content.size() > max_marker_size) {
      return std::nullopt;
    }

    auto line_end = content.find('\n');
    auto line = content.substr(0, line_end == std::string_view::npos ? content.size() : line_end);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
      line.remove_suffix(1);
    }
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
      line.remove_prefix(1);
    }

    if (line.empty()) {
      return std::nullopt;
    }
    return std::string {line};
  }

  enum class heal_action_e {
    none,  ///< No marker: no transition was interrupted
    retain_marker,  ///< Endpoint missing: keep the marker so a later pass can heal
    clear_marker,  ///< Endpoint already visible: the marker is stale
    reshow_endpoint,  ///< Endpoint present but hidden: restore its visibility
  };

  /** @brief Decide how to heal an interrupted endpoint visibility transition. */
  constexpr heal_action_e classify_heal(
    const bool marker_present,
    const bool endpoint_found,
    const bool endpoint_active
  ) {
    if (!marker_present) {
      return heal_action_e::none;
    }
    if (!endpoint_found) {
      return heal_action_e::retain_marker;
    }
    if (endpoint_active) {
      return heal_action_e::clear_marker;
    }
    return heal_action_e::reshow_endpoint;
  }
}  // namespace platf::audio::visibility_recovery
