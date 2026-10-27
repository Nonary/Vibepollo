#pragma once

#include "display_snapshot_result.h"

#include <cstdint>
#include <functional>
#include <mutex>

namespace display_helper_integration {
  // Coordinator failures are independent of the helper's REVERT state. Keep
  // their baseline protected without holding a display lock across helper IPC.
  class DisplayRestoreGuard {
  public:
    bool protects_baseline() {
      std::lock_guard lock(mutex_);
      return failed_ || applying_;
    }

    display_helper::BaselinePublisher baseline_publisher(bool protected_layout = false) {
      std::lock_guard lock(mutex_);
      if (failed_ != protected_layout || applying_) return {};
      return [this, generation = generation_](const std::function<void()> &publish) {
        std::lock_guard lock(mutex_);
        if (generation != generation_) return false;
        publish();
        return true;
      };
    }

    template<class Apply>
    bool compose(Apply apply) {
      std::lock_guard serialize(compose_mutex_);
      {
        std::lock_guard lock(mutex_);
        ++generation_;
        failed_ = applying_ = true;
      }
      const auto finish = [&](bool ok) {
        std::lock_guard lock(mutex_);
        failed_ = !ok;
        applying_ = false;
        return ok;
      };
      try {
        return finish(apply());
      } catch (...) {
        finish(false);
        throw;
      }
    }

    template<class Send, class ReportSkip>
    bool export_snapshot(Send send, ReportSkip report_skip) {
      return snapshot([&](const auto &superseded) {
        if (superseded() || !send(superseded)) return display_helper::SnapshotResult::failed;
        return display_helper::SnapshotResult::dispatched;
      }, report_skip);
    }

    template<class Capture, class ReportSkip>
    bool snapshot(Capture capture, ReportSkip report_skip) {
      std::uint64_t generation;
      {
        std::lock_guard lock(mutex_);
        if (failed_ || applying_ || capturing_) {
          report_skip(failed_ ? "failed compose; protected layout requires repair" : (applying_ ? "compose in progress" : "snapshot in progress"));
          return false;
        }
        generation = generation_;
        capturing_ = true;
      }
      const auto superseded = [&] {
        std::lock_guard lock(mutex_);
        return generation != generation_;
      };
      const auto finish = [&](display_helper::SnapshotResult result) {
        std::lock_guard lock(mutex_);
        capturing_ = false;
        if (generation != generation_) {
          report_skip(result == display_helper::SnapshotResult::failed ? "snapshot superseded by compose" : "accepted snapshot superseded by compose; helper save may already be queued");
          return false;
        }
        return result != display_helper::SnapshotResult::failed;
      };
      try {
        if (superseded()) return finish(display_helper::SnapshotResult::failed);
        return finish(capture(superseded));
      } catch (...) {
        finish(display_helper::SnapshotResult::failed);
        throw;
      }
    }

  private:
    std::mutex mutex_;
    std::mutex compose_mutex_;
    std::uint64_t generation_ = 0;
    bool failed_ = false;
    bool applying_ = false;
    bool capturing_ = false;
  };
}
