/**
 * @file src/display_device.cpp
 * @brief Definitions for display device handling.
 */
// header include
#include "display_device.h"

// lib includes
#include <boost/algorithm/string.hpp>
#include <display_device/json.h>
#include <regex>

// local includes
#include "platform/common.h"
#include "rtsp.h"

// No direct helper calls here; this unit now focuses on parsing and small conveniences.
#ifdef _WIN32
  #include <display_device/windows/win_api_layer.h>
  #include <display_device/windows/win_display_device.h>
#endif

namespace display_device {
  namespace {

    /**
     * @brief Convert string to unsigned int.
     * @note For random reason there is std::stoi, but not std::stou...
     * @param value String to be converted
     * @return Parsed unsigned integer.
     */
    unsigned int stou(const std::string &value) {
      unsigned long result {std::stoul(value)};
      if (result > std::numeric_limits<unsigned int>::max()) {
        throw std::out_of_range("stou");
      }
      return result;
    }

    /**
     * @brief Parse resolution value from the string.
     * @param input String to be parsed.
     * @param output Reference to output variable to fill in.
     * @returns True on successful parsing (empty string allowed), false otherwise.
     *
     * @examples
     * std::optional<Resolution> resolution;
     * if (parse_resolution_string("1920x1080", resolution)) {
     *   if (resolution) {
     *     BOOST_LOG(info) << "Value was specified";
     *   }
     *   else {
     *     BOOST_LOG(info) << "Value was empty";
     *   }
     * }
     * @examples_end
     */
    bool parse_resolution_string(const std::string &input, std::optional<Resolution> &output) {
      const std::string trimmed_input {boost::algorithm::trim_copy(input)};
      const std::regex resolution_regex {R"(^(\d+)x(\d+)$)"};

      if (std::smatch match; std::regex_match(trimmed_input, match, resolution_regex)) {
        try {
          output = Resolution {
            stou(match[1].str()),
            stou(match[2].str())
          };
          return true;
        } catch (const std::out_of_range &) {
          BOOST_LOG(error) << "Failed to parse resolution string " << trimmed_input << " (number out of range).";
        } catch (const std::exception &err) {
          BOOST_LOG(error) << "Failed to parse resolution string " << trimmed_input << ":\n"
                           << err.what();
        }
      } else {
        if (trimmed_input.empty()) {
          output = std::nullopt;
          return true;
        }

        BOOST_LOG(error) << "Failed to parse resolution string " << trimmed_input << R"(. It must match a "1920x1080" pattern!)";
      }

      return false;
    }

    /**
     * @brief Parse refresh rate value from the string.
     * @param input String to be parsed.
     * @param output Reference to output variable to fill in.
     * @param allow_decimal_point Specify whether the decimal point is allowed or not.
     * @returns True on successful parsing (empty string allowed), false otherwise.
     *
     * @examples
     * std::optional<FloatingPoint> refresh_rate;
     * if (parse_refresh_rate_string("59.95", refresh_rate)) {
     *   if (refresh_rate) {
     *     BOOST_LOG(info) << "Value was specified";
     *   }
     *   else {
     *     BOOST_LOG(info) << "Value was empty";
     *   }
     * }
     * @examples_end
     */
    bool parse_refresh_rate_string(const std::string &input, std::optional<FloatingPoint> &output, const bool allow_decimal_point = true) {
      static const auto is_zero {[](const auto &character) {
        return character == '0';
      }};
      const std::string trimmed_input {boost::algorithm::trim_copy(input)};
      const std::regex refresh_rate_regex {allow_decimal_point ? R"(^(\d+)(?:\.(\d+))?$)" : R"(^(\d+)$)"};

      if (std::smatch match; std::regex_match(trimmed_input, match, refresh_rate_regex)) {
        try {
          // Here we are trimming zeros from the string to possibly reduce out of bounds case
          std::string trimmed_match_1 {boost::algorithm::trim_left_copy_if(match[1].str(), is_zero)};
          if (trimmed_match_1.empty()) {
            trimmed_match_1 = "0"s;  // Just in case ALL the string is full of zeros, we want to leave one
          }

          std::string trimmed_match_2;
          if (allow_decimal_point && match[2].matched) {
            trimmed_match_2 = boost::algorithm::trim_right_copy_if(match[2].str(), is_zero);
          }

          if (!trimmed_match_2.empty()) {
            // We have a decimal point and will have to split it into numerator and denominator.
            // For example:
            //   59.995:
            //     numerator = 59995
            //     denominator = 1000

            // We are essentially removing the decimal point here: 59.995 -> 59995
            const std::string numerator_str {trimmed_match_1 + trimmed_match_2};
            const auto numerator {stou(numerator_str)};

            // Here we are counting decimal places and calculating denominator: 10^decimal_places
            const auto denominator {static_cast<unsigned int>(std::pow(10, trimmed_match_2.size()))};

            output = Rational {numerator, denominator};
          } else {
            // We do not have a decimal point, just a valid number.
            // For example:
            //   60:
            //     numerator = 60
            //     denominator = 1
            output = Rational {stou(trimmed_match_1), 1};
          }
          return true;
        } catch (const std::out_of_range &) {
          BOOST_LOG(error) << "Failed to parse refresh rate string " << trimmed_input << " (number out of range).";
        } catch (const std::exception &err) {
          BOOST_LOG(error) << "Failed to parse refresh rate string " << trimmed_input << ":\n"
                           << err.what();
        }
      } else {
        if (trimmed_input.empty()) {
          output = std::nullopt;
          return true;
        }

        BOOST_LOG(error) << "Failed to parse refresh rate string " << trimmed_input << ". Must have a pattern of " << (allow_decimal_point ? R"("123" or "123.456")" : R"("123")") << "!";
      }

      return false;
    }

    /**
     * @brief Parse device preparation option from the user configuration and the session information.
     * @param video_config User's video related configuration.
     * @returns Parsed device preparation value we need to use.
     *          Empty optional if no preparation nor configuration shall take place.
     *
     * @examples
     * const config::video_t &video_config { config::video };
     * const auto device_prep_option = parse_device_prep_option(video_config);
     * @examples_end
     */
    std::optional<SingleDisplayConfiguration::DevicePreparation> parse_device_prep_option(const config::video_t &video_config) {
      using enum config::video_t::dd_t::config_option_e;
      using enum SingleDisplayConfiguration::DevicePreparation;

      switch (video_config.dd.configuration_option) {
        case verify_only:
          return VerifyOnly;
        case ensure_active:
          return EnsureActive;
        case ensure_primary:
          return EnsurePrimary;
        case ensure_only_display:
          return EnsureOnlyDisplay;
        case disabled:
          break;
      }

      return std::nullopt;
    }

    /**
     * @brief Parse resolution option from the user configuration and the session information.
     * @param video_config User's video related configuration.
     * @param session Session information.
     * @param config A reference to a display config object that will be modified on success.
     * @returns True on successful parsing, false otherwise.
     *
     * @examples
     * const std::shared_ptr<rtsp_stream::launch_session_t> launch_session;
     * const config::video_t &video_config { config::video };
     *
     * SingleDisplayConfiguration config;
     * const bool success = parse_resolution_option(video_config, *launch_session, config);
     * @examples_end
     */
    bool parse_resolution_option(const config::video_t &video_config, const rtsp_stream::launch_session_t &session, SingleDisplayConfiguration &config) {
      using resolution_option_e = config::video_t::dd_t::resolution_option_e;

      switch (video_config.dd.resolution_option) {
        case resolution_option_e::automatic:
          {
            if (session.width >= 0 && session.height >= 0) {
              config.m_resolution = Resolution {
                static_cast<unsigned int>(session.width),
                static_cast<unsigned int>(session.height)
              };
            } else {
              BOOST_LOG(error) << "Resolution provided by client session config is invalid: " << session.width << "x" << session.height;
              return false;
            }
            break;
          }
        case resolution_option_e::manual:
          {
            if (!parse_resolution_string(video_config.dd.manual_resolution, config.m_resolution)) {
              BOOST_LOG(error) << "Failed to parse manual resolution string!";
              return false;
            }

            if (!config.m_resolution) {
              BOOST_LOG(error) << "Manual resolution must be specified!";
              return false;
            }
            break;
          }
        case resolution_option_e::disabled:
          break;
      }

      return true;
    }

    /**
     * @brief Parse refresh rate option from the user configuration and the session information.
     * @param video_config User's video related configuration.
     * @param session Session information.
     * @param config A reference to a config object that will be modified on success.
     * @returns True on successful parsing, false otherwise.
     *
     * @examples
     * const std::shared_ptr<rtsp_stream::launch_session_t> launch_session;
     * const config::video_t &video_config { config::video };
     *
     * SingleDisplayConfiguration config;
     * const bool success = parse_refresh_rate_option(video_config, *launch_session, config);
     * @examples_end
     */
    bool parse_refresh_rate_option(const config::video_t &video_config, const rtsp_stream::launch_session_t &session, SingleDisplayConfiguration &config) {
      using refresh_rate_option_e = config::video_t::dd_t::refresh_rate_option_e;

      switch (video_config.dd.refresh_rate_option) {
        case refresh_rate_option_e::automatic:
          {
            const int target_fps = (session.framegen_refresh_rate && *session.framegen_refresh_rate > 0)
                                     ? *session.framegen_refresh_rate
                                     : session.fps;
            if (target_fps >= 0) {
              config.m_refresh_rate = Rational {static_cast<unsigned int>(target_fps), 1};
            } else {
              BOOST_LOG(error) << "FPS value provided by client session config is invalid: " << target_fps;
              return false;
            }
            break;
          }
        case refresh_rate_option_e::manual:
          {
            if (!parse_refresh_rate_string(video_config.dd.manual_refresh_rate, config.m_refresh_rate)) {
              BOOST_LOG(error) << "Failed to parse manual refresh rate string!";
              return false;
            }

            if (!config.m_refresh_rate) {
              BOOST_LOG(error) << "Manual refresh rate must be specified!";
              return false;
            }
            break;
          }
        case refresh_rate_option_e::prefer_highest:
          {
            // Hint to Windows to pick the highest available refresh rate for the mode.
            // Strategy: request an unrealistically high refresh rate (e.g. 10000 Hz),
            // and with SDC_ALLOW_CHANGES the OS will clamp to the closest supported value,
            // which for an oversized request resolves to the maximum available.
            config.m_refresh_rate = Rational {10000u, 1u};
            break;
          }
        case refresh_rate_option_e::disabled:
          break;
      }

      return true;
    }

    /**
     * @brief Parse HDR option from the user configuration and the session information.
     * @param video_config User's video related configuration.
     * @param session Session information.
     * @returns Parsed HDR state value we need to switch to.
     *          Empty optional if no action is required.
     *
     * @examples
     * const std::shared_ptr<rtsp_stream::launch_session_t> launch_session;
     * const config::video_t &video_config { config::video };
     * const auto hdr_option = parse_hdr_option(video_config, *launch_session);
     * @examples_end
     */
    std::optional<HdrState> parse_hdr_option(const config::video_t &video_config, const rtsp_stream::launch_session_t &session) {
      using hdr_option_e = config::video_t::dd_t::hdr_option_e;

      if (video_config.dd.wa.dummy_plug_hdr10) {
        return HdrState::Enabled;
      }

      switch (video_config.dd.hdr_option) {
        case hdr_option_e::automatic:
          return session.enable_hdr ? HdrState::Enabled : HdrState::Disabled;
        case hdr_option_e::disabled:
          break;
      }

      return std::nullopt;
    }

    /**
     * @brief Indicates which remapping fields and config structure shall be used.
     */
    enum class remapping_type_e {
      mixed,  ///! Both reseolution and refresh rate may be remapped
      resolution_only,  ///! Only resolution will be remapped
      refresh_rate_only  ///! Only refresh rate will be remapped
    };

    /**
     * @brief Determine the ramapping type from the user config.
     * @param video_config User's video related configuration.
     * @returns Enum value if remapping can be performed, null optional if remapping shall be skipped.
     */
    std::optional<remapping_type_e> determine_remapping_type(const config::video_t &video_config) {
      using dd_t = config::video_t::dd_t;
      const bool auto_resolution {video_config.dd.resolution_option == dd_t::resolution_option_e::automatic};
      const bool auto_refresh_rate {video_config.dd.refresh_rate_option == dd_t::refresh_rate_option_e::automatic};

      if (auto_resolution && auto_refresh_rate) {
        return remapping_type_e::mixed;
      }

      if (auto_resolution) {
        return remapping_type_e::resolution_only;
      }

      if (auto_refresh_rate) {
        return remapping_type_e::refresh_rate_only;
      }

      return std::nullopt;
    }

    /**
     * @brief Contains remapping data parsed from the string values.
     */
    struct parsed_remapping_entry_t {
      std::optional<Resolution> requested_resolution;
      std::optional<FloatingPoint> requested_fps;
      std::optional<Resolution> final_resolution;
      std::optional<FloatingPoint> final_refresh_rate;
    };

    /**
     * @brief Check if resolution is to be mapped based on remmaping type.
     * @param type Remapping type to check.
     * @returns True if resolution is to be mapped, false otherwise.
     */
    bool is_resolution_mapped(const remapping_type_e type) {
      return type == remapping_type_e::resolution_only || type == remapping_type_e::mixed;
    }

    /**
     * @brief Check if FPS is to be mapped based on remmaping type.
     * @param type Remapping type to check.
     * @returns True if FPS is to be mapped, false otherwise.
     */
    bool is_fps_mapped(const remapping_type_e type) {
      return type == remapping_type_e::refresh_rate_only || type == remapping_type_e::mixed;
    }

    /**
     * @brief Parse the remapping entry from the config into an internal structure.
     * @param entry Entry to parse.
     * @param type Specify which entry fields should be parsed.
     * @returns Parsed structure or null optional if a necessary field could not be parsed.
     */
    std::optional<parsed_remapping_entry_t> parse_remapping_entry(const config::video_t::dd_t::mode_remapping_entry_t &entry, const remapping_type_e type) {
      parsed_remapping_entry_t result {};

      if (is_resolution_mapped(type) && (!parse_resolution_string(entry.requested_resolution, result.requested_resolution) ||
                                         !parse_resolution_string(entry.final_resolution, result.final_resolution))) {
        return std::nullopt;
      }

      if (is_fps_mapped(type) && (!parse_refresh_rate_string(entry.requested_fps, result.requested_fps, false) ||
                                  !parse_refresh_rate_string(entry.final_refresh_rate, result.final_refresh_rate))) {
        return std::nullopt;
      }

      return result;
    }

    /**
     * @brief Remap the the requested display mode based on the config.
     * @param video_config User's video related configuration.
     * @param session Session information.
     * @param config A reference to a config object that will be modified on success.
     * @returns True if the remapping was performed or skipped, false if remapping has failed due to invalid config.
     *
     * @examples
     * const std::shared_ptr<rtsp_stream::launch_session_t> launch_session;
     * const config::video_t &video_config { config::video };
     *
     * SingleDisplayConfiguration config;
     * const bool success = remap_display_mode_if_needed(video_config, *launch_session, config);
     * @examples_end
     */
    bool remap_display_mode_if_needed(const config::video_t &video_config, const rtsp_stream::launch_session_t &session, SingleDisplayConfiguration &config) {
      const auto remapping_type {determine_remapping_type(video_config)};
      if (!remapping_type) {
        return true;
      }

      const auto &remapping_list {[&]() {
        using enum remapping_type_e;

        switch (*remapping_type) {
          case resolution_only:
            return video_config.dd.mode_remapping.resolution_only;
          case refresh_rate_only:
            return video_config.dd.mode_remapping.refresh_rate_only;
          case mixed:
          default:
            return video_config.dd.mode_remapping.mixed;
        }
      }()};

      if (remapping_list.empty()) {
        BOOST_LOG(debug) << "No values are available for display mode remapping.";
        return true;
      }
      BOOST_LOG(debug) << "Trying to remap display modes...";

      const auto entry_to_string {[type = *remapping_type](const config::video_t::dd_t::mode_remapping_entry_t &entry) {
        const bool mapping_resolution {is_resolution_mapped(type)};
        const bool mapping_fps {is_fps_mapped(type)};

        // clang-format off
        return (mapping_resolution ? "  - requested resolution: "s + entry.requested_resolution + "\n" : "") +
               (mapping_fps ?        "  - requested FPS: "s + entry.requested_fps + "\n" : "") +
               (mapping_resolution ? "  - final resolution: "s + entry.final_resolution + "\n" : "") +
               (mapping_fps ?        "  - final refresh rate: "s + entry.final_refresh_rate : "");
        // clang-format on
      }};

      for (const auto &entry : remapping_list) {
        const auto parsed_entry {parse_remapping_entry(entry, *remapping_type)};
        if (!parsed_entry) {
          BOOST_LOG(error) << "Failed to parse remapping entry from:\n"
                           << entry_to_string(entry);
          return false;
        }

        if (!parsed_entry->final_resolution && !parsed_entry->final_refresh_rate) {
          BOOST_LOG(error) << "At least one final value must be set for remapping display modes! Entry:\n"
                           << entry_to_string(entry);
          return false;
        }

        // Note: at this point config should already have parsed resolution set.
        if (parsed_entry->requested_resolution && parsed_entry->requested_resolution != config.m_resolution) {
          BOOST_LOG(verbose) << "Skipping remapping because requested resolutions do not match! Entry:\n"
                             << entry_to_string(entry);
          continue;
        }

        // Note: at this point config should already have parsed refresh rate set.
        if (parsed_entry->requested_fps && parsed_entry->requested_fps != config.m_refresh_rate) {
          BOOST_LOG(verbose) << "Skipping remapping because requested FPS do not match! Entry:\n"
                             << entry_to_string(entry);
          continue;
        }

        BOOST_LOG(info) << "Remapping requested display mode. Entry:\n"
                        << entry_to_string(entry);
        if (parsed_entry->final_resolution) {
          config.m_resolution = parsed_entry->final_resolution;
        }
        if (parsed_entry->final_refresh_rate) {
          config.m_refresh_rate = parsed_entry->final_refresh_rate;
        }
        break;
      }

      return true;
    }

    // All non-helper scheduling and platform display APIs are removed. We route exclusively
    // through the Windows display helper (no-ops on other platforms).
  }  // namespace

  // Old in-process API removed: no init/apply/revert/enumeration here.

#ifdef _WIN32
  static bool iequals(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) {
      return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
      if (std::tolower((unsigned char) a[i]) != std::tolower((unsigned char) b[i])) {
        return false;
      }
    }
    return true;
  }

  static std::string resolve_device_id(const std::string &output_name) {
    if (output_name.empty()) {
      return output_name;
    }

    try {
      auto api = std::make_shared<display_device::WinApiLayer>();
      display_device::WinDisplayDevice dd(api);
      const auto devices = dd.enumAvailableDevices();

      for (const auto &d : devices) {
        if (d.m_device_id.empty()) {
          continue;
        }

        if (iequals(d.m_device_id, output_name) ||
            (!d.m_display_name.empty() && iequals(d.m_display_name, output_name)) ||
            (!d.m_friendly_name.empty() && iequals(d.m_friendly_name, output_name))) {
          return d.m_device_id;
        }
      }
    } catch (...) {
    }

    return output_name;
  }
#else
  static std::string resolve_device_id(const std::string &output_name) {
    return output_name;
  }
#endif

  std::string map_output_name(const std::string &output_name) {
#ifdef _WIN32
    try {
      if (output_name.empty()) {
        return output_name;
      }

      // If caller already provided a Windows logical display name, return it.
      // These are of the form "\\.\\DISPLAY#".
      const auto is_win_display_name = [&]() -> bool {
        // Minimal check: begins with \\.\DISPLAY (case-insensitive)
        static const std::string prefix = "\\\\.\\DISPLAY";
        if (output_name.size() < prefix.size()) {
          return false;
        }
        for (size_t i = 0; i < prefix.size(); ++i) {
          if (std::tolower((unsigned char) output_name[i]) != std::tolower((unsigned char) prefix[i])) {
            return false;
          }
        }
        return true;
      }();
      if (is_win_display_name) {
        return output_name;
      }

      // Otherwise try to map any provided identifier (device_id, friendly name, or display name)
      // to the Windows logical display name using libdisplaydevice enumeration.
      auto api = std::make_shared<display_device::WinApiLayer>();
      display_device::WinDisplayDevice dd(api);
      const auto devices = dd.enumAvailableDevices();

      auto equals_ci = [](const std::string &a, const std::string &b) {
        if (a.size() != b.size()) {
          return false;
        }
        for (size_t i = 0; i < a.size(); ++i) {
          if (std::tolower((unsigned char) a[i]) != std::tolower((unsigned char) b[i])) {
            return false;
          }
        }
        return true;
      };

      for (const auto &d : devices) {
        // Match against device_id (preferred), display_name, or friendly_name
        if ((!d.m_device_id.empty() && equals_ci(d.m_device_id, output_name)) ||
            (!d.m_display_name.empty() && equals_ci(d.m_display_name, output_name)) ||
            (!d.m_friendly_name.empty() && equals_ci(d.m_friendly_name, output_name))) {
          if (!d.m_display_name.empty()) {
            return d.m_display_name;  // Return logical name consumable by DXGI
          }
          break;
        }
      }

      // Fallback to original if not found
      return output_name;
    } catch (...) {
      // If enumeration fails for any reason, fall back to the provided value.
      return output_name;
    }
#else
    // Non-Windows: no mapping needed
    return output_name;
#endif
  }

  std::variant<failed_to_parse_tag_t, configuration_disabled_tag_t, SingleDisplayConfiguration> parse_configuration(const config::video_t &video_config, const rtsp_stream::launch_session_t &session) {
    const auto device_prep {parse_device_prep_option(video_config)};
    if (!device_prep) {
      return configuration_disabled_tag_t {};
    }

    SingleDisplayConfiguration config;
    config.m_device_id = video_config.output_name;
    config.m_device_prep = *device_prep;
    config.m_hdr_state = parse_hdr_option(video_config, session);

    if (!parse_resolution_option(video_config, session, config)) {
      // Error already logged
      return failed_to_parse_tag_t {};
    }

    if (!parse_refresh_rate_option(video_config, session, config)) {
      // Error already logged
      return failed_to_parse_tag_t {};
    }

    if (!remap_display_mode_if_needed(video_config, session, config)) {
      // Error already logged
      return failed_to_parse_tag_t {};
    }

    return config;
  }
}  // namespace display_device
