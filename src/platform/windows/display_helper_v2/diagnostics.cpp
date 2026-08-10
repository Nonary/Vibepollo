#include "src/platform/windows/display_helper_v2/diagnostics.h"

#include <mutex>
#include <utility>

namespace display_helper::v2::diagnostics {
  namespace {
    std::mutex sink_mutex;
    Sink sink;
  }

  void set_sink(Sink next_sink) {
    std::lock_guard lock {sink_mutex};
    sink = std::move(next_sink);
  }

  void emit(Level level, std::string message) {
    Sink active_sink;
    {
      std::lock_guard lock {sink_mutex};
      active_sink = sink;
    }
    if (active_sink) {
      active_sink(level, message);
    }
  }
}  // namespace display_helper::v2::diagnostics
