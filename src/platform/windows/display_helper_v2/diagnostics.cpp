#include "src/platform/windows/display_helper_v2/diagnostics.h"

#include <mutex>
#include <utility>

namespace display_helper::v2::diagnostics {
  namespace {
    struct SinkState {
      std::mutex mutex;
      Sink sink;
    };

    SinkState &sink_state() {
      // The Boost adapter registers from another translation unit's static
      // initializer. Construct the storage on first use so a later global
      // initializer cannot overwrite the already registered sink.
      static SinkState state;
      return state;
    }
  }

  void set_sink(Sink next_sink) {
    auto &state = sink_state();
    std::lock_guard lock {state.mutex};
    state.sink = std::move(next_sink);
  }

  void emit(Level level, std::string message) {
    Sink active_sink;
    {
      auto &state = sink_state();
      std::lock_guard lock {state.mutex};
      active_sink = state.sink;
    }
    if (active_sink) {
      active_sink(level, message);
    }
  }
}  // namespace display_helper::v2::diagnostics
