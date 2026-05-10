/**
 * @file src/session_history_storage.h
 * @brief Internal SQLite storage helpers for the session history subsystem.
 */
#pragma once

// standard includes
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// lib includes
#include <sqlite3.h>

// local includes
#include "session_history.h"

namespace session_history::storage {

  struct sqlite_deleter {
    void operator()(sqlite3 *db) const;
  };

  using db_ptr = std::unique_ptr<sqlite3, sqlite_deleter>;

  struct stmt_deleter {
    void operator()(sqlite3_stmt *stmt) const;
  };

  using stmt_ptr = std::unique_ptr<sqlite3_stmt, stmt_deleter>;

  enum class delete_apply_e {
    deleted,
    not_found,
    failed
  };

  stmt_ptr prepare(sqlite3 *db, const char *sql);
  bool exec(sqlite3 *db, const char *sql);
  double now_unix();
  void tighten_history_db_permissions(const std::filesystem::path &db_path);

  bool open_write_db(const std::string &db_path, db_ptr &out_db);
  bool open_read_db(const std::string &db_path, db_ptr &out_db);
  bool apply_schema_and_migrations(sqlite3 *db, int schema_version);

  bool process_begin(sqlite3 *db, const session_metadata_t &metadata);
  bool process_end(sqlite3 *db, const std::string &uuid);
  bool process_sample(sqlite3 *db, const session_sample_t &sample, int max_samples_per_session);
  bool process_event(sqlite3 *db, const session_event_t &event, int max_events_per_session);
  delete_apply_e process_delete(sqlite3 *db, const std::string &uuid);
  bool process_prune(sqlite3 *db, int max_history_sessions);

  std::vector<session_summary_t> read_session_summaries(sqlite3 *db, int limit, int offset);
  std::optional<session_detail_t> read_session_detail(
    sqlite3 *db,
    const std::string &uuid,
    bool include_all,
    int default_detail_sample_limit,
    int default_detail_event_limit);

}  // namespace session_history::storage
