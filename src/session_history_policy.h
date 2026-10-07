/**
 * @file src/session_history_policy.h
 * @brief Pure scheduling and retention policy for session history writes.
 */
#pragma once

#include "session_history.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace session_history::policy {
  enum class queue_kind_e { control, priority, regular, sample };
  enum class enqueue_result_e { accepted, queue_full };

  struct queue_limits_t {
    std::size_t control = 0;
    std::size_t priority = 0;
    std::size_t regular = 0;
    std::size_t sample = 0;
  };

  /**
   * @brief Snapshot supplied by the writer when reporting its externally
   * visible health. Kept value-only so status semantics can be verified
   * without starting the writer thread or opening a filesystem database.
   */
  struct status_inputs_t {
    bool running = false;
    bool has_write_db = false;
    bool degraded = false;
    std::uint64_t dropped_samples = 0;
    std::uint64_t failed_writes = 0;
    std::size_t pending_control_commands = 0;
    std::size_t pending_priority_commands = 0;
    std::size_t pending_regular_commands = 0;
    std::size_t pending_samples = 0;
  };

  enqueue_result_e accept(queue_kind_e kind, std::size_t current_size, const queue_limits_t &limits);
  bool flushes_before_barrier(
    queue_kind_e candidate_kind,
    std::string_view candidate_uuid,
    std::uint64_t candidate_sequence,
    std::string_view barrier_uuid,
    std::uint64_t barrier_sequence);
  double retention_cutoff_unix(int ttl_days, double now_unix);
  history_status_t make_status(const status_inputs_t &inputs);
}  // namespace session_history::policy
