/**
 * @file src/session_history_policy.h
 * @brief Pure scheduling and retention policy for session history writes.
 */
#pragma once

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

  enqueue_result_e accept(queue_kind_e kind, std::size_t current_size, const queue_limits_t &limits);
  bool flushes_before_barrier(
    queue_kind_e candidate_kind,
    std::string_view candidate_uuid,
    std::uint64_t candidate_sequence,
    std::string_view barrier_uuid,
    std::uint64_t barrier_sequence);
  double retention_cutoff_unix(int ttl_days, double now_unix);
}  // namespace session_history::policy
