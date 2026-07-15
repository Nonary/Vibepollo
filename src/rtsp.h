/**
 * @file src/rtsp.h
 * @brief Declarations for RTSP streaming.
 */
#pragma once

// standard includes
#include "config.h"
#include "framegen_policy.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <list>
#include <map>
#include <memory>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
// local includes
#include "crypto.h"
#include "thread_safe.h"

#ifdef _WIN32
  #include <winsock2.h>
  #include <windows.h>
#endif

// Resolve circular dependencies
namespace stream {
  struct session_t;
}

namespace rtsp_stream {
  constexpr auto RTSP_SETUP_PORT = 21;

  struct launch_session_t {
    uint32_t id;

    crypto::aes_t gcm_key;
    crypto::aes_t iv;

    std::string av_ping_payload;
    uint32_t control_connect_data;

    std::string device_name;
    std::string unique_id;
    std::string client_uuid;
    std::string client_name;
    std::optional<std::string> hdr_profile;
    crypto::PERM perm;
    int appid;

    bool input_only;
    bool host_audio;
    int width;
    int height;
    int fps;
    int gcmap;

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
    bool continuous_audio;
    bool enable_hdr;
    // Resolved global/per-client policy, independent of the client's HDR marker.
    bool prefer_sdr_10bit = false;
    // Explicit HDR-off override. Unlike prefer_sdr_10bit, this does not request Main10.
    bool force_sdr = false;
    bool enable_sops;
    bool client_display_mode_override;
    bool client_requests_virtual_display;
    bool virtual_display;
    uint32_t scale_factor;
    bool virtual_display_failed;
    bool virtual_display_detach_with_app;
    std::optional<config::video_t::virtual_display_mode_e> virtual_display_mode_override;
    std::optional<config::video_t::virtual_display_layout_e> virtual_display_layout_override;
    std::optional<config::video_t::dd_t::config_option_e> dd_config_option_override;
    std::optional<std::string> output_name_override;
    bool display_config_preapplied = false;
    std::array<std::uint8_t, 16> virtual_display_guid_bytes {};
    std::string virtual_display_device_id;
    std::optional<std::chrono::steady_clock::time_point> virtual_display_ready_since;
    bool virtual_display_recreated_on_demand = false;
    bool virtual_display_needs_resume_apply = false;
    std::optional<std::vector<std::vector<std::string>>> virtual_display_topology_snapshot;

    /// @brief Pre-virtual-display device refresh rates captured before VD creation.
    /// Maps device_id to {numerator, denominator} of the original refresh rate.
    std::optional<std::map<std::string, std::pair<unsigned int, unsigned int>>> pre_virtual_display_refresh_rates;
    bool gen1_framegen_fix;
    bool gen2_framegen_fix;
    bool frame_generation_enabled = false;
    bool lossless_scaling_framegen;
    std::optional<int> framegen_refresh_rate;
    int framegen_refresh_multiplier = 1;
    std::string frame_generation_provider;
    std::optional<double> lossless_scaling_target_fps;
    std::optional<int> lossless_scaling_rtss_limit;

    std::optional<crypto::cipher::gcm_t> rtsp_cipher;
    std::string rtsp_url_scheme;
    uint32_t rtsp_iv_counter;

    std::list<crypto::command_entry_t> client_do_cmds;
    std::list<crypto::command_entry_t> client_undo_cmds;

    std::string client_cert;

#ifdef _WIN32
    enum class display_helper_gate_status_e : uint8_t {
      proceed,  ///< Verified/ready (or no-op)
      proceed_gaveup,  ///< Unknown/unavailable/timeout
      abort_failed  ///< Verified failure (capture proceeds anyway; logged)
    };

    /// Soft gate: capture start waits (bounded) for the display helper's apply
    /// verification so the first frames aren't grabbed mid-modeset.
    std::shared_future<display_helper_gate_status_e> display_helper_gate;
#endif

    /**
     * @brief Build an isolated copy for the background RTSP startup worker.
     *
     * stream::session::alloc()/start() run on the startup thread while the io_context
     * thread still owns and reuses the original launch session (reserve_launch_session,
     * respond() cipher/IV, expiry). The worker must therefore operate on this clone
     * rather than the live original. launch_session_t is non-copyable (the move-only
     * rtsp_cipher), so the fields are copied explicitly.
     *
     * Contract: copy every field that stream::session::alloc() or the cmd_announce()
     * startup lambda reads. If you add such a field, copy it here too -- the
     * RtspStartupSnapshot tests guard this (a dropped `perm` caused Vibepollo #280,
     * where streaming sessions ran at PERM::_no and silently discarded all input).
     */
    [[nodiscard]] std::shared_ptr<launch_session_t> clone_for_startup() const;
  };

  inline bool effective_hdr_requested(const launch_session_t &session) {
    return session.enable_hdr && !session.prefer_sdr_10bit && !session.force_sdr;
  }

  inline bool framegen_capture_fix_enabled(const launch_session_t &session) {
    return session.gen1_framegen_fix || session.gen2_framegen_fix;
  }

  inline int framegen_refresh_multiplier(const launch_session_t &session) {
    if (!session.framegen_refresh_rate || *session.framegen_refresh_rate <= 0) {
      return 1;
    }
    return session.framegen_refresh_multiplier > 1 ? session.framegen_refresh_multiplier : 1;
  }

  inline int saturating_refresh_fps(int fps, int multiplier) {
    return framegen::saturating_refresh_fps(fps, multiplier);
  }

  inline framegen::stream_start_policy_t make_framegen_stream_start_policy(
    const launch_session_t &session,
    std::optional<int> lossless_rtss_limit,
    std::string_view capture_mode,
    bool auto_capture_uses_wgc,
    bool auto_virtual_framegen_limiter,
    int virtual_display_refresh_multiplier
  ) {
    return framegen::make_stream_start_policy({
      .fps = session.fps,
      .frame_generation_enabled = session.frame_generation_enabled,
      .gen1_framegen_fix = session.gen1_framegen_fix,
      .gen2_framegen_fix = session.gen2_framegen_fix,
      .lossless_scaling_framegen = session.lossless_scaling_framegen,
      .lossless_rtss_limit = lossless_rtss_limit,
      .frame_generation_provider = session.frame_generation_provider,
      .uses_virtual_display = session.virtual_display,
      .capture_mode = std::string(capture_mode),
      .auto_capture_uses_wgc = auto_capture_uses_wgc,
      .auto_virtual_framegen_limiter = auto_virtual_framegen_limiter,
      .virtual_display_refresh_multiplier = virtual_display_refresh_multiplier,
    });
  }

  void launch_session_raise(std::shared_ptr<launch_session_t> launch_session);

  /**
   * @brief Clear state for the specified launch session.
   * @param launch_session_id The ID of the session to clear.
   */
  void launch_session_clear(uint32_t launch_session_id);
  /**
   * @brief Publish whether an HDR stream launch is pending before the RTSP session is active.
   *
   * Vulkan applications can query swapchain formats immediately at process startup. The Vulkan HDR
   * implicit layer therefore needs HDR stream intent before the launched app reaches the RTSP
   * ANNOUNCE path that creates the active streaming session.
   */
  void set_vulkan_hdr_layer_pending_stream(bool active);

  /**
   * @brief Get the number of active sessions.
   * @return Count of active sessions.
   */
  int session_count();

  std::shared_ptr<stream::session_t>
    find_session(const std::string_view &uuid);

  std::list<std::string>
    get_all_session_uuids();

  std::vector<std::shared_ptr<stream::session_t>>
    get_sessions_snapshot();

  /**
   * @brief Terminates all running streaming sessions.
   */
  void terminate_sessions();
  void terminate_sessions_by_cert(std::string_view cert);

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
