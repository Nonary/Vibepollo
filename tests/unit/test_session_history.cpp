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

#include <chrono>
#include <filesystem>
#include <thread>

namespace {

  std::filesystem::path
    make_temp_db_path(const char *label) {
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path = std::filesystem::path("vibepollo-tests-" + std::string(label) + "-" + std::to_string(stamp) + ".sqlite");
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
    return m;
  }

  /**
   * @brief RAII guard that calls session_history::shutdown() on destruction.
   *
   * Ensures background threads are stopped even when a test assertion
   * throws.
   */
  struct HistoryGuard {
    ~HistoryGuard() {
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
  EXPECT_EQ(detail->summary.codec, "h264");
  EXPECT_EQ(detail->summary.host_cpu_model, "TestCPU");
  EXPECT_EQ(detail->summary.host_gpu_model, "TestGPU");

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
