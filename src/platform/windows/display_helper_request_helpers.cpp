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
  #include <boost/algorithm/string/predicate.hpp>
  #include <algorithm>
  #include <limits>
  #include <type_traits>

namespace display_helper_integration::helpers {
  namespace {
    constexpr int kIsolatedVirtualDisplayOffset = 64000;

    struct layout_flags_t {
      display_helper_integration::VirtualDisplayArrangement arrangement;
      display_device::SingleDisplayConfiguration::DevicePreparation device_prep;
      bool isolated = false;
    };

    int safe_add_int(int value, int delta) {
      const auto result = static_cast<long long>(value) + static_cast<long long>(delta);
      if (result > std::numeric_limits<int>::max()) {
        return std::numeric_limits<int>::max();
      }
      if (result < std::numeric_limits<int>::min()) {
        return std::numeric_limits<int>::min();
      }
      return static_cast<int>(result);
    }

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
      int display_fps,
      bool resolution_disabled,
      bool refresh_rate_disabled
    ) {
      // Only apply resolution override if resolution changes are not disabled by user
      if (!resolution_disabled && !config.m_resolution && effective_width > 0 && effective_height > 0) {
        config.m_resolution = display_device::Resolution {
          static_cast<unsigned int>(effective_width),
          static_cast<unsigned int>(effective_height)
        };
      }
      // Only apply refresh rate override if refresh rate changes are not disabled by user
      if (!refresh_rate_disabled && !config.m_refresh_rate && display_fps > 0) {
        config.m_refresh_rate = display_device::Rational {static_cast<unsigned int>(display_fps), 1u};
      }
    }

    int safe_double_int(int value) {
      if (value <= 0) {
        return value;
      }
      if (value > std::numeric_limits<int>::max() / 2) {
        return std::numeric_limits<int>::max();
      }
      return value * 2;
    }

    double get_refresh_rate_value(const display_device::FloatingPoint &value) {
      return std::visit(
        [](const auto &v) -> double {
          using V = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<V, display_device::Rational>) {
            return v.m_denominator > 0
                     ? static_cast<double>(v.m_numerator) / v.m_denominator
                     : static_cast<double>(v.m_numerator);
          } else {
            return static_cast<double>(v);
          }
        },
        value
      );
    }

    void ensure_minimum_refresh_if_present(std::optional<display_device::FloatingPoint> &value, int minimum_fps) {
      if (!value || minimum_fps <= 0) {
        return;
      }
      const double current = get_refresh_rate_value(*value);
      if (current >= static_cast<double>(minimum_fps)) {
        return;  // Already at or above minimum, don't change
      }
      // Set to minimum fps as a Rational
      value = display_device::Rational {static_cast<unsigned int>(minimum_fps), 1u};
    }
  }  // namespace

  SessionDisplayConfigurationHelper::SessionDisplayConfigurationHelper(const config::video_t &video_config, const rtsp_stream::launch_session_t &session)
      : video_config_ {video_config}, effective_video_config_ {video_config}, session_ {session} {
    if (session.dd_config_option_override) {
      effective_video_config_.dd.configuration_option = *session.dd_config_option_override;
    }
    if (session.virtual_display_mode_override) {
      effective_video_config_.virtual_display_mode = *session.virtual_display_mode_override;
    }
    if (auto runtime_output_override = config::runtime_output_name_override()) {
      if (runtime_output_override && !runtime_output_override->empty()) {
        effective_video_config_.output_name = *runtime_output_override;
      }
    }
    if (effective_video_config_.dd.configuration_option == config::video_t::dd_t::config_option_e::disabled &&
        !effective_video_config_.output_name.empty()) {
      effective_video_config_.dd.configuration_option = config::video_t::dd_t::config_option_e::ensure_active;
    }
  }

  bool SessionDisplayConfigurationHelper::configure(DisplayApplyBuilder &builder) const {
    builder.set_session(session_);
    builder.set_hdr_toggle_flag(effective_video_config_.dd.wa.hdr_toggle);
    BOOST_LOG(debug) << "session_.virtual_display_layout_override has_value: " << session_.virtual_display_layout_override.has_value();
    if (session_.virtual_display_layout_override) {
      BOOST_LOG(debug) << "session_.virtual_display_layout_override value: " << static_cast<int>(*session_.virtual_display_layout_override);
    }
    BOOST_LOG(debug) << "video_config_.virtual_display_layout: " << static_cast<int>(effective_video_config_.virtual_display_layout);
    const auto effective_layout =
      session_.virtual_display_layout_override.value_or(effective_video_config_.virtual_display_layout);
    BOOST_LOG(debug) << "effective_layout: " << static_cast<int>(effective_layout);
    const auto layout_flags = describe_layout(effective_layout);
    BOOST_LOG(debug) << "layout_flags.arrangement: " << static_cast<int>(layout_flags.arrangement);
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
    BOOST_LOG(debug) << "effective_width: " << effective_width;
    const int effective_height = session_.height;
    BOOST_LOG(debug) << "effective_height: " << effective_height;
    const int base_fps = session_.fps;
    BOOST_LOG(debug) << "base_fps: " << base_fps;
    std::optional<int> framegen_refresh = session_.framegen_refresh_rate;
    const bool framegen_active = framegen_refresh && *framegen_refresh > 0;
    BOOST_LOG(debug) << "framegen_refresh: " << (framegen_refresh ? std::to_string(*framegen_refresh) : "nullopt");
    const int framegen_display_fps = framegen_active ? *framegen_refresh : 0;
    const int display_fps = framegen_display_fps > 0 ? framegen_display_fps : base_fps;
    BOOST_LOG(debug) << "display_fps: " << display_fps;

    const auto config_mode = effective_video_config_.virtual_display_mode;
    BOOST_LOG(debug) << "config_mode: " << static_cast<int>(config_mode);
    const bool config_selects_virtual = (config_mode == config::video_t::virtual_display_mode_e::per_client || config_mode == config::video_t::virtual_display_mode_e::shared);
    BOOST_LOG(debug) << "config_selects_virtual: " << config_selects_virtual;
    const bool metadata_requests_virtual = session_.app_metadata && session_.app_metadata->virtual_screen;
    BOOST_LOG(debug) << "metadata_requests_virtual: " << metadata_requests_virtual;
    const bool session_requests_virtual = session_.virtual_display || config_selects_virtual || metadata_requests_virtual;
    BOOST_LOG(debug) << "session_requests_virtual: " << session_requests_virtual;
    const bool double_virtual_refresh = session_requests_virtual && effective_video_config_.dd.wa.virtual_double_refresh;
    // Either option (virtual_double_refresh or framegen) requests a minimum of 2x base fps
    const bool needs_double_minimum = double_virtual_refresh || framegen_active;
    const int minimum_fps = needs_double_minimum ? safe_double_int(base_fps) : base_fps;
    // Use the higher of display_fps (which may already be doubled by framegen) or the minimum
    const int effective_virtual_display_fps = std::max(display_fps, minimum_fps);
    BOOST_LOG(debug) << "double_virtual_refresh: " << double_virtual_refresh;
    BOOST_LOG(debug) << "needs_double_minimum: " << needs_double_minimum;
    BOOST_LOG(debug) << "minimum_fps: " << minimum_fps;
    BOOST_LOG(debug) << "effective_display_fps: "
                     << (session_requests_virtual ? effective_virtual_display_fps : display_fps);

    if (session_requests_virtual) {
      return configure_virtual_display(
        builder,
        effective_layout,
        effective_width,
        effective_height,
        effective_virtual_display_fps,
        minimum_fps
      );
    }
    return configure_standard(builder, effective_layout, effective_width, effective_height, display_fps);
  }

  bool SessionDisplayConfigurationHelper::configure_virtual_display(
    DisplayApplyBuilder &builder,
    const config::video_t::virtual_display_layout_e layout,
    const int effective_width,
    const int effective_height,
    const int display_fps,
    const int minimum_fps
  ) const {
    const auto parsed = display_device::parse_configuration(effective_video_config_, session_);
    const auto *cfg = std::get_if<display_device::SingleDisplayConfiguration>(&parsed);
    if (!cfg) {
      builder.set_action(DisplayApplyAction::Skip);
      return false;
    }

    auto vd_cfg = *cfg;

    std::string target_device_id;
    if (auto resolved = resolve_virtual_device_id(effective_video_config_, session_)) {
      target_device_id = *resolved;
    }
    vd_cfg.m_device_id = target_device_id;
    const auto layout_flags = describe_layout(layout);
    vd_cfg.m_device_prep = layout_flags.device_prep;
    if (minimum_fps > 0 && vd_cfg.m_refresh_rate) {
      ensure_minimum_refresh_if_present(vd_cfg.m_refresh_rate, minimum_fps);
    }
    const bool resolution_disabled = effective_video_config_.dd.resolution_option == config::video_t::dd_t::resolution_option_e::disabled;
    const bool refresh_rate_disabled = effective_video_config_.dd.refresh_rate_option == config::video_t::dd_t::refresh_rate_option_e::disabled;
    apply_resolution_refresh_overrides(vd_cfg, effective_width, effective_height, display_fps, resolution_disabled, refresh_rate_disabled);

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
    const bool dummy_plug_mode = effective_video_config_.dd.wa.dummy_plug_hdr10;
    const bool desktop_session = session_targets_desktop(session_);
    const bool gen1_framegen_fix = session_.gen1_framegen_fix;
    const bool gen2_framegen_fix = session_.gen2_framegen_fix;
    const bool best_effort_refresh = config::frame_limiter.disable_vsync &&
                                     (!platf::has_nvidia_gpu() || !platf::frame_limiter_nvcp::is_available());
    bool should_force_refresh = gen1_framegen_fix || gen2_framegen_fix || best_effort_refresh;
    if (dummy_plug_mode && !gen1_framegen_fix && !gen2_framegen_fix) {
      should_force_refresh = false;
    }

    const auto parsed = display_device::parse_configuration(effective_video_config_, session_);
    if (const auto *cfg = std::get_if<display_device::SingleDisplayConfiguration>(&parsed)) {
      auto cfg_effective = *cfg;
      if (session_.virtual_display && !session_.virtual_display_device_id.empty()) {
        cfg_effective.m_device_id = session_.virtual_display_device_id;
      }
      BOOST_LOG(info) << "Display helper apply (standard): target device_id=" << cfg_effective.m_device_id
                      << " prep=" << static_cast<int>(cfg_effective.m_device_prep);

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
      const bool resolution_disabled = effective_video_config_.dd.resolution_option == config::video_t::dd_t::resolution_option_e::disabled;
      const bool refresh_rate_disabled = effective_video_config_.dd.refresh_rate_option == config::video_t::dd_t::refresh_rate_option_e::disabled;

      if (should_force_refresh) {
        cfg_effective.m_refresh_rate = display_device::Rational {10000u, 1u};
        // Only set resolution if user hasn't explicitly disabled resolution changes
        if (!resolution_disabled && !cfg_effective.m_resolution && effective_width >= 0 && effective_height >= 0) {
          cfg_effective.m_resolution = display_device::Resolution {
            static_cast<unsigned int>(effective_width),
            static_cast<unsigned int>(effective_height)
          };
        }
      }

      apply_resolution_refresh_overrides(cfg_effective, effective_width, effective_height, display_fps, resolution_disabled, refresh_rate_disabled);

      builder.set_configuration(cfg_effective);
      builder.set_virtual_display_watchdog(false);
      builder.set_action(DisplayApplyAction::Apply);
      return true;
    }

    if (std::holds_alternative<display_device::configuration_disabled_tag_t>(parsed)) {
      if (dummy_plug_mode && !gen1_framegen_fix && !gen2_framegen_fix && !desktop_session) {
        display_device::SingleDisplayConfiguration cfg_override;
        cfg_override.m_device_id = session_.virtual_display_device_id.empty() ? effective_video_config_.output_name : session_.virtual_display_device_id;
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
      builder.set_virtual_display_watchdog(false);
      return true;
    }

    builder.set_action(DisplayApplyAction::Skip);
    return false;
  }

  SessionMonitorPositionHelper::SessionMonitorPositionHelper(const config::video_t &video_config, const rtsp_stream::launch_session_t &session)
      : video_config_ {video_config}, effective_video_config_ {video_config}, session_ {session} {
    if (session.dd_config_option_override) {
      effective_video_config_.dd.configuration_option = *session.dd_config_option_override;
    }
    if (session.virtual_display_mode_override) {
      effective_video_config_.virtual_display_mode = *session.virtual_display_mode_override;
    }
    if (auto runtime_output_override = config::runtime_output_name_override()) {
      if (runtime_output_override && !runtime_output_override->empty()) {
        effective_video_config_.output_name = *runtime_output_override;
      }
    }
    if (effective_video_config_.dd.configuration_option == config::video_t::dd_t::config_option_e::disabled &&
        !effective_video_config_.output_name.empty()) {
      effective_video_config_.dd.configuration_option = config::video_t::dd_t::config_option_e::ensure_active;
    }
  }

  void SessionMonitorPositionHelper::configure(DisplayApplyBuilder &builder) const {
    auto &topology = builder.mutable_topology();
    std::string default_device_id;
    if (!session_.virtual_display_device_id.empty()) {
      default_device_id = session_.virtual_display_device_id;
    } else if (!effective_video_config_.output_name.empty()) {
      default_device_id = effective_video_config_.output_name;
    } else {
      default_device_id = "";
    }
    BOOST_LOG(info) << "Display helper topology: default device_id=" << default_device_id;

    BOOST_LOG(debug) << "session_.virtual_display_layout_override has_value: " << session_.virtual_display_layout_override.has_value();
    if (session_.virtual_display_layout_override) {
      BOOST_LOG(debug) << "session_.virtual_display_layout_override value: " << static_cast<int>(*session_.virtual_display_layout_override);
    }
    BOOST_LOG(debug) << "video_config_.virtual_display_layout: " << static_cast<int>(effective_video_config_.virtual_display_layout);
    const auto effective_layout =
      session_.virtual_display_layout_override.value_or(effective_video_config_.virtual_display_layout);
    const auto layout_flags = describe_layout(effective_layout);
    const auto resolved_virtual_device_id = resolve_virtual_device_id(effective_video_config_, session_);
    const std::string topology_device_id =
      resolved_virtual_device_id && !resolved_virtual_device_id->empty() ? *resolved_virtual_device_id : default_device_id;
    bool topology_overridden = false;
    if (session_.virtual_display &&
        session_.virtual_display_topology_snapshot &&
        layout_flags.arrangement != display_helper_integration::VirtualDisplayArrangement::Exclusive) {
      const std::string merged_device_id =
        !session_.virtual_display_device_id.empty() ? session_.virtual_display_device_id : topology_device_id;
      if (!merged_device_id.empty()) {
        auto merged_topology = *session_.virtual_display_topology_snapshot;
        const auto already_present = std::any_of(
          merged_topology.begin(),
          merged_topology.end(),
          [&](const std::vector<std::string> &group) {
            return std::any_of(group.begin(), group.end(), [&](const std::string &device_id) {
              return boost::iequals(device_id, merged_device_id);
            });
          }
        );
        if (!already_present) {
          merged_topology.push_back({merged_device_id});
        }
        if (!merged_topology.empty()) {
          topology.topology = std::move(merged_topology);
          topology_overridden = true;
        }
      }
    }

    if (!topology_overridden && topology.topology.empty() && !default_device_id.empty()) {
      topology.topology = {{default_device_id}};
    }

    if (!layout_flags.isolated) {
      return;
    }

    if (layout_flags.arrangement == display_helper_integration::VirtualDisplayArrangement::ExtendedPrimaryIsolated) {
      const std::string virtual_device_id =
        (resolved_virtual_device_id && !resolved_virtual_device_id->empty()) ? *resolved_virtual_device_id : default_device_id;
      if (virtual_device_id.empty()) {
        return;
      }

      // Primary displays typically want to live at (0,0), so in primary+isolated mode we keep the virtual
      // display at the origin and shift every other display far away so the mouse cannot escape.
      topology.monitor_positions[virtual_device_id] = display_device::Point {0, 0};

      auto devices = platf::display_helper::Coordinator::instance().enumerate_devices(display_device::DeviceEnumerationDetail::Minimal);
      if (!devices) {
        return;
      }

      bool have_non_virtual = false;
      int min_x = 0;
      int min_y = 0;
      for (const auto &device : *devices) {
        if (device.m_device_id.empty() || !device.m_info) {
          continue;
        }
        if (boost::iequals(device.m_device_id, virtual_device_id)) {
          continue;
        }
        const auto &pt = device.m_info->m_origin_point;
        if (!have_non_virtual) {
          min_x = pt.m_x;
          min_y = pt.m_y;
          have_non_virtual = true;
        } else {
          min_x = std::min(min_x, pt.m_x);
          min_y = std::min(min_y, pt.m_y);
        }
      }

      if (!have_non_virtual) {
        return;
      }

      const int dx = kIsolatedVirtualDisplayOffset - min_x;
      const int dy = kIsolatedVirtualDisplayOffset - min_y;
      for (const auto &device : *devices) {
        if (device.m_device_id.empty() || !device.m_info) {
          continue;
        }
        if (boost::iequals(device.m_device_id, virtual_device_id)) {
          continue;
        }
        const auto &pt = device.m_info->m_origin_point;
        topology.monitor_positions[device.m_device_id] = display_device::Point {safe_add_int(pt.m_x, dx), safe_add_int(pt.m_y, dy)};
      }
      return;
    }

    std::string isolated_device_id =
      (resolved_virtual_device_id && !resolved_virtual_device_id->empty()) ? *resolved_virtual_device_id : default_device_id;
    if (isolated_device_id.empty()) {
      return;
    }

    // Capture current positions of all active displays to preserve their arrangement.
    // When the topology is applied, Windows may rearrange displays. By storing all
    // current positions, we ensure non-isolated displays return to their original
    // locations after the topology change.
    auto devices = platf::display_helper::Coordinator::instance().enumerate_devices(display_device::DeviceEnumerationDetail::Minimal);
    if (devices) {
      for (const auto &device : *devices) {
        if (device.m_device_id.empty() || !device.m_info) {
          continue;
        }
        // Skip the device that will be isolated - its position will be set below
        if (boost::iequals(device.m_device_id, isolated_device_id)) {
          continue;
        }
        topology.monitor_positions[device.m_device_id] = device.m_info->m_origin_point;
      }
    }

    topology.monitor_positions[isolated_device_id] = display_device::Point {kIsolatedVirtualDisplayOffset, kIsolatedVirtualDisplayOffset};
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
