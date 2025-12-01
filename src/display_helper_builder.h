/**
 * @file src/display_helper_builder.h
 * @brief Builder types used to construct display helper apply requests.
 */
#pragma once

#include "src/rtsp.h"

#include <display_device/types.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace display_helper_integration {

  /**
   * @brief Describes the action requested from the display helper.
   */
  enum class DisplayApplyAction {
    Skip,   ///< Do not dispatch anything to the helper.
    Apply,  ///< Apply the provided configuration.
    Revert  ///< Revert the helper state.
  };

  /**
   * @brief Desired layout for virtual displays.
   */
  enum class VirtualDisplayArrangement {
    Exclusive,
    Extended,
    ExtendedPrimary,
    ExtendedIsolated,
    ExtendedPrimaryIsolated
  };

  /**
   * @brief Snapshot of overrides that should be recorded for active sessions.
   */
  struct ActiveSessionState {
    std::optional<std::string> device_id_override;
    std::optional<int> fps_override;
    std::optional<int> width_override;
    std::optional<int> height_override;
    std::optional<bool> virtual_display_override;
    std::optional<int> framegen_refresh_override;
  };

  /**
   * @brief Definition of the desired topology and monitor placement.
   */
  struct DisplayTopologyDefinition {
    std::vector<std::vector<std::string>> topology;
    std::map<std::string, display_device::Point> monitor_positions;
  };

  /**
   * @brief Concrete request payload built for dispatching to the helper.
   */
  struct DisplayApplyRequest {
    DisplayApplyAction action {DisplayApplyAction::Skip};
    std::optional<display_device::SingleDisplayConfiguration> configuration;
    ActiveSessionState session_overrides {};
    bool enable_virtual_display_watchdog {false};
    bool attach_hdr_toggle_flag {false};
    const rtsp_stream::launch_session_t *session {nullptr};
    DisplayTopologyDefinition topology {};
    std::optional<VirtualDisplayArrangement> virtual_display_arrangement;
  };

  /**
   * @brief Helper used to assemble DisplayApplyRequest instances.
   */
  class DisplayApplyBuilder {
   public:
    DisplayApplyBuilder &set_session(const rtsp_stream::launch_session_t &session);
    DisplayApplyBuilder &set_action(DisplayApplyAction action);
    DisplayApplyBuilder &set_configuration(const display_device::SingleDisplayConfiguration &config);
    DisplayApplyBuilder &clear_configuration();
    DisplayApplyBuilder &set_virtual_display_watchdog(bool enable);
    DisplayApplyBuilder &set_hdr_toggle_flag(bool enable);
    DisplayApplyBuilder &set_topology(const DisplayTopologyDefinition &topology);
    DisplayTopologyDefinition &mutable_topology();
    ActiveSessionState &mutable_session_overrides();
    DisplayApplyBuilder &set_virtual_display_arrangement(std::optional<VirtualDisplayArrangement> arrangement);

    [[nodiscard]] DisplayApplyRequest build() const;

  private:
    const rtsp_stream::launch_session_t *session_ {nullptr};
    DisplayApplyAction action_ {DisplayApplyAction::Skip};
    std::optional<display_device::SingleDisplayConfiguration> configuration_;
    ActiveSessionState session_overrides_ {};
    DisplayTopologyDefinition topology_ {};
    bool enable_virtual_display_watchdog_ {false};
    bool attach_hdr_toggle_flag_ {false};
    std::optional<VirtualDisplayArrangement> virtual_display_arrangement_;
  };

}  // namespace display_helper_integration
