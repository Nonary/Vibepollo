/**
 * @file src/session_history_writer.h
 * @brief Internal writer/readback support for the session history facade.
 */
#pragma once

#include "session_history.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace session_history::writer {

  struct settings_t {
    bool enabled = true;
    int ttl_days = 0;
    std::uint64_t max_db_size_bytes = 0;
  };

  void update_settings(const settings_t &settings);
  bool is_enabled();
  bool is_available();

  void init(const std::string &db_path);
  void shutdown();

  bool enqueue_begin(const session_metadata_t &metadata);
  bool enqueue_end(const std::string &uuid);
  bool enqueue_event(const session_event_t &event);
  bool enqueue_sample(session_sample_t sample);
  bool enqueue_prune();

  std::vector<session_summary_t> list_sessions(int limit = 25, int offset = 0);
  std::optional<session_detail_t> get_session_detail(const std::string &uuid, bool include_all = false);
  delete_result_e delete_session(const std::string &uuid);
  history_status_t get_status();

#ifdef SUNSHINE_TESTS
  bool set_session_end_time_for_tests(const std::string &uuid, double end_time_unix);
  bool prune_now_for_tests();
  void configure_queue_limits_for_tests(
    std::size_t priority_limit,
    std::size_t regular_limit,
    std::size_t sample_limit,
    std::size_t sample_batch_size);
  void reset_queue_limits_for_tests();
#endif

}  // namespace session_history::writer
