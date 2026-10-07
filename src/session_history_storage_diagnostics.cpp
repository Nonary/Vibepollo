/**
 * @file src/session_history_storage_diagnostics.cpp
 * @brief Standard-library diagnostics dispatch for session history storage.
 */

#include "session_history_storage_diagnostics.h"

// standard includes
#include <atomic>

namespace session_history::storage::diagnostics {
  namespace {
    std::atomic<sink_t> g_sink {nullptr};
  }

  void set_sink(sink_t sink) {
    g_sink.store(sink, std::memory_order_release);
  }

  void log(level_e level, const std::string &message) {
    if (const auto sink = g_sink.load(std::memory_order_acquire)) {
      sink(level, message);
    }
  }

}  // namespace session_history::storage::diagnostics
