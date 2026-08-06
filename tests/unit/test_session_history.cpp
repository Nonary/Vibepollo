/**
 * @file tests/unit/test_session_history.cpp
 * @brief Deterministic in-memory session history storage and scheduling tests.
 */
#include "../tests_common.h"

#include <src/session_history_policy.h>
#include <src/session_history_storage.h>

#include <sqlite3.h>
#include <string>

namespace {
  constexpr int schema_version = 7;

  bool exec_sql(sqlite3 *db, const char *sql) {
    return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
  }

  std::size_t count_rows(sqlite3 *db, const char *table, const std::string &uuid = {}) {
    sqlite3_stmt *stmt = nullptr;
    const std::string sql = uuid.empty() ?
      std::string("SELECT COUNT(*) FROM ") + table :
      std::string("SELECT COUNT(*) FROM ") + table + " WHERE session_uuid = ?";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
      return 0;
    }
    if (!uuid.empty()) {
      sqlite3_bind_text(stmt, 1, uuid.c_str(), -1, SQLITE_TRANSIENT);
    }
    const auto count = sqlite3_step(stmt) == SQLITE_ROW ? static_cast<std::size_t>(sqlite3_column_int64(stmt, 0)) : 0;
    sqlite3_finalize(stmt);
    return count;
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
      found = name && reinterpret_cast<const char *>(name) == std::string(column);
      if (found) break;
    }
    sqlite3_finalize(stmt);
    return found;
  }

  session_history::storage::db_ptr open_history() {
    session_history::storage::db_ptr db;
    if (!session_history::storage::open_memory_db(db) ||
        !session_history::storage::apply_schema_and_migrations(db.get(), schema_version)) {
      return {};
    }
    return db;
  }

  session_history::session_metadata_t metadata(const std::string &uuid, const std::string &app = "Test App") {
    session_history::session_metadata_t value;
    value.uuid = uuid;
    value.protocol = "rtsp";
    value.client_name = "Client";
    value.device_name = "Device";
    value.app_name = app;
    value.width = 1920;
    value.height = 1080;
    value.target_fps = 60;
    value.encoder_bitrate_kbps = 20000;
    value.requested_bitrate_kbps = 25000;
    value.codec = "h264";
    value.audio_channels = 2;
    return value;
  }

  session_history::session_sample_t sample(const std::string &uuid, double timestamp, std::uint64_t frames = 10) {
    session_history::session_sample_t value;
    value.session_uuid = uuid;
    value.timestamp_unix = timestamp;
    value.frames_sent = frames;
    value.bytes_sent_total = frames * 1024;
    value.packets_sent_video = frames;
    value.last_frame_index = static_cast<std::int64_t>(frames);
    value.actual_fps = 60;
    return value;
  }

  session_history::session_event_t event(const std::string &uuid, double timestamp, std::string payload = {}) {
    return {uuid, timestamp, "marker", std::move(payload)};
  }
}  // namespace

TEST(SessionHistoryStorage, FreshSchemaAndDeterministicLifecycleRoundTrip) {
  auto db = open_history();
  ASSERT_TRUE(db);
  const std::string uuid = "11111111-1111-1111-1111-111111111111";
  ASSERT_TRUE(session_history::storage::process_begin_at(db.get(), metadata(uuid, "Solitaire"), 1000.0));
  ASSERT_TRUE(session_history::storage::process_sample(db.get(), sample(uuid, 1001.0), 10));
  ASSERT_TRUE(session_history::storage::process_event(db.get(), event(uuid, 1002.0, "payload"), 10));
  ASSERT_TRUE(session_history::storage::process_end_at(db.get(), uuid, 1010.0));

  const auto detail = session_history::storage::read_session_detail(db.get(), uuid, true, 2, 2);
  ASSERT_TRUE(detail.has_value());
  EXPECT_EQ(detail->summary.app_name, "Solitaire");
  EXPECT_EQ(detail->summary.encoder_bitrate_kbps, 20000);
  EXPECT_EQ(detail->summary.requested_bitrate_kbps, 25000);
  EXPECT_DOUBLE_EQ(detail->summary.duration_seconds, 10.0);
  ASSERT_EQ(detail->samples.size(), 1u);
  EXPECT_EQ(detail->samples[0].session_uuid, uuid);
  ASSERT_EQ(detail->events.size(), 1u);
  EXPECT_EQ(detail->events[0].session_uuid, uuid);
}

TEST(SessionHistoryStorage, LateSamplesAndEventsAfterEndAreDropped) {
  auto db = open_history();
  ASSERT_TRUE(db);
  const std::string uuid = "22222222-2222-2222-2222-222222222222";
  ASSERT_TRUE(session_history::storage::process_begin_at(db.get(), metadata(uuid), 100.0));
  ASSERT_TRUE(session_history::storage::process_end_at(db.get(), uuid, 110.0));
  EXPECT_TRUE(session_history::storage::process_sample(db.get(), sample(uuid, 111.0), 10));
  EXPECT_TRUE(session_history::storage::process_event(db.get(), event(uuid, 112.0), 10));
  EXPECT_EQ(count_rows(db.get(), "samples", uuid), 0u);
  EXPECT_EQ(count_rows(db.get(), "events", uuid), 0u);
}

TEST(SessionHistoryStorage, DeleteCascadesAndMissingDeleteIsDistinguished) {
  auto db = open_history();
  ASSERT_TRUE(db);
  const std::string uuid = "33333333-3333-3333-3333-333333333333";
  ASSERT_TRUE(session_history::storage::process_begin_at(db.get(), metadata(uuid), 1.0));
  ASSERT_TRUE(session_history::storage::process_sample(db.get(), sample(uuid, 2.0), 10));
  ASSERT_TRUE(session_history::storage::process_event(db.get(), event(uuid, 3.0), 10));
  ASSERT_TRUE(session_history::storage::process_end_at(db.get(), uuid, 4.0));
  EXPECT_EQ(session_history::storage::process_delete(db.get(), uuid), session_history::storage::delete_apply_e::deleted);
  EXPECT_EQ(count_rows(db.get(), "samples", uuid), 0u);
  EXPECT_EQ(count_rows(db.get(), "events", uuid), 0u);
  EXPECT_EQ(session_history::storage::process_delete(db.get(), uuid), session_history::storage::delete_apply_e::not_found);
}

TEST(SessionHistoryStorage, LegacyBitrateColumnsMigrateInMemory) {
  session_history::storage::db_ptr db;
  ASSERT_TRUE(session_history::storage::open_memory_db(db));
  ASSERT_TRUE(exec_sql(db.get(),
    "CREATE TABLE sessions (uuid TEXT PRIMARY KEY, protocol TEXT NOT NULL, client_name TEXT, device_name TEXT, app_name TEXT,"
    "width INTEGER, height INTEGER, target_fps INTEGER, target_bitrate_kbps INTEGER, target_requested_bitrate_kbps INTEGER,"
    "codec TEXT, hdr INTEGER DEFAULT 0, yuv444 INTEGER DEFAULT 0, audio_channels INTEGER, start_time_unix REAL NOT NULL,"
    "end_time_unix REAL, duration_seconds REAL, verdict TEXT DEFAULT 'unknown', server_version TEXT, host_cpu_model TEXT,"
    "host_gpu_model TEXT, stream_gpu_model TEXT); PRAGMA user_version = 5;"));
  ASSERT_TRUE(session_history::storage::apply_schema_and_migrations(db.get(), schema_version));
  EXPECT_TRUE(column_exists(db.get(), "sessions", "encoder_bitrate_kbps"));
  EXPECT_TRUE(column_exists(db.get(), "sessions", "requested_bitrate_kbps"));
  EXPECT_FALSE(column_exists(db.get(), "sessions", "target_bitrate_kbps"));
  EXPECT_FALSE(column_exists(db.get(), "sessions", "target_requested_bitrate_kbps"));
}

TEST(SessionHistoryStorage, PerSessionCapsKeepNewestRows) {
  auto db = open_history();
  ASSERT_TRUE(db);
  const std::string uuid = "44444444-4444-4444-4444-444444444444";
  ASSERT_TRUE(session_history::storage::process_begin_at(db.get(), metadata(uuid), 1.0));
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(session_history::storage::process_sample(db.get(), sample(uuid, 10.0 + i, i + 1), 2));
    ASSERT_TRUE(session_history::storage::process_event(db.get(), event(uuid, 20.0 + i, std::to_string(i)), 3));
  }
  EXPECT_EQ(count_rows(db.get(), "samples", uuid), 2u);
  EXPECT_EQ(count_rows(db.get(), "events", uuid), 3u);
}

TEST(SessionHistoryStorage, DetailLimitsReportTruncationAndIncludeAllRestoresRows) {
  auto db = open_history();
  ASSERT_TRUE(db);
  const std::string uuid = "55555555-5555-5555-5555-555555555555";
  ASSERT_TRUE(session_history::storage::process_begin_at(db.get(), metadata(uuid), 1.0));
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(session_history::storage::process_sample(db.get(), sample(uuid, 10.0 + i), 10));
    ASSERT_TRUE(session_history::storage::process_event(db.get(), event(uuid, 20.0 + i), 10));
  }
  ASSERT_TRUE(session_history::storage::process_end_at(db.get(), uuid, 30.0));
  const auto limited = session_history::storage::read_session_detail(db.get(), uuid, false, 2, 2);
  ASSERT_TRUE(limited.has_value());
  EXPECT_TRUE(limited->samples_truncated);
  EXPECT_TRUE(limited->events_truncated);
  EXPECT_EQ(limited->samples.size(), 2u);
  EXPECT_EQ(limited->events.size(), 2u);
  const auto all = session_history::storage::read_session_detail(db.get(), uuid, true, 2, 2);
  ASSERT_TRUE(all.has_value());
  EXPECT_EQ(all->samples.size(), 4u);
  EXPECT_EQ(all->events.size(), 4u);
}

TEST(SessionHistoryStorage, RetentionAndQuotaPruneOldestEndedSessions) {
  auto db = open_history();
  ASSERT_TRUE(db);
  for (int i = 0; i < 3; ++i) {
    const auto uuid = "quota-" + std::to_string(i);
    ASSERT_TRUE(session_history::storage::process_begin_at(db.get(), metadata(uuid), 100.0 + i));
    ASSERT_TRUE(session_history::storage::process_event(db.get(), event(uuid, 101.0 + i, std::string(4096, 'x')), 10));
    ASSERT_TRUE(session_history::storage::process_end_at(db.get(), uuid, 110.0 + i));
  }
  session_history::storage::prune_options_t retention;
  retention.prune_sessions_ended_before_unix = 111.0;
  ASSERT_TRUE(session_history::storage::process_prune(db.get(), retention));
  EXPECT_EQ(session_history::storage::read_session_summaries(db.get(), 10, 0).size(), 2u);

  session_history::storage::prune_options_t quota;
  quota.max_db_size_bytes = 1;
  ASSERT_TRUE(session_history::storage::process_prune(db.get(), quota));
  EXPECT_TRUE(session_history::storage::read_session_summaries(db.get(), 10, 0).empty());
}

TEST(SessionHistoryPolicy, QueuePressureAndLifecycleBarriersAreDeterministic) {
  using namespace session_history::policy;
  const queue_limits_t limits {.control = 1, .priority = 1, .regular = 1, .sample = 1};
  EXPECT_EQ(accept(queue_kind_e::sample, 0, limits), enqueue_result_e::accepted);
  EXPECT_EQ(accept(queue_kind_e::sample, 1, limits), enqueue_result_e::queue_full);
  EXPECT_EQ(accept(queue_kind_e::control, 0, limits), enqueue_result_e::accepted);
  EXPECT_TRUE(flushes_before_barrier(queue_kind_e::sample, "session", 4, "session", 5));
  EXPECT_TRUE(flushes_before_barrier(queue_kind_e::priority, "session", 3, "session", 5));
  EXPECT_FALSE(flushes_before_barrier(queue_kind_e::sample, "other", 4, "session", 5));
  EXPECT_FALSE(flushes_before_barrier(queue_kind_e::sample, "session", 6, "session", 5));
}

TEST(SessionHistoryPolicy, RetentionUsesInjectedClock) {
  EXPECT_DOUBLE_EQ(session_history::policy::retention_cutoff_unix(1, 200000.0), 113600.0);
  EXPECT_DOUBLE_EQ(session_history::policy::retention_cutoff_unix(0, 200000.0), 0.0);
}
