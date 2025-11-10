/**
 * @file src/platform/windows/display_helper_request_helpers.h
 */
#pragma once

#include "src/config.h"
#include "src/display_helper_builder.h"
#include "src/rtsp.h"

#include <optional>

namespace display_helper_integration::helpers {

  /**
   * @brief Configures builder instances with session-derived display settings.
   */
  class SessionDisplayConfigurationHelper {
   public:
    SessionDisplayConfigurationHelper(const config::video_t &video_config, const rtsp_stream::launch_session_t &session);

    /**
     * @brief Populate the provided builder with configuration data.
     * @return True if a request should be dispatched; false if configuration parsing failed.
     */
    bool configure(DisplayApplyBuilder &builder) const;

   private:
    bool configure_virtual_display(
      DisplayApplyBuilder &builder,
      config::video_t::virtual_display_layout_e layout,
      int effective_width,
      int effective_height,
      int display_fps
    ) const;
    bool configure_standard(
      DisplayApplyBuilder &builder,
      config::video_t::virtual_display_layout_e layout,
      int effective_width,
      int effective_height,
      int display_fps
    ) const;

    const config::video_t &video_config_;
    const rtsp_stream::launch_session_t &session_;
  };

  /**
   * @brief Captures monitor topology/position hints for the helper.
   */
  class SessionMonitorPositionHelper {
   public:
    SessionMonitorPositionHelper(const config::video_t &video_config, const rtsp_stream::launch_session_t &session);
    void configure(DisplayApplyBuilder &builder) const;

   private:
    const config::video_t &video_config_;
    const rtsp_stream::launch_session_t &session_;
  };

  /**
   * @brief Convenience helper that builds a DisplayApplyRequest from config/session data.
   */
  [[nodiscard]] std::optional<DisplayApplyRequest> build_request_from_session(const config::video_t &video_config, const rtsp_stream::launch_session_t &session);

}  // namespace display_helper_integration::helpers
