/** @file src/display_device_policy.cpp */
#include "display_device_policy.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <regex>

namespace display_device::policy {
  namespace {
    using dd_t = video_config_t::dd_t;

    std::string trim(std::string value) {
      const auto first = value.find_first_not_of(" \t\r\n");
      if (first == std::string::npos) return {};
      const auto last = value.find_last_not_of(" \t\r\n");
      return value.substr(first, last - first + 1);
    }

    unsigned int stou(const std::string &value) {
      const auto result = std::stoul(value);
      if (result > std::numeric_limits<unsigned int>::max()) throw std::out_of_range("unsigned integer");
      return static_cast<unsigned int>(result);
    }

    bool parse_resolution(const std::string &input, std::optional<resolution_t> &output) {
      auto value = trim(input);
      const std::string multiplication_sign = "\xC3\x97";
      for (std::size_t pos {}; (pos = value.find(multiplication_sign, pos)) != std::string::npos; ++pos) value.replace(pos, multiplication_sign.size(), "x");
      static const std::regex pattern {R"(^(\d+)x(\d+)$)"};
      std::smatch match;
      if (!std::regex_match(value, match, pattern)) {
        if (value.empty()) { output.reset(); return true; }
        return false;
      }
      try { output = resolution_t {stou(match[1].str()), stou(match[2].str())}; return true; }
      catch (const std::exception &) { return false; }
    }

    bool parse_refresh_rate(const std::string &input, std::optional<rational_t> &output, bool decimal = true) {
      const auto value = trim(input);
      const std::regex pattern {decimal ? R"(^(\d+)(?:\.(\d+))?$)" : R"(^(\d+)$)"};
      std::smatch match;
      if (!std::regex_match(value, match, pattern)) {
        if (value.empty()) { output.reset(); return true; }
        return false;
      }
      try {
        auto whole = match[1].str();
        whole.erase(0, std::min(whole.find_first_not_of('0'), whole.size() - 1));
        auto fraction = match[2].matched ? match[2].str() : std::string {};
        fraction.erase(fraction.find_last_not_of('0') + 1);
        if (fraction.empty()) output = rational_t {stou(whole), 1};
        else output = rational_t {stou(whole + fraction), static_cast<unsigned int>(std::pow(10, fraction.size()))};
        return true;
      } catch (const std::exception &) { return false; }
    }

    std::optional<device_preparation_e> device_preparation(dd_t::config_option_e option) {
      using enum dd_t::config_option_e;
      switch (option) {
        case verify_only: return device_preparation_e::VerifyOnly;
        case ensure_active: return device_preparation_e::EnsureActive;
        case ensure_primary: return device_preparation_e::EnsurePrimary;
        case ensure_only_display: return device_preparation_e::EnsureOnlyDisplay;
        case disabled: return std::nullopt;
      }
      return std::nullopt;
    }

    std::uint32_t effective_refresh_millihz(const session_t &session) {
      if (session.framegen_refresh_millihz && *session.framegen_refresh_millihz > 0) return *session.framegen_refresh_millihz;
      if (session.client_display_refresh_millihz > 0) return session.client_display_refresh_millihz;
      if (session.framegen_refresh_rate && *session.framegen_refresh_rate > 0) return static_cast<std::uint32_t>(*session.framegen_refresh_rate) * 1000u;
      return session.fps > 0 ? static_cast<std::uint32_t>(session.fps) * 1000u : 0;
    }

    bool parse_resolution_option(const video_config_t &video, const session_t &session, configuration_t &out) {
      if (session.resolution_override) {
        if (session.resolution_override->width <= 0 || session.resolution_override->height <= 0) return false;
        out.m_resolution = resolution_t {static_cast<unsigned int>(session.resolution_override->width), static_cast<unsigned int>(session.resolution_override->height)};
        return true;
      }
      if (session.client_display_mode_override) {
        if (session.width < 0 || session.height < 0) return false;
        out.m_resolution = resolution_t {static_cast<unsigned int>(session.width), static_cast<unsigned int>(session.height)};
        return true;
      }
      using enum dd_t::resolution_option_e;
      switch (video.dd.resolution_option) {
        case automatic:
          if (session.width < 0 || session.height < 0) return false;
          out.m_resolution = resolution_t {static_cast<unsigned int>(session.width), static_cast<unsigned int>(session.height)};
          break;
        case manual:
          if (!parse_resolution(video.dd.manual_resolution, out.m_resolution) || !out.m_resolution) return false;
          break;
        case disabled: break;
      }
      return true;
    }

    bool parse_refresh_option(const video_config_t &video, const session_t &session, configuration_t &out) {
      if (session.client_display_mode_override) {
        const auto value = effective_refresh_millihz(session);
        if (!value) return false;
        out.m_refresh_rate = rational_t {value, 1000};
        return true;
      }
      using enum dd_t::refresh_rate_option_e;
      switch (video.dd.refresh_rate_option) {
        case automatic: {
          const auto value = effective_refresh_millihz(session);
          // A zero client FPS is a valid explicit value in the policy contract
          // (and is represented as 0/1 by callers).  A negative FPS, absent a
          // valid higher-priority rate, remains invalid.
          if (!value && session.fps < 0) return false;
          out.m_refresh_rate = rational_t {value, 1000};
          break;
        }
        case manual: if (!parse_refresh_rate(video.dd.manual_refresh_rate, out.m_refresh_rate) || !out.m_refresh_rate) return false; break;
        case prefer_highest: out.m_refresh_rate = rational_t {10000, 1}; break;
        case disabled: break;
      }
      return true;
    }

    enum class remapping_type_e { mixed, resolution_only, refresh_rate_only };
    std::optional<remapping_type_e> remapping_type(const video_config_t &video) {
      const bool res = video.dd.resolution_option == dd_t::resolution_option_e::automatic;
      const bool fps = video.dd.refresh_rate_option == dd_t::refresh_rate_option_e::automatic;
      if (res && fps) return remapping_type_e::mixed;
      if (res) return remapping_type_e::resolution_only;
      if (fps) return remapping_type_e::refresh_rate_only;
      return std::nullopt;
    }
    bool remap(const video_config_t &video, const session_t &session, configuration_t &out) {
      if (session.client_display_mode_override) return true;
      const auto type = remapping_type(video); if (!type) return true;
      const auto &entries = *type == remapping_type_e::mixed ? video.dd.mode_remapping.mixed : *type == remapping_type_e::resolution_only ? video.dd.mode_remapping.resolution_only : video.dd.mode_remapping.refresh_rate_only;
      const bool map_res = *type != remapping_type_e::refresh_rate_only;
      const bool map_fps = *type != remapping_type_e::resolution_only;
      for (const auto &entry : entries) {
        std::optional<resolution_t> request_res, final_res;
        std::optional<rational_t> request_fps, final_fps;
        if ((map_res && (!parse_resolution(entry.requested_resolution, request_res) || !parse_resolution(entry.final_resolution, final_res))) ||
            // Requested FPS selects an integer client rate, while a remapped
            // display mode may legitimately use a fractional refresh rate.
            (map_fps && (!parse_refresh_rate(entry.requested_fps, request_fps, false) || !parse_refresh_rate(entry.final_refresh_rate, final_fps)))) return false;
        if (!final_res && !final_fps) return false;
        if ((request_res && request_res != out.m_resolution) || (request_fps && request_fps != out.m_refresh_rate)) continue;
        if (final_res) out.m_resolution = final_res;
        if (final_fps) out.m_refresh_rate = final_fps;
        break;
      }
      return true;
    }
  }  // namespace

  bool effective_hdr_requested(const session_t &session) { return session.enable_hdr && !session.force_sdr; }
  bool effective_10bit_sdr_requested(const session_t &session) {
    return session.prefer_sdr_10bit && !effective_hdr_requested(session);
  }

  std::variant<failed_to_parse_tag_t, configuration_disabled_tag_t, configuration_t> parse_configuration(const video_config_t &video, const session_t &session) {
    const auto preparation = device_preparation(video.dd.configuration_option);
    if (!preparation) return configuration_disabled_tag_t {};
    configuration_t out {.m_device_id = video.output_name, .m_device_prep = *preparation};
    if (video.rtx_hdr_enabled) out.m_hdr_state = hdr_state_e::Disabled;
    else if (video.dd.wa.dummy_plug_hdr10) out.m_hdr_state = hdr_state_e::Enabled;
    else if (video.dd.hdr_option == dd_t::hdr_option_e::automatic) out.m_hdr_state = effective_hdr_requested(session) ? hdr_state_e::Enabled : hdr_state_e::Disabled;
    if (!parse_resolution_option(video, session, out) || !parse_refresh_option(video, session, out) || !remap(video, session, out)) return failed_to_parse_tag_t {};
    return out;
  }

  bool refresh_rate_override_active(const video_config_t &video, const session_t &session) {
    if (video.dd.refresh_rate_option == dd_t::refresh_rate_option_e::manual) return true;
    if (session.client_display_mode_override) return false;
    const auto type = remapping_type(video); if (!type || *type == remapping_type_e::resolution_only) return false;
    const auto &entries = *type == remapping_type_e::mixed ? video.dd.mode_remapping.mixed : video.dd.mode_remapping.refresh_rate_only;
    const int fps = session.framegen_refresh_rate && *session.framegen_refresh_rate > 0 ? *session.framegen_refresh_rate : session.fps;
    if (fps < 0) return false;
    const rational_t requested {static_cast<unsigned int>(fps), 1};
    for (const auto &entry : entries) {
      std::optional<rational_t> request, final;
      if (!parse_refresh_rate(entry.requested_fps, request, false) || !parse_refresh_rate(entry.final_refresh_rate, final, false)) return false;
      if (final && (!request || *request == requested)) return true;
    }
    return false;
  }
}  // namespace display_device::policy
