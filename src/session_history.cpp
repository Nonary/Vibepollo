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
#include <filesystem>
#include <future>
#include <mutex>
#include <system_error>
#include <thread>
#include <unordered_map>

// lib includes
#include <sqlite3.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <AclAPI.h>
#endif

// local includes
#include "session_history.h"
#include "host_stats.h"
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

    void tighten_history_db_permissions(const std::filesystem::path &db_path) {
#ifdef _WIN32
      auto apply_windows_permissions = [&](const std::filesystem::path &path) {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) || ec) {
          return;
        }

        SID_IDENTIFIER_AUTHORITY nt_authority = SECURITY_NT_AUTHORITY;
        PSID admin_sid = nullptr;
        PSID system_sid = nullptr;
        if (!AllocateAndInitializeSid(&nt_authority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                                      0, 0, 0, 0, 0, 0, &admin_sid)) {
          BOOST_LOG(warning) << "session_history: failed to allocate Administrators SID for " << path.string();
          return;
        }
        auto free_admin_sid = util::fail_guard([admin_sid]() {
          FreeSid(admin_sid);
        });

        if (!AllocateAndInitializeSid(&nt_authority, 1, SECURITY_LOCAL_SYSTEM_RID,
                                      0, 0, 0, 0, 0, 0, 0, &system_sid)) {
          BOOST_LOG(warning) << "session_history: failed to allocate SYSTEM SID for " << path.string();
          return;
        }
        auto free_system_sid = util::fail_guard([system_sid]() {
          FreeSid(system_sid);
        });

        EXPLICIT_ACCESSW access[2] {};
        access[0].grfAccessPermissions = GENERIC_ALL;
        access[0].grfAccessMode = SET_ACCESS;
        access[0].grfInheritance = NO_INHERITANCE;
        access[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access[0].Trustee.TrusteeType = TRUSTEE_IS_GROUP;
        access[0].Trustee.ptstrName = static_cast<LPWSTR>(admin_sid);

        access[1].grfAccessPermissions = GENERIC_ALL;
        access[1].grfAccessMode = SET_ACCESS;
        access[1].grfInheritance = NO_INHERITANCE;
        access[1].Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access[1].Trustee.TrusteeType = TRUSTEE_IS_USER;
        access[1].Trustee.ptstrName = static_cast<LPWSTR>(system_sid);

        PACL raw_acl = nullptr;
        DWORD acl_status = SetEntriesInAclW(2, access, nullptr, &raw_acl);
        if (acl_status != ERROR_SUCCESS) {
          BOOST_LOG(warning) << "session_history: SetEntriesInAclW failed for " << path.string()
                             << " (error=" << acl_status << ")";
          return;
        }
        auto free_acl = util::fail_guard([raw_acl]() {
          LocalFree(raw_acl);
        });

        DWORD sec_status = SetNamedSecurityInfoW(
          const_cast<LPWSTR>(path.c_str()),
          SE_FILE_OBJECT,
          DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
          nullptr,
          nullptr,
          raw_acl,
          nullptr);
        if (sec_status != ERROR_SUCCESS) {
          BOOST_LOG(warning) << "session_history: SetNamedSecurityInfoW failed for " << path.string()
                             << " (error=" << sec_status << ")";
        }
      };

      apply_windows_permissions(db_path);
      apply_windows_permissions(db_path.string() + "-wal");
      apply_windows_permissions(db_path.string() + "-shm");
#else
      const auto apply_posix_permissions = [&](const std::filesystem::path &path) {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) || ec) {
          return;
        }
        std::filesystem::permissions(
          path,
          std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
          std::filesystem::perm_options::replace,
          ec);
        if (ec) {
          BOOST_LOG(warning) << "session_history: failed to tighten permissions for " << path.string()
                             << ": " << ec.message();
        }
      };

      apply_posix_permissions(db_path);
      apply_posix_permissions(db_path.string() + "-wal");
      apply_posix_permissions(db_path.string() + "-shm");
#endif
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
        target_requested_bitrate_kbps INTEGER,
        codec TEXT,
        hdr INTEGER DEFAULT 0,
        yuv444 INTEGER DEFAULT 0,
        audio_channels INTEGER,
        start_time_unix REAL NOT NULL,
        end_time_unix REAL,
        duration_seconds REAL,
        verdict TEXT DEFAULT 'unknown',
        server_version TEXT,
        host_cpu_model TEXT,
        host_gpu_model TEXT
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
        frame_interval_jitter_ms REAL DEFAULT 0,
        host_cpu_percent REAL DEFAULT -1,
        host_gpu_percent REAL DEFAULT -1,
        host_gpu_encoder_percent REAL DEFAULT -1,
        host_ram_percent REAL DEFAULT -1,
        host_vram_percent REAL DEFAULT -1,
        host_cpu_temp_c REAL DEFAULT -1,
        host_gpu_temp_c REAL DEFAULT -1,
        host_net_rx_bps REAL DEFAULT -1,
        host_net_tx_bps REAL DEFAULT -1
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
      CREATE INDEX IF NOT EXISTS idx_events_time ON events(session_uuid, timestamp_unix);
      CREATE INDEX IF NOT EXISTS idx_sessions_end_time
      ON sessions(end_time_unix DESC)
      WHERE end_time_unix IS NOT NULL;
    )";

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

    db_ptr g_write_db;
    db_ptr g_read_db;
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

    bool process_begin(sqlite3 *db, const session_metadata_t &m) {
      auto s = prepare(db,
        "INSERT OR IGNORE INTO sessions "
        "(uuid, protocol, client_name, device_name, app_name, "
        " width, height, target_fps, target_bitrate_kbps, target_requested_bitrate_kbps, "
        " codec, hdr, yuv444, audio_channels, start_time_unix, server_version, host_cpu_model, host_gpu_model) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
      if (!s) return false;

      sqlite3_bind_text(s.get(), 1, m.uuid.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(s.get(), 2, m.protocol.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(s.get(), 3, m.client_name.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(s.get(), 4, m.device_name.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(s.get(), 5, m.app_name.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(s.get(), 6, m.width);
      sqlite3_bind_int(s.get(), 7, m.height);
      sqlite3_bind_int(s.get(), 8, m.target_fps);
      sqlite3_bind_int(s.get(), 9, m.encoder_bitrate_kbps);
      sqlite3_bind_int(s.get(), 10, m.requested_bitrate_kbps);
      sqlite3_bind_text(s.get(), 11, m.codec.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(s.get(), 12, m.hdr ? 1 : 0);
      sqlite3_bind_int(s.get(), 13, m.yuv444 ? 1 : 0);
      sqlite3_bind_int(s.get(), 14, m.audio_channels);
      sqlite3_bind_double(s.get(), 15, now_unix());
      sqlite3_bind_text(s.get(), 16, m.server_version.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(s.get(), 17, m.host_cpu_model.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(s.get(), 18, m.host_gpu_model.c_str(), -1, SQLITE_TRANSIENT);

      if (sqlite3_step(s.get()) != SQLITE_DONE) {
        BOOST_LOG(error) << "session_history: begin_session insert failed for uuid=" << m.uuid
                         << ": " << sqlite3_errmsg(db);
        return false;
      }

      // Defensive: if no row was inserted then a session with this uuid
      // already exists. With the per-stream history_uuid contract this
      // should never happen; warn loudly so any future regression is
      // visible in the logs and we don't silently merge sessions.
      if (sqlite3_changes(db) == 0) {
        BOOST_LOG(warning)
          << "session_history: begin_session ignored - uuid already present in DB: "
          << m.uuid << " (sessions will be merged into the existing row)";
      }
      return true;
    }

    bool session_accepts_live_updates(sqlite3 *db, const std::string &uuid) {
      auto s = prepare(db, "SELECT end_time_unix FROM sessions WHERE uuid = ?");
      if (!s) return false;

      sqlite3_bind_text(s.get(), 1, uuid.c_str(), -1, SQLITE_TRANSIENT);
      if (sqlite3_step(s.get()) != SQLITE_ROW) {
        return false;
      }

      return sqlite3_column_type(s.get(), 0) == SQLITE_NULL;
    }

    bool process_end(sqlite3 *db, const std::string &uuid) {
      double end_time = now_unix();

      // Compute verdict from samples
      std::string verdict = "unknown";
      {
        auto s = prepare(db,
          "SELECT COUNT(*), "
          "SUM(CASE WHEN encode_latency_ms > 16 THEN 1 ELSE 0 END), "
          "MAX(client_reported_losses), "
          "MAX(frames_sent) "
          "FROM samples WHERE session_uuid = ?");
        if (s) {
          sqlite3_bind_text(s.get(), 1, uuid.c_str(), -1, SQLITE_TRANSIENT);
          if (sqlite3_step(s.get()) == SQLITE_ROW) {
            int sample_count = sqlite3_column_int(s.get(), 0);
            int high_latency_count = sqlite3_column_int(s.get(), 1);
            int64_t max_reported_losses = sqlite3_column_int64(s.get(), 2);
            int64_t max_frames_sent = sqlite3_column_int64(s.get(), 3);

            if (sample_count > 0) {
              double loss_ratio = max_frames_sent > 0 ? static_cast<double>(max_reported_losses) / static_cast<double>(max_frames_sent) : 0;

              if (loss_ratio > 0.05) {
                verdict = "failed";
              }
              else if (high_latency_count > 0 || max_reported_losses > 0) {
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
      if (!s) return false;

      sqlite3_bind_double(s.get(), 1, end_time);
      sqlite3_bind_double(s.get(), 2, end_time);
      sqlite3_bind_text(s.get(), 3, verdict.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(s.get(), 4, uuid.c_str(), -1, SQLITE_TRANSIENT);
      if (sqlite3_step(s.get()) != SQLITE_DONE) {
        BOOST_LOG(error) << "session_history: end_session update failed for uuid=" << uuid
                         << ": " << sqlite3_errmsg(db);
        return false;
      }
      return true;
    }

    bool process_sample(sqlite3 *db, const session_sample_t &sample) {
      if (!session_accepts_live_updates(db, sample.session_uuid)) {
        BOOST_LOG(debug) << "session_history: dropping stale sample for ended or missing session "
                         << sample.session_uuid;
        return true;
      }
      auto s = prepare(db,
        "INSERT INTO samples "
        "(session_uuid, timestamp_unix, bytes_sent_total, packets_sent_video, "
        " frames_sent, last_frame_index, video_dropped, audio_dropped, "
        " client_reported_losses, idr_requests, ref_invalidations, "
        " encode_latency_ms, actual_fps, actual_bitrate_kbps, frame_interval_jitter_ms, "
        " host_cpu_percent, host_gpu_percent, host_gpu_encoder_percent, "
        " host_ram_percent, host_vram_percent, host_cpu_temp_c, host_gpu_temp_c, "
        " host_net_rx_bps, host_net_tx_bps) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
      if (!s) return false;

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
      sqlite3_bind_double(s.get(), 16, sample.host_cpu_percent);
      sqlite3_bind_double(s.get(), 17, sample.host_gpu_percent);
      sqlite3_bind_double(s.get(), 18, sample.host_gpu_encoder_percent);
      sqlite3_bind_double(s.get(), 19, sample.host_ram_percent);
      sqlite3_bind_double(s.get(), 20, sample.host_vram_percent);
      sqlite3_bind_double(s.get(), 21, sample.host_cpu_temp_c);
      sqlite3_bind_double(s.get(), 22, sample.host_gpu_temp_c);
      sqlite3_bind_double(s.get(), 23, sample.host_net_rx_bps);
      sqlite3_bind_double(s.get(), 24, sample.host_net_tx_bps);

      if (sqlite3_step(s.get()) != SQLITE_DONE) {
        BOOST_LOG(error) << "session_history: sample insert failed for uuid=" << sample.session_uuid
                         << ": " << sqlite3_errmsg(db);
        return false;
      }

      auto trim = prepare(
        db,
        "DELETE FROM samples WHERE id IN ("
        "  SELECT id FROM samples WHERE session_uuid = ? "
        "  ORDER BY timestamp_unix DESC, id DESC LIMIT -1 OFFSET ?"
        ")");
      if (!trim) {
        return false;
      }
      sqlite3_bind_text(trim.get(), 1, sample.session_uuid.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(trim.get(), 2, MAX_SAMPLES_PER_SESSION);
      if (sqlite3_step(trim.get()) != SQLITE_DONE) {
        BOOST_LOG(error) << "session_history: sample trim failed for uuid=" << sample.session_uuid
                         << ": " << sqlite3_errmsg(db);
        return false;
      }
      return true;
    }

    bool process_event(sqlite3 *db, const session_event_t &evt) {
      if (!session_accepts_live_updates(db, evt.session_uuid)) {
        BOOST_LOG(debug) << "session_history: dropping stale event for ended or missing session "
                         << evt.session_uuid << " type=" << evt.event_type;
        return true;
      }
      auto s = prepare(db,
        "INSERT INTO events (session_uuid, timestamp_unix, event_type, payload) "
        "VALUES (?,?,?,?)");
      if (!s) return false;

      sqlite3_bind_text(s.get(), 1, evt.session_uuid.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_double(s.get(), 2, evt.timestamp_unix);
      sqlite3_bind_text(s.get(), 3, evt.event_type.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(s.get(), 4, evt.payload.c_str(), -1, SQLITE_TRANSIENT);

      if (sqlite3_step(s.get()) != SQLITE_DONE) {
        BOOST_LOG(error) << "session_history: event insert failed for uuid=" << evt.session_uuid
                         << " type=" << evt.event_type << ": " << sqlite3_errmsg(db);
        return false;
      }

      auto trim = prepare(
        db,
        "DELETE FROM events WHERE id IN ("
        "  SELECT id FROM events WHERE session_uuid = ? "
        "  ORDER BY timestamp_unix DESC, id DESC LIMIT -1 OFFSET ?"
        ")");
      if (!trim) {
        return false;
      }
      sqlite3_bind_text(trim.get(), 1, evt.session_uuid.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(trim.get(), 2, MAX_EVENTS_PER_SESSION);
      if (sqlite3_step(trim.get()) != SQLITE_DONE) {
        BOOST_LOG(error) << "session_history: event trim failed for uuid=" << evt.session_uuid
                         << " type=" << evt.event_type << ": " << sqlite3_errmsg(db);
        return false;
      }
      return true;
    }

    enum class delete_apply_e {
      deleted,
      not_found,
      failed
    };

    delete_apply_e process_delete(sqlite3 *db, const std::string &uuid) {
      constexpr const char *DELETE_EVENTS = "DELETE FROM events WHERE session_uuid = ?";
      constexpr const char *DELETE_SAMPLES = "DELETE FROM samples WHERE session_uuid = ?";
      constexpr const char *DELETE_SESSION = "DELETE FROM sessions WHERE uuid = ?";

      int affected = 0;
      for (const auto &[sql, track_changes] : {
             std::pair {DELETE_EVENTS, false},
             std::pair {DELETE_SAMPLES, false},
             std::pair {DELETE_SESSION, true},
           }) {
        auto stmt = prepare(db, sql);
        if (!stmt) return delete_apply_e::failed;

        sqlite3_bind_text(stmt.get(), 1, uuid.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
          BOOST_LOG(error) << "session_history: delete failed for uuid=" << uuid << ": " << sqlite3_errmsg(db);
          return delete_apply_e::failed;
        }
        if (track_changes) {
          affected = sqlite3_changes(db);
        }
      }

      BOOST_LOG(info) << "Deleted session "sv << uuid << " from history (rows="sv << affected << ")"sv;
      return affected > 0 ? delete_apply_e::deleted : delete_apply_e::not_found;
    }

    bool process_prune(sqlite3 *db) {
      // Keep only the most recent MAX_HISTORY_SESSIONS completed sessions
      const auto limit_str = std::to_string(MAX_HISTORY_SESSIONS);
      const std::string del_events =
        "DELETE FROM events WHERE session_uuid IN ("
        "  SELECT uuid FROM sessions WHERE end_time_unix IS NOT NULL "
        "  ORDER BY end_time_unix DESC LIMIT -1 OFFSET " + limit_str +
        ")";
      const std::string del_samples =
        "DELETE FROM samples WHERE session_uuid IN ("
        "  SELECT uuid FROM sessions WHERE end_time_unix IS NOT NULL "
        "  ORDER BY end_time_unix DESC LIMIT -1 OFFSET " + limit_str +
        ")";
      const std::string del_sessions =
        "DELETE FROM sessions WHERE end_time_unix IS NOT NULL "
        "AND uuid NOT IN ("
        "  SELECT uuid FROM sessions WHERE end_time_unix IS NOT NULL "
        "  ORDER BY end_time_unix DESC LIMIT " + limit_str +
        ")";
      return exec(db, del_events.c_str()) &&
             exec(db, del_samples.c_str()) &&
             exec(db, del_sessions.c_str());
    }

    bool apply_write_cmd(sqlite3 *db, write_cmd_t &cmd, std::vector<std::pair<std::shared_ptr<std::promise<bool>>, bool>> &completions) {
      switch (cmd.type) {
        case cmd_type::begin_session:
          return process_begin(db, cmd.metadata);
        case cmd_type::end_session:
          return process_end(db, cmd.uuid);
        case cmd_type::delete_session: {
          auto result = process_delete(db, cmd.uuid);
          if (cmd.completion) {
            completions.emplace_back(cmd.completion, result == delete_apply_e::deleted);
          }
          return result != delete_apply_e::failed;
        }
        case cmd_type::insert_sample:
          return process_sample(db, cmd.sample);
        case cmd_type::insert_event:
          return process_event(db, cmd.event);
        case cmd_type::prune:
          return process_prune(db);
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
      if (!exec(g_write_db.get(), "BEGIN TRANSACTION")) {
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
        exec(g_write_db.get(), "ROLLBACK");
        resolve_completions(completions, false);
        return false;
      }

      const bool committed = exec(g_write_db.get(), "COMMIT");
      if (!committed) {
        BOOST_LOG(error) << "session_history: failed to commit write transaction";
        exec(g_write_db.get(), "ROLLBACK");
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

        double ts = now_unix();
        auto host = host_stats::latest();
        sample_rtsp_sessions(ts, host);
        sample_webrtc_sessions(ts, host);
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
      out.encoder_bitrate_kbps = sqlite3_column_int(s, 8);
      auto col9 = sqlite3_column_text(s, 9);
      out.codec = col9 ? reinterpret_cast<const char *>(col9) : "";
      out.hdr = sqlite3_column_int(s, 10) != 0;
      out.yuv444 = sqlite3_column_int(s, 11) != 0;
      out.audio_channels = sqlite3_column_int(s, 12);
      out.start_time_unix = sqlite3_column_double(s, 13);
      out.end_time_unix = sqlite3_column_double(s, 14);
      out.duration_seconds = sqlite3_column_double(s, 15);
      auto col16 = sqlite3_column_text(s, 16);
      out.verdict = col16 ? reinterpret_cast<const char *>(col16) : "unknown";
      // Column 17 is target_requested_bitrate_kbps and may be NULL for sessions
      // recorded before that column existed, in which case we fall back to the
      // encode bitrate so the UI just shows a single value.
      if (sqlite3_column_count(s) > 17 && sqlite3_column_type(s, 17) != SQLITE_NULL) {
        out.requested_bitrate_kbps = sqlite3_column_int(s, 17);
      } else {
        out.requested_bitrate_kbps = out.encoder_bitrate_kbps;
      }
      if (sqlite3_column_count(s) > 18 && sqlite3_column_type(s, 18) != SQLITE_NULL) {
        auto col18 = sqlite3_column_text(s, 18);
        out.server_version = col18 ? reinterpret_cast<const char *>(col18) : "";
      }
      // Columns 19/20 are post-phase-21 host identification; may be missing
      // for sessions recorded before that migration.
      if (sqlite3_column_count(s) > 19 && sqlite3_column_type(s, 19) != SQLITE_NULL) {
        auto col19 = sqlite3_column_text(s, 19);
        out.host_cpu_model = col19 ? reinterpret_cast<const char *>(col19) : "";
      }
      if (sqlite3_column_count(s) > 20 && sqlite3_column_type(s, 20) != SQLITE_NULL) {
        auto col20 = sqlite3_column_text(s, 20);
        out.host_gpu_model = col20 ? reinterpret_cast<const char *>(col20) : "";
      }
      return out;
    }

    std::size_t read_count(sqlite3 *db, const char *sql, const std::string &uuid) {
      auto stmt = prepare(db, sql);
      if (!stmt) {
        return 0;
      }
      sqlite3_bind_text(stmt.get(), 1, uuid.c_str(), -1, SQLITE_TRANSIENT);
      if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        return 0;
      }
      return static_cast<std::size_t>(sqlite3_column_int64(stmt.get(), 0));
    }

    int read_pragma_int(sqlite3 *db, const char *sql) {
      auto stmt = prepare(db, sql);
      if (!stmt) {
        return 0;
      }
      if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        return 0;
      }
      return sqlite3_column_int(stmt.get(), 0);
    }

  }  // anonymous namespace

  // ── Public API ───────────────────────────────────────────────────

  void init(const std::string &db_path) {
    BOOST_LOG(info) << "session_history: initializing database at " << db_path;
    const std::filesystem::path history_db_path {db_path};

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
    sqlite3_busy_timeout(g_write_db.get(), 3000);
    exec(g_write_db.get(), "PRAGMA foreign_keys = ON");

    // Apply schema
    if (!exec(g_write_db.get(), SCHEMA_SQL)) {
      BOOST_LOG(error) << "session_history: schema creation failed";
      g_write_db.reset();
      return;
    }
    tighten_history_db_permissions(history_db_path);

    // Versioned migrations for older databases. Each step remains guarded by
    // column existence so pre-versioning databases can be upgraded safely.
    {
      auto column_exists = [&](const char *table, const char *column) -> bool {
        sqlite3_stmt *check = nullptr;
        std::string sql = std::string("PRAGMA table_info(") + table + ")";
        bool found = false;
        if (sqlite3_prepare_v2(g_write_db.get(), sql.c_str(), -1, &check, nullptr) == SQLITE_OK) {
          while (sqlite3_step(check) == SQLITE_ROW) {
            const unsigned char *name = sqlite3_column_text(check, 1);
            if (name && std::string(reinterpret_cast<const char *>(name)) == column) {
              found = true;
              break;
            }
          }
        }
        if (check) sqlite3_finalize(check);
        return found;
      };

      auto add_column = [&](const char *table, const char *column, const char *type_default) {
        if (!column_exists(table, column)) {
          std::string sql = std::string("ALTER TABLE ") + table + " ADD COLUMN " + column + " " + type_default;
          exec(g_write_db.get(), sql.c_str());
        }
      };

      const int schema_version = read_pragma_int(g_write_db.get(), "PRAGMA user_version");

      if (schema_version < 2) {
        add_column("sessions", "target_requested_bitrate_kbps", "INTEGER");
      }
      if (schema_version < 3) {
        add_column("sessions", "host_cpu_model", "TEXT");
        add_column("sessions", "host_gpu_model", "TEXT");
        add_column("samples", "host_cpu_percent", "REAL DEFAULT -1");
        add_column("samples", "host_gpu_percent", "REAL DEFAULT -1");
        add_column("samples", "host_gpu_encoder_percent", "REAL DEFAULT -1");
        add_column("samples", "host_ram_percent", "REAL DEFAULT -1");
        add_column("samples", "host_vram_percent", "REAL DEFAULT -1");
        add_column("samples", "host_cpu_temp_c", "REAL DEFAULT -1");
        add_column("samples", "host_gpu_temp_c", "REAL DEFAULT -1");
        add_column("samples", "host_net_rx_bps", "REAL DEFAULT -1");
        add_column("samples", "host_net_tx_bps", "REAL DEFAULT -1");
      }
      if (schema_version < 4) {
        add_column("sessions", "yuv444", "INTEGER DEFAULT 0");
        add_column("sessions", "server_version", "TEXT");
      }

      exec(
        g_write_db.get(),
        ("PRAGMA user_version = " + std::to_string(SESSION_HISTORY_SCHEMA_VERSION)).c_str());
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
    sqlite3_busy_timeout(g_read_db.get(), 3000);
    exec(g_read_db.get(), "PRAGMA foreign_keys = ON");
    tighten_history_db_permissions(history_db_path);

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
    evt.timestamp_unix = now_unix();
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
    evt.timestamp_unix = now_unix();
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
    evt.timestamp_unix = now_unix();
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
    std::vector<session_summary_t> result;
    if (!g_read_db) return result;
    std::lock_guard lk {g_read_mutex};

    auto s = prepare(g_read_db.get(),
      "SELECT uuid, protocol, client_name, device_name, app_name, "
      "width, height, target_fps, target_bitrate_kbps, codec, hdr, yuv444, audio_channels, "
      "start_time_unix, end_time_unix, duration_seconds, verdict, target_requested_bitrate_kbps, server_version, "
      "host_cpu_model, host_gpu_model "
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

  std::optional<session_detail_t> get_session_detail(const std::string &uuid, bool include_all) {
    if (!g_read_db) return std::nullopt;
    std::lock_guard lk {g_read_mutex};

    // Fetch session summary
    auto s = prepare(g_read_db.get(),
      "SELECT uuid, protocol, client_name, device_name, app_name, "
      "width, height, target_fps, target_bitrate_kbps, codec, hdr, yuv444, audio_channels, "
      "start_time_unix, end_time_unix, duration_seconds, verdict, target_requested_bitrate_kbps, server_version, "
      "host_cpu_model, host_gpu_model "
      "FROM sessions WHERE uuid = ?");
    if (!s) return std::nullopt;
    sqlite3_bind_text(s.get(), 1, uuid.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(s.get()) != SQLITE_ROW) return std::nullopt;

    session_detail_t detail;
    detail.summary = row_to_summary(s.get());
    detail.total_samples = read_count(g_read_db.get(), "SELECT COUNT(*) FROM samples WHERE session_uuid = ?", uuid);
    detail.total_events = read_count(g_read_db.get(), "SELECT COUNT(*) FROM events WHERE session_uuid = ?", uuid);
    detail.samples_truncated = !include_all && detail.total_samples > DEFAULT_DETAIL_SAMPLE_LIMIT;
    detail.events_truncated = !include_all && detail.total_events > DEFAULT_DETAIL_EVENT_LIMIT;

    // Fetch samples
    auto ss = prepare(g_read_db.get(),
      include_all ?
        "SELECT session_uuid, timestamp_unix, bytes_sent_total, packets_sent_video, "
        "frames_sent, last_frame_index, video_dropped, audio_dropped, "
        "client_reported_losses, idr_requests, ref_invalidations, "
        "encode_latency_ms, actual_fps, actual_bitrate_kbps, frame_interval_jitter_ms, "
        "host_cpu_percent, host_gpu_percent, host_gpu_encoder_percent, "
        "host_ram_percent, host_vram_percent, host_cpu_temp_c, host_gpu_temp_c, "
        "host_net_rx_bps, host_net_tx_bps "
        "FROM samples WHERE session_uuid = ? ORDER BY timestamp_unix"
        :
        "SELECT session_uuid, timestamp_unix, bytes_sent_total, packets_sent_video, "
        "frames_sent, last_frame_index, video_dropped, audio_dropped, "
        "client_reported_losses, idr_requests, ref_invalidations, "
        "encode_latency_ms, actual_fps, actual_bitrate_kbps, frame_interval_jitter_ms, "
        "host_cpu_percent, host_gpu_percent, host_gpu_encoder_percent, "
        "host_ram_percent, host_vram_percent, host_cpu_temp_c, host_gpu_temp_c, "
        "host_net_rx_bps, host_net_tx_bps "
        "FROM ("
        "  SELECT session_uuid, timestamp_unix, bytes_sent_total, packets_sent_video, "
        "  frames_sent, last_frame_index, video_dropped, audio_dropped, "
        "  client_reported_losses, idr_requests, ref_invalidations, "
        "  encode_latency_ms, actual_fps, actual_bitrate_kbps, frame_interval_jitter_ms, "
        "  host_cpu_percent, host_gpu_percent, host_gpu_encoder_percent, "
        "  host_ram_percent, host_vram_percent, host_cpu_temp_c, host_gpu_temp_c, "
        "  host_net_rx_bps, host_net_tx_bps "
        "  FROM samples WHERE session_uuid = ? ORDER BY timestamp_unix DESC LIMIT ?"
        ") ORDER BY timestamp_unix");
    if (ss) {
      sqlite3_bind_text(ss.get(), 1, uuid.c_str(), -1, SQLITE_TRANSIENT);
      if (!include_all) {
        sqlite3_bind_int(ss.get(), 2, DEFAULT_DETAIL_SAMPLE_LIMIT);
      }
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
        // Phase 21 columns are NULL for sessions recorded before that migration.
        auto read_optional_real = [&](int col, double fallback) {
          if (sqlite3_column_count(ss.get()) > col && sqlite3_column_type(ss.get(), col) != SQLITE_NULL) {
            return sqlite3_column_double(ss.get(), col);
          }
          return fallback;
        };
        sample.host_cpu_percent = read_optional_real(15, -1);
        sample.host_gpu_percent = read_optional_real(16, -1);
        sample.host_gpu_encoder_percent = read_optional_real(17, -1);
        sample.host_ram_percent = read_optional_real(18, -1);
        sample.host_vram_percent = read_optional_real(19, -1);
        sample.host_cpu_temp_c = read_optional_real(20, -1);
        sample.host_gpu_temp_c = read_optional_real(21, -1);
        sample.host_net_rx_bps = read_optional_real(22, -1);
        sample.host_net_tx_bps = read_optional_real(23, -1);
        detail.samples.push_back(std::move(sample));
      }
    }

    // Fetch events
    auto es = prepare(g_read_db.get(),
      include_all ?
        "SELECT session_uuid, timestamp_unix, event_type, payload "
        "FROM events WHERE session_uuid = ? ORDER BY timestamp_unix"
        :
        "SELECT session_uuid, timestamp_unix, event_type, payload "
        "FROM ("
        "  SELECT session_uuid, timestamp_unix, event_type, payload "
        "  FROM events WHERE session_uuid = ? ORDER BY timestamp_unix DESC LIMIT ?"
        ") ORDER BY timestamp_unix");
    if (es) {
      sqlite3_bind_text(es.get(), 1, uuid.c_str(), -1, SQLITE_TRANSIENT);
      if (!include_all) {
        sqlite3_bind_int(es.get(), 2, DEFAULT_DETAIL_EVENT_LIMIT);
      }
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
