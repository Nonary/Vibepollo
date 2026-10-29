#pragma once

#include <functional>

namespace display_helper {
  using BaselinePublisher = std::function<bool(const std::function<void()> &)>;
  using BaselineReset = std::function<void(const std::function<void()> &)>;
  enum class SnapshotResult { failed, dispatched, saved };
}
