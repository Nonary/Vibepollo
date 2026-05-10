/**
 * @file src/session_history.cpp
 * @brief SQLite-backed session history persistence, sampling, and computed metrics.
 *
 * All database mutations go through a dedicated writer thread fed by a
 * thread-safe queue. A periodic sampling timer snapshots active session
 * counters and computes derived metrics (FPS, bitrate, jitter) via a
 * private per-session aggregator. Read queries use a separate read-only
 * connection for zero writer contention.
 */

// standard includes
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <mutex>
#include <system_error>
#include <thread>
#include <unordered_map>

// local includes
#include "session_history.h"
#include "session_history_storage.h"
#include "host_stats.h"
#include "logging.h"
#include "rtsp.h"
#include "stream.h"
#include "webrtc_stream.h"

using namespace std::literals;

namespace session_history {
  namespace {

    // Storage/schema/SQL helpers live in session_history_storage.cpp.

    // ── Write command variants ─────────────────────────────────────

    enum class cmd_type {
      begin_session,
      end_session,
      delete_session,
      insert_sample,
      insert_event,
      prune,
      stop
    };

    struct write_cmd_t {
      cmd_type type;
      session_metadata_t metadata;
      session_sample_t sample;
      session_event_t event;
      std::string uuid;
      std::shared_ptr<std::promise<bool>> completion;
    };

    // ── Per-session aggregator (private, not in session_t) ─────────

    struct aggregator_t {
      double prev_timestamp = 0;
      std::uint64_t prev_frames_sent = 0;
      std::uint64_t prev_bytes_sent = 0;
      std::int64_t prev_losses = 0;

      // Welford online variance for frame-interval jitter
      int welford_n = 0;
      double welford_mean = 0;
      double welford_m2 = 0;

      double last_actual_fps = 0;
      double last_actual_bitrate_kbps = 0;
      double last_jitter_ms = 0;

      // Event detection state
      bool had_any_losses = false;      // first_drop detection
      bool in_stall = false;            // stall/recovery detection
      int zero_frame_ticks = 0;         // consecutive ticks with 0 new frames

      void update(double ts, std::uint64_t frames, std::uint64_t bytes, std::int64_t losses) {
        if (prev_timestamp > 0) {
          double dt = ts - prev_timestamp;
          if (dt > 0.01) {
            if (frames < prev_frames_sent || bytes < prev_bytes_sent) {
              prev_timestamp = ts;
              prev_frames_sent = frames;
              prev_bytes_sent = bytes;
              prev_losses = losses;
              return;
            }

            auto dframes = static_cast<double>(frames - prev_frames_sent);
            auto dbytes = static_cast<double>(bytes - prev_bytes_sent);

            last_actual_fps = dframes / dt;
            last_actual_bitrate_kbps = (dbytes * 8.0) / (dt * 1000.0);

            // Welford update on inter-sample frame interval (ms per frame)
            if (dframes > 0) {
              double interval_ms = (dt * 1000.0) / dframes;
              ++welford_n;
              double delta = interval_ms - welford_mean;
              welford_mean += delta / welford_n;
              double delta2 = interval_ms - welford_mean;
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

    // ── Module state ───────────────────────────────────────────────

    storage::db_ptr g_write_db;
    storage::db_ptr g_read_db;
    std::mutex g_read_mutex;

    // Thread-safe write queue (simple mutex + condition_variable + vector)
    std::mutex g_queue_mutex;
    std::condition_variable g_queue_cv;
    std::vector<write_cmd_t> g_queue;

    std::thread g_writer_thread;
    std::thread g_sampler_thread;
    std::atomic<bool> g_running {false};

    // Per-session aggregators. Mutated by the sampler thread, by the caller of
    // end_session(), and read by the HTTP handler thread (get_active_sessions).
    // Always access under g_aggregators_mutex.
    std::mutex g_aggregators_mutex;
    std::unordered_map<std::string, aggregator_t> g_aggregators;

    // Track active session UUIDs for the /active endpoint
    std::mutex g_active_mutex;
    std::unordered_map<std::string, session_metadata_t> g_active_sessions;

    constexpr int MAX_HISTORY_SESSIONS = 50;
    constexpr auto SAMPLE_INTERVAL = std::chrono::seconds(2);
    constexpr std::size_t MAX_PENDING_WRITE_COMMANDS = 4096;
    constexpr int DEFAULT_DETAIL_SAMPLE_LIMIT = 1800;
    constexpr int DEFAULT_DETAIL_EVENT_LIMIT = 500;
    constexpr int MAX_SAMPLES_PER_SESSION = 7200;
    constexpr int MAX_EVENTS_PER_SESSION = 2000;
    constexpr int SESSION_HISTORY_SCHEMA_VERSION = 4;
    constexpr auto DELETE_WAIT_TIMEOUT = std::chrono::seconds(5);

    // ── Queue helpers ──────────────────────────────────────────────

    bool enqueue(write_cmd_t cmd) {
      {
        std::lock_guard lk {g_queue_mutex};
        if (!g_running.load(std::memory_order_acquire) || !g_write_db) {
          return false;
        }
        if (cmd.type == cmd_type::insert_sample && g_queue.size() >= MAX_PENDING_WRITE_COMMANDS) {
          BOOST_LOG(warning) << "session_history: dropping sample because writer queue is full";
          return false;
        }
        g_queue.push_back(std::move(cmd));
      }
      g_queue_cv.notify_one();
      return true;
    }

    std::vector<write_cmd_t> drain_queue() {
      std::vector<write_cmd_t> batch;
      {
        std::unique_lock lk {g_queue_mutex};
        g_queue_cv.wait_for(lk, 500ms, [] { return !g_queue.empty(); });
        batch.swap(g_queue);
      }
      return batch;
    }

    // ── Writer thread ──────────────────────────────────────────────

    bool apply_write_cmd(sqlite3 *db, write_cmd_t &cmd, std::vector<std::pair<std::shared_ptr<std::promise<bool>>, bool>> &completions) {
      switch (cmd.type) {
        case cmd_type::begin_session:
          return storage::process_begin(db, cmd.metadata);
        case cmd_type::end_session:
          return storage::process_end(db, cmd.uuid);
        case cmd_type::delete_session: {
          auto result = storage::process_delete(db, cmd.uuid);
          if (cmd.completion) {
            completions.emplace_back(cmd.completion, result == storage::delete_apply_e::deleted);
          }
          return result != storage::delete_apply_e::failed;
        }
        case cmd_type::insert_sample:
          return storage::process_sample(db, cmd.sample, MAX_SAMPLES_PER_SESSION);
        case cmd_type::insert_event:
          return storage::process_event(db, cmd.event, MAX_EVENTS_PER_SESSION);
        case cmd_type::prune:
          return storage::process_prune(db, MAX_HISTORY_SESSIONS);
        case cmd_type::stop:
          return true;
      }
      return true;
    }

    void resolve_completions(std::vector<std::pair<std::shared_ptr<std::promise<bool>>, bool>> &completions, bool committed) {
      for (auto &[completion, result] : completions) {
        if (completion) {
          completion->set_value(committed && result);
        }
      }
    }

    bool process_batch(std::vector<write_cmd_t> &batch) {
      if (batch.empty()) {
        return true;
      }

      std::vector<std::pair<std::shared_ptr<std::promise<bool>>, bool>> completions;
      if (!storage::exec(g_write_db.get(), "BEGIN TRANSACTION")) {
        BOOST_LOG(error) << "session_history: failed to begin write transaction";
        resolve_completions(completions, false);
        return false;
      }

      bool ok = true;
      for (auto &cmd : batch) {
        if (!apply_write_cmd(g_write_db.get(), cmd, completions)) {
          ok = false;
          break;
        }
      }

      if (!ok) {
        storage::exec(g_write_db.get(), "ROLLBACK");
        resolve_completions(completions, false);
        return false;
      }

      const bool committed = storage::exec(g_write_db.get(), "COMMIT");
      if (!committed) {
        BOOST_LOG(error) << "session_history: failed to commit write transaction";
        storage::exec(g_write_db.get(), "ROLLBACK");
      }
      resolve_completions(completions, committed);
      return committed;
    }

    void writer_loop() {
      BOOST_LOG(info) << "session_history: writer thread started";
      while (g_running.load(std::memory_order_relaxed)) {
        auto batch = drain_queue();
        if (batch.empty()) continue;

        process_batch(batch);
      }

      // Final drain
      {
        std::lock_guard lk {g_queue_mutex};
        if (!g_queue.empty()) {
          process_batch(g_queue);
          g_queue.clear();
        }
      }
      BOOST_LOG(info) << "session_history: writer thread stopped";
    }

    // ── Sampler thread ─────────────────────────────────────────────

    struct pending_event_t {
      std::string event_type;
      std::string payload;
    };

    struct aggregated_sample_t {
      double actual_fps = 0;
      double actual_bitrate_kbps = 0;
      double frame_interval_jitter_ms = 0;
      std::vector<pending_event_t> pending_events;
    };

    // Detect automatic events based on aggregator state changes. The caller
    // emits them after releasing g_aggregators_mutex so event recording never
    // runs while the aggregator map is locked.
    std::vector<pending_event_t> detect_events(aggregator_t &agg, std::int64_t current_losses, std::uint64_t current_frames) {
      std::vector<pending_event_t> pending;

      // First drop in session: fire as soon as any losses appear, including
      // on the very first sample (no need for a prior sample to compare against).
      if (!agg.had_any_losses && current_losses > 0) {
        agg.had_any_losses = true;
        pending.push_back({"first_drop", ""});
      }

      // Drop burst: >10 new losses in a single sample interval
      if (agg.prev_timestamp > 0) {
        auto delta_losses = current_losses - agg.prev_losses;
        if (delta_losses > 10) {
          pending.push_back({"drop_burst", std::to_string(delta_losses) + " losses in interval"});
        }
      }

      // Stall detection: no new frames for >=2 consecutive ticks (4+ seconds)
      if (agg.prev_timestamp > 0 && current_frames == agg.prev_frames_sent) {
        agg.zero_frame_ticks++;
        if (agg.zero_frame_ticks >= 2 && !agg.in_stall) {
          agg.in_stall = true;
          const auto stall_seconds = agg.zero_frame_ticks * std::chrono::duration_cast<std::chrono::seconds>(SAMPLE_INTERVAL).count();
          pending.push_back({"stall", "No frames for " + std::to_string(stall_seconds) + "s"});
        }
      }
      else {
        if (agg.in_stall) {
          pending.push_back({"recovery", "Frames resumed after stall"});
          agg.in_stall = false;
        }
        agg.zero_frame_ticks = 0;
      }

      return pending;
    }

    aggregated_sample_t update_aggregator(
      const std::string &session_uuid,
      double ts,
      std::uint64_t frames_total,
      std::uint64_t bytes_total,
      std::int64_t losses_total) {
      aggregated_sample_t result;
      std::lock_guard lk {g_aggregators_mutex};
      auto &agg = g_aggregators[session_uuid];
      result.pending_events = detect_events(agg, losses_total, frames_total);
      agg.update(ts, frames_total, bytes_total, losses_total);
      result.actual_fps = agg.last_actual_fps;
      result.actual_bitrate_kbps = agg.last_actual_bitrate_kbps;
      result.frame_interval_jitter_ms = agg.last_jitter_ms;
      return result;
    }

    void populate_host_snapshot(session_sample_t &sample, const platf::host_stats_t &host) {
      sample.host_cpu_percent = host.cpu_percent;
      sample.host_gpu_percent = host.gpu_percent;
      sample.host_gpu_encoder_percent = host.gpu_encoder_percent;
      sample.host_ram_percent = host.ram_total_bytes > 0
                                  ? static_cast<double>(host.ram_used_bytes) * 100.0 / static_cast<double>(host.ram_total_bytes)
                                  : -1.0;
      const auto vram_used_bytes = host.vram_total_bytes > 0 && host.vram_used_bytes > host.vram_total_bytes
                                     ? host.vram_total_bytes
                                     : host.vram_used_bytes;
      sample.host_vram_percent = host.vram_total_bytes > 0
                                   ? static_cast<double>(vram_used_bytes) * 100.0 / static_cast<double>(host.vram_total_bytes)
                                   : -1.0;
      sample.host_cpu_temp_c = host.cpu_temp_c;
      sample.host_gpu_temp_c = host.gpu_temp_c;
      sample.host_net_rx_bps = host.net_rx_bps;
      sample.host_net_tx_bps = host.net_tx_bps;
    }

    void enqueue_sample(session_sample_t sample) {
      write_cmd_t cmd;
      cmd.type = cmd_type::insert_sample;
      cmd.sample = std::move(sample);
      (void) enqueue(std::move(cmd));
    }

    void sample_rtsp_sessions(double ts, const platf::host_stats_t &host) {
      auto infos = stream::get_all_session_info();
      for (const auto &info : infos) {
        const auto aggregated = update_aggregator(
          info.uuid,
          ts,
          info.frames_sent,
          info.bytes_sent,
          info.client_reported_losses);

        for (const auto &evt : aggregated.pending_events) {
          record_event(info.uuid, evt.event_type, evt.payload);
        }

        session_sample_t s;
        s.session_uuid = info.uuid;
        s.timestamp_unix = ts;
        s.bytes_sent_total = info.bytes_sent;
        s.packets_sent_video = info.packets_sent;
        s.frames_sent = info.frames_sent;
        s.last_frame_index = info.last_frame_index;
        s.client_reported_losses = info.client_reported_losses;
        s.idr_requests = info.idr_requests;
        s.ref_invalidations = info.invalidate_ref_count;
        s.encode_latency_ms = info.encode_latency_ms;
        s.actual_fps = aggregated.actual_fps;
        s.actual_bitrate_kbps = aggregated.actual_bitrate_kbps;
        s.frame_interval_jitter_ms = aggregated.frame_interval_jitter_ms;
        populate_host_snapshot(s, host);

        enqueue_sample(std::move(s));
      }
    }

    void sample_webrtc_sessions(double ts, const platf::host_stats_t &host) {
      auto sessions = webrtc_stream::list_sessions();
      for (const auto &ws : sessions) {
        // Use true frame index (one per encoded video frame) for FPS, and
        // accumulated video+audio byte totals for bitrate. video_dropped
        // serves as the "losses" signal for this protocol.
        const auto frames_total = static_cast<std::uint64_t>(
          ws.last_video_frame_index > 0 ? ws.last_video_frame_index : 0);
        const auto bytes_total = ws.video_bytes_total + ws.audio_bytes_total;
        const auto losses_total = static_cast<std::int64_t>(ws.video_dropped);

        const auto aggregated = update_aggregator(
          ws.id,
          ts,
          frames_total,
          bytes_total,
          losses_total);

        for (const auto &evt : aggregated.pending_events) {
          record_event(ws.id, evt.event_type, evt.payload);
        }

        session_sample_t s;
        s.session_uuid = ws.id;
        s.timestamp_unix = ts;
        s.bytes_sent_total = bytes_total;
        s.packets_sent_video = ws.video_packets;
        s.frames_sent = frames_total;
        s.last_frame_index = ws.last_video_frame_index;
        s.video_dropped = ws.video_dropped;
        s.audio_dropped = ws.audio_dropped;
        s.client_reported_losses = losses_total;
        s.actual_fps = aggregated.actual_fps;
        s.actual_bitrate_kbps = aggregated.actual_bitrate_kbps;
        s.frame_interval_jitter_ms = aggregated.frame_interval_jitter_ms;
        populate_host_snapshot(s, host);

        enqueue_sample(std::move(s));
      }
    }

    void sampler_loop() {
      BOOST_LOG(info) << "session_history: sampler thread started";
      while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(SAMPLE_INTERVAL);
        if (!g_running.load(std::memory_order_relaxed)) break;

        bool has_active = false;
        {
          std::lock_guard lk {g_active_mutex};
          has_active = !g_active_sessions.empty();
        }
        if (!has_active) continue;

        double ts = storage::now_unix();
        auto host = host_stats::latest();
        sample_rtsp_sessions(ts, host);
        sample_webrtc_sessions(ts, host);
      }
      BOOST_LOG(info) << "session_history: sampler thread stopped";
    }

  }  // anonymous namespace

  // ── Public API ───────────────────────────────────────────────────

  void init(const std::string &db_path) {
    BOOST_LOG(info) << "session_history: initializing database at " << db_path;
    const std::filesystem::path history_db_path {db_path};

    if (!storage::open_write_db(db_path, g_write_db)) {
      return;
    }
    sqlite3_busy_timeout(g_write_db.get(), 3000);
    storage::exec(g_write_db.get(), "PRAGMA foreign_keys = ON");

    if (!storage::apply_schema_and_migrations(g_write_db.get(), SESSION_HISTORY_SCHEMA_VERSION)) {
      g_write_db.reset();
      return;
    }
    storage::tighten_history_db_permissions(history_db_path);

    if (!storage::open_read_db(db_path, g_read_db)) {
      BOOST_LOG(warning) << "session_history: falling back to the write connection for read queries";
    }
    if (g_read_db) {
      sqlite3_busy_timeout(g_read_db.get(), 3000);
      storage::exec(g_read_db.get(), "PRAGMA foreign_keys = ON");
      storage::tighten_history_db_permissions(history_db_path);
    }

    g_running.store(true, std::memory_order_release);
    g_writer_thread = std::thread {writer_loop};
    g_sampler_thread = std::thread {sampler_loop};

    // Prune old sessions on startup
    write_cmd_t cmd;
    cmd.type = cmd_type::prune;
    (void) enqueue(std::move(cmd));

    BOOST_LOG(info) << "session_history: initialized";
  }

  void shutdown() {
    BOOST_LOG(info) << "session_history: shutting down";

    g_running.store(false, std::memory_order_release);
    g_queue_cv.notify_all();

    if (g_sampler_thread.joinable()) {
      g_sampler_thread.join();
    }
    if (g_writer_thread.joinable()) {
      g_writer_thread.join();
    }

    {
      std::lock_guard lk {g_aggregators_mutex};
      g_aggregators.clear();
    }
    {
      std::lock_guard lk {g_active_mutex};
      g_active_sessions.clear();
    }

    g_read_db.reset();
    g_write_db.reset();

    BOOST_LOG(info) << "session_history: shut down";
  }

  void begin_session(const session_metadata_t &metadata) {
    BOOST_LOG(info) << "session_history: begin_session uuid=" << metadata.uuid
                    << " protocol=" << metadata.protocol;
    // Auto-populate static host identification from the host_stats subsystem
    // so callers in stream.cpp / webrtc_stream.cpp don't need to know about it.
    session_metadata_t enriched = metadata;
    if (enriched.host_cpu_model.empty() || enriched.host_gpu_model.empty()) {
      auto info = host_stats::info();
      if (enriched.host_cpu_model.empty()) {
        enriched.host_cpu_model = info.cpu_model;
      }
      if (enriched.host_gpu_model.empty()) {
        enriched.host_gpu_model = info.gpu_model;
      }
    }
    {
      std::lock_guard lk {g_active_mutex};
      g_active_sessions[enriched.uuid] = enriched;
    }

    write_cmd_t cmd;
    cmd.type = cmd_type::begin_session;
    cmd.metadata = enriched;
    if (!enqueue(std::move(cmd))) {
      BOOST_LOG(error) << "session_history: failed to queue begin_session for uuid=" << enriched.uuid;
    }

    // Emit stream_started event
    session_event_t evt;
    evt.session_uuid = enriched.uuid;
    evt.timestamp_unix = storage::now_unix();
    evt.event_type = "stream_started";
    write_cmd_t evt_cmd;
    evt_cmd.type = cmd_type::insert_event;
    evt_cmd.event = std::move(evt);
    if (!enqueue(std::move(evt_cmd))) {
      BOOST_LOG(error) << "session_history: failed to queue stream_started event for uuid=" << enriched.uuid;
    }
  }

  void end_session(const std::string &uuid) {
    BOOST_LOG(info) << "session_history: end_session uuid=" << uuid;
    {
      std::lock_guard lk {g_active_mutex};
      g_active_sessions.erase(uuid);
    }
    {
      std::lock_guard lk {g_aggregators_mutex};
      g_aggregators.erase(uuid);
    }

    // Emit stream_ended event
    session_event_t evt;
    evt.session_uuid = uuid;
    evt.timestamp_unix = storage::now_unix();
    evt.event_type = "stream_ended";
    write_cmd_t evt_cmd;
    evt_cmd.type = cmd_type::insert_event;
    evt_cmd.event = std::move(evt);
    if (!enqueue(std::move(evt_cmd))) {
      BOOST_LOG(error) << "session_history: failed to queue stream_ended event for uuid=" << uuid;
    }

    write_cmd_t cmd;
    cmd.type = cmd_type::end_session;
    cmd.uuid = uuid;
    if (!enqueue(std::move(cmd))) {
      BOOST_LOG(error) << "session_history: failed to queue end_session for uuid=" << uuid;
    }

    // Prune after session ends
    write_cmd_t prune_cmd;
    prune_cmd.type = cmd_type::prune;
    (void) enqueue(std::move(prune_cmd));
  }

  void record_event(const std::string &uuid, const std::string &event_type, const std::string &payload) {
    session_event_t evt;
    evt.session_uuid = uuid;
    evt.timestamp_unix = storage::now_unix();
    evt.event_type = event_type;
    evt.payload = payload;
    write_cmd_t cmd;
    cmd.type = cmd_type::insert_event;
    cmd.event = std::move(evt);
    if (!enqueue(std::move(cmd))) {
      BOOST_LOG(error) << "session_history: failed to queue event '" << event_type << "' for uuid=" << uuid;
    }
  }

  std::vector<session_summary_t> list_sessions(int limit, int offset) {
    auto *db = g_read_db ? g_read_db.get() : g_write_db.get();
    if (!db) return {};
    std::lock_guard lk {g_read_mutex};
    return storage::read_session_summaries(db, limit, offset);
  }

  std::optional<session_detail_t> get_session_detail(const std::string &uuid, bool include_all) {
    auto *db = g_read_db ? g_read_db.get() : g_write_db.get();
    if (!db) return std::nullopt;
    std::lock_guard lk {g_read_mutex};
    return storage::read_session_detail(
      db,
      uuid,
      include_all,
      DEFAULT_DETAIL_SAMPLE_LIMIT,
      DEFAULT_DETAIL_EVENT_LIMIT);
  }

  std::vector<active_session_t> get_active_sessions() {
    std::vector<active_session_t> result;

    // Snapshot active metadata
    std::unordered_map<std::string, session_metadata_t> active_copy;
    {
      std::lock_guard lk {g_active_mutex};
      active_copy = g_active_sessions;
    }
    if (active_copy.empty()) return result;

    // Merge RTSP live data
    auto rtsp_infos = stream::get_all_session_info();
    for (const auto &info : rtsp_infos) {
      auto it = active_copy.find(info.uuid);
      if (it == active_copy.end()) continue;

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
      as.codec = std::string(stream::video_format_name(info.video_format));
      as.hdr = info.dynamic_range > 0;
      as.yuv444 = info.yuv444;
      as.uptime_seconds = info.uptime_seconds;
      as.encode_latency_ms = info.encode_latency_ms;
      as.frames_sent = info.frames_sent;
      as.bytes_sent = info.bytes_sent;
      as.client_reported_losses = info.client_reported_losses;
      as.idr_requests = info.idr_requests;

      // Fill aggregated metrics if available
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

    // Merge WebRTC live data
    auto webrtc_sessions = webrtc_stream::list_sessions();
    for (const auto &ws : webrtc_sessions) {
      auto it = active_copy.find(ws.id);
      if (it == active_copy.end()) continue;

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
      as.codec = ws.codec.value_or("");
      as.hdr = ws.hdr.value_or(false);
      as.yuv444 = ws.yuv444.value_or(false);
      as.frames_sent = static_cast<std::uint64_t>(
        ws.last_video_frame_index > 0 ? ws.last_video_frame_index : 0);
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

  delete_result_e delete_session(const std::string &uuid) {
    if (!g_write_db || !g_running.load(std::memory_order_acquire)) {
      return delete_result_e::unavailable;
    }

    {
      std::lock_guard lk {g_active_mutex};
      if (g_active_sessions.contains(uuid)) {
        return delete_result_e::active_session;
      }
    }

    auto completion = std::make_shared<std::promise<bool>>();
    auto result = completion->get_future();

    write_cmd_t cmd;
    cmd.type = cmd_type::delete_session;
    cmd.uuid = uuid;
    cmd.completion = completion;
    if (!enqueue(std::move(cmd))) {
      return delete_result_e::unavailable;
    }

    if (result.wait_for(DELETE_WAIT_TIMEOUT) != std::future_status::ready) {
      BOOST_LOG(error) << "session_history: timed out waiting for delete completion for uuid=" << uuid;
      return delete_result_e::timeout;
    }

    return result.get() ? delete_result_e::deleted : delete_result_e::not_found;
  }

}  // namespace session_history
