/**
 * @file tests/unit/test_session_history.cpp
 * @brief Tests for src/session_history.* — SQLite persistence of streaming
 *        session metadata, samples and events.
 *
 * Exercises the public lifecycle (init/begin/end/record_event/list/detail/
 * delete/shutdown) end-to-end against a temporary on-disk database. The
 * writer thread is asynchronous so tests poll briefly after each mutation
 * before asserting on the read side.
 */
#include "../tests_common.h"

#include <src/config.h>
#include <src/session_history.h>
#include <src/session_history_storage.h>

#include <chrono>
#include <filesystem>
#include <sqlite3.h>
#include <thread>

namespace {

  std::filesystem::path
    make_temp_db_path(const char *label) {
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path = std::filesystem::temp_directory_path() /
                ("vibepollo-tests-" + std::string(label) + "-" + std::to_string(stamp) + ".sqlite");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return path;
  }

  /**
   * @brief Poll the read API until @p pred is true or the timeout elapses.
   *
   * Necessary because the writer thread is asynchronous — begin_session /
   * record_event / end_session enqueue commands and return immediately.
   */
  template<typename Predicate>
  bool
    wait_for(Predicate &&pred, std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (pred()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred();
  }

  bool exec_sql(sqlite3 *db, const char *sql) {
    char *err = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (err) {
      sqlite3_free(err);
    }
    return rc == SQLITE_OK;
  }

  bool column_exists(sqlite3 *db, const char *table, const char *column) {
    sqlite3_stmt *stmt = nullptr;
    const std::string sql = std::string("PRAGMA table_info(") + table + ")";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
      return false;
    }

    bool found = false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      const auto *name = sqlite3_column_text(stmt, 1);
      if (name && std::string(reinterpret_cast<const char *>(name)) == column) {
        found = true;
        break;
      }
    }
    sqlite3_finalize(stmt);
    return found;
  }

  bool foreign_key_has_delete_cascade(sqlite3 *db, const char *table, const char *parent_table, const char *from_column) {
    sqlite3_stmt *stmt = nullptr;
    const std::string sql = std::string("PRAGMA foreign_key_list(") + table + ")";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
      return false;
    }

    bool found = false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      const auto *parent = sqlite3_column_text(stmt, 2);
      const auto *from = sqlite3_column_text(stmt, 3);
      const auto *on_delete = sqlite3_column_text(stmt, 6);
      if (parent && from && on_delete &&
          std::string(reinterpret_cast<const char *>(parent)) == parent_table &&
          std::string(reinterpret_cast<const char *>(from)) == from_column &&
          std::string(reinterpret_cast<const char *>(on_delete)) == "CASCADE") {
        found = true;
        break;
      }
    }

    sqlite3_finalize(stmt);
    return found;
  }

  std::size_t count_rows_for_uuid(sqlite3 *db, const char *table, const std::string &uuid) {
    sqlite3_stmt *stmt = nullptr;
    const std::string sql = std::string("SELECT COUNT(*) FROM ") + table + " WHERE session_uuid = ?";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
      return 0;
    }

    sqlite3_bind_text(stmt, 1, uuid.c_str(), -1, SQLITE_TRANSIENT);
    std::size_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      count = static_cast<std::size_t>(sqlite3_column_int64(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return count;
  }

  session_history::session_metadata_t
    make_metadata(const std::string &uuid, const std::string &app = "Test App") {
    session_history::session_metadata_t m;
    m.uuid = uuid;
    m.protocol = "rtsp";
    m.client_name = "TestClient";
    m.device_name = "TestDevice";
    m.app_name = app;
    m.width = 1920;
    m.height = 1080;
    m.target_fps = 60;
    m.encoder_bitrate_kbps = 20000;
    m.requested_bitrate_kbps = 25000;
    m.codec = "h264";
    m.hdr = false;
    m.yuv444 = false;
    m.audio_channels = 2;
    m.host_cpu_model = "TestCPU";
    m.host_gpu_model = "TestGPU";
    m.stream_gpu_model = "StreamGPU";
    return m;
  }

  session_history::session_sample_t
    make_sample(const std::string &uuid, std::uint64_t frames_sent = 10, std::uint64_t bytes_sent_total = 4096) {
    session_history::session_sample_t sample;
    sample.session_uuid = uuid;
    sample.timestamp_unix = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    sample.frames_sent = frames_sent;
    sample.bytes_sent_total = bytes_sent_total;
    sample.packets_sent_video = frames_sent;
    sample.last_frame_index = static_cast<std::int64_t>(frames_sent);
    sample.encode_latency_ms = 4.0;
    sample.actual_fps = 60.0;
    sample.actual_bitrate_kbps = 12000.0;
    return sample;
  }

  /**
   * @brief RAII guard that calls session_history::shutdown() on destruction.
   *
   * Ensures background threads are stopped even when a test assertion
   * throws.
   */
  struct HistoryGuard {
    ~HistoryGuard() {
      session_history::reset_queue_limits_for_tests();
      session_history::shutdown();
    }
  };

  struct SunshineConfigGuard {
    config::sunshine_t saved = config::sunshine;

    ~SunshineConfigGuard() {
      config::sunshine = saved;
    }
  };

}  // namespace

TEST(SessionHistory, InitAndShutdown) {
  auto path = make_temp_db_path("init");
  session_history::init(path.string());
  HistoryGuard guard;

  // No sessions yet — list must succeed and return empty.
  auto rows = session_history::list_sessions(50, 0);
  EXPECT_TRUE(rows.empty());

  // Active sessions must also be empty.
  auto active = session_history::get_active_sessions();
  EXPECT_TRUE(active.empty());
}

TEST(SessionHistory, BeginEndPersists) {
  auto path = make_temp_db_path("begin-end");
  session_history::init(path.string());
  HistoryGuard guard;

  const std::string uuid = "11111111-1111-1111-1111-111111111111";
  session_history::begin_session(make_metadata(uuid, "Solitaire"));

  // get_active_sessions() reflects the live RTSP / WebRTC state, not the
  // history module's begin_session bookkeeping, so we don't assert against
  // it here. The session row should still be persisted by the writer.

  // Wait for the writer to persist the begin_session row. list_sessions()
  // only returns *ended* sessions, so we end it first to make it queryable.
  session_history::end_session(uuid);
  bool persisted = wait_for([&] {
    auto rows = session_history::list_sessions(50, 0);
    for (const auto &r : rows) {
      if (r.uuid == uuid) return true;
    }
    return false;
  });
  ASSERT_TRUE(persisted) << "session row never appeared in list_sessions()";

  // Detail lookup must succeed.
  auto detail = session_history::get_session_detail(uuid);
  ASSERT_TRUE(detail.has_value());
  EXPECT_EQ(detail->summary.uuid, uuid);
  EXPECT_EQ(detail->summary.app_name, "Solitaire");
  EXPECT_EQ(detail->summary.encoder_bitrate_kbps, 20000);
  EXPECT_EQ(detail->summary.requested_bitrate_kbps, 25000);
  EXPECT_EQ(detail->summary.codec, "H.264");
  EXPECT_EQ(detail->summary.host_cpu_model, "TestCPU");
  EXPECT_EQ(detail->summary.host_gpu_model, "TestGPU");
  EXPECT_EQ(detail->summary.stream_gpu_model, "StreamGPU");

  // Row must still be present in history with an end_time set.
  auto detail_after = session_history::get_session_detail(uuid);
  ASSERT_TRUE(detail_after.has_value());
  // Wait a moment for the end_session writer command to flush.
  wait_for([&] {
    auto d = session_history::get_session_detail(uuid);
    return d && d->summary.end_time_unix > 0;
  });
  detail_after = session_history::get_session_detail(uuid);
  ASSERT_TRUE(detail_after.has_value());
  EXPECT_GT(detail_after->summary.end_time_unix, 0.0);
}

TEST(SessionHistory, EventsAreRecorded) {
  auto path = make_temp_db_path("events");
  session_history::init(path.string());
  HistoryGuard guard;

  const std::string uuid = "22222222-2222-2222-2222-222222222222";
  session_history::begin_session(make_metadata(uuid));
  session_history::record_event(uuid, "drop_burst", R"({"count":42})");
  session_history::end_session(uuid);

  // Wait for events to flush. begin_session itself emits stream_started
  // and end_session emits stream_ended, so we expect at least three
  // events for this session.
  bool got_events = wait_for([&] {
    auto d = session_history::get_session_detail(uuid);
    return d && d->events.size() >= 3;
  });
  ASSERT_TRUE(got_events) << "events never appeared";

  auto detail = session_history::get_session_detail(uuid);
  ASSERT_TRUE(detail.has_value());
  bool found_started = false, found_ended = false, found_burst = false;
  for (const auto &e : detail->events) {
    if (e.event_type == "stream_started") found_started = true;
    if (e.event_type == "stream_ended") found_ended = true;
    if (e.event_type == "drop_burst") {
      found_burst = true;
      EXPECT_NE(e.payload.find("42"), std::string::npos);
    }
  }
  EXPECT_TRUE(found_started);
  EXPECT_TRUE(found_ended);
  EXPECT_TRUE(found_burst);
}

TEST(SessionHistory, DeleteRemovesSessionAndChildren) {
  auto path = make_temp_db_path("delete");
  session_history::init(path.string());
  HistoryGuard guard;

  const std::string uuid = "33333333-3333-3333-3333-333333333333";
  session_history::begin_session(make_metadata(uuid));
  ASSERT_TRUE(session_history::record_sample_for_tests(make_sample(uuid)));
  session_history::record_event(uuid, "test_event");
  session_history::end_session(uuid);

  // Wait for persistence.
  ASSERT_TRUE(wait_for([&] {
    return session_history::get_session_detail(uuid).has_value();
  }));

  EXPECT_EQ(session_history::delete_session(uuid), session_history::delete_result_e::deleted);

  // After delete, detail and list must no longer contain the uuid.
  auto detail = session_history::get_session_detail(uuid);
  EXPECT_FALSE(detail.has_value());

  auto rows = session_history::list_sessions(50, 0);
  for (const auto &r : rows) {
    EXPECT_NE(r.uuid, uuid);
  }

}

TEST(SessionHistory, DeleteFlushesQueuedWritesBeforeReturning) {
  auto path = make_temp_db_path("delete-flush");
  session_history::init(path.string());
  HistoryGuard guard;

  const std::string uuid = "66666666-6666-6666-6666-666666666666";
  session_history::begin_session(make_metadata(uuid));
  session_history::record_event(uuid, "queued_event", R"({"ok":true})");
  session_history::end_session(uuid);

  // delete_session() should wait for the writer thread to process the queued
  // lifecycle mutations and commit the delete before returning.
  EXPECT_EQ(session_history::delete_session(uuid), session_history::delete_result_e::deleted);

  EXPECT_FALSE(session_history::get_session_detail(uuid).has_value());
  auto rows = session_history::list_sessions(50, 0);
  for (const auto &r : rows) {
    EXPECT_NE(r.uuid, uuid);
  }
}

TEST(SessionHistory, ListSessionsRespectsLimit) {
  auto path = make_temp_db_path("limit");
  session_history::init(path.string());
  HistoryGuard guard;

  for (int i = 0; i < 5; ++i) {
    std::string uuid = "44444444-4444-4444-4444-44444444444" + std::to_string(i);
    session_history::begin_session(make_metadata(uuid, "App" + std::to_string(i)));
    session_history::end_session(uuid);
  }

  // Wait for at least 5 rows to appear.
  ASSERT_TRUE(wait_for([&] {
    return session_history::list_sessions(50, 0).size() >= 5;
  }));

  auto limited = session_history::list_sessions(2, 0);
  EXPECT_LE(limited.size(), 2u);
}

TEST(SessionHistory, GetSessionDetailMissingReturnsEmpty) {
  auto path = make_temp_db_path("missing");
  session_history::init(path.string());
  HistoryGuard guard;

  auto detail = session_history::get_session_detail("does-not-exist");
  EXPECT_FALSE(detail.has_value());

  // delete_session() on an unknown uuid must not throw and should
  // report no rows affected.
  EXPECT_EQ(session_history::delete_session("also-not-there"), session_history::delete_result_e::not_found);
}

TEST(SessionHistory, DisabledHistorySkipsPersistence) {
  SunshineConfigGuard config_guard;
  config::sunshine.session_history_enabled = false;

  auto path = make_temp_db_path("disabled");
  session_history::init(path.string());
  HistoryGuard guard;

  const std::string uuid = "77777777-7777-7777-7777-777777777777";
  session_history::begin_session(make_metadata(uuid));
  session_history::record_event(uuid, "ignored_event");
  session_history::end_session(uuid);

  EXPECT_TRUE(session_history::list_sessions(50, 0).empty());
  EXPECT_FALSE(session_history::get_session_detail(uuid).has_value());
  EXPECT_EQ(session_history::delete_session(uuid), session_history::delete_result_e::unavailable);
}

TEST(SessionHistory, DeleteRejectsActiveSessions) {
  auto path = make_temp_db_path("active-delete");
  session_history::init(path.string());
  HistoryGuard guard;

  const std::string uuid = "abababab-abab-abab-abab-abababababab";
  session_history::begin_session(make_metadata(uuid));

  EXPECT_EQ(session_history::delete_session(uuid), session_history::delete_result_e::active_session);

  session_history::end_session(uuid);
}

TEST(SessionHistory, HistoryStatusReportsDroppedSamplesWhenQueueBackpressures) {
  auto path = make_temp_db_path("history-status");
  session_history::init(path.string());
  HistoryGuard guard;

  session_history::configure_queue_limits_for_tests(64, 64, 1, 1);

  const std::string uuid = "34343434-3434-3434-3434-343434343434";
  session_history::begin_session(make_metadata(uuid));

  bool dropped = false;
  for (int i = 0; i < 2000; ++i) {
    auto sample = make_sample(uuid, static_cast<std::uint64_t>(i + 1), 8192);
    if (!session_history::record_sample_for_tests(sample)) {
      dropped = true;
      break;
    }
  }

  ASSERT_TRUE(dropped);
  ASSERT_TRUE(wait_for([&] {
    const auto status = session_history::get_history_status();
    return status.degraded && status.dropped_samples > 0;
  }));

  const auto status = session_history::get_history_status();
  EXPECT_TRUE(status.available);
  EXPECT_TRUE(status.degraded);
  EXPECT_GT(status.dropped_samples, 0u);
}

TEST(SessionHistoryStorage, DeleteCascadesChildRows) {
  auto path = make_temp_db_path("storage-delete-cascade");

  session_history::storage::db_ptr db;
  ASSERT_TRUE(session_history::storage::open_write_db(path.string(), db));
  ASSERT_TRUE(session_history::storage::apply_schema_and_migrations(db.get(), 7));

  const std::string uuid = "cdcdcdcd-cdcd-cdcd-cdcd-cdcdcdcdcdcd";
  ASSERT_TRUE(session_history::storage::process_begin(db.get(), make_metadata(uuid)));
  ASSERT_TRUE(session_history::storage::process_sample(db.get(), make_sample(uuid), 7200));

  session_history::session_event_t event;
  event.session_uuid = uuid;
  event.timestamp_unix = session_history::storage::now_unix();
  event.event_type = "storage-test";
  ASSERT_TRUE(session_history::storage::process_event(db.get(), event, 2000));
  ASSERT_TRUE(session_history::storage::process_end(db.get(), uuid));

  EXPECT_GT(count_rows_for_uuid(db.get(), "samples", uuid), 0u);
  EXPECT_GT(count_rows_for_uuid(db.get(), "events", uuid), 0u);

  EXPECT_EQ(session_history::storage::process_delete(db.get(), uuid), session_history::storage::delete_apply_e::deleted);
  EXPECT_EQ(count_rows_for_uuid(db.get(), "samples", uuid), 0u);
  EXPECT_EQ(count_rows_for_uuid(db.get(), "events", uuid), 0u);
}

TEST(SessionHistory, MigratesLegacyBitrateColumnsToCanonicalNames) {
  auto path = make_temp_db_path("legacy-bitrate-migration");

  sqlite3 *seed_db = nullptr;
  ASSERT_EQ(sqlite3_open(path.string().c_str(), &seed_db), SQLITE_OK);
  ASSERT_TRUE(exec_sql(seed_db,
    "CREATE TABLE sessions ("
    "uuid TEXT PRIMARY KEY,"
    "protocol TEXT NOT NULL,"
    "client_name TEXT,"
    "device_name TEXT,"
    "app_name TEXT,"
    "width INTEGER,"
    "height INTEGER,"
    "target_fps INTEGER,"
    "target_bitrate_kbps INTEGER,"
    "target_requested_bitrate_kbps INTEGER,"
    "codec TEXT,"
    "hdr INTEGER DEFAULT 0,"
    "yuv444 INTEGER DEFAULT 0,"
    "audio_channels INTEGER,"
    "start_time_unix REAL NOT NULL,"
    "end_time_unix REAL,"
    "duration_seconds REAL,"
    "verdict TEXT DEFAULT 'unknown',"
    "server_version TEXT,"
    "host_cpu_model TEXT,"
    "host_gpu_model TEXT,"
    "stream_gpu_model TEXT"
    ");"
    "PRAGMA user_version = 5;"));
  sqlite3_close(seed_db);

  session_history::storage::db_ptr migrated_db;
  ASSERT_TRUE(session_history::storage::open_write_db(path.string(), migrated_db));
  ASSERT_TRUE(session_history::storage::apply_schema_and_migrations(migrated_db.get(), 7));
  EXPECT_TRUE(column_exists(migrated_db.get(), "sessions", "encoder_bitrate_kbps"));
  EXPECT_TRUE(column_exists(migrated_db.get(), "sessions", "requested_bitrate_kbps"));
  EXPECT_FALSE(column_exists(migrated_db.get(), "sessions", "target_bitrate_kbps"));
  EXPECT_FALSE(column_exists(migrated_db.get(), "sessions", "target_requested_bitrate_kbps"));
  migrated_db.reset();

  session_history::init(path.string());
  HistoryGuard guard;

  const std::string uuid = "44444444-4444-4444-4444-444444444444";
  session_history::begin_session(make_metadata(uuid, "Migrated Session"));
  session_history::end_session(uuid);

  ASSERT_TRUE(wait_for([&] {
    auto rows = session_history::list_sessions(50, 0);
    return std::any_of(rows.begin(), rows.end(), [&](const auto &row) {
      return row.uuid == uuid;
    });
  }));

  auto detail = session_history::get_session_detail(uuid, false);
  ASSERT_TRUE(detail.has_value());
  EXPECT_EQ(detail->summary.encoder_bitrate_kbps, 20000);
  EXPECT_EQ(detail->summary.requested_bitrate_kbps, 25000);
}

TEST(SessionHistory, MigratesChildTablesToOnDeleteCascade) {
  auto path = make_temp_db_path("cascade-migration");

  sqlite3 *seed_db = nullptr;
  ASSERT_EQ(sqlite3_open(path.string().c_str(), &seed_db), SQLITE_OK);
  ASSERT_TRUE(exec_sql(seed_db,
    "CREATE TABLE sessions ("
    "uuid TEXT PRIMARY KEY,"
    "protocol TEXT NOT NULL,"
    "client_name TEXT,"
    "device_name TEXT,"
    "app_name TEXT,"
    "width INTEGER,"
    "height INTEGER,"
    "target_fps INTEGER,"
    "encoder_bitrate_kbps INTEGER,"
    "requested_bitrate_kbps INTEGER,"
    "codec TEXT,"
    "hdr INTEGER DEFAULT 0,"
    "yuv444 INTEGER DEFAULT 0,"
    "audio_channels INTEGER,"
    "start_time_unix REAL NOT NULL,"
    "end_time_unix REAL,"
    "duration_seconds REAL,"
    "verdict TEXT DEFAULT 'unknown',"
    "server_version TEXT,"
    "host_cpu_model TEXT,"
    "host_gpu_model TEXT,"
    "stream_gpu_model TEXT"
    ");"
    "CREATE TABLE samples ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "session_uuid TEXT NOT NULL REFERENCES sessions(uuid),"
    "timestamp_unix REAL NOT NULL,"
    "bytes_sent_total INTEGER DEFAULT 0,"
    "packets_sent_video INTEGER DEFAULT 0,"
    "frames_sent INTEGER DEFAULT 0,"
    "last_frame_index INTEGER DEFAULT 0,"
    "video_dropped INTEGER DEFAULT 0,"
    "audio_dropped INTEGER DEFAULT 0,"
    "client_reported_losses INTEGER DEFAULT 0,"
    "idr_requests INTEGER DEFAULT 0,"
    "ref_invalidations INTEGER DEFAULT 0,"
    "encode_latency_ms REAL DEFAULT 0,"
    "actual_fps REAL DEFAULT 0,"
    "actual_bitrate_kbps REAL DEFAULT 0,"
    "frame_interval_jitter_ms REAL DEFAULT 0,"
    "host_cpu_percent REAL DEFAULT -1,"
    "host_gpu_percent REAL DEFAULT -1,"
    "host_gpu_encoder_percent REAL DEFAULT -1,"
    "host_ram_percent REAL DEFAULT -1,"
    "host_vram_percent REAL DEFAULT -1,"
    "host_cpu_temp_c REAL DEFAULT -1,"
    "host_gpu_temp_c REAL DEFAULT -1,"
    "host_net_rx_bps REAL DEFAULT -1,"
    "host_net_tx_bps REAL DEFAULT -1"
    ");"
    "CREATE TABLE events ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "session_uuid TEXT NOT NULL REFERENCES sessions(uuid),"
    "timestamp_unix REAL NOT NULL,"
    "event_type TEXT NOT NULL,"
    "payload TEXT"
    ");"
    "PRAGMA user_version = 6;"));
  sqlite3_close(seed_db);

  session_history::storage::db_ptr migrated_db;
  ASSERT_TRUE(session_history::storage::open_write_db(path.string(), migrated_db));
  ASSERT_TRUE(session_history::storage::apply_schema_and_migrations(migrated_db.get(), 7));
  EXPECT_TRUE(foreign_key_has_delete_cascade(migrated_db.get(), "samples", "sessions", "session_uuid"));
  EXPECT_TRUE(foreign_key_has_delete_cascade(migrated_db.get(), "events", "sessions", "session_uuid"));
}

TEST(SessionHistory, RetentionDaysPrunesOldEndedSessionsOnStartup) {
  SunshineConfigGuard config_guard;
  config::sunshine.session_history_enabled = true;
  config::sunshine.session_history_ttl_days = 0;
  config::sunshine.session_history_db_size_limit_mb = 0;

  auto path = make_temp_db_path("ttl");
  session_history::init(path.string());
  HistoryGuard guard;

  const std::string old_uuid = "88888888-8888-8888-8888-888888888888";
  const std::string new_uuid = "99999999-9999-9999-9999-999999999999";
  session_history::begin_session(make_metadata(old_uuid, "Old App"));
  session_history::end_session(old_uuid);
  session_history::begin_session(make_metadata(new_uuid, "New App"));
  session_history::end_session(new_uuid);

  ASSERT_TRUE(wait_for([&] {
    return session_history::list_sessions(50, 0).size() >= 2;
  }));

  session_history::configure_retention_for_tests(true, 1, 0);
  const auto stale_end_time =
    std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count() - (3.0 * 24.0 * 60.0 * 60.0);
  ASSERT_TRUE(session_history::set_session_end_time_for_tests(old_uuid, stale_end_time));
  ASSERT_TRUE(session_history::prune_now_for_tests());

  ASSERT_TRUE(wait_for([&] {
    auto rows = session_history::list_sessions(50, 0);
    return rows.size() == 1;
  }));

  const auto rows = session_history::list_sessions(50, 0);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows.front().uuid, new_uuid);
}

TEST(SessionHistoryStorage, PruneCascadesChildRows) {
  auto path = make_temp_db_path("storage-prune-cascade");

  session_history::storage::db_ptr db;
  ASSERT_TRUE(session_history::storage::open_write_db(path.string(), db));
  ASSERT_TRUE(session_history::storage::apply_schema_and_migrations(db.get(), 7));

  const std::string old_uuid = "efefefef-efef-efef-efef-efefefefefef";
  const std::string new_uuid = "12121212-1212-1212-1212-121212121212";

  ASSERT_TRUE(session_history::storage::process_begin(db.get(), make_metadata(old_uuid, "Old App")));
  ASSERT_TRUE(session_history::storage::process_sample(db.get(), make_sample(old_uuid), 7200));
  ASSERT_TRUE(session_history::storage::process_event(
    db.get(),
    session_history::session_event_t {old_uuid, session_history::storage::now_unix(), "old-event", {}},
    2000));
  ASSERT_TRUE(session_history::storage::process_end(db.get(), old_uuid));

  ASSERT_TRUE(session_history::storage::process_begin(db.get(), make_metadata(new_uuid, "New App")));
  ASSERT_TRUE(session_history::storage::process_sample(db.get(), make_sample(new_uuid), 7200));
  ASSERT_TRUE(session_history::storage::process_event(
    db.get(),
    session_history::session_event_t {new_uuid, session_history::storage::now_unix(), "new-event", {}},
    2000));
  ASSERT_TRUE(session_history::storage::process_end(db.get(), new_uuid));

  const auto stale_end_time =
    std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count() - (3.0 * 24.0 * 60.0 * 60.0);
  ASSERT_TRUE(session_history::storage::force_set_end_time(db.get(), old_uuid, stale_end_time));

  session_history::storage::prune_options_t options;
  options.prune_sessions_ended_before_unix =
    std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count() - (24.0 * 60.0 * 60.0);
  ASSERT_TRUE(session_history::storage::process_prune(db.get(), options));

  EXPECT_EQ(count_rows_for_uuid(db.get(), "samples", old_uuid), 0u);
  EXPECT_EQ(count_rows_for_uuid(db.get(), "events", old_uuid), 0u);
  EXPECT_GT(count_rows_for_uuid(db.get(), "samples", new_uuid), 0u);
  EXPECT_GT(count_rows_for_uuid(db.get(), "events", new_uuid), 0u);
}

TEST(SessionHistory, DbQuotaPrunesOldestEndedSessionsOnStartup) {
  SunshineConfigGuard config_guard;
  config::sunshine.session_history_enabled = true;
  config::sunshine.session_history_ttl_days = 0;
  config::sunshine.session_history_db_size_limit_mb = 0;

  auto path = make_temp_db_path("quota");
  session_history::init(path.string());
  HistoryGuard guard;

  for (int i = 0; i < 8; ++i) {
    const std::string uuid = "aaaaaaa0-0000-0000-0000-00000000000" + std::to_string(i);
    session_history::begin_session(make_metadata(uuid, "QuotaApp" + std::to_string(i)));
    for (int e = 0; e < 12; ++e) {
      session_history::record_event(uuid, "quota_payload", std::string(32 * 1024, static_cast<char>('a' + i)));
    }
    session_history::end_session(uuid);
  }

  ASSERT_TRUE(wait_for([&] {
    return session_history::list_sessions(50, 0).size() >= 8;
  }, std::chrono::seconds(6)));

  session_history::configure_retention_for_tests(true, 0, 1024ull * 1024ull);
  ASSERT_TRUE(session_history::prune_now_for_tests());

  ASSERT_TRUE(wait_for([&] {
    return session_history::list_sessions(50, 0).size() < 8;
  }, std::chrono::seconds(6)));

  EXPECT_LT(session_history::list_sessions(50, 0).size(), 8u);
}

TEST(SessionHistory, DropsLateEventQueuedAfterEndSession) {
  auto path = make_temp_db_path("late-event");
  session_history::init(path.string());
  HistoryGuard guard;

  const std::string uuid = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";
  session_history::begin_session(make_metadata(uuid));
  session_history::end_session(uuid);
  session_history::record_event(uuid, "late_event", R"({"should":"drop"})");

  ASSERT_TRUE(wait_for([&] {
    auto detail = session_history::get_session_detail(uuid);
    return detail && detail->summary.end_time_unix > 0 && detail->events.size() == 2;
  }));

  const auto detail = session_history::get_session_detail(uuid);
  ASSERT_TRUE(detail.has_value());
  EXPECT_EQ(detail->events.size(), 2u);
  for (const auto &event : detail->events) {
    EXPECT_NE(event.event_type, "late_event");
  }
}

TEST(SessionHistory, DropsLateSampleQueuedAfterEndSession) {
  auto path = make_temp_db_path("late-sample");
  session_history::init(path.string());
  HistoryGuard guard;

  const std::string uuid = "cccccccc-cccc-cccc-cccc-cccccccccccc";
  session_history::begin_session(make_metadata(uuid));
  session_history::end_session(uuid);
  ASSERT_TRUE(session_history::record_sample_for_tests(make_sample(uuid)));

  ASSERT_TRUE(wait_for([&] {
    auto detail = session_history::get_session_detail(uuid);
    return detail && detail->summary.end_time_unix > 0;
  }));

  const auto detail = session_history::get_session_detail(uuid);
  ASSERT_TRUE(detail.has_value());
  EXPECT_EQ(detail->total_samples, 0u);
  EXPECT_TRUE(detail->samples.empty());
}

TEST(SessionHistory, DetailSamplesAndEventsPreserveSessionUuid) {
  auto path = make_temp_db_path("detail-uuid");
  session_history::init(path.string());
  HistoryGuard guard;

  const std::string uuid = "dddddddd-dddd-dddd-dddd-dddddddddddd";
  session_history::begin_session(make_metadata(uuid));
  ASSERT_TRUE(session_history::record_sample_for_tests(make_sample(uuid, 42, 16384)));
  session_history::record_event(uuid, "marker", R"({"kind":"detail"})");
  session_history::end_session(uuid);

  ASSERT_TRUE(wait_for([&] {
    auto detail = session_history::get_session_detail(uuid);
    return detail && !detail->samples.empty() && detail->events.size() >= 3;
  }));

  const auto detail = session_history::get_session_detail(uuid);
  ASSERT_TRUE(detail.has_value());
  EXPECT_EQ(detail->samples.front().session_uuid, uuid);

  bool found_marker = false;
  for (const auto &event : detail->events) {
    EXPECT_EQ(event.session_uuid, uuid);
    if (event.event_type == "marker") {
      found_marker = true;
    }
  }
  EXPECT_TRUE(found_marker);
}

TEST(SessionHistory, DetailDefaultEventLimitSetsTruncationAndIncludeAllRestoresRows) {
  auto path = make_temp_db_path("detail-limit");
  session_history::init(path.string());
  HistoryGuard guard;

  const std::string uuid = "eeeeeeee-eeee-eeee-eeee-eeeeeeeeeeee";
  session_history::begin_session(make_metadata(uuid));
  for (int i = 0; i < 550; ++i) {
    session_history::record_event(uuid, "detail_limit", std::to_string(i));
  }
  session_history::end_session(uuid);

  ASSERT_TRUE(wait_for([&] {
    auto detail = session_history::get_session_detail(uuid);
    return detail && detail->summary.end_time_unix > 0 && detail->total_events >= 552;
  }, std::chrono::seconds(6)));

  const auto limited_detail = session_history::get_session_detail(uuid);
  ASSERT_TRUE(limited_detail.has_value());
  EXPECT_TRUE(limited_detail->events_truncated);
  EXPECT_EQ(limited_detail->total_events, 552u);
  EXPECT_EQ(limited_detail->events.size(), 500u);

  const auto full_detail = session_history::get_session_detail(uuid, true);
  ASSERT_TRUE(full_detail.has_value());
  EXPECT_FALSE(full_detail->events_truncated);
  EXPECT_EQ(full_detail->total_events, 552u);
  EXPECT_EQ(full_detail->events.size(), 552u);
}
