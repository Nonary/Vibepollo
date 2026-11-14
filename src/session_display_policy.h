/**
 * @file src/session_display_policy.h
 * @brief Resolve per-session display preparation without modifying persistent settings.
 */
#pragma once

#include "config.h"
#include "legacy_generated_app_identities.h"
#include "platform/windows/virtual_display_policy.h"

namespace proc::display_policy {
  using preparation_e = config::video_t::dd_t::config_option_e;

  // Selecting the desktop applet is an explicit virtual target selection,
  // including when the client's usual game preference is a physical display.
  // The neutral token leaves provider selection to the configured driver.
  template <class Session>
  void apply_builtin_virtual_display(Session &session, const std::string_view app_uuid, const config::video_t &video) {
    if (app_uuid != VIRTUAL_DISPLAY_UUID) return;
    session.virtual_display = true;
    session.output_name_override = "sunshine:virtual_display";
    if (session.virtual_display_mode_override.value_or(video.virtual_display_mode) == config::video_t::virtual_display_mode_e::disabled) {
      session.virtual_display_mode_override = config::video_t::virtual_display_mode_e::per_client;
    }
    if (!session.dd_config_option_override &&
        (video.dd.configuration_option == preparation_e::disabled || video.dd.configuration_option == preparation_e::verify_only)) {
      session.dd_config_option_override = preparation_e::ensure_active;
    }
  }

  // Launch/resume never fail because this screen is missing: the stream starts
  // on the existing display, and the client receives this warning code together
  // with the fallback text below as its message.
  inline constexpr std::string_view virtual_display_unavailable_warning = "virtual_display_unavailable";

  /**
   * @brief Describe a built-in Virtual Display stream that starts without its own screen.
   * @return Text for the host log and the client warning, or empty when the app is not the
   *         built-in desktop or its screen exists.
   */
  inline std::string builtin_virtual_display_fallback(const std::string_view app_uuid, const bool created, const config::video_t &video) {
    if (app_uuid != VIRTUAL_DISPLAY_UUID || created) return {};
    // Creation also fails while another stream is ending or a display restore
    // is running, so do not present a driver repair as the only cause.
    return video.dd.use_sunshine_virtual_display_driver ?
      "Virtual Display could not create a virtual screen with the Vibepollo display driver; streaming the existing display instead. Wait for any other stream or display restore to finish, then launch it again. If it keeps happening, repair the driver." :
      "Virtual Display could not create a virtual screen with the SudoVDA driver; streaming the existing display instead. Wait for any other stream or display restore to finish, then launch it again. If it keeps happening, check that SudoVDA is installed or select the Vibepollo display driver.";
  }

  /**
   * @brief Read the generic representation of a session preparation override.
   * @param overrides Application or authenticated client runtime overrides.
   * @return The explicit preparation, or no value when it is inherited.
   * @note Invalid values follow config.cpp's disabled fallback.
   */
  inline std::optional<preparation_e> preparation_override(const std::unordered_map<std::string, std::string> &overrides) {
    const auto it = overrides.find("dd_configuration_option");
    if (it == overrides.end()) {
      return std::nullopt;
    }
    if (it->second == "verify_only") return preparation_e::verify_only;
    if (it->second == "ensure_active") return preparation_e::ensure_active;
    if (it->second == "ensure_primary") return preparation_e::ensure_primary;
    if (it->second == "ensure_only_display") return preparation_e::ensure_only_display;
    return preparation_e::disabled;
  }

  /**
   * @brief Identify an explicit virtual target or layout before preparation consumes it.
   * @param session The display fields assembled so far for this session.
   * @return The explicit virtual/physical choice, or no value for inheritance.
   */
  template <class Session>
  std::optional<bool> virtual_display_override(const Session &session) {
    if (session.client_virtual_display_override) {
      return *session.client_virtual_display_override;
    }
    if (session.client_requests_virtual_display) {
      return true;
    }
    if (session.output_name_override) {
      std::string_view output {*session.output_name_override};
      const auto first = output.find_first_not_of(" \t\r\n");
      if (first == std::string_view::npos) return false;
      output = output.substr(first, output.find_last_not_of(" \t\r\n") - first + 1);
      // Both host drivers share this token; SudoVDA also accepts its legacy alias.
      return VDISPLAY::policy::is_virtual_display_selection(output, true);
    }
    if (session.virtual_display_mode_override &&
        *session.virtual_display_mode_override == config::video_t::virtual_display_mode_e::disabled) {
      return false;
    }
    if (session.virtual_display || session.virtual_display_mode_override || session.virtual_display_layout_override) {
      return true;
    }
    return std::nullopt;
  }

  /** @brief Resolved explicit preparation and a deferred virtual-only promotion. */
  struct preparation_t {
    std::optional<preparation_e> option;
    bool virtual_override = false;
  };

  /**
   * @brief Resolve Client -> Application -> Global preparation across representations.
   * @param inherited Effective config fallback, including any runtime overrides.
   * @param application Dedicated app preparation, falling back to its generic value.
   * @param client Authenticated client's generic preparation.
   * @param client_virtual Whether the client explicitly requests a virtual target/layout.
   * @param session_virtual Whether the assembled session explicitly requests one.
   * @return Ephemeral preparation and whether a higher virtual choice may lift VerifyOnly.
   * @note An application's explicit virtual choice also lifts its own VerifyOnly;
   *       a client preparation still takes precedence over an application choice.
   */
  inline preparation_t resolve_preparation(
    const preparation_e inherited,
    const std::optional<preparation_e> application,
    const std::optional<preparation_e> client,
    const bool client_virtual,
    const bool session_virtual
  ) {
    const auto explicit_preparation = client ? client : application;
    const int preparation_level = client ? 2 : application ? 1 : 0;
    const int virtual_level = client_virtual ? 2 : session_virtual ? 1 : 0;
    const bool virtual_choice_wins = virtual_level > preparation_level ||
                                     (virtual_level == 1 && preparation_level == 1);
    if (explicit_preparation.value_or(inherited) == preparation_e::verify_only && virtual_choice_wins) {
      return {explicit_preparation, true};
    }
    return {explicit_preparation, false};
  }

  /**
   * @brief Apply virtual-only preparation after a virtual target is known.
   * @param inherited Effective global preparation.
   * @param session Resolved session policy and current target state.
   * @param virtual_display_intended Whether this is the initial virtual creation request.
   * @return Effective preparation, preserving physical fallback and true VerifyOnly.
   */
  template <class Session>
  preparation_e effective_preparation(const preparation_e inherited, const Session &session, const bool virtual_display_intended = false) {
    const auto option = session.dd_config_option_override.value_or(inherited);
    if ((session.virtual_display || virtual_display_intended) && session.virtual_display_preparation_override &&
        option == preparation_e::verify_only) {
      // The virtual layout chooses EnsureActive/Primary/OnlyDisplay downstream.
      return preparation_e::ensure_active;
    }
    return option;
  }
}  // namespace proc::display_policy
