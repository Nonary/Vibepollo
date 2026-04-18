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
#include <mutex>
#include <thread>
#include <unordered_map>

// lib includes
#include <sqlite3.h>

// local includes
#include "session_history.h"
#include "logging.h"
#include "rtsp.h"
#include "stream.h"
#include "webrtc_stream.h"

using namespace std::literals;

namespace session_history {
  namespace {

    // ── SQLite helpers ─────────────────────────────────────────────

    struct sqlite_deleter {
      void operator()(sqlite3 *db) const {
        if (db) sqlite3_close(db);
      }
    };

    using db_ptr = std::unique_ptr<sqlite3, sqlite_deleter>;

    struct stmt_deleter {
      void operator()(sqlite3_stmt *s) const {
        if (s) sqlite3_finalize(s);
      }
    };

    using stmt_ptr = std::unique_ptr<sqlite3_stmt, stmt_deleter>;

    stmt_ptr prepare(sqlite3 *db, const char *sql) {
      sqlite3_stmt *raw = nullptr;
      int rc = sqlite3_prepare_v2(db, sql, -1, &raw, nullptr);
      if (rc != SQLITE_OK) {
        BOOST_LOG(error) << "session_history: prepare failed (" << rc << "): " << sqlite3_errmsg(db)
                         << " | SQL: " << sql;
        return nullptr;
      }
      return stmt_ptr {raw};
    }

    bool exec(sqlite3 *db, const char *sql) {
      char *err = nullptr;
      int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
      if (rc != SQLITE_OK) {
        BOOST_LOG(error) << "session_history: exec failed (" << rc << "): " << (err ? err : "unknown");
        sqlite3_free(err);
        return false;
      }
      return true;
    }

    double now_unix() {
      auto tp = std::chrono::system_clock::now();
      return std::chrono::duration<double>(tp.time_since_epoch()).count();
    }

    // ── Schema ─────────────────────────────────────────────────────

    constexpr const char *SCHEMA_SQL = R"(
      PRAGMA journal_mode = WAL;
      PRAGMA synchronous = NORMAL;

      CREATE TABLE IF NOT EXISTS sessions (
        uuid TEXT PRIMARY KEY,
        protocol TEXT NOT NULL,
        client_name TEXT,
        device_name TEXT,
        app_name TEXT,
        width INTEGER,
        height INTEGER,
        target_fps INTEGER,
        target_bitrate_kbps INTEGER,
        codec TEXT,
        hdr INTEGER DEFAULT 0,
        audio_channels INTEGER,
        start_time_unix REAL NOT NULL,
        end_time_unix REAL,
        duration_seconds REAL,
        verdict TEXT DEFAULT 'unknown'
      );

      CREATE TABLE IF NOT EXISTS samples (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        session_uuid TEXT NOT NULL REFERENCES sessions(uuid),
        timestamp_unix REAL NOT NULL,
        bytes_sent_total INTEGER DEFAULT 0,
        packets_sent_video INTEGER DEFAULT 0,
        frames_sent INTEGER DEFAULT 0,
        last_frame_index INTEGER DEFAULT 0,
        video_dropped INTEGER DEFAULT 0,
        audio_dropped INTEGER DEFAULT 0,
        client_reported_losses INTEGER DEFAULT 0,
        idr_requests INTEGER DEFAULT 0,
        ref_invalidations INTEGER DEFAULT 0,
        encode_latency_ms REAL DEFAULT 0,
        actual_fps REAL DEFAULT 0,
        actual_bitrate_kbps REAL DEFAULT 0,
        frame_interval_jitter_ms REAL DEFAULT 0
      );

      CREATE INDEX IF NOT EXISTS idx_samples_session ON samples(session_uuid);
      CREATE INDEX IF NOT EXISTS idx_samples_time ON samples(session_uuid, timestamp_unix);

      CREATE TABLE IF NOT EXISTS events (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        session_uuid TEXT NOT NULL REFERENCES sessions(uuid),
        timestamp_unix REAL NOT NULL,
        event_type TEXT NOT NULL,
        payload TEXT
      );

      CREATE INDEX IF NOT EXISTS idx_events_session ON events(session_uuid);
    )";

    // ── Write command variants ─────────────────────────────────────

    enum class cmd_type {
      begin_session,
      end_session,
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

    db_ptr g_write_db;
    db_ptr g_read_db;

    // Thread-safe write queue (simple mutex + condition_variable + vector)
    std::mutex g_queue_mutex;
    std::condition_variable g_queue_cv;
    std::vector<write_cmd_t> g_queue;

    std::thread g_writer_thread;
    std::thread g_sampler_thread;
    std::atomic<bool> g_running {false};

    // Per-session aggregators (only accessed by sampler thread)
    std::unordered_map<std::string, aggregator_t> g_aggregators;

    // Track active session UUIDs for the /active endpoint
    std::mutex g_active_mutex;
    std::unordered_map<std::string, session_metadata_t> g_active_sessions;

    constexpr int MAX_HISTORY_SESSIONS = 50;
    constexpr auto SAMPLE_INTERVAL = std::chrono::seconds(2);

    // ── Queue helpers ──────────────────────────────────────────────

    void enqueue(write_cmd_t cmd) {
      {
        std::lock_guard lk {g_queue_mutex};
        g_queue.push_back(std::move(cmd));
      }
      g_queue_cv.notify_one();
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

    void process_begin(sqlite3 *db, const session_metadata_t &m) {
      auto s = prepare(db,
        "INSERT OR IGNORE INTO sessions "
        "(uuid, protocol, client_name, device_name, app_name, "
        " width, height, target_fps, target_bitrate_kbps, codec, hdr, audio_channels, start_time_unix) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)");
      if (!s) return;

      sqlite3_bind_text(s.get(), 1, m.uuid.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(s.get(), 2, m.protocol.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(s.get(), 3, m.client_name.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(s.get(), 4, m.device_name.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(s.get(), 5, m.app_name.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(s.get(), 6, m.width);
      sqlite3_bind_int(s.get(), 7, m.height);
      sqlite3_bind_int(s.get(), 8, m.target_fps);
      sqlite3_bind_int(s.get(), 9, m.target_bitrate_kbps);
      sqlite3_bind_text(s.get(), 10, m.codec.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(s.get(), 11, m.hdr ? 1 : 0);
      sqlite3_bind_int(s.get(), 12, m.audio_channels);
      sqlite3_bind_double(s.get(), 13, now_unix());

      sqlite3_step(s.get());
    }

    void process_end(sqlite3 *db, const std::string &uuid) {
      double end_time = now_unix();

      // Compute verdict from samples
      std::string verdict = "unknown";
      {
        auto s = prepare(db,
          "SELECT COUNT(*), "
          "SUM(CASE WHEN encode_latency_ms > 16 THEN 1 ELSE 0 END), "
          "SUM(client_reported_losses), "
          "SUM(frames_sent) "
          "FROM samples WHERE session_uuid = ?");
        if (s) {
          sqlite3_bind_text(s.get(), 1, uuid.c_str(), -1, SQLITE_TRANSIENT);
          if (sqlite3_step(s.get()) == SQLITE_ROW) {
            int sample_count = sqlite3_column_int(s.get(), 0);
            int high_latency_count = sqlite3_column_int(s.get(), 1);
            int64_t total_losses = sqlite3_column_int64(s.get(), 2);
            int64_t total_frames = sqlite3_column_int64(s.get(), 3);

            if (sample_count > 0) {
              double loss_ratio = total_frames > 0 ? static_cast<double>(total_losses) / static_cast<double>(total_frames) : 0;

              if (loss_ratio > 0.05) {
                verdict = "failed";
              }
              else if (high_latency_count > 0 || total_losses > 0) {
                verdict = "degraded";
              }
              else {
                verdict = "healthy";
              }
            }
          }
        }
      }

      auto s = prepare(db,
        "UPDATE sessions SET end_time_unix = ?, "
        "duration_seconds = ? - start_time_unix, "
        "verdict = ? "
        "WHERE uuid = ?");
      if (!s) return;

      sqlite3_bind_double(s.get(), 1, end_time);
      sqlite3_bind_double(s.get(), 2, end_time);
      sqlite3_bind_text(s.get(), 3, verdict.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(s.get(), 4, uuid.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_step(s.get());
    }

    void process_sample(sqlite3 *db, const session_sample_t &sample) {
      auto s = prepare(db,
        "INSERT INTO samples "
        "(session_uuid, timestamp_unix, bytes_sent_total, packets_sent_video, "
        " frames_sent, last_frame_index, video_dropped, audio_dropped, "
        " client_reported_losses, idr_requests, ref_invalidations, "
        " encode_latency_ms, actual_fps, actual_bitrate_kbps, frame_interval_jitter_ms) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
      if (!s) return;

      sqlite3_bind_text(s.get(), 1, sample.session_uuid.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_double(s.get(), 2, sample.timestamp_unix);
      sqlite3_bind_int64(s.get(), 3, static_cast<int64_t>(sample.bytes_sent_total));
      sqlite3_bind_int64(s.get(), 4, static_cast<int64_t>(sample.packets_sent_video));
      sqlite3_bind_int64(s.get(), 5, static_cast<int64_t>(sample.frames_sent));
      sqlite3_bind_int64(s.get(), 6, sample.last_frame_index);
      sqlite3_bind_int64(s.get(), 7, static_cast<int64_t>(sample.video_dropped));
      sqlite3_bind_int64(s.get(), 8, static_cast<int64_t>(sample.audio_dropped));
      sqlite3_bind_int64(s.get(), 9, sample.client_reported_losses);
      sqlite3_bind_int(s.get(), 10, static_cast<int>(sample.idr_requests));
      sqlite3_bind_int(s.get(), 11, static_cast<int>(sample.ref_invalidations));
      sqlite3_bind_double(s.get(), 12, sample.encode_latency_ms);
      sqlite3_bind_double(s.get(), 13, sample.actual_fps);
      sqlite3_bind_double(s.get(), 14, sample.actual_bitrate_kbps);
      sqlite3_bind_double(s.get(), 15, sample.frame_interval_jitter_ms);

      sqlite3_step(s.get());
    }

    void process_event(sqlite3 *db, const session_event_t &evt) {
      auto s = prepare(db,
        "INSERT INTO events (session_uuid, timestamp_unix, event_type, payload) "
        "VALUES (?,?,?,?)");
      if (!s) return;

      sqlite3_bind_text(s.get(), 1, evt.session_uuid.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_double(s.get(), 2, evt.timestamp_unix);
      sqlite3_bind_text(s.get(), 3, evt.event_type.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(s.get(), 4, evt.payload.c_str(), -1, SQLITE_TRANSIENT);

      sqlite3_step(s.get());
    }

    void process_prune(sqlite3 *db) {
      // Keep only the most recent MAX_HISTORY_SESSIONS completed sessions
      exec(db,
        "DELETE FROM events WHERE session_uuid IN ("
        "  SELECT uuid FROM sessions WHERE end_time_unix IS NOT NULL "
        "  ORDER BY end_time_unix DESC LIMIT -1 OFFSET 50"
        ")");
      exec(db,
        "DELETE FROM samples WHERE session_uuid IN ("
        "  SELECT uuid FROM sessions WHERE end_time_unix IS NOT NULL "
        "  ORDER BY end_time_unix DESC LIMIT -1 OFFSET 50"
        ")");
      exec(db,
        "DELETE FROM sessions WHERE end_time_unix IS NOT NULL "
        "AND uuid NOT IN ("
        "  SELECT uuid FROM sessions WHERE end_time_unix IS NOT NULL "
        "  ORDER BY end_time_unix DESC LIMIT 50"
        ")");
    }

    void writer_loop() {
      BOOST_LOG(info) << "session_history: writer thread started";
      while (g_running.load(std::memory_order_relaxed)) {
        auto batch = drain_queue();
        if (batch.empty()) continue;

        exec(g_write_db.get(), "BEGIN TRANSACTION");
        for (auto &cmd : batch) {
          switch (cmd.type) {
            case cmd_type::begin_session:
              process_begin(g_write_db.get(), cmd.metadata);
              break;
            case cmd_type::end_session:
              process_end(g_write_db.get(), cmd.uuid);
              break;
            case cmd_type::insert_sample:
              process_sample(g_write_db.get(), cmd.sample);
              break;
            case cmd_type::insert_event:
              process_event(g_write_db.get(), cmd.event);
              break;
            case cmd_type::prune:
              process_prune(g_write_db.get());
              break;
            case cmd_type::stop:
              break;
          }
        }
        exec(g_write_db.get(), "COMMIT");
      }

      // Final drain
      {
        std::lock_guard lk {g_queue_mutex};
        if (!g_queue.empty()) {
          exec(g_write_db.get(), "BEGIN TRANSACTION");
          for (auto &cmd : g_queue) {
            switch (cmd.type) {
              case cmd_type::begin_session:
                process_begin(g_write_db.get(), cmd.metadata);
                break;
              case cmd_type::end_session:
                process_end(g_write_db.get(), cmd.uuid);
                break;
              case cmd_type::insert_sample:
                process_sample(g_write_db.get(), cmd.sample);
                break;
              case cmd_type::insert_event:
                process_event(g_write_db.get(), cmd.event);
                break;
              default:
                break;
            }
          }
          exec(g_write_db.get(), "COMMIT");
          g_queue.clear();
        }
      }
      BOOST_LOG(info) << "session_history: writer thread stopped";
    }

    // ── Sampler thread ─────────────────────────────────────────────

    // Detect and emit automatic events based on aggregator state changes
    void detect_events(const std::string &uuid, aggregator_t &agg, std::int64_t current_losses, std::uint64_t current_frames) {
      // First drop in session
      if (!agg.had_any_losses && current_losses > 0 && agg.prev_losses == 0 && agg.prev_timestamp > 0) {
        agg.had_any_losses = true;
        record_event(uuid, "first_drop", "");
      }
      else if (current_losses > 0) {
        agg.had_any_losses = true;
      }

      // Drop burst: >10 new losses in a single sample interval
      if (agg.prev_timestamp > 0) {
        auto delta_losses = current_losses - agg.prev_losses;
        if (delta_losses > 10) {
          record_event(uuid, "drop_burst", std::to_string(delta_losses) + " losses in interval");
        }
      }

      // Stall detection: no new frames for >=2 consecutive ticks (4+ seconds)
      if (agg.prev_timestamp > 0 && current_frames == agg.prev_frames_sent) {
        agg.zero_frame_ticks++;
        if (agg.zero_frame_ticks >= 2 && !agg.in_stall) {
          agg.in_stall = true;
          record_event(uuid, "stall", "No frames for " + std::to_string(agg.zero_frame_ticks * 2) + "s");
        }
      }
      else {
        if (agg.in_stall) {
          record_event(uuid, "recovery", "Frames resumed after stall");
          agg.in_stall = false;
        }
        agg.zero_frame_ticks = 0;
      }
    }

    void sample_rtsp_sessions(double ts) {
      auto infos = stream::get_all_session_info();
      for (const auto &info : infos) {
        auto &agg = g_aggregators[info.uuid];
        detect_events(info.uuid, agg, info.client_reported_losses, info.frames_sent);
        agg.update(ts, info.frames_sent, info.bytes_sent, info.client_reported_losses);

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
        s.actual_fps = agg.last_actual_fps;
        s.actual_bitrate_kbps = agg.last_actual_bitrate_kbps;
        s.frame_interval_jitter_ms = agg.last_jitter_ms;

        write_cmd_t cmd;
        cmd.type = cmd_type::insert_sample;
        cmd.sample = std::move(s);
        enqueue(std::move(cmd));
      }
    }

    void sample_webrtc_sessions(double ts) {
      auto sessions = webrtc_stream::list_sessions();
      for (const auto &ws : sessions) {
        auto &agg = g_aggregators[ws.id];
        detect_events(ws.id, agg, static_cast<int64_t>(ws.video_dropped), ws.video_packets);
        agg.update(ts, ws.video_packets, 0, static_cast<int64_t>(ws.video_dropped));

        session_sample_t s;
        s.session_uuid = ws.id;
        s.timestamp_unix = ts;
        s.bytes_sent_total = 0;  // WebRTC doesn't have cumulative bytes yet
        s.packets_sent_video = ws.video_packets;
        s.frames_sent = ws.video_packets;
        s.video_dropped = ws.video_dropped;
        s.audio_dropped = ws.audio_dropped;
        s.actual_fps = agg.last_actual_fps;
        s.actual_bitrate_kbps = agg.last_actual_bitrate_kbps;
        s.frame_interval_jitter_ms = agg.last_jitter_ms;

        write_cmd_t cmd;
        cmd.type = cmd_type::insert_sample;
        cmd.sample = std::move(s);
        enqueue(std::move(cmd));
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

        double ts = now_unix();
        sample_rtsp_sessions(ts);
        sample_webrtc_sessions(ts);
      }
      BOOST_LOG(info) << "session_history: sampler thread stopped";
    }

    // ── Read helpers ───────────────────────────────────────────────

    session_summary_t row_to_summary(sqlite3_stmt *s) {
      session_summary_t out;
      out.uuid = reinterpret_cast<const char *>(sqlite3_column_text(s, 0));
      out.protocol = reinterpret_cast<const char *>(sqlite3_column_text(s, 1));
      auto col2 = sqlite3_column_text(s, 2);
      out.client_name = col2 ? reinterpret_cast<const char *>(col2) : "";
      auto col3 = sqlite3_column_text(s, 3);
      out.device_name = col3 ? reinterpret_cast<const char *>(col3) : "";
      auto col4 = sqlite3_column_text(s, 4);
      out.app_name = col4 ? reinterpret_cast<const char *>(col4) : "";
      out.width = sqlite3_column_int(s, 5);
      out.height = sqlite3_column_int(s, 6);
      out.target_fps = sqlite3_column_int(s, 7);
      out.target_bitrate_kbps = sqlite3_column_int(s, 8);
      auto col9 = sqlite3_column_text(s, 9);
      out.codec = col9 ? reinterpret_cast<const char *>(col9) : "";
      out.hdr = sqlite3_column_int(s, 10) != 0;
      out.audio_channels = sqlite3_column_int(s, 11);
      out.start_time_unix = sqlite3_column_double(s, 12);
      out.end_time_unix = sqlite3_column_double(s, 13);
      out.duration_seconds = sqlite3_column_double(s, 14);
      auto col15 = sqlite3_column_text(s, 15);
      out.verdict = col15 ? reinterpret_cast<const char *>(col15) : "unknown";
      return out;
    }

  }  // anonymous namespace

  // ── Public API ───────────────────────────────────────────────────

  void init(const std::string &db_path) {
    BOOST_LOG(info) << "session_history: initializing database at " << db_path;

    // Open write connection
    {
      sqlite3 *raw = nullptr;
      int rc = sqlite3_open(db_path.c_str(), &raw);
      if (rc != SQLITE_OK) {
        BOOST_LOG(error) << "session_history: failed to open write DB: " << sqlite3_errmsg(raw);
        sqlite3_close(raw);
        return;
      }
      g_write_db.reset(raw);
    }

    // Apply schema
    if (!exec(g_write_db.get(), SCHEMA_SQL)) {
      BOOST_LOG(error) << "session_history: schema creation failed";
      g_write_db.reset();
      return;
    }

    // Open read connection (read-only)
    {
      sqlite3 *raw = nullptr;
      int rc = sqlite3_open_v2(db_path.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr);
      if (rc != SQLITE_OK) {
        BOOST_LOG(error) << "session_history: failed to open read DB: " << sqlite3_errmsg(raw);
        sqlite3_close(raw);
        g_write_db.reset();
        return;
      }
      g_read_db.reset(raw);
    }

    g_running.store(true, std::memory_order_release);
    g_writer_thread = std::thread {writer_loop};
    g_sampler_thread = std::thread {sampler_loop};

    // Prune old sessions on startup
    write_cmd_t cmd;
    cmd.type = cmd_type::prune;
    enqueue(std::move(cmd));

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

    g_aggregators.clear();
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
    {
      std::lock_guard lk {g_active_mutex};
      g_active_sessions[metadata.uuid] = metadata;
    }

    write_cmd_t cmd;
    cmd.type = cmd_type::begin_session;
    cmd.metadata = metadata;
    enqueue(std::move(cmd));

    // Emit stream_started event
    session_event_t evt;
    evt.session_uuid = metadata.uuid;
    evt.timestamp_unix = now_unix();
    evt.event_type = "stream_started";
    write_cmd_t evt_cmd;
    evt_cmd.type = cmd_type::insert_event;
    evt_cmd.event = std::move(evt);
    enqueue(std::move(evt_cmd));
  }

  void end_session(const std::string &uuid) {
    BOOST_LOG(info) << "session_history: end_session uuid=" << uuid;
    {
      std::lock_guard lk {g_active_mutex};
      g_active_sessions.erase(uuid);
    }
    g_aggregators.erase(uuid);

    // Emit stream_ended event
    session_event_t evt;
    evt.session_uuid = uuid;
    evt.timestamp_unix = now_unix();
    evt.event_type = "stream_ended";
    write_cmd_t evt_cmd;
    evt_cmd.type = cmd_type::insert_event;
    evt_cmd.event = std::move(evt);
    enqueue(std::move(evt_cmd));

    write_cmd_t cmd;
    cmd.type = cmd_type::end_session;
    cmd.uuid = uuid;
    enqueue(std::move(cmd));

    // Prune after session ends
    write_cmd_t prune_cmd;
    prune_cmd.type = cmd_type::prune;
    enqueue(std::move(prune_cmd));
  }

  void record_event(const std::string &uuid, const std::string &event_type, const std::string &payload) {
    session_event_t evt;
    evt.session_uuid = uuid;
    evt.timestamp_unix = now_unix();
    evt.event_type = event_type;
    evt.payload = payload;
    write_cmd_t cmd;
    cmd.type = cmd_type::insert_event;
    cmd.event = std::move(evt);
    enqueue(std::move(cmd));
  }

  std::vector<session_summary_t> list_sessions(int limit, int offset) {
    std::vector<session_summary_t> result;
    if (!g_read_db) return result;

    auto s = prepare(g_read_db.get(),
      "SELECT uuid, protocol, client_name, device_name, app_name, "
      "width, height, target_fps, target_bitrate_kbps, codec, hdr, audio_channels, "
      "start_time_unix, end_time_unix, duration_seconds, verdict "
      "FROM sessions WHERE end_time_unix IS NOT NULL "
      "ORDER BY end_time_unix DESC LIMIT ? OFFSET ?");
    if (!s) return result;

    sqlite3_bind_int(s.get(), 1, limit);
    sqlite3_bind_int(s.get(), 2, offset);

    while (sqlite3_step(s.get()) == SQLITE_ROW) {
      result.push_back(row_to_summary(s.get()));
    }
    return result;
  }

  std::optional<session_detail_t> get_session_detail(const std::string &uuid) {
    if (!g_read_db) return std::nullopt;

    // Fetch session summary
    auto s = prepare(g_read_db.get(),
      "SELECT uuid, protocol, client_name, device_name, app_name, "
      "width, height, target_fps, target_bitrate_kbps, codec, hdr, audio_channels, "
      "start_time_unix, end_time_unix, duration_seconds, verdict "
      "FROM sessions WHERE uuid = ?");
    if (!s) return std::nullopt;
    sqlite3_bind_text(s.get(), 1, uuid.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(s.get()) != SQLITE_ROW) return std::nullopt;

    session_detail_t detail;
    detail.summary = row_to_summary(s.get());

    // Fetch samples
    auto ss = prepare(g_read_db.get(),
      "SELECT session_uuid, timestamp_unix, bytes_sent_total, packets_sent_video, "
      "frames_sent, last_frame_index, video_dropped, audio_dropped, "
      "client_reported_losses, idr_requests, ref_invalidations, "
      "encode_latency_ms, actual_fps, actual_bitrate_kbps, frame_interval_jitter_ms "
      "FROM samples WHERE session_uuid = ? ORDER BY timestamp_unix");
    if (ss) {
      sqlite3_bind_text(ss.get(), 1, uuid.c_str(), -1, SQLITE_TRANSIENT);
      while (sqlite3_step(ss.get()) == SQLITE_ROW) {
        session_sample_t sample;
        auto col0 = sqlite3_column_text(ss.get(), 0);
        sample.session_uuid = col0 ? reinterpret_cast<const char *>(col0) : "";
        sample.timestamp_unix = sqlite3_column_double(ss.get(), 1);
        sample.bytes_sent_total = static_cast<uint64_t>(sqlite3_column_int64(ss.get(), 2));
        sample.packets_sent_video = static_cast<uint64_t>(sqlite3_column_int64(ss.get(), 3));
        sample.frames_sent = static_cast<uint64_t>(sqlite3_column_int64(ss.get(), 4));
        sample.last_frame_index = sqlite3_column_int64(ss.get(), 5);
        sample.video_dropped = static_cast<uint64_t>(sqlite3_column_int64(ss.get(), 6));
        sample.audio_dropped = static_cast<uint64_t>(sqlite3_column_int64(ss.get(), 7));
        sample.client_reported_losses = sqlite3_column_int64(ss.get(), 8);
        sample.idr_requests = static_cast<uint32_t>(sqlite3_column_int(ss.get(), 9));
        sample.ref_invalidations = static_cast<uint32_t>(sqlite3_column_int(ss.get(), 10));
        sample.encode_latency_ms = sqlite3_column_double(ss.get(), 11);
        sample.actual_fps = sqlite3_column_double(ss.get(), 12);
        sample.actual_bitrate_kbps = sqlite3_column_double(ss.get(), 13);
        sample.frame_interval_jitter_ms = sqlite3_column_double(ss.get(), 14);
        detail.samples.push_back(std::move(sample));
      }
    }

    // Fetch events
    auto es = prepare(g_read_db.get(),
      "SELECT session_uuid, timestamp_unix, event_type, payload "
      "FROM events WHERE session_uuid = ? ORDER BY timestamp_unix");
    if (es) {
      sqlite3_bind_text(es.get(), 1, uuid.c_str(), -1, SQLITE_TRANSIENT);
      while (sqlite3_step(es.get()) == SQLITE_ROW) {
        session_event_t evt;
        auto col0 = sqlite3_column_text(es.get(), 0);
        evt.session_uuid = col0 ? reinterpret_cast<const char *>(col0) : "";
        evt.timestamp_unix = sqlite3_column_double(es.get(), 1);
        auto col2 = sqlite3_column_text(es.get(), 2);
        evt.event_type = col2 ? reinterpret_cast<const char *>(col2) : "";
        auto col3 = sqlite3_column_text(es.get(), 3);
        evt.payload = col3 ? reinterpret_cast<const char *>(col3) : "";
        detail.events.push_back(std::move(evt));
      }
    }

    return detail;
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
      as.target_bitrate_kbps = info.bitrate_kbps;
      as.codec = info.video_format == 0 ? "H.264" : info.video_format == 1 ? "HEVC" : info.video_format == 2 ? "AV1" : "Unknown";
      as.hdr = info.dynamic_range > 0;
      as.uptime_seconds = info.uptime_seconds;
      as.encode_latency_ms = info.encode_latency_ms;
      as.frames_sent = info.frames_sent;
      as.bytes_sent = info.bytes_sent;
      as.client_reported_losses = info.client_reported_losses;
      as.idr_requests = info.idr_requests;

      // Fill aggregated metrics if available
      auto agg_it = g_aggregators.find(info.uuid);
      if (agg_it != g_aggregators.end()) {
        as.actual_fps = agg_it->second.last_actual_fps;
        as.actual_bitrate_kbps = agg_it->second.last_actual_bitrate_kbps;
        as.frame_interval_jitter_ms = agg_it->second.last_jitter_ms;
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
      as.target_bitrate_kbps = ws.bitrate_kbps.value_or(0);
      as.codec = ws.codec.value_or("");
      as.hdr = ws.hdr.value_or(false);
      as.frames_sent = ws.video_packets;

      auto agg_it = g_aggregators.find(ws.id);
      if (agg_it != g_aggregators.end()) {
        as.actual_fps = agg_it->second.last_actual_fps;
        as.actual_bitrate_kbps = agg_it->second.last_actual_bitrate_kbps;
        as.frame_interval_jitter_ms = agg_it->second.last_jitter_ms;
      }

      result.push_back(std::move(as));
    }

    return result;
  }

}  // namespace session_history
