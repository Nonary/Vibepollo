/**
 * @file src/platform/windows/display_helper_request_helpers.cpp
 */

#ifdef _WIN32

  #include "display_helper_request_helpers.h"

  #include "src/display_device.h"
  #include "src/globals.h"
  #include "src/platform/common.h"
  #include "src/process.h"
  #include "src/platform/windows/display_helper_coordinator.h"
  #include "src/platform/windows/frame_limiter_nvcp.h"
  #include "src/platform/windows/misc.h"
  #include "src/platform/windows/virtual_display.h"

  #include <display_device/json.h>
  #include <algorithm>

namespace display_helper_integration::helpers {
  namespace {
    constexpr int kIsolatedVirtualDisplayOffset = 64000;

    struct layout_flags_t {
      display_helper_integration::VirtualDisplayArrangement arrangement;
      display_device::SingleDisplayConfiguration::DevicePreparation device_prep;
      bool isolated = false;
    };

    layout_flags_t describe_layout(const config::video_t::virtual_display_layout_e layout) {
      using enum display_helper_integration::VirtualDisplayArrangement;
      using Prep = display_device::SingleDisplayConfiguration::DevicePreparation;
      switch (layout) {
        case config::video_t::virtual_display_layout_e::extended:
          return {Extended, Prep::EnsureActive, false};
        case config::video_t::virtual_display_layout_e::extended_primary:
          return {ExtendedPrimary, Prep::EnsurePrimary, false};
        case config::video_t::virtual_display_layout_e::extended_isolated:
          return {ExtendedIsolated, Prep::EnsureActive, true};
        case config::video_t::virtual_display_layout_e::extended_primary_isolated:
          return {ExtendedPrimaryIsolated, Prep::EnsurePrimary, true};
        case config::video_t::virtual_display_layout_e::exclusive:
        default:
          return {Exclusive, Prep::EnsureOnlyDisplay, false};
      }
    }

    bool session_targets_desktop(const rtsp_stream::launch_session_t &session) {
      const auto apps = proc::proc.get_apps();
      if (apps.empty()) {
        return false;
      }

      const auto app_id = std::to_string(session.appid);
      const auto it = std::find_if(apps.begin(), apps.end(), [&](const proc::ctx_t &app) {
        return app.id == app_id;
      });

      if (it == apps.end()) {
        return session.appid <= 0;
      }

      return it->cmd.empty() && it->playnite_id.empty();
    }

    std::optional<std::string> resolve_virtual_device_id(const config::video_t &video_config, const rtsp_stream::launch_session_t &session) {
      if (!session.virtual_display_device_id.empty()) {
        return session.virtual_display_device_id;
      }

      if (auto resolved = platf::display_helper::Coordinator::instance().resolve_virtual_display_device_id()) {
        return resolved;
      }

      if (!video_config.output_name.empty()) {
        return video_config.output_name;
      }

      return std::nullopt;
    }

    void apply_resolution_refresh_overrides(
      display_device::SingleDisplayConfiguration &config,
      int effective_width,
      int effective_height,
      int display_fps
    ) {
      if (!config.m_resolution && effective_width > 0 && effective_height > 0) {
        config.m_resolution = display_device::Resolution {
          static_cast<unsigned int>(effective_width),
          static_cast<unsigned int>(effective_height)
        };
      }
      if (!config.m_refresh_rate && display_fps > 0) {
        config.m_refresh_rate = display_device::Rational {static_cast<unsigned int>(display_fps), 1u};
      }
    }
  }  // namespace

  SessionDisplayConfigurationHelper::SessionDisplayConfigurationHelper(const config::video_t &video_config, const rtsp_stream::launch_session_t &session)
      : video_config_ {video_config}, session_ {session} {}

  bool SessionDisplayConfigurationHelper::configure(DisplayApplyBuilder &builder) const {
    builder.set_session(session_);
    builder.set_hdr_toggle_flag(video_config_.dd.wa.hdr_toggle);
    const auto effective_layout =
      session_.virtual_display_layout_override.value_or(video_config_.virtual_display_layout);
    const auto layout_flags = describe_layout(effective_layout);
    builder.set_virtual_display_arrangement(layout_flags.arrangement);

    auto &overrides = builder.mutable_session_overrides();
    if (session_.width > 0) {
      overrides.width_override = session_.width;
    }
    if (session_.height > 0) {
      overrides.height_override = session_.height;
    }
    if (session_.framegen_refresh_rate && *session_.framegen_refresh_rate > 0) {
      overrides.framegen_refresh_override = session_.framegen_refresh_rate;
      overrides.fps_override = session_.framegen_refresh_rate;
    } else if (session_.fps > 0) {
      overrides.fps_override = session_.fps;
    }
    overrides.virtual_display_override = session_.virtual_display;

    const int effective_width = session_.width;
    const int effective_height = session_.height;
    const int base_fps = session_.fps;
    std::optional<int> framegen_refresh = session_.framegen_refresh_rate;
    const int display_fps = framegen_refresh && *framegen_refresh > 0 ? *framegen_refresh : base_fps;

    const auto config_mode = video_config_.virtual_display_mode;
    const bool config_selects_virtual = (config_mode == config::video_t::virtual_display_mode_e::per_client || config_mode == config::video_t::virtual_display_mode_e::shared);
    const bool metadata_requests_virtual = session_.app_metadata && session_.app_metadata->virtual_screen;
    const bool session_requests_virtual = session_.virtual_display || config_selects_virtual || metadata_requests_virtual;

    if (session_requests_virtual) {
      return configure_virtual_display(builder, effective_layout, effective_width, effective_height, display_fps);
    }
    return configure_standard(builder, effective_layout, effective_width, effective_height, display_fps);
  }

  bool SessionDisplayConfigurationHelper::configure_virtual_display(
    DisplayApplyBuilder &builder,
    const config::video_t::virtual_display_layout_e layout,
    const int effective_width,
    const int effective_height,
    const int display_fps
  ) const {
    const auto parsed = display_device::parse_configuration(video_config_, session_);
    const auto *cfg = std::get_if<display_device::SingleDisplayConfiguration>(&parsed);
    if (!cfg) {
      builder.set_action(DisplayApplyAction::Skip);
      return false;
    }

    auto vd_cfg = *cfg;

    std::string target_device_id;
    if (auto resolved = resolve_virtual_device_id(video_config_, session_)) {
      target_device_id = *resolved;
    }
    vd_cfg.m_device_id = target_device_id;
    const auto layout_flags = describe_layout(layout);
    vd_cfg.m_device_prep = layout_flags.device_prep;
    apply_resolution_refresh_overrides(vd_cfg, effective_width, effective_height, display_fps);

    auto &overrides = builder.mutable_session_overrides();
    overrides.device_id_override = target_device_id.empty() ? std::nullopt : std::optional<std::string>(target_device_id);
    overrides.virtual_display_override = true;
    if (effective_width > 0) {
      overrides.width_override = effective_width;
    }
    if (effective_height > 0) {
      overrides.height_override = effective_height;
    }
    if (display_fps > 0) {
      overrides.fps_override = display_fps;
    }
    overrides.framegen_refresh_override = session_.framegen_refresh_rate;

    if (!target_device_id.empty()) {
      builder.set_device_blacklist(target_device_id);
    } else {
      builder.set_device_blacklist(std::nullopt);
    }

    builder.set_configuration(vd_cfg);
    builder.set_virtual_display_watchdog(true);
    builder.set_action(DisplayApplyAction::Apply);
    return true;
  }

  bool SessionDisplayConfigurationHelper::configure_standard(
    DisplayApplyBuilder &builder,
    const config::video_t::virtual_display_layout_e layout,
    const int effective_width,
    const int effective_height,
    const int display_fps
  ) const {
    const bool dummy_plug_mode = video_config_.dd.wa.dummy_plug_hdr10;
    const bool desktop_session = session_targets_desktop(session_);
    const bool gen1_framegen_fix = session_.gen1_framegen_fix;
    const bool gen2_framegen_fix = session_.gen2_framegen_fix;
    const bool best_effort_refresh = config::frame_limiter.disable_vsync &&
                                     (!platf::has_nvidia_gpu() || !platf::frame_limiter_nvcp::is_available());
    bool should_force_refresh = gen1_framegen_fix || gen2_framegen_fix || best_effort_refresh;
    if (dummy_plug_mode && !gen1_framegen_fix && !gen2_framegen_fix) {
      should_force_refresh = false;
    }

    const auto parsed = display_device::parse_configuration(video_config_, session_);
    if (const auto *cfg = std::get_if<display_device::SingleDisplayConfiguration>(&parsed)) {
      auto cfg_effective = *cfg;
      if (session_.virtual_display && !session_.virtual_display_device_id.empty()) {
        cfg_effective.m_device_id = session_.virtual_display_device_id;
      }

      if (session_.virtual_display) {
        const auto layout_flags = describe_layout(layout);
        cfg_effective.m_device_prep = layout_flags.device_prep;
      }

      if (dummy_plug_mode && !gen1_framegen_fix && !gen2_framegen_fix && !desktop_session) {
        cfg_effective.m_refresh_rate = display_device::Rational {30u, 1u};
        cfg_effective.m_hdr_state = display_device::HdrState::Enabled;
      }
      if (dummy_plug_mode && (gen1_framegen_fix || gen2_framegen_fix) && !desktop_session) {
        cfg_effective.m_hdr_state = display_device::HdrState::Enabled;
      }
      if (should_force_refresh) {
        cfg_effective.m_refresh_rate = display_device::Rational {10000u, 1u};
        if (!cfg_effective.m_resolution && effective_width >= 0 && effective_height >= 0) {
          cfg_effective.m_resolution = display_device::Resolution {
            static_cast<unsigned int>(effective_width),
            static_cast<unsigned int>(effective_height)
          };
        }
      }

      apply_resolution_refresh_overrides(cfg_effective, effective_width, effective_height, display_fps);

      builder.set_configuration(cfg_effective);
      builder.set_device_blacklist(std::nullopt);
      builder.set_virtual_display_watchdog(false);
      builder.set_action(DisplayApplyAction::Apply);
      return true;
    }

    if (std::holds_alternative<display_device::configuration_disabled_tag_t>(parsed)) {
      if (dummy_plug_mode && !gen1_framegen_fix && !gen2_framegen_fix && !desktop_session) {
        display_device::SingleDisplayConfiguration cfg_override;
        cfg_override.m_device_id = session_.virtual_display_device_id.empty() ? video_config_.output_name : session_.virtual_display_device_id;
        if (effective_width >= 0 && effective_height >= 0) {
          cfg_override.m_resolution = display_device::Resolution {
            static_cast<unsigned int>(effective_width),
            static_cast<unsigned int>(effective_height)
          };
        }
        cfg_override.m_refresh_rate = display_device::Rational {30u, 1u};
        cfg_override.m_hdr_state = display_device::HdrState::Enabled;
        builder.set_configuration(cfg_override);
        builder.set_action(DisplayApplyAction::Apply);
        return true;
      }

      builder.clear_configuration();
      builder.set_action(DisplayApplyAction::Revert);
      builder.set_device_blacklist(std::nullopt);
      builder.set_virtual_display_watchdog(false);
      return true;
    }

    builder.set_action(DisplayApplyAction::Skip);
    return false;
  }

  SessionMonitorPositionHelper::SessionMonitorPositionHelper(const config::video_t &video_config, const rtsp_stream::launch_session_t &session)
      : video_config_ {video_config}, session_ {session} {}

  void SessionMonitorPositionHelper::configure(DisplayApplyBuilder &builder) const {
    auto &topology = builder.mutable_topology();
    std::string device_id;
    if (!session_.virtual_display_device_id.empty()) {
      device_id = session_.virtual_display_device_id;
    } else if (!video_config_.output_name.empty()) {
      device_id = video_config_.output_name;
    } else {
      device_id = "DISPLAY_PRIMARY";
    }

    if (topology.topology.empty()) {
      topology.topology = {{device_id}};
    }
    const auto effective_layout =
      session_.virtual_display_layout_override.value_or(video_config_.virtual_display_layout);
    const auto layout_flags = describe_layout(effective_layout);
    if (layout_flags.isolated) {
      topology.monitor_positions[device_id] = display_device::Point {kIsolatedVirtualDisplayOffset, kIsolatedVirtualDisplayOffset};
    }
  }

  std::optional<DisplayApplyRequest> build_request_from_session(const config::video_t &video_config, const rtsp_stream::launch_session_t &session) {
    DisplayApplyBuilder builder;
    SessionDisplayConfigurationHelper config_helper(video_config, session);
    if (!config_helper.configure(builder)) {
      return std::nullopt;
    }

    SessionMonitorPositionHelper monitor_helper(video_config, session);
    monitor_helper.configure(builder);

    return builder.build();
  }
}  // namespace display_helper_integration::helpers

#endif  // _WIN32
