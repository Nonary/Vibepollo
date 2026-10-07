/**
 * @file src/session_history_policy.cpp
 * @brief Pure scheduling and retention policy for session history writes.
 */

#include "session_history_policy.h"

namespace session_history::policy {
  enqueue_result_e accept(queue_kind_e kind, std::size_t current_size, const queue_limits_t &limits) {
    std::size_t limit = 0;
    switch (kind) {
      case queue_kind_e::control:
        limit = limits.control;
        break;
      case queue_kind_e::priority:
        limit = limits.priority;
        break;
      case queue_kind_e::regular:
        limit = limits.regular;
        break;
      case queue_kind_e::sample:
        limit = limits.sample;
        break;
    }
    return current_size < limit ? enqueue_result_e::accepted : enqueue_result_e::queue_full;
  }

  bool flushes_before_barrier(
    queue_kind_e candidate_kind,
    std::string_view candidate_uuid,
    std::uint64_t candidate_sequence,
    std::string_view barrier_uuid,
    std::uint64_t barrier_sequence) {
    return
      (candidate_kind == queue_kind_e::sample || candidate_kind == queue_kind_e::priority) &&
      !barrier_uuid.empty() &&
      candidate_uuid == barrier_uuid &&
      candidate_sequence < barrier_sequence;
  }

  double retention_cutoff_unix(int ttl_days, double now_unix) {
    return ttl_days > 0 ? now_unix - (static_cast<double>(ttl_days) * 24.0 * 60.0 * 60.0) : 0.0;
  }

  history_status_t make_status(const status_inputs_t &inputs) {
    return {
      .available = inputs.running && inputs.has_write_db,
      .degraded = inputs.degraded,
      .dropped_samples = inputs.dropped_samples,
      .failed_writes = inputs.failed_writes,
      .pending_control_commands = inputs.pending_control_commands,
      .pending_priority_commands = inputs.pending_priority_commands,
      .pending_regular_commands = inputs.pending_regular_commands,
      .pending_samples = inputs.pending_samples,
    };
  }
}  // namespace session_history::policy
