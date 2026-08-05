/**
 * @file src/stream.h
 * @brief Declarations for the streaming protocols.
 */
#pragma once

// standard includes
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// lib includes
#include <boost/asio.hpp>

// local includes
#include "audio.h"
#include "crypto.h"
#include "thread_safe.h"
#include "video.h"
#include "stream_protocol.h"

namespace rtsp_stream {
  struct launch_session_t;
}

namespace stream {
  constexpr auto VIDEO_STREAM_PORT = 9;
  constexpr auto CONTROL_PORT = 10;
  constexpr auto AUDIO_STREAM_PORT = 11;

  constexpr std::string_view video_format_name(int video_format) {
    switch (video_format) {
      case 0:
        return "H.264";
      case 1:
        return "HEVC";
      case 2:
        return "AV1";
      default:
        return "Unknown";
    }
  }

  struct session_t;

  struct config_t {
    audio::config_t audio;
    video::config_t monitor;

    int packetsize;
    int minRequiredFecPackets;
    int mlFeatureFlags;
    int controlProtocolType;
    int audioQosType;
    int videoQosType;

    uint32_t encryptionFlagsEnabled;

    std::optional<int> gcmap;
    bool gen1_framegen_fix;
    bool gen2_framegen_fix;
    bool frame_generation_enabled;
    bool lossless_scaling_framegen;
    std::string frame_generation_provider;
    std::optional<double> lossless_scaling_target_fps;
    std::optional<int> lossless_scaling_rtss_limit;
  };

  namespace session {
    extern std::atomic_uint running_sessions;
    // Counts RTSP joins through their complete post-session cleanup tail.
    // Observers use this instead of entering blocking session cleanup.
    extern std::atomic_uint teardown_sessions;
    extern std::atomic_uint cleanup_reservations;

    class cleanup_reservation_t {
    public:
      cleanup_reservation_t();
      ~cleanup_reservation_t();

      cleanup_reservation_t(const cleanup_reservation_t &) = delete;
      cleanup_reservation_t &operator=(const cleanup_reservation_t &) = delete;
    };

    struct shared_runtime_finalize_context_t {
      bool ignore_current_rtsp_teardown {false};
      bool ignore_current_webrtc_teardown {false};
      bool apply_deferred_config {true};
      bool force_display_revert_when_idle {false};
      std::optional<std::array<std::uint8_t, 16>> virtual_display_guid_bytes;
    };

    /**
     * These helpers require nvhttp::stream_lifecycle_mutex() to be held.
     */
    bool has_shared_runtime_owner(const shared_runtime_finalize_context_t &context = {});
    void arm_shared_runtime_cleanup(
      std::optional<std::array<std::uint8_t, 16>> virtual_display_guid_bytes = std::nullopt
    );
    void start_shared_platform_if_needed();
    bool finalize_shared_runtime_if_idle(
      std::string_view reason,
      const shared_runtime_finalize_context_t &context = {}
    );

    enum class state_e : int {
      STOPPED,  ///< The session is stopped
      STOPPING,  ///< The session is stopping
      STARTING,  ///< The session is starting
      RUNNING,  ///< The session is running
    };

    std::shared_ptr<session_t> alloc(config_t &config, rtsp_stream::launch_session_t &launch_session);
    std::string uuid(const session_t &session);
    bool uuid_match(const session_t &session, const std::string_view &uuid);
    bool update_device_info(session_t &session, const std::string &name, const crypto::PERM &newPerm);
    int start(session_t &session, const std::string &addr_string);
    void stop(session_t &session);
    void graceful_stop(session_t &session);
    void join(session_t &session);
    state_e state(session_t &session);
    inline bool send(session_t &session, const std::string_view &payload);
  }  // namespace session

  void cancel_paused_display_cleanup();

  struct session_info_t {
    std::string uuid;
    std::string device_name;
    int width;
    int height;
    int fps;
    int encoder_bitrate_kbps;
    int requested_bitrate_kbps;  // Original client-requested wire bitrate (== encoder_bitrate_kbps for clients that don't send maximumBitrateKbps)
    int video_format;  // 0=H.264, 1=HEVC, 2=AV1
    int dynamic_range;  // Encoder bit depth: 0=8-bit, 1=10-bit
    bool hdr;
    bool yuv444;
    int audio_channels;
    std::string stream_gpu_model;
    std::string state;

    // Real-time performance counters
    std::uint64_t frames_sent;
    std::uint64_t packets_sent;
    std::uint64_t bytes_sent;
    std::uint32_t idr_requests;
    std::uint32_t invalidate_ref_count;
    std::int64_t client_reported_losses;
    double encode_latency_ms;  // last frame encode latency in ms
    std::int64_t last_frame_index;
    double uptime_seconds;
  };

  std::vector<session_info_t> get_all_session_info();

  void request_idr_for_all_sessions();

  /**
   * @brief Apply a new encoder bitrate to active streaming sessions at runtime.
   * @param client_uuid Target client UUID; when empty, applies to every active session.
   * @param bitrate_kbps New encoder bitrate in kbps (already clamped by the caller).
   * @return Number of sessions updated.
   */
  int set_bitrate_for_sessions(const std::string &client_uuid, int bitrate_kbps);
}  // namespace stream
