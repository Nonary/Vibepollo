/**
 * @file src/webrtc_stream.h
 * @brief Declarations for WebRTC session tracking and frame handoff.
 */
#pragma once

// standard includes
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// local includes
#include "audio.h"
#include "video.h"

namespace webrtc_stream {
  struct SessionOptions {
    bool audio = true;
    bool video = true;
    bool encoded = true;
  };

  struct SessionState {
    std::string id;
    bool audio = true;
    bool video = true;
    bool encoded = true;

    std::uint64_t audio_packets = 0;
    std::uint64_t video_packets = 0;
    std::uint64_t audio_dropped = 0;
    std::uint64_t video_dropped = 0;
    bool has_remote_offer = false;
    std::size_t ice_candidates = 0;

    std::optional<std::chrono::steady_clock::time_point> last_audio_time;
    std::optional<std::chrono::steady_clock::time_point> last_video_time;

    std::size_t last_audio_bytes = 0;
    std::size_t last_video_bytes = 0;
    bool last_video_idr = false;
    std::int64_t last_video_frame_index = 0;
  };

  bool has_active_sessions();

  SessionState create_session(const SessionOptions &options);
  bool close_session(std::string_view id);
  std::optional<SessionState> get_session(std::string_view id);
  std::vector<SessionState> list_sessions();

  void submit_video_packet(video::packet_raw_t &packet);
  void submit_audio_packet(const audio::buffer_t &packet);

  bool set_remote_offer(std::string_view id, const std::string &sdp, const std::string &type);
  bool add_ice_candidate(std::string_view id, std::string mid, int mline_index, std::string candidate);

  std::string get_server_cert_fingerprint();
  std::string get_server_cert_pem();
}  // namespace webrtc_stream
