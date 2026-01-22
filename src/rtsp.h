/**
 * @file src/rtsp.h
 * @brief Declarations for RTSP streaming.
 */
#pragma once

// standard includes
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <list>
#include <optional>
#include <string>
#include <vector>

#include "config.h"
// local includes
#include "crypto.h"
#include "thread_safe.h"

namespace rtsp_stream {
  constexpr auto RTSP_SETUP_PORT = 21;

  struct launch_session_t {
    uint32_t id;

    crypto::aes_t gcm_key;
    crypto::aes_t iv;

    std::string av_ping_payload;
    uint32_t control_connect_data;

    bool host_audio;
    std::string unique_id;
    std::string client_uuid;
    std::string client_name;
    std::string device_name;
    std::optional<std::string> hdr_profile;
    int width;
    int height;
    int fps;
    int gcmap;
    int appid;

    struct app_metadata_t {
      std::string id;
      std::string name;
      bool virtual_screen;
      bool has_command;
      bool has_playnite;
      bool playnite_fullscreen;
    };

    std::optional<app_metadata_t> app_metadata;
    int surround_info;
    std::string surround_params;
    bool enable_hdr;
    bool enable_sops;
    bool client_display_mode_override;
    bool client_requests_virtual_display;
    bool virtual_display;
    bool virtual_display_failed;
    bool virtual_display_detach_with_app;
    std::optional<config::video_t::virtual_display_mode_e> virtual_display_mode_override;
    std::optional<config::video_t::virtual_display_layout_e> virtual_display_layout_override;
    std::optional<config::video_t::dd_t::config_option_e> dd_config_option_override;
    std::optional<std::string> output_name_override;
    std::array<std::uint8_t, 16> virtual_display_guid_bytes {};
    std::string virtual_display_device_id;
    std::optional<std::chrono::steady_clock::time_point> virtual_display_ready_since;
    std::optional<std::vector<std::vector<std::string>>> virtual_display_topology_snapshot;
    bool gen1_framegen_fix;
    bool gen2_framegen_fix;
    bool lossless_scaling_framegen;
    std::optional<int> framegen_refresh_rate;
    std::string frame_generation_provider;
    std::optional<int> lossless_scaling_target_fps;
    std::optional<int> lossless_scaling_rtss_limit;

    std::optional<crypto::cipher::gcm_t> rtsp_cipher;
    std::string rtsp_url_scheme;
    uint32_t rtsp_iv_counter;

  };

  void launch_session_raise(std::shared_ptr<launch_session_t> launch_session);

  /**
   * @brief Clear state for the specified launch session.
   * @param launch_session_id The ID of the session to clear.
   */
  void launch_session_clear(uint32_t launch_session_id);

  /**
   * @brief Get the number of active sessions.
   * @return Count of active sessions.
   */
  int session_count();

  /**
   * @brief Terminates all running streaming sessions.
   */
  void terminate_sessions();

  /**
   * @brief Get the client UUIDs for all active sessions.
   */
  std::list<std::string> get_all_session_client_uuids();

  /**
   * @brief Stop any active sessions for a given client UUID.
   * @return True if one or more sessions were stopped.
   */
  bool disconnect_client_sessions(const std::string &client_uuid);

  /**
   * @brief Runs the RTSP server loop.
   */
  void start();
}  // namespace rtsp_stream
