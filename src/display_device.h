/**
 * @file src/display_device.h
 * @brief Declarations for display device handling.
 */
#pragma once

// standard includes
#include <filesystem>
#include <memory>

// lib includes
#include <display_device/types.h>

// forward declarations
namespace platf {
  class deinit_t;
}

namespace config {
  struct video_t;
}

namespace rtsp_stream {
  struct launch_session_t;
}

namespace display_device {
  // Old in-process display API removed. Only parsing helpers and light mapping remain here.

  /**
   * @brief Map configured output name to a platform display identifier used by capture backends.
   *
   * On Windows, if `output_name` is a device GUID from libdisplaydevice, this returns the
   * corresponding `\\\\.\\DISPLAY#` string. Otherwise returns `output_name` unchanged.
   */
  [[nodiscard]] std::string map_output_name(const std::string &output_name);

  /**
   * @brief Check whether a configured output name matches a known physical display.
   */
  [[nodiscard]] bool output_exists(const std::string &output_name);

  /**
   * @brief Check whether a configured output is currently active/attached to the desktop.
   *
   * On Windows this indicates whether the output has a logical `\\\\.\\DISPLAY#` name, which
   * is required by DXGI-based capture backends.
   */
  [[nodiscard]] bool output_is_active(const std::string &output_name);

  /**
   * @brief A tag structure indicating that configuration parsing has failed.
   */
  struct failed_to_parse_tag_t {};

  /**
   * @brief A tag structure indicating that configuration is disabled.
   */
  struct configuration_disabled_tag_t {};

  /**
   * @brief Parse the user configuration and the session information.
   * @param video_config User's video related configuration.
   * @param session Session information.
   * @return Parsed single display configuration or
   *         a tag indicating that the parsing has failed or
   *         a tag indicating that the user does not want to perform any configuration.
   *
   * @examples
   * const std::shared_ptr<rtsp_stream::launch_session_t> launch_session;
   * const config::video_t &video_config { config::video };
   *
   * const auto config { parse_configuration(video_config, *launch_session) };
   * if (const auto *parsed_config { std::get_if<SingleDisplayConfiguration>(&result) }; parsed_config) {
   *    configure_display(*config);
   * }
   * @examples_end
   */
  [[nodiscard]] std::variant<failed_to_parse_tag_t, configuration_disabled_tag_t, SingleDisplayConfiguration> parse_configuration(const config::video_t &video_config, const rtsp_stream::launch_session_t &session);

  /**
   * @brief Check if a display refresh-rate override is active for the session.
   *
   * This returns true when a manual refresh rate is configured or when a
   * refresh-rate remapping entry applies to the session's requested FPS.
   */
  [[nodiscard]] bool refresh_rate_override_active(const config::video_t &video_config, const rtsp_stream::launch_session_t &session);
}  // namespace display_device
