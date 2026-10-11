#pragma once

#include <cstdint>
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

    template<class Capture, class Disconnected, class ReportSkip>
    bool snapshot(Capture capture, Disconnected disconnected, ReportSkip report_skip) {
      std::uint64_t generation;
      bool failed;
      {
        std::lock_guard lock(mutex_);
        if (applying_ || capturing_) {
          report_skip(applying_ ? "compose in progress" : "snapshot in progress");
          return false;
        }
        generation = generation_;
        failed = failed_;
        capturing_ = true;
      }
      const auto superseded = [&] {
        std::lock_guard lock(mutex_);
        return generation != generation_;
      };
      const auto finish = [&](bool ok) {
        std::lock_guard lock(mutex_);
        capturing_ = false;
        if (generation != generation_) {
          report_skip("snapshot superseded by compose");
          return false;
        }
        return ok;
      };
      try {
        // The potentially side-effecting All-paths probe is only justified
        // when failed-compose admission would otherwise reject the snapshot.
        if (failed) {
          const bool gone = disconnected();
          if (superseded()) return finish(false);
          if (!gone) {
            report_skip("failed compose; saved baseline devices still connected or connectivity unknown");
            return finish(false);
          }
          {
            std::lock_guard lock(mutex_);
            if (generation == generation_) failed_ = false;
          }
        }
        if (superseded()) return finish(false);
        return finish(capture(superseded));
      } catch (...) {
        finish(false);
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
