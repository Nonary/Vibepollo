/**
 * @file src/session_history_storage.cpp
 * @brief Internal SQLite storage and schema helpers for session history.
 */

// standard includes
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

// local includes
#include "session_history_storage.h"
#include "logging.h"
#include "utility.h"

#ifdef _WIN32
  #include <winsock2.h>
  #include <AclAPI.h>
#endif

namespace session_history::storage {
  namespace {

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
        encoder_bitrate_kbps INTEGER,
        requested_bitrate_kbps INTEGER,
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
        host_gpu_model TEXT,
        stream_gpu_model TEXT
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

    bool session_accepts_live_updates(sqlite3 *db, const std::string &uuid) {
      auto stmt = prepare(db, "SELECT end_time_unix FROM sessions WHERE uuid = ?");
      if (!stmt) return false;

      sqlite3_bind_text(stmt.get(), 1, uuid.c_str(), -1, SQLITE_TRANSIENT);
      if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        return false;
      }

      return sqlite3_column_type(stmt.get(), 0) == SQLITE_NULL;
    }

    session_summary_t row_to_summary(sqlite3_stmt *stmt) {
      session_summary_t out;
      out.uuid = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
      out.protocol = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
      auto col2 = sqlite3_column_text(stmt, 2);
      out.client_name = col2 ? reinterpret_cast<const char *>(col2) : "";
      auto col3 = sqlite3_column_text(stmt, 3);
      out.device_name = col3 ? reinterpret_cast<const char *>(col3) : "";
      auto col4 = sqlite3_column_text(stmt, 4);
      out.app_name = col4 ? reinterpret_cast<const char *>(col4) : "";
      out.width = sqlite3_column_int(stmt, 5);
      out.height = sqlite3_column_int(stmt, 6);
      out.target_fps = sqlite3_column_int(stmt, 7);
      out.encoder_bitrate_kbps = sqlite3_column_int(stmt, 8);
      auto col9 = sqlite3_column_text(stmt, 9);
      out.codec = col9 ? reinterpret_cast<const char *>(col9) : "";
      out.hdr = sqlite3_column_int(stmt, 10) != 0;
      out.yuv444 = sqlite3_column_int(stmt, 11) != 0;
      out.audio_channels = sqlite3_column_int(stmt, 12);
      out.start_time_unix = sqlite3_column_double(stmt, 13);
      out.end_time_unix = sqlite3_column_double(stmt, 14);
      out.duration_seconds = sqlite3_column_double(stmt, 15);
      auto col16 = sqlite3_column_text(stmt, 16);
      out.verdict = col16 ? reinterpret_cast<const char *>(col16) : "unknown";
      if (sqlite3_column_count(stmt) > 17 && sqlite3_column_type(stmt, 17) != SQLITE_NULL) {
        out.requested_bitrate_kbps = sqlite3_column_int(stmt, 17);
      } else {
        out.requested_bitrate_kbps = out.encoder_bitrate_kbps;
      }
      if (sqlite3_column_count(stmt) > 18 && sqlite3_column_type(stmt, 18) != SQLITE_NULL) {
        auto col18 = sqlite3_column_text(stmt, 18);
        out.server_version = col18 ? reinterpret_cast<const char *>(col18) : "";
      }
      if (sqlite3_column_count(stmt) > 19 && sqlite3_column_type(stmt, 19) != SQLITE_NULL) {
        auto col19 = sqlite3_column_text(stmt, 19);
        out.host_cpu_model = col19 ? reinterpret_cast<const char *>(col19) : "";
      }
      if (sqlite3_column_count(stmt) > 20 && sqlite3_column_type(stmt, 20) != SQLITE_NULL) {
        auto col20 = sqlite3_column_text(stmt, 20);
        out.host_gpu_model = col20 ? reinterpret_cast<const char *>(col20) : "";
      }
      if (sqlite3_column_count(stmt) > 21 && sqlite3_column_type(stmt, 21) != SQLITE_NULL) {
        auto col21 = sqlite3_column_text(stmt, 21);
        out.stream_gpu_model = col21 ? reinterpret_cast<const char *>(col21) : "";
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

    std::int64_t read_pragma_int64(sqlite3 *db, const char *sql) {
      auto stmt = prepare(db, sql);
      if (!stmt) {
        return 0;
      }
      if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        return 0;
      }
      return sqlite3_column_int64(stmt.get(), 0);
    }

    bool delete_session_rows(sqlite3 *db, const std::string &uuid, bool track_changes, int &affected) {
      constexpr const char *DELETE_EVENTS = "DELETE FROM events WHERE session_uuid = ?";
      constexpr const char *DELETE_SAMPLES = "DELETE FROM samples WHERE session_uuid = ?";
      constexpr const char *DELETE_SESSION = "DELETE FROM sessions WHERE uuid = ?";

      affected = 0;
      for (const auto &[sql, counts_changes] : {
             std::pair {DELETE_EVENTS, false},
             std::pair {DELETE_SAMPLES, false},
             std::pair {DELETE_SESSION, true},
           }) {
        auto stmt = prepare(db, sql);
        if (!stmt) return false;

        sqlite3_bind_text(stmt.get(), 1, uuid.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
          BOOST_LOG(error) << "session_history: delete failed for uuid=" << uuid << ": " << sqlite3_errmsg(db);
          return false;
        }
        if (track_changes && counts_changes) {
          affected = sqlite3_changes(db);
        }
      }

      return true;
    }

    bool prune_sessions_by_limit(sqlite3 *db, int max_history_sessions) {
      const auto limit_str = std::to_string(max_history_sessions);
      const std::string delete_events =
        "DELETE FROM events WHERE session_uuid IN ("
        "  SELECT uuid FROM sessions WHERE end_time_unix IS NOT NULL "
        "  ORDER BY end_time_unix DESC LIMIT -1 OFFSET " + limit_str +
        ")";
      const std::string delete_samples =
        "DELETE FROM samples WHERE session_uuid IN ("
        "  SELECT uuid FROM sessions WHERE end_time_unix IS NOT NULL "
        "  ORDER BY end_time_unix DESC LIMIT -1 OFFSET " + limit_str +
        ")";
      const std::string delete_sessions =
        "DELETE FROM sessions WHERE end_time_unix IS NOT NULL "
        "AND uuid NOT IN ("
        "  SELECT uuid FROM sessions WHERE end_time_unix IS NOT NULL "
        "  ORDER BY end_time_unix DESC LIMIT " + limit_str +
        ")";
      return exec(db, delete_events.c_str()) &&
             exec(db, delete_samples.c_str()) &&
             exec(db, delete_sessions.c_str());
    }

    bool prune_sessions_older_than(sqlite3 *db, double cutoff_unix) {
      auto delete_events = prepare(
        db,
        "DELETE FROM events WHERE session_uuid IN ("
        "  SELECT uuid FROM sessions WHERE end_time_unix IS NOT NULL AND end_time_unix < ?"
        ")"
      );
      if (!delete_events) return false;
      sqlite3_bind_double(delete_events.get(), 1, cutoff_unix);
      if (sqlite3_step(delete_events.get()) != SQLITE_DONE) {
        BOOST_LOG(error) << "session_history: TTL event prune failed: " << sqlite3_errmsg(db);
        return false;
      }

      auto delete_samples = prepare(
        db,
        "DELETE FROM samples WHERE session_uuid IN ("
        "  SELECT uuid FROM sessions WHERE end_time_unix IS NOT NULL AND end_time_unix < ?"
        ")"
      );
      if (!delete_samples) return false;
      sqlite3_bind_double(delete_samples.get(), 1, cutoff_unix);
      if (sqlite3_step(delete_samples.get()) != SQLITE_DONE) {
        BOOST_LOG(error) << "session_history: TTL sample prune failed: " << sqlite3_errmsg(db);
        return false;
      }

      auto delete_sessions = prepare(
        db,
        "DELETE FROM sessions WHERE end_time_unix IS NOT NULL AND end_time_unix < ?"
      );
      if (!delete_sessions) return false;
      sqlite3_bind_double(delete_sessions.get(), 1, cutoff_unix);
      if (sqlite3_step(delete_sessions.get()) != SQLITE_DONE) {
        BOOST_LOG(error) << "session_history: TTL session prune failed: " << sqlite3_errmsg(db);
        return false;
      }

      return true;
    }

    std::optional<std::string> oldest_ended_session_uuid(sqlite3 *db) {
      auto stmt = prepare(
        db,
        "SELECT uuid FROM sessions WHERE end_time_unix IS NOT NULL ORDER BY end_time_unix ASC LIMIT 1"
      );
      if (!stmt) {
        return std::nullopt;
      }
      if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        return std::string {};
      }
      auto text = sqlite3_column_text(stmt.get(), 0);
      if (!text) {
        return std::string {};
      }
      return std::string(reinterpret_cast<const char *>(text));
    }

    std::uint64_t estimate_live_db_bytes(sqlite3 *db) {
      const auto page_size = static_cast<std::uint64_t>(std::max<std::int64_t>(read_pragma_int64(db, "PRAGMA page_size"), 0));
      const auto page_count = static_cast<std::uint64_t>(std::max<std::int64_t>(read_pragma_int64(db, "PRAGMA page_count"), 0));
      const auto freelist_count = static_cast<std::uint64_t>(std::max<std::int64_t>(read_pragma_int64(db, "PRAGMA freelist_count"), 0));
      const auto live_pages = page_count > freelist_count ? page_count - freelist_count : 0;
      return live_pages * page_size;
    }

  }  // namespace

  void sqlite_deleter::operator()(sqlite3 *db) const {
    if (db) sqlite3_close(db);
  }

  void stmt_deleter::operator()(sqlite3_stmt *stmt) const {
    if (stmt) sqlite3_finalize(stmt);
  }

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

  bool checkpoint(sqlite3 *db) {
    return exec(db, "PRAGMA wal_checkpoint(TRUNCATE)");
  }

#ifdef SUNSHINE_TESTS
  bool force_set_end_time(sqlite3 *db, const std::string &uuid, double end_time_unix) {
    auto stmt = prepare(
      db,
      "UPDATE sessions SET end_time_unix = ?, duration_seconds = ? - start_time_unix WHERE uuid = ?"
    );
    if (!stmt) return false;

    sqlite3_bind_double(stmt.get(), 1, end_time_unix);
    sqlite3_bind_double(stmt.get(), 2, end_time_unix);
    sqlite3_bind_text(stmt.get(), 3, uuid.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
      BOOST_LOG(error) << "session_history: force_set_end_time failed for uuid=" << uuid
                       << ": " << sqlite3_errmsg(db);
      return false;
    }
    return sqlite3_changes(db) > 0;
  }
#endif

  double now_unix() {
    auto tp = std::chrono::system_clock::now();
    return std::chrono::duration<double>(tp.time_since_epoch()).count();
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

    const auto parent_path = db_path.parent_path();
    if (!parent_path.empty() && db_path.filename() == "session_history.db") {
      apply_windows_permissions(parent_path);
    }
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

    const auto apply_posix_directory_permissions = [&](const std::filesystem::path &path) {
      std::error_code ec;
      if (path.empty() || !std::filesystem::exists(path, ec) || ec) {
        return;
      }
      std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace,
        ec);
      if (ec) {
        BOOST_LOG(warning) << "session_history: failed to tighten directory permissions for " << path.string()
                           << ": " << ec.message();
      }
    };

    if (db_path.filename() == "session_history.db") {
      apply_posix_directory_permissions(db_path.parent_path());
    }
    apply_posix_permissions(db_path);
    apply_posix_permissions(db_path.string() + "-wal");
    apply_posix_permissions(db_path.string() + "-shm");
#endif
  }

  bool open_write_db(const std::string &db_path, db_ptr &out_db) {
    const std::filesystem::path path {db_path};
    if (!path.parent_path().empty()) {
      std::error_code ec;
      std::filesystem::create_directories(path.parent_path(), ec);
      if (ec) {
        BOOST_LOG(error) << "session_history: failed to create DB directory " << path.parent_path().string()
                         << ": " << ec.message();
        return false;
      }
    }

    sqlite3 *raw = nullptr;
    int rc = sqlite3_open(db_path.c_str(), &raw);
    if (rc != SQLITE_OK) {
      BOOST_LOG(error) << "session_history: failed to open write DB: " << sqlite3_errmsg(raw);
      sqlite3_close(raw);
      return false;
    }
    out_db.reset(raw);
    return true;
  }

  bool open_read_db(const std::string &db_path, db_ptr &out_db) {
    sqlite3 *raw = nullptr;
    int rc = sqlite3_open_v2(db_path.c_str(), &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) {
      BOOST_LOG(error) << "session_history: failed to open read DB: " << sqlite3_errmsg(raw);
      sqlite3_close(raw);
      return false;
    }
    out_db.reset(raw);
    return true;
  }

  bool apply_schema_and_migrations(sqlite3 *db, int schema_version) {
    if (!exec(db, SCHEMA_SQL)) {
      BOOST_LOG(error) << "session_history: schema creation failed";
      return false;
    }

    auto column_exists = [&](const char *table, const char *column) -> bool {
      sqlite3_stmt *check = nullptr;
      std::string sql = std::string("PRAGMA table_info(") + table + ")";
      bool found = false;
      if (sqlite3_prepare_v2(db, sql.c_str(), -1, &check, nullptr) == SQLITE_OK) {
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
        exec(db, sql.c_str());
      }
    };

    auto rename_column = [&](const char *table, const char *old_name, const char *new_name) {
      if (!column_exists(table, old_name) || column_exists(table, new_name)) {
        return true;
      }
      const std::string sql = std::string("ALTER TABLE ") + table + " RENAME COLUMN " + old_name + " TO " + new_name;
      if (!exec(db, sql.c_str())) {
        BOOST_LOG(error) << "session_history: failed to rename " << table << "." << old_name << " to " << new_name;
        return false;
      }
      return true;
    };

    const int current_schema_version = read_pragma_int(db, "PRAGMA user_version");
    (void) schema_version;

    if (current_schema_version < 2) {
      add_column("sessions", "target_requested_bitrate_kbps", "INTEGER");
    }
    if (current_schema_version < 3) {
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
    if (current_schema_version < 4) {
      add_column("sessions", "yuv444", "INTEGER DEFAULT 0");
      add_column("sessions", "server_version", "TEXT");
    }
    if (current_schema_version < 5) {
      add_column("sessions", "stream_gpu_model", "TEXT");
    }
    if (current_schema_version < 6) {
      if (!rename_column("sessions", "target_bitrate_kbps", "encoder_bitrate_kbps")) {
        return false;
      }
      if (!rename_column("sessions", "target_requested_bitrate_kbps", "requested_bitrate_kbps")) {
        return false;
      }
    }

    return exec(db, ("PRAGMA user_version = " + std::to_string(schema_version)).c_str());
  }

  bool process_begin(sqlite3 *db, const session_metadata_t &metadata) {
    auto stmt = prepare(db,
      "INSERT OR IGNORE INTO sessions "
      "(uuid, protocol, client_name, device_name, app_name, "
      " width, height, target_fps, encoder_bitrate_kbps, requested_bitrate_kbps, "
      " codec, hdr, yuv444, audio_channels, start_time_unix, server_version, host_cpu_model, host_gpu_model, stream_gpu_model) "
      "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    if (!stmt) return false;

    sqlite3_bind_text(stmt.get(), 1, metadata.uuid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, metadata.protocol.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 3, metadata.client_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 4, metadata.device_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 5, metadata.app_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 6, metadata.width);
    sqlite3_bind_int(stmt.get(), 7, metadata.height);
    sqlite3_bind_int(stmt.get(), 8, metadata.target_fps);
    sqlite3_bind_int(stmt.get(), 9, metadata.encoder_bitrate_kbps);
    sqlite3_bind_int(stmt.get(), 10, metadata.requested_bitrate_kbps);
    sqlite3_bind_text(stmt.get(), 11, metadata.codec.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 12, metadata.hdr ? 1 : 0);
    sqlite3_bind_int(stmt.get(), 13, metadata.yuv444 ? 1 : 0);
    sqlite3_bind_int(stmt.get(), 14, metadata.audio_channels);
    sqlite3_bind_double(stmt.get(), 15, now_unix());
    sqlite3_bind_text(stmt.get(), 16, metadata.server_version.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 17, metadata.host_cpu_model.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 18, metadata.host_gpu_model.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 19, metadata.stream_gpu_model.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
      BOOST_LOG(error) << "session_history: begin_session insert failed for uuid=" << metadata.uuid
                       << ": " << sqlite3_errmsg(db);
      return false;
    }

    if (sqlite3_changes(db) == 0) {
      BOOST_LOG(warning)
        << "session_history: begin_session ignored - uuid already present in DB: "
        << metadata.uuid << " (sessions will be merged into the existing row)";
    }
    return true;
  }

  bool process_end(sqlite3 *db, const std::string &uuid) {
    double end_time = now_unix();

    std::string verdict = "unknown";
    {
      auto stmt = prepare(db,
        "SELECT COUNT(*), "
        "SUM(CASE WHEN encode_latency_ms > 16 THEN 1 ELSE 0 END), "
        "MAX(client_reported_losses), "
        "MAX(frames_sent) "
        "FROM samples WHERE session_uuid = ?");
      if (stmt) {
        sqlite3_bind_text(stmt.get(), 1, uuid.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
          int sample_count = sqlite3_column_int(stmt.get(), 0);
          int high_latency_count = sqlite3_column_int(stmt.get(), 1);
          std::int64_t max_reported_losses = sqlite3_column_int64(stmt.get(), 2);
          std::int64_t max_frames_sent = sqlite3_column_int64(stmt.get(), 3);

          if (sample_count > 0) {
            double loss_ratio = max_frames_sent > 0 ? static_cast<double>(max_reported_losses) / static_cast<double>(max_frames_sent) : 0;

            if (loss_ratio > 0.05) {
              verdict = "failed";
            } else if (high_latency_count > 0 || max_reported_losses > 0) {
              verdict = "degraded";
            } else {
              verdict = "healthy";
            }
          }
        }
      }
    }

    auto stmt = prepare(db,
      "UPDATE sessions SET end_time_unix = ?, "
      "duration_seconds = ? - start_time_unix, "
      "verdict = ? "
      "WHERE uuid = ?");
    if (!stmt) return false;

    sqlite3_bind_double(stmt.get(), 1, end_time);
    sqlite3_bind_double(stmt.get(), 2, end_time);
    sqlite3_bind_text(stmt.get(), 3, verdict.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 4, uuid.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
      BOOST_LOG(error) << "session_history: end_session update failed for uuid=" << uuid
                       << ": " << sqlite3_errmsg(db);
      return false;
    }
    return true;
  }

  bool process_sample(sqlite3 *db, const session_sample_t &sample, int max_samples_per_session) {
    if (!session_accepts_live_updates(db, sample.session_uuid)) {
      BOOST_LOG(debug) << "session_history: dropping stale sample for ended or missing session "
                       << sample.session_uuid;
      return true;
    }
    auto stmt = prepare(db,
      "INSERT INTO samples "
      "(session_uuid, timestamp_unix, bytes_sent_total, packets_sent_video, "
      " frames_sent, last_frame_index, video_dropped, audio_dropped, "
      " client_reported_losses, idr_requests, ref_invalidations, "
      " encode_latency_ms, actual_fps, actual_bitrate_kbps, frame_interval_jitter_ms, "
      " host_cpu_percent, host_gpu_percent, host_gpu_encoder_percent, "
      " host_ram_percent, host_vram_percent, host_cpu_temp_c, host_gpu_temp_c, "
      " host_net_rx_bps, host_net_tx_bps) "
      "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    if (!stmt) return false;

    sqlite3_bind_text(stmt.get(), 1, sample.session_uuid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt.get(), 2, sample.timestamp_unix);
    sqlite3_bind_int64(stmt.get(), 3, static_cast<std::int64_t>(sample.bytes_sent_total));
    sqlite3_bind_int64(stmt.get(), 4, static_cast<std::int64_t>(sample.packets_sent_video));
    sqlite3_bind_int64(stmt.get(), 5, static_cast<std::int64_t>(sample.frames_sent));
    sqlite3_bind_int64(stmt.get(), 6, sample.last_frame_index);
    sqlite3_bind_int64(stmt.get(), 7, static_cast<std::int64_t>(sample.video_dropped));
    sqlite3_bind_int64(stmt.get(), 8, static_cast<std::int64_t>(sample.audio_dropped));
    sqlite3_bind_int64(stmt.get(), 9, sample.client_reported_losses);
    sqlite3_bind_int(stmt.get(), 10, static_cast<int>(sample.idr_requests));
    sqlite3_bind_int(stmt.get(), 11, static_cast<int>(sample.ref_invalidations));
    sqlite3_bind_double(stmt.get(), 12, sample.encode_latency_ms);
    sqlite3_bind_double(stmt.get(), 13, sample.actual_fps);
    sqlite3_bind_double(stmt.get(), 14, sample.actual_bitrate_kbps);
    sqlite3_bind_double(stmt.get(), 15, sample.frame_interval_jitter_ms);
    sqlite3_bind_double(stmt.get(), 16, sample.host_cpu_percent);
    sqlite3_bind_double(stmt.get(), 17, sample.host_gpu_percent);
    sqlite3_bind_double(stmt.get(), 18, sample.host_gpu_encoder_percent);
    sqlite3_bind_double(stmt.get(), 19, sample.host_ram_percent);
    sqlite3_bind_double(stmt.get(), 20, sample.host_vram_percent);
    sqlite3_bind_double(stmt.get(), 21, sample.host_cpu_temp_c);
    sqlite3_bind_double(stmt.get(), 22, sample.host_gpu_temp_c);
    sqlite3_bind_double(stmt.get(), 23, sample.host_net_rx_bps);
    sqlite3_bind_double(stmt.get(), 24, sample.host_net_tx_bps);

    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
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
    sqlite3_bind_int(trim.get(), 2, max_samples_per_session);
    if (sqlite3_step(trim.get()) != SQLITE_DONE) {
      BOOST_LOG(error) << "session_history: sample trim failed for uuid=" << sample.session_uuid
                       << ": " << sqlite3_errmsg(db);
      return false;
    }
    return true;
  }

  bool process_event(sqlite3 *db, const session_event_t &event, int max_events_per_session) {
    if (!session_accepts_live_updates(db, event.session_uuid)) {
      BOOST_LOG(debug) << "session_history: dropping stale event for ended or missing session "
                       << event.session_uuid << " type=" << event.event_type;
      return true;
    }
    auto stmt = prepare(db,
      "INSERT INTO events (session_uuid, timestamp_unix, event_type, payload) "
      "VALUES (?,?,?,?)");
    if (!stmt) return false;

    sqlite3_bind_text(stmt.get(), 1, event.session_uuid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt.get(), 2, event.timestamp_unix);
    sqlite3_bind_text(stmt.get(), 3, event.event_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 4, event.payload.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
      BOOST_LOG(error) << "session_history: event insert failed for uuid=" << event.session_uuid
                       << " type=" << event.event_type << ": " << sqlite3_errmsg(db);
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
    sqlite3_bind_text(trim.get(), 1, event.session_uuid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(trim.get(), 2, max_events_per_session);
    if (sqlite3_step(trim.get()) != SQLITE_DONE) {
      BOOST_LOG(error) << "session_history: event trim failed for uuid=" << event.session_uuid
                       << " type=" << event.event_type << ": " << sqlite3_errmsg(db);
      return false;
    }
    return true;
  }

  delete_apply_e process_delete(sqlite3 *db, const std::string &uuid) {
    int affected = 0;
    if (!delete_session_rows(db, uuid, true, affected)) {
      return delete_apply_e::failed;
    }

    BOOST_LOG(info) << "Deleted session " << uuid << " from history (rows=" << affected << ")";
    return affected > 0 ? delete_apply_e::deleted : delete_apply_e::not_found;
  }

  bool process_prune(sqlite3 *db, const prune_options_t &options) {
    if (options.max_history_sessions > 0 && !prune_sessions_by_limit(db, options.max_history_sessions)) {
      return false;
    }

    if (options.prune_sessions_ended_before_unix > 0 &&
        !prune_sessions_older_than(db, options.prune_sessions_ended_before_unix)) {
      return false;
    }

    if (options.max_db_size_bytes > 0) {
      while (estimate_live_db_bytes(db) > options.max_db_size_bytes) {
        auto oldest = oldest_ended_session_uuid(db);
        if (!oldest.has_value()) {
          return false;
        }
        if (oldest->empty()) {
          break;
        }

        int affected = 0;
        if (!delete_session_rows(db, *oldest, false, affected)) {
          return false;
        }
      }
    }

    return true;
  }

  std::vector<session_summary_t> read_session_summaries(sqlite3 *db, int limit, int offset) {
    std::vector<session_summary_t> result;
    auto stmt = prepare(db,
      "SELECT uuid, protocol, client_name, device_name, app_name, "
      "width, height, target_fps, encoder_bitrate_kbps, codec, hdr, yuv444, audio_channels, "
      "start_time_unix, end_time_unix, duration_seconds, verdict, requested_bitrate_kbps, server_version, "
      "host_cpu_model, host_gpu_model, stream_gpu_model "
      "FROM sessions WHERE end_time_unix IS NOT NULL "
      "ORDER BY end_time_unix DESC LIMIT ? OFFSET ?");
    if (!stmt) return result;

    sqlite3_bind_int(stmt.get(), 1, limit);
    sqlite3_bind_int(stmt.get(), 2, offset);

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
      result.push_back(row_to_summary(stmt.get()));
    }
    return result;
  }

  std::optional<session_detail_t> read_session_detail(
    sqlite3 *db,
    const std::string &uuid,
    bool include_all,
    int default_detail_sample_limit,
    int default_detail_event_limit) {
    auto stmt = prepare(db,
      "SELECT uuid, protocol, client_name, device_name, app_name, "
      "width, height, target_fps, encoder_bitrate_kbps, codec, hdr, yuv444, audio_channels, "
      "start_time_unix, end_time_unix, duration_seconds, verdict, requested_bitrate_kbps, server_version, "
      "host_cpu_model, host_gpu_model, stream_gpu_model "
      "FROM sessions WHERE uuid = ?");
    if (!stmt) return std::nullopt;
    sqlite3_bind_text(stmt.get(), 1, uuid.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) return std::nullopt;

    session_detail_t detail;
    detail.summary = row_to_summary(stmt.get());
    detail.total_samples = read_count(db, "SELECT COUNT(*) FROM samples WHERE session_uuid = ?", uuid);
    detail.total_events = read_count(db, "SELECT COUNT(*) FROM events WHERE session_uuid = ?", uuid);
    detail.samples_truncated = !include_all && detail.total_samples > default_detail_sample_limit;
    detail.events_truncated = !include_all && detail.total_events > default_detail_event_limit;

    auto sample_stmt = prepare(db,
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
    if (sample_stmt) {
      sqlite3_bind_text(sample_stmt.get(), 1, uuid.c_str(), -1, SQLITE_TRANSIENT);
      if (!include_all) {
        sqlite3_bind_int(sample_stmt.get(), 2, default_detail_sample_limit);
      }
      while (sqlite3_step(sample_stmt.get()) == SQLITE_ROW) {
        session_sample_t sample;
        auto col0 = sqlite3_column_text(sample_stmt.get(), 0);
        sample.session_uuid = col0 ? reinterpret_cast<const char *>(col0) : "";
        sample.timestamp_unix = sqlite3_column_double(sample_stmt.get(), 1);
        sample.bytes_sent_total = static_cast<std::uint64_t>(sqlite3_column_int64(sample_stmt.get(), 2));
        sample.packets_sent_video = static_cast<std::uint64_t>(sqlite3_column_int64(sample_stmt.get(), 3));
        sample.frames_sent = static_cast<std::uint64_t>(sqlite3_column_int64(sample_stmt.get(), 4));
        sample.last_frame_index = sqlite3_column_int64(sample_stmt.get(), 5);
        sample.video_dropped = static_cast<std::uint64_t>(sqlite3_column_int64(sample_stmt.get(), 6));
        sample.audio_dropped = static_cast<std::uint64_t>(sqlite3_column_int64(sample_stmt.get(), 7));
        sample.client_reported_losses = sqlite3_column_int64(sample_stmt.get(), 8);
        sample.idr_requests = static_cast<std::uint32_t>(sqlite3_column_int(sample_stmt.get(), 9));
        sample.ref_invalidations = static_cast<std::uint32_t>(sqlite3_column_int(sample_stmt.get(), 10));
        sample.encode_latency_ms = sqlite3_column_double(sample_stmt.get(), 11);
        sample.actual_fps = sqlite3_column_double(sample_stmt.get(), 12);
        sample.actual_bitrate_kbps = sqlite3_column_double(sample_stmt.get(), 13);
        sample.frame_interval_jitter_ms = sqlite3_column_double(sample_stmt.get(), 14);
        auto read_optional_real = [&](int col, double fallback) {
          if (sqlite3_column_count(sample_stmt.get()) > col && sqlite3_column_type(sample_stmt.get(), col) != SQLITE_NULL) {
            return sqlite3_column_double(sample_stmt.get(), col);
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

    auto event_stmt = prepare(db,
      include_all ?
        "SELECT session_uuid, timestamp_unix, event_type, payload "
        "FROM events WHERE session_uuid = ? ORDER BY timestamp_unix"
        :
        "SELECT session_uuid, timestamp_unix, event_type, payload "
        "FROM ("
        "  SELECT session_uuid, timestamp_unix, event_type, payload "
        "  FROM events WHERE session_uuid = ? ORDER BY timestamp_unix DESC LIMIT ?"
        ") ORDER BY timestamp_unix");
    if (event_stmt) {
      sqlite3_bind_text(event_stmt.get(), 1, uuid.c_str(), -1, SQLITE_TRANSIENT);
      if (!include_all) {
        sqlite3_bind_int(event_stmt.get(), 2, default_detail_event_limit);
      }
      while (sqlite3_step(event_stmt.get()) == SQLITE_ROW) {
        session_event_t event;
        auto col0 = sqlite3_column_text(event_stmt.get(), 0);
        event.session_uuid = col0 ? reinterpret_cast<const char *>(col0) : "";
        event.timestamp_unix = sqlite3_column_double(event_stmt.get(), 1);
        auto col2 = sqlite3_column_text(event_stmt.get(), 2);
        event.event_type = col2 ? reinterpret_cast<const char *>(col2) : "";
        auto col3 = sqlite3_column_text(event_stmt.get(), 3);
        event.payload = col3 ? reinterpret_cast<const char *>(col3) : "";
        detail.events.push_back(std::move(event));
      }
    }

    return detail;
  }

}  // namespace session_history::storage
