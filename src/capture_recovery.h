#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace video::capture_recovery {
  inline bool same_name(const std::string &left, const std::string &right) {
    return !left.empty() && left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(), [](unsigned char a, unsigned char b) {
             return std::tolower(a) == std::tolower(b);
           });
  }

  // Resolve the current device identity for a GDI output name. A GDI name is
  // mutable across topology changes, so it is only a lookup key; callers must
  // compare the resolved device ID with the identity they intend to observe.
  template<typename DeviceList, typename NameOf, typename IdOf>
  std::optional<std::string> resolve_device_id_for_output(
    const std::string &output,
    const DeviceList &devices,
    NameOf name_of,
    IdOf id_of
  ) {
    if (output.empty()) {
      return std::nullopt;
    }

    std::optional<std::string> resolved;
    for (const auto &device : devices) {
      if (!same_name(name_of(device), output)) {
        continue;
      }
      const auto &device_id = id_of(device);
      if (device_id.empty()) {
        return std::nullopt;
      }
      if (resolved && !same_name(*resolved, device_id)) {
        return std::nullopt;
      }
      resolved = device_id;
    }
    return resolved;
  }

  struct observation_t {
    std::uint64_t generation;
    // Only stable target IDs can acknowledge the watched recovery.
    std::vector<std::string> device_ids;
    std::uint64_t last_identity_lookup_instance {0};
    std::uint64_t retry_identity_lookup_instance {0};
    std::chrono::steady_clock::time_point retry_identity_lookup_after {};
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

    std::shared_ptr<observation_t> watch(std::vector<std::string> device_ids) {
      auto observation = std::make_shared<observation_t>();
      observation->device_ids = std::move(device_ids);
      std::lock_guard lock(mutex_);
      observation->generation = generation_.fetch_add(1, std::memory_order_relaxed);
      std::erase_if(observers_, [](const auto &entry) {
        return entry.expired();
      });
      observers_.emplace_back(observation);
      active_.store(true, std::memory_order_release);
      return observation;
    }

    bool requires_identity_lookup(
      std::uint64_t instance,
      std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()
    ) {
      if (!active_.load(std::memory_order_acquire)) {
        return false;
      }
      std::lock_guard lock(mutex_);
      bool required = false;
      std::erase_if(observers_, [&](const auto &entry) {
        const auto observation = entry.lock();
        if (!observation) {
          return true;
        }
        const bool retry_ready =
          instance != observation->retry_identity_lookup_instance || now >= observation->retry_identity_lookup_after;
        if (instance > observation->generation && instance != observation->last_identity_lookup_instance && retry_ready) {
          required = true;
        }
        return false;
      });
      active_.store(!observers_.empty(), std::memory_order_release);
      return required;
    }

    void identity_lookup_failed(
      std::uint64_t instance,
      std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()
    ) {
      if (!active_.load(std::memory_order_acquire)) {
        return;
      }
      std::lock_guard lock(mutex_);
      std::erase_if(observers_, [&](const auto &entry) {
        const auto observation = entry.lock();
        if (!observation) {
          return true;
        }
        if (instance > observation->generation && instance != observation->last_identity_lookup_instance) {
          observation->retry_identity_lookup_instance = instance;
          observation->retry_identity_lookup_after = now + std::chrono::milliseconds(500);
        }
        return false;
      });
      active_.store(!observers_.empty(), std::memory_order_release);
    }

    void frame(
      const std::string &device_id,
      std::uint64_t instance,
      std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()
    ) {
      if (!active_.load(std::memory_order_acquire)) {
        return;
      }
      std::lock_guard lock(mutex_);
      std::erase_if(observers_, [&](const auto &entry) {
        const auto observation = entry.lock();
        if (!observation) {
          return true;
        }
        const auto identity_matches = std::any_of(
          observation->device_ids.begin(), observation->device_ids.end(),
          [&](const auto &candidate) { return same_name(candidate, device_id); }
        );
        if (instance > observation->generation) {
          if (identity_matches) {
            observation->last_identity_lookup_instance = instance;
            observation->retry_identity_lookup_instance = 0;
            observation->frame_observed.store(true, std::memory_order_release);
            return true;
          }
          // A current name-to-ID mapping can be valid yet point at a different
          // output after a topology change. Retry on the same capture instance
          // in case Windows reassigns the output name while that capture lives.
          observation->retry_identity_lookup_instance = instance;
          observation->retry_identity_lookup_after = now + std::chrono::milliseconds(500);
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

  // This is the production frame path used by video.cpp. Resolve only while a
  // live observer is waiting, then confirm by current device ID rather than by
  // the mutable GDI name alone. Failed lookups and mismatched identities are
  // retried with a short delay.
  template<typename DeviceListProvider, typename NameOf, typename IdOf>
  void observe_frame_from_current_device_list(
    observations_t &observations,
    const std::string &output,
    std::uint64_t instance,
    DeviceListProvider devices_for_output,
    NameOf name_of,
    IdOf id_of,
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()
  ) {
    if (!observations.requires_identity_lookup(instance, now)) {
      return;
    }
    const auto devices = devices_for_output();
    if (!devices) {
      observations.identity_lookup_failed(instance, now);
      return;
    }
    const auto device_id = resolve_device_id_for_output(output, *devices, name_of, id_of);
    if (device_id) {
      observations.frame(*device_id, instance, now);
    } else {
      observations.identity_lookup_failed(instance, now);
    }
  }

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
