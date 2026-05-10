/**
 * @file src/session_history_sampler.cpp
 * @brief Internal active-session tracking and periodic sampling for session history.
 */

// standard includes
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>
#include <unordered_map>

// local includes
#include "session_history_sampler.h"
#include "host_stats.h"
#include "logging.h"
#include "rtsp.h"
#include "session_history_storage.h"
#include "stream.h"
#include "webrtc_stream.h"
#include "session_history_writer.h"

using namespace std::literals;

namespace session_history::sampler {
  namespace {

    struct aggregator_t {
      double prev_timestamp = 0;
      std::uint64_t prev_frames_sent = 0;
      std::uint64_t prev_bytes_sent = 0;
      std::int64_t prev_losses = 0;

      int welford_n = 0;
      double welford_mean = 0;
      double welford_m2 = 0;

      double last_actual_fps = 0;
      double last_actual_bitrate_kbps = 0;
      double last_jitter_ms = 0;

      bool had_any_losses = false;
      bool in_stall = false;
      int zero_frame_ticks = 0;

      void update(double ts, std::uint64_t frames, std::uint64_t bytes, std::int64_t losses) {
        if (prev_timestamp > 0) {
          const double dt = ts - prev_timestamp;
          if (dt > 0.01) {
            if (frames < prev_frames_sent || bytes < prev_bytes_sent) {
              prev_timestamp = ts;
              prev_frames_sent = frames;
              prev_bytes_sent = bytes;
              prev_losses = losses;
              return;
            }

            const auto dframes = static_cast<double>(frames - prev_frames_sent);
            const auto dbytes = static_cast<double>(bytes - prev_bytes_sent);

            last_actual_fps = dframes / dt;
            last_actual_bitrate_kbps = (dbytes * 8.0) / (dt * 1000.0);

            if (dframes > 0) {
              const double interval_ms = (dt * 1000.0) / dframes;
              ++welford_n;
              const double delta = interval_ms - welford_mean;
              welford_mean += delta / welford_n;
              const double delta2 = interval_ms - welford_mean;
              welford_m2 += delta * delta2;
              last_jitter_ms = welford_n > 1 ? std::sqrt(welford_m2 / (welford_n - 1)) : 0;
            }
          }
        }
        prev_timestamp = ts;
        prev_frames_sent = frames;
        prev_bytes_sent = bytes;
        prev_losses = losses;
      }
    };

    struct pending_event_t {
      std::string event_type;
      std::string payload;
    };

    struct aggregated_sample_t {
      double actual_fps = 0;
      double actual_bitrate_kbps = 0;
      double frame_interval_jitter_ms = 0;
      double encode_latency_ms = 0;
    };

    std::thread g_sampler_thread;
    std::atomic<bool> g_running {false};

    std::mutex g_aggregators_mutex;
    std::unordered_map<std::string, aggregator_t> g_aggregators;

    std::mutex g_active_mutex;
    std::unordered_map<std::string, session_metadata_t> g_active_sessions;

    constexpr auto SAMPLE_INTERVAL = std::chrono::seconds(2);

    std::vector<pending_event_t> detect_events(aggregator_t &agg, std::int64_t current_losses, std::uint64_t current_frames) {
      std::vector<pending_event_t> events;

      if (!agg.had_any_losses && current_losses > 0) {
        agg.had_any_losses = true;
        events.push_back({"first_drop_in_session", {}});
      }

      if (current_losses > agg.prev_losses && (current_losses - agg.prev_losses) >= 5) {
        events.push_back({"drop_burst", std::to_string(current_losses - agg.prev_losses)});
      }

      const auto new_frames = current_frames >= agg.prev_frames_sent ? current_frames - agg.prev_frames_sent : 0;
      if (new_frames == 0) {
        ++agg.zero_frame_ticks;
        if (!agg.in_stall && agg.zero_frame_ticks >= 2) {
          agg.in_stall = true;
          events.push_back({"stall", {}});
        }
      } else {
        agg.zero_frame_ticks = 0;
        if (agg.in_stall) {
          agg.in_stall = false;
          events.push_back({"recovery", {}});
        }
      }

      return events;
    }

    aggregated_sample_t update_aggregator(
      const std::string &session_uuid,
      double ts,
      std::uint64_t frames_sent,
      std::uint64_t bytes_sent_total,
      std::int64_t client_losses,
      std::vector<pending_event_t> &pending_events) {
      std::lock_guard lk {g_aggregators_mutex};
      auto &agg = g_aggregators[session_uuid];
      pending_events = detect_events(agg, client_losses, frames_sent);
      agg.update(ts, frames_sent, bytes_sent_total, client_losses);
      return {
        .actual_fps = agg.last_actual_fps,
        .actual_bitrate_kbps = agg.last_actual_bitrate_kbps,
        .frame_interval_jitter_ms = agg.last_jitter_ms,
      };
    }

    void populate_host_snapshot(session_sample_t &sample, const platf::host_stats_t &host) {
      sample.host_cpu_percent = host.cpu_percent;
      sample.host_gpu_percent = host.gpu_percent;
      sample.host_gpu_encoder_percent = host.gpu_encoder_percent;
      sample.host_cpu_temp_c = host.cpu_temp_c;
      sample.host_gpu_temp_c = host.gpu_temp_c;
      sample.host_net_rx_bps = host.net_rx_bps;
      sample.host_net_tx_bps = host.net_tx_bps;

      if (host.ram_total_bytes > 0) {
        sample.host_ram_percent = std::clamp(
          static_cast<double>(host.ram_used_bytes) * 100.0 / static_cast<double>(host.ram_total_bytes),
          0.0,
          100.0);
      }

      if (host.vram_total_bytes > 0) {
        sample.host_vram_percent = std::clamp(
          static_cast<double>(host.vram_used_bytes) * 100.0 / static_cast<double>(host.vram_total_bytes),
          0.0,
          100.0);
      }
    }

    void sample_rtsp_sessions(double ts, const platf::host_stats_t &host) {
      auto snapshot = stream::get_all_session_info();
      std::unordered_map<std::string, session_metadata_t> active_copy;
      {
        std::lock_guard lk {g_active_mutex};
        active_copy = g_active_sessions;
      }

      for (const auto &info : snapshot) {
        if (!active_copy.contains(info.uuid)) {
          continue;
        }

        std::vector<pending_event_t> pending_events;
        const auto aggregated = update_aggregator(
          info.uuid,
          ts,
          info.frames_sent,
          info.bytes_sent,
          info.client_reported_losses,
          pending_events);

        for (const auto &evt : pending_events) {
          session_event_t event;
          event.session_uuid = info.uuid;
          event.timestamp_unix = ts;
          event.event_type = evt.event_type;
          event.payload = evt.payload;
          (void) writer::enqueue_event(event);
        }

        session_sample_t sample;
        sample.session_uuid = info.uuid;
        sample.timestamp_unix = ts;
        sample.bytes_sent_total = info.bytes_sent;
        sample.packets_sent_video = info.packets_sent;
        sample.frames_sent = info.frames_sent;
        sample.last_frame_index = info.last_frame_index;
        sample.client_reported_losses = info.client_reported_losses;
        sample.idr_requests = info.idr_requests;
        sample.ref_invalidations = info.invalidate_ref_count;
        sample.encode_latency_ms = info.encode_latency_ms;
        sample.actual_fps = aggregated.actual_fps;
        sample.actual_bitrate_kbps = aggregated.actual_bitrate_kbps;
        sample.frame_interval_jitter_ms = aggregated.frame_interval_jitter_ms;
        populate_host_snapshot(sample, host);
        (void) writer::enqueue_sample(std::move(sample));
      }
    }

    void sample_webrtc_sessions(double ts, const platf::host_stats_t &host) {
      auto snapshot = webrtc_stream::list_sessions();
      std::unordered_map<std::string, session_metadata_t> active_copy;
      {
        std::lock_guard lk {g_active_mutex};
        active_copy = g_active_sessions;
      }

      for (const auto &info : snapshot) {
        if (!active_copy.contains(info.id)) {
          continue;
        }

        double last_video_age_ms = 0;
        if (info.last_video_time) {
          last_video_age_ms = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - *info.last_video_time)
                                .count();
        }

        const std::uint64_t frames_sent = static_cast<std::uint64_t>(info.last_video_frame_index > 0 ? info.last_video_frame_index : 0);
        const std::uint64_t bytes_sent_total = info.video_bytes_total + info.audio_bytes_total;
        const std::int64_t client_losses = static_cast<std::int64_t>(info.video_dropped);

        std::vector<pending_event_t> pending_events;
        const auto aggregated = update_aggregator(
          info.id,
          ts,
          frames_sent,
          bytes_sent_total,
          client_losses,
          pending_events);

        for (const auto &evt : pending_events) {
          session_event_t event;
          event.session_uuid = info.id;
          event.timestamp_unix = ts;
          event.event_type = evt.event_type;
          event.payload = evt.payload;
          (void) writer::enqueue_event(event);
        }

        session_sample_t sample;
        sample.session_uuid = info.id;
        sample.timestamp_unix = ts;
        sample.bytes_sent_total = bytes_sent_total;
        sample.packets_sent_video = info.video_packets;
        sample.frames_sent = frames_sent;
        sample.last_frame_index = info.last_video_frame_index;
        sample.video_dropped = info.video_dropped;
        sample.audio_dropped = info.audio_dropped;
        sample.client_reported_losses = client_losses;
        sample.encode_latency_ms = last_video_age_ms;
        sample.actual_fps = aggregated.actual_fps;
        sample.actual_bitrate_kbps = aggregated.actual_bitrate_kbps;
        sample.frame_interval_jitter_ms = aggregated.frame_interval_jitter_ms;
        populate_host_snapshot(sample, host);
        (void) writer::enqueue_sample(std::move(sample));
      }
    }

    void sampler_loop() {
      while (g_running.load(std::memory_order_acquire)) {
        bool has_active = false;
        {
          std::lock_guard lk {g_active_mutex};
          has_active = !g_active_sessions.empty();
        }

        if (has_active && writer::is_available()) {
          const double ts = storage::now_unix();
          const auto host = host_stats::latest();
          sample_rtsp_sessions(ts, host);
          sample_webrtc_sessions(ts, host);
        }

        std::this_thread::sleep_for(SAMPLE_INTERVAL);
      }
    }

  }  // namespace

  void init() {
    g_running.store(true, std::memory_order_release);
    g_sampler_thread = std::thread {sampler_loop};
  }

  void shutdown() {
    g_running.store(false, std::memory_order_release);
    if (g_sampler_thread.joinable()) {
      g_sampler_thread.join();
    }

    {
      std::lock_guard lk {g_aggregators_mutex};
      g_aggregators.clear();
    }
    {
      std::lock_guard lk {g_active_mutex};
      g_active_sessions.clear();
    }
  }

  void register_session(const session_metadata_t &metadata) {
    std::lock_guard lk {g_active_mutex};
    g_active_sessions[metadata.uuid] = metadata;
  }

  void unregister_session(const std::string &uuid) {
    {
      std::lock_guard lk {g_active_mutex};
      g_active_sessions.erase(uuid);
    }
    {
      std::lock_guard lk {g_aggregators_mutex};
      g_aggregators.erase(uuid);
    }
  }

  bool is_active(const std::string &uuid) {
    std::lock_guard lk {g_active_mutex};
    return g_active_sessions.contains(uuid);
  }

  std::vector<active_session_t> get_active_sessions() {
    std::vector<active_session_t> result;
    std::unordered_map<std::string, session_metadata_t> active_copy;
    {
      std::lock_guard lk {g_active_mutex};
      active_copy = g_active_sessions;
    }

    for (const auto &info : stream::get_all_session_info()) {
      auto it = active_copy.find(info.uuid);
      if (it == active_copy.end()) {
        continue;
      }

      active_session_t as;
      as.uuid = info.uuid;
      as.protocol = "rtsp";
      as.client_name = it->second.client_name;
      as.device_name = info.device_name;
      as.app_name = it->second.app_name;
      as.width = info.width;
      as.height = info.height;
      as.target_fps = info.fps;
      as.encoder_bitrate_kbps = info.encoder_bitrate_kbps;
      as.requested_bitrate_kbps = info.requested_bitrate_kbps ? info.requested_bitrate_kbps : info.encoder_bitrate_kbps;
      as.codec = !it->second.codec.empty() ? it->second.codec : std::string(stream::video_format_name(info.video_format));
      as.hdr = info.dynamic_range > 0;
      as.yuv444 = info.yuv444;
      as.uptime_seconds = info.uptime_seconds;
      as.encode_latency_ms = info.encode_latency_ms;
      as.frames_sent = info.frames_sent;
      as.bytes_sent = info.bytes_sent;
      as.client_reported_losses = info.client_reported_losses;
      as.idr_requests = info.idr_requests;

      {
        std::lock_guard lk {g_aggregators_mutex};
        auto agg_it = g_aggregators.find(info.uuid);
        if (agg_it != g_aggregators.end()) {
          as.actual_fps = agg_it->second.last_actual_fps;
          as.actual_bitrate_kbps = agg_it->second.last_actual_bitrate_kbps;
          as.frame_interval_jitter_ms = agg_it->second.last_jitter_ms;
        }
      }

      result.push_back(std::move(as));
      active_copy.erase(info.uuid);
    }

    for (const auto &ws : webrtc_stream::list_sessions()) {
      auto it = active_copy.find(ws.id);
      if (it == active_copy.end()) {
        continue;
      }

      active_session_t as;
      as.uuid = ws.id;
      as.protocol = "webrtc";
      as.client_name = ws.client_name.value_or("");
      as.device_name = "";
      as.app_name = it->second.app_name;
      as.width = ws.width.value_or(0);
      as.height = ws.height.value_or(0);
      as.target_fps = ws.fps.value_or(0);
      as.encoder_bitrate_kbps = ws.bitrate_kbps.value_or(0);
      as.requested_bitrate_kbps = ws.bitrate_kbps.value_or(0);
      as.codec = !it->second.codec.empty() ? it->second.codec : stream::canonical_codec_name(ws.codec.value_or(""));
      as.hdr = ws.hdr.value_or(false);
      as.yuv444 = ws.yuv444.value_or(false);
      as.frames_sent = static_cast<std::uint64_t>(ws.last_video_frame_index > 0 ? ws.last_video_frame_index : 0);
      as.bytes_sent = ws.video_bytes_total + ws.audio_bytes_total;
      as.client_reported_losses = static_cast<std::int64_t>(ws.video_dropped);

      {
        std::lock_guard lk {g_aggregators_mutex};
        auto agg_it = g_aggregators.find(ws.id);
        if (agg_it != g_aggregators.end()) {
          as.actual_fps = agg_it->second.last_actual_fps;
          as.actual_bitrate_kbps = agg_it->second.last_actual_bitrate_kbps;
          as.frame_interval_jitter_ms = agg_it->second.last_jitter_ms;
        }
      }

      result.push_back(std::move(as));
    }

    return result;
  }

}  // namespace session_history::sampler
