#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

// Exact existing outputs are borrowed, never created or configured. This gate is
// independent of legacy managed-display ownership and cannot be overridden by a
// terminal restore. Tokens may move between pending/startup/capture threads.
namespace display_target {
  enum class platform_e { linux_drm, windows_device };
  enum class kind_e { physical, shared_virtual };
  struct descriptor_t {
    platform_e platform {};
    kind_e kind {};
    std::string exact_id;
    bool operator==(const descriptor_t &) const = default;
  };
  struct mode_t {
    int width {}, height {};
    std::uint32_t refresh_millihz {};
    bool hdr {};
    bool operator==(const mode_t &) const = default;
  };
  struct observation_t {
    descriptor_t target;
    mode_t mode;
    std::string capture_output;
    std::shared_ptr<void> runtime_hold;
  };
  using probe_t = std::function<std::optional<observation_t>(const descriptor_t &)>;

  class registry_t {
    struct barrier_t {
      std::mutex mutex;
      std::atomic<std::size_t> borrowers {0}, mutations {0};
      std::atomic<bool> uncertain {false}, permanent_fence {false};
      std::thread::id mutation_owner;
    };
    struct hold_t {
      std::shared_ptr<barrier_t> barrier;
      descriptor_t target;
      mode_t mode;
      std::string output;
      std::shared_ptr<void> runtime_hold;
      explicit hold_t(std::shared_ptr<barrier_t> value): barrier(std::move(value)) { ++barrier->borrowers; }
      ~hold_t() { runtime_hold.reset(); --barrier->borrowers; }
    };
    struct owner_t {
      std::uint64_t generation {};
      std::shared_ptr<hold_t> hold;
    };
    std::shared_ptr<barrier_t> barrier_ = std::make_shared<barrier_t>();
    std::unordered_map<std::string, owner_t> owners_;
    std::unordered_map<std::string, std::uint64_t> observed_, released_;
    bool closed_ = false;
  public:
    struct result_t {
      std::shared_ptr<void> hold;
      std::optional<descriptor_t> target;
      std::string output, error;
      explicit operator bool() const { return hold != nullptr; }
    };
    class mutation_t {
      std::shared_ptr<barrier_t> barrier_;
      friend class registry_t;
      explicit mutation_t(std::shared_ptr<barrier_t> barrier): barrier_(std::move(barrier)) {}
    public:
      mutation_t() = default;
      mutation_t(const mutation_t &) = delete;
      mutation_t &operator=(const mutation_t &) = delete;
      mutation_t(mutation_t &&other) noexcept: barrier_(std::move(other.barrier_)) {}
      ~mutation_t() {
        if (!barrier_) return;
        std::lock_guard lock {barrier_->mutex};
        if (barrier_->mutations && --barrier_->mutations == 0) barrier_->mutation_owner = {};
      }
      explicit operator bool() const { return barrier_ != nullptr; }
      // An unconfirmed helper completion fences new borrowers until restart.
      void completion_unknown() {
        if (!barrier_) return;
        std::lock_guard lock {barrier_->mutex};
        barrier_->uncertain.store(true, std::memory_order_release);
      }
    };
    mutation_t begin_mutation() {
      std::lock_guard lock {barrier_->mutex};
      const auto caller = std::this_thread::get_id();
      if (barrier_->permanent_fence.load(std::memory_order_acquire) ||
          barrier_->uncertain.load(std::memory_order_acquire) || barrier_->borrowers != 0 ||
          (barrier_->mutations != 0 && barrier_->mutation_owner != caller)) return {};
      if (barrier_->mutations == 0) barrier_->mutation_owner = caller;
      ++barrier_->mutations;
      return mutation_t {barrier_};
    }
    bool protected_output() const {
      return barrier_->permanent_fence.load(std::memory_order_acquire) ||
             barrier_->uncertain.load(std::memory_order_acquire) || barrier_->borrowers != 0;
    }
    bool completion_uncertain() const { return barrier_->uncertain; }
    // May run from a native runtime_hold destructor while the registry mutex
    // is held. No locks, callbacks, or native actions are allowed here.
    void fence_unknown_release() noexcept {
      // This first atomic store is the common linearization point observed by
      // every borrow and ordinary mutation gate, even outside an active hold.
      barrier_->permanent_fence.store(true, std::memory_order_release);
      barrier_->uncertain.store(true, std::memory_order_release);
    }
    result_t acquire(const std::string &client, std::uint64_t generation, const descriptor_t &target, mode_t mode, const probe_t &probe) {
      std::unique_lock lock {barrier_->mutex};
      if (closed_) return {{}, {}, {}, "Target lease admission has shut down."};
      if (client.empty() || generation == 0 || target.exact_id.empty() || mode.width <= 0 || mode.height <= 0 || mode.refresh_millihz == 0)
        return {{}, {}, {}, "Invalid exact target lease identity or mode."};
      if (generation < observed_[client] || (released_.contains(client) && generation <= released_[client]))
        return {{}, {}, {}, "Stale exact target lease generation."};
      observed_[client] = generation;
      const auto existing = owners_.find(client);
      if (existing != owners_.end() && (existing->second.hold->target != target || existing->second.hold->mode != mode))
        return {{}, {}, {}, "Disconnect Monitor before changing the retained exact target or mode."};
      if (barrier_->permanent_fence.load(std::memory_order_acquire) || barrier_->mutations != 0 || barrier_->uncertain)
        return {{}, {}, {}, "Display mutation is in flight or its completion is unconfirmed."};
      // Install protection before the native read. No native mutation can enter
      // between observing the exact mode and publishing the retained binding.
      auto hold = existing == owners_.end() ? std::make_shared<hold_t>(barrier_) : existing->second.hold;
      const auto previous_hold = existing == owners_.end() ? nullptr : existing->second.hold;
      lock.unlock();
      std::optional<observation_t> observed;
      try { if (probe) observed = probe(target); } catch (...) {}
      lock.lock();
      const auto current = owners_.find(client);
      if (barrier_->permanent_fence.load(std::memory_order_acquire) || barrier_->uncertain.load(std::memory_order_acquire) || closed_ || observed_[client] != generation || (released_.contains(client) && generation <= released_[client]) ||
          (current != owners_.end() && current->second.hold != previous_hold)) {
        lock.unlock();
        return {{}, {}, {}, "Target admission was revoked or superseded during observation."};
      }
      if (!observed || observed->target != target || observed->capture_output.empty()) {
        lock.unlock();
        return {{}, {}, {}, "The exact target is missing, renamed, inactive, or unavailable for capture."};
      }
      if (observed->mode != mode) {
        lock.unlock();
        return {{}, {}, {}, "The existing target resolution, refresh, or HDR differs from the request; borrowing cannot change it."};
      }
      if (previous_hold && hold->output != observed->capture_output) {
        lock.unlock();
        return {{}, {}, {}, "The retained exact target capture identity changed."};
      }
      try {
        if (!previous_hold) {
          hold->target = target;
          hold->mode = mode;
          hold->output = observed->capture_output;
          hold->runtime_hold = std::move(observed->runtime_hold);
        }
        owners_.insert_or_assign(client, owner_t {generation, hold});
        result_t result {hold, target, hold->output, {}};
        // A rejected probe can own a native inhibitor whose destructor may
        // perform bounded IPC. Ensure every return destroys it after unlock.
        lock.unlock();
        return result;
      } catch (...) {
        if (lock.owns_lock()) lock.unlock();
        throw;
      }
    }
    bool revalidate(const std::shared_ptr<void> &token, const probe_t &probe) const {
      if (!token || !probe) return false;
      const auto hold = std::static_pointer_cast<hold_t>(token);
      try {
        const auto observed = probe(hold->target);
        return observed && observed->target == hold->target && observed->mode == hold->mode && observed->capture_output == hold->output;
      } catch (...) { return false; }
    }
    std::optional<result_t> snapshot(const std::string &client, std::uint64_t generation) const {
      std::lock_guard lock {barrier_->mutex};
      const auto it = owners_.find(client);
      if (it == owners_.end() || it->second.generation != generation) return std::nullopt;
      const auto &hold = it->second.hold;
      return result_t {hold, hold->target, hold->output, {}};
    }
    // nullopt delegates to the legacy per-client runtime. A stale event is
    // handled without releasing the newer owner or its pending/capture holds.
    std::optional<bool> release(const std::string &client, std::uint64_t generation) {
      std::unique_lock lock {barrier_->mutex};
      const auto it = owners_.find(client);
      if (it == owners_.end()) {
        if (released_.contains(client) && generation <= released_[client]) return true;
        return std::nullopt;
      }
      if (generation != it->second.generation) return true;
      released_[client] = generation;
      auto retired = std::move(it->second.hold);
      owners_.erase(it);
      lock.unlock();
      retired.reset();
      return true;
    }
    void revoke(const std::string &client) {
      std::unique_lock lock {barrier_->mutex};
      auto &released_generation = released_[client];
      if (released_generation < observed_[client]) released_generation = observed_[client];
      if (const auto it = owners_.find(client); it != owners_.end()) {
        if (released_generation < it->second.generation) released_generation = it->second.generation;
        auto retired = std::move(it->second.hold);
        owners_.erase(it);
        lock.unlock();
        retired.reset();
      }
    }
    void shutdown() {
      std::unordered_map<std::string, owner_t> retired;
      std::unique_lock lock {barrier_->mutex};
      closed_ = true;
      for (const auto &[client, owner] : owners_) released_[client] = owner.generation;
      retired.swap(owners_);
      lock.unlock();
      retired.clear();
    }
  };
  // A native hold may be the final process-global destructor. Keep its atomic
  // fence alive until process death; explicit shutdown() still closes admission.
  inline registry_t &leases() { static auto *registry = new registry_t; return *registry; }
}
