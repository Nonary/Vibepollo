#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace video::capture_recovery {
  struct observation_t {
    std::uint64_t generation;
    std::vector<std::string> outputs;
    std::atomic<bool> frame_observed {false};
  };

  // Recovery observes a frame from a newly initialized capture instance of the
  // exact output. A frame for another output, an old capture, or a timeout
  // cannot acknowledge it. This confirms output readiness, not a specific
  // client's encoder state. With no observer the per-frame path is one atomic load.
  class observations_t {
  public:
    std::uint64_t begin_capture() {
      return generation_.fetch_add(1, std::memory_order_relaxed);
    }

    std::shared_ptr<observation_t> watch(std::vector<std::string> outputs) {
      auto observation = std::make_shared<observation_t>();
      observation->outputs = std::move(outputs);
      std::lock_guard lock(mutex_);
      observation->generation = generation_.fetch_add(1, std::memory_order_relaxed);
      std::erase_if(observers_, [](const auto &entry) {
        return entry.expired();
      });
      observers_.emplace_back(observation);
      active_.store(true, std::memory_order_release);
      return observation;
    }

    void frame(const std::string &output, std::uint64_t instance) {
      if (!active_.load(std::memory_order_acquire)) {
        return;
      }
      std::lock_guard lock(mutex_);
      std::erase_if(observers_, [&](const auto &entry) {
        const auto observation = entry.lock();
        if (!observation) {
          return true;
        }
        const auto equal = [&](const std::string &candidate) {
          return !candidate.empty() && candidate.size() == output.size() &&
                 std::equal(candidate.begin(), candidate.end(), output.begin(), [](unsigned char a, unsigned char b) {
                   return std::tolower(a) == std::tolower(b);
                 });
        };
        if (instance > observation->generation && std::any_of(observation->outputs.begin(), observation->outputs.end(), equal)) {
          observation->frame_observed.store(true, std::memory_order_release);
          return true;
        }
        return false;
      });
      active_.store(!observers_.empty(), std::memory_order_release);
    }

  private:
    std::atomic<std::uint64_t> generation_ {1};
    std::atomic<bool> active_ {false};
    std::mutex mutex_;
    std::vector<std::weak_ptr<observation_t>> observers_;
  };

  inline observations_t &observations() {
    static observations_t state;
    return state;
  }

  enum class confirmation_status {
    waiting,
    retry,
    stalled,
    confirmed
  };

  // Keep an active target unconfirmed until capture produces a new frame.
  // Retry capture initialization a bounded number of times, then continue
  // watching for a late frame without repeatedly mutating the display.
  class confirmation_t {
  public:
    using clock = std::chrono::steady_clock;

    void start(std::shared_ptr<observation_t> observation, clock::time_point now) {
      observation_ = std::move(observation);
      attempts_ = 1;
      retry_after_ = now + std::chrono::seconds(5);
    }

    bool pending() const {
      return static_cast<bool>(observation_);
    }

    void reset() {
      observation_.reset();
    }

    confirmation_status poll(clock::time_point now) {
      if (observation_->frame_observed.load(std::memory_order_acquire)) {
        reset();
        return confirmation_status::confirmed;
      }
      if (now < retry_after_ || attempts_ > 3) {
        return confirmation_status::waiting;
      }
      retry_after_ = now + std::chrono::seconds(5);
      if (++attempts_ > 3) {
        return confirmation_status::stalled;
      }
      return confirmation_status::retry;
    }

  private:
    std::shared_ptr<observation_t> observation_;
    clock::time_point retry_after_ {};
    unsigned int attempts_ {0};
  };
}  // namespace video::capture_recovery
