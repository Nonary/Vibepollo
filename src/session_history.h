/**
 * @file src/session_history.h
 * @brief Declarations for the session history persistence module.
 *
 * Provides SQLite-backed storage of streaming session metadata, periodic
 * performance samples, and discrete events. A dedicated writer thread
 * serializes all mutations; read queries use a separate read-only connection.
 *
 * Computed metrics (actual FPS, actual bitrate, frame-interval jitter) are
 * maintained in a private per-session aggregator inside this module and are
 * NOT added to session_t::stats or session_info_t.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace session_history {

  // ── Types ──────────────────────────────────────────────────────────

  struct session_metadata_t {
    std::string uuid;
    std::string protocol;  // "rtsp" or "webrtc"
    std::string client_name;
    std::string device_name;
    std::string app_name;
    int width = 0;
    int height = 0;
    int target_fps = 0;
    int target_bitrate_kbps = 0;
    int target_requested_bitrate_kbps = 0;
    std::string codec;
    bool hdr = false;
    int audio_channels = 0;
    // Static host identification captured at session start.
    std::string host_cpu_model;
    std::string host_gpu_model;
  };

  struct session_sample_t {
    std::string session_uuid;
    double timestamp_unix = 0;

    std::uint64_t bytes_sent_total = 0;
    std::uint64_t packets_sent_video = 0;
    std::uint64_t frames_sent = 0;
    std::int64_t last_frame_index = 0;

    std::uint64_t video_dropped = 0;
    std::uint64_t audio_dropped = 0;
    std::int64_t client_reported_losses = 0;
    std::uint32_t idr_requests = 0;
    std::uint32_t ref_invalidations = 0;

    double encode_latency_ms = 0;

    // Computed by the history module's aggregator (not from atomics)
    double actual_fps = 0;
    double actual_bitrate_kbps = 0;
    double frame_interval_jitter_ms = 0;

    // Host system snapshot. -1 indicates "not available on this platform".
    double host_cpu_percent = -1;
    double host_gpu_percent = -1;
    double host_gpu_encoder_percent = -1;
    double host_ram_percent = -1;
    double host_vram_percent = -1;
    double host_cpu_temp_c = -1;
    double host_gpu_temp_c = -1;
  };

  struct session_event_t {
    std::string session_uuid;
    double timestamp_unix = 0;
    std::string event_type;
    std::string payload;  // JSON or empty
  };

  struct session_summary_t {
    std::string uuid;
    std::string protocol;
    std::string client_name;
    std::string device_name;
    std::string app_name;
    int width = 0;
    int height = 0;
    int target_fps = 0;
    int target_bitrate_kbps = 0;
    int target_requested_bitrate_kbps = 0;
    std::string codec;
    bool hdr = false;
    int audio_channels = 0;
    double start_time_unix = 0;
    double end_time_unix = 0;
    double duration_seconds = 0;
    std::string verdict;  // "healthy", "degraded", "failed", "unknown"
    std::string host_cpu_model;
    std::string host_gpu_model;
  };

  struct session_detail_t {
    session_summary_t summary;
    std::vector<session_sample_t> samples;
    std::vector<session_event_t> events;
  };

  struct active_session_t {
    std::string uuid;
    std::string protocol;
    std::string client_name;
    std::string device_name;
    std::string app_name;
    int width = 0;
    int height = 0;
    int target_fps = 0;
    int target_bitrate_kbps = 0;
    std::string codec;
    bool hdr = false;
    double uptime_seconds = 0;

    // Latest computed metrics
    double actual_fps = 0;
    double actual_bitrate_kbps = 0;
    double encode_latency_ms = 0;
    double frame_interval_jitter_ms = 0;

    // Cumulative counters
    std::uint64_t frames_sent = 0;
    std::uint64_t bytes_sent = 0;
    std::int64_t client_reported_losses = 0;
    std::uint32_t idr_requests = 0;
  };

  // ── Lifecycle ──────────────────────────────────────────────────────

  /**
   * @brief Initialize the session history module.
   * @param db_path Filesystem path for the SQLite database file.
   *
   * Opens/creates the database, applies schema migrations, and starts the
   * writer thread. Must be called once during startup.
   */
  void init(const std::string &db_path);

  /**
   * @brief Shut down the session history module.
   *
   * Stops the sampling timer and writer thread, flushes pending writes,
   * and closes database connections.
   */
  void shutdown();

  // ── Session lifecycle (called from stream.cpp / webrtc_stream.cpp) ─

  void begin_session(const session_metadata_t &metadata);
  void end_session(const std::string &uuid);

  // ── Event recording ───────────────────────────────────────────────

  void record_event(const std::string &uuid, const std::string &event_type, const std::string &payload = "");

  // ── Read API (called from confighttp.cpp) ──────────────────────────

  std::vector<session_summary_t> list_sessions(int limit = 25, int offset = 0);
  std::optional<session_detail_t> get_session_detail(const std::string &uuid);
  std::vector<active_session_t> get_active_sessions();
  bool delete_session(const std::string &uuid);

}  // namespace session_history
