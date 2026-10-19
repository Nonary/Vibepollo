/**
 * @file src/audio_role_control.h
 * @brief Render-role snapshot and guarded restoration shared by Windows and tests.
 */
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace audio {
  template<class Role, std::size_t Count>
  class role_control_t {
  public:
    using ids_t = std::array<std::wstring, Count>;

    struct pending_t {
      Role role;
      std::wstring preferred_id;
      std::wstring expected_current_id;
      bool fallback_transition = false;
    };

    using pending_list_t = std::vector<pending_t>;
    using token_t = std::shared_ptr<std::atomic_bool>;

    struct handoff_t {
      std::wstring steam_device_id;
      pending_list_t role_restores;
      std::uint64_t assignment_epoch = 0;
    };
    enum class retry_result_e {
      restored,
      no_longer_owned,
      inactive,
      no_device,
      failed
    };

    // The legacy visibility worker and the direct role controller share this
    // registry. Epoch/token arbitration is production logic, never an OS seam.
    struct registry_t {
      std::mutex mutex;
      std::uint64_t epoch {};
      ids_t desired;
      pending_list_t pending;
      std::wstring steam;
      token_t token;
      std::jthread worker;

      std::pair<handoff_t, std::jthread> begin(ids_t targets) {
        std::scoped_lock lock(mutex);
        handoff_t handoff {std::move(steam), std::move(pending), 0};
        if (token) {
          token->store(false, std::memory_order_release);
        }
        token.reset();
        pending.clear();
        steam.clear();
        if (++epoch == 0) {
          ++epoch;
        }
        desired = std::move(targets);
        handoff.assignment_epoch = epoch;
        return {std::move(handoff), std::move(worker)};
      }

      bool current(std::uint64_t value) {
        std::scoped_lock lock(mutex);
        return epoch == value;
      }

      bool current(std::uint64_t value, Role role, const std::wstring &id) {
        std::scoped_lock lock(mutex);
        return epoch == value && desired[static_cast<std::size_t>(role)] == id;
      }

      bool publish(std::uint64_t value, Role role, const std::wstring &id) {
        std::scoped_lock lock(mutex);
        if (epoch != value) {
          return false;
        }
        desired[static_cast<std::size_t>(role)] = id;
        return true;
      }

      bool owns_locked(const token_t &value, std::uint64_t generation) const {
        return value && value->load(std::memory_order_acquire) && token == value &&
               worker.get_id() == std::this_thread::get_id() && epoch == generation;
      }

      bool owns(const std::stop_token &stop, const token_t &value, std::uint64_t generation) {
        if (stop.stop_requested() || !value || !value->load(std::memory_order_acquire)) {
          return false;
        }
        std::scoped_lock lock(mutex);
        return owns_locked(value, generation);
      }
    };

    // These callbacks only cross device/OS boundaries or schedule an already
    // computed retry. No callback selects roles, targets or ownership epochs.
    struct device_api_t {
      std::function<ids_t()> defaults;
      std::function<long(Role, const std::wstring &)> write;
      std::function<std::optional<std::wstring>(const std::wstring &)> resolve;
      std::function<void(std::jthread)> retire;
      std::function<void(const std::wstring &, pending_list_t, std::uint64_t)> schedule;
      std::function<void(std::uint64_t)> clear_preferred;
      std::function<void(Role, long)> write_failed;
    };

    role_control_t(registry_t &registry, device_api_t devices):
        registry_ {registry},
        devices_ {std::move(devices)} {}

    void capture(ids_t defaults) {
      captured_ = std::move(defaults);
      captured_valid_ = true;
    }

    const ids_t &captured() const {
      return captured_;
    }

    const std::wstring &assigned() const {
      return assigned_;
    }

    std::uint64_t assignment_epoch() const {
      return assignment_epoch_;
    }

    bool assignment_active() const {
      return assignment_active_;
    }

    static pending_list_t normalize(pending_list_t roles, const std::wstring &steam, const ids_t &current) {
      pending_list_t result;
      for (auto &role : roles) {
        const auto &id = current[static_cast<std::size_t>(role.role)];
        if (role.preferred_id.empty() || id.empty()) {
          continue;
        }
        const bool retained = id == role.expected_current_id || id == role.preferred_id;
        const bool transition = role.fallback_transition && !steam.empty() && role.expected_current_id == steam;
        if (!retained && !transition) {
          continue;
        }
        role.expected_current_id = id;
        role.fallback_transition = false;
        result.push_back(std::move(role));
      }
      return result;
    }

    int assign(const std::wstring &endpoint) {
      const auto current = devices_.defaults();
      ids_t desired;
      desired.fill(endpoint);
      auto handoff = begin(std::move(desired));
      assignment_epoch_ = handoff.assignment_epoch;
      auto inherited = normalize(std::move(handoff.role_restores), handoff.steam_device_id, current);
      if (assigned_.empty()) {
        // A snapshot taken before topology mutation is authoritative. Reading
        // live defaults here would replace DP/HDMI with Windows' fallback.
        if (!captured_valid_) {
          capture(current);
        }
        for (const auto &role : inherited) {
          captured_[index(role.role)] = role.preferred_id;
        }
        assigned_ = endpoint;
      }
      int failures = 0;
      assignment_active_ = true;
      pending_list_t failed;
      for (std::size_t n = 0; n < Count; ++n) {
        const auto role = static_cast<Role>(n);
        auto result = write(assignment_epoch_, role, endpoint);
        if (!result) {
          assignment_active_ = false;
          break;
        }
        if (*result != 0) {
          ++failures;
          report(role, *result);
          for (const auto &pending : inherited) {
            if (pending.role == role) {
              failed.push_back(pending);
              break;
            }
          }
        }
      }
      if (assignment_active_ && !failed.empty() && !handoff.steam_device_id.empty()) {
        schedule(handoff.steam_device_id, std::move(failed), assignment_epoch_);
      }
      return failures;
    }

    int restore() {
      const auto current = devices_.defaults();
      auto desired = current;
      for (std::size_t n = 0; n < Count; ++n) {
        if (!assigned_.empty() && current[n] == assigned_ && !captured_[n].empty() && captured_[n] != assigned_) {
          desired[n] = captured_[n];
        }
      }
      restore_handoff_ = begin(std::move(desired));
      const auto epoch = restore_handoff_.assignment_epoch;
      int failures = 0;
      for (std::size_t n = 0; n < Count; ++n) {
        if (assigned_.empty() || current[n] != assigned_ || captured_[n].empty() || captured_[n] == assigned_) {
          continue;
        }
        const auto role = static_cast<Role>(n);
        // Best effort: preserve choices observed by this final check. MMDevice
        // has no atomic default-role compare-and-set; a change during the
        // subsequent endpoint write can still be overwritten.
        const auto latest = devices_.defaults()[n];
        if (latest != assigned_) {
          if (!registry_.publish(epoch, role, latest)) {
            break;
          }
          continue;
        }
        auto result = write(epoch, role, captured_[n]);
        if (!result) {
          break;
        }
        if (*result < 0) {
          report(role, *result);
          ++failures;
        }
      }
      return failures;
    }

    void reset_failed(const std::wstring &live_steam) {
      auto inherited = std::move(restore_handoff_);
      restore_handoff_ = {};
      if (inherited.assignment_epoch == 0) {
        inherited = begin(devices_.defaults());
      } else if (!registry_.current(inherited.assignment_epoch)) {
        return;
      }
      const auto steam = live_steam.empty() ? inherited.steam_device_id : live_steam;
      if (steam.empty()) {
        devices_.clear_preferred(inherited.assignment_epoch);
        return;
      }
      const auto current = devices_.defaults();
      pending_list_t roles;
      if (assigned_ == steam) {
        for (std::size_t n = 0; n < Count; ++n) {
          if (current[n] == assigned_) {
            roles.push_back({static_cast<Role>(n), captured_[n] == assigned_ ? std::wstring {} : captured_[n], assigned_});
          }
        }
      }
      for (auto &old : normalize(std::move(inherited.role_restores), inherited.steam_device_id, current)) {
        bool present = false;
        for (const auto &role : roles) {
          if (role.role == old.role) {
            present = true;
          }
        }
        if (!present) {
          roles.push_back(std::move(old));
        }
      }
      if (roles.empty()) {
        devices_.clear_preferred(inherited.assignment_epoch);
        return;
      }
      schedule(steam, std::move(roles), inherited.assignment_epoch);
    }

    retry_result_e retry(const pending_t &role, const std::stop_token &stop, const token_t &token, std::uint64_t epoch) {
      if (!registry_.owns(stop, token, epoch)) {
        return retry_result_e::inactive;
      }
      if (devices_.defaults()[index(role.role)] != role.expected_current_id) {
        adopt_worker(role.role, token, epoch);
        return registry_.owns(stop, token, epoch) ? retry_result_e::no_longer_owned : retry_result_e::inactive;
      }
      if (role.preferred_id.empty()) {
        return retry_result_e::no_device;
      }
      const auto target = devices_.resolve(role.preferred_id);
      if (!target) {
        return retry_result_e::no_device;
      }
      if (devices_.defaults()[index(role.role)] != role.expected_current_id) {
        adopt_worker(role.role, token, epoch);
        return registry_.owns(stop, token, epoch) ? retry_result_e::no_longer_owned : retry_result_e::inactive;
      }
      {
        std::scoped_lock lock(registry_.mutex);
        if (!registry_.owns_locked(token, epoch)) {
          return retry_result_e::inactive;
        }
        registry_.desired[index(role.role)] = *target;
      }
      const auto status = devices_.write(role.role, *target);
      if (!registry_.owns(stop, token, epoch) || !registry_.current(epoch, role.role, *target)) {
        reassert(role.role);
        return retry_result_e::inactive;
      }
      if (status < 0) {
        report(role.role, status);
        return retry_result_e::failed;
      }
      return retry_result_e::restored;
    }

  private:
    void schedule(const std::wstring &steam, pending_list_t roles, std::uint64_t epoch) {
      {
        std::scoped_lock lock(registry_.mutex);
        if (registry_.epoch != epoch) {
          return;
        }
        registry_.pending = roles;
        registry_.steam = steam;
      }
      try {
        devices_.schedule(steam, std::move(roles), epoch);
      } catch (...) {
        // A scheduler failure before worker publication must not leave a
        // phantom handoff for the next stream. Never clear a successor's work.
        std::scoped_lock lock(registry_.mutex);
        if (registry_.epoch == epoch && !registry_.token) {
          registry_.pending.clear();
          registry_.steam.clear();
        }
        throw;
      }
    }

    static std::size_t index(Role role) {
      return static_cast<std::size_t>(role);
    }

    handoff_t begin(ids_t desired) {
      auto [handoff, worker] = registry_.begin(std::move(desired));
      devices_.retire(std::move(worker));
      return handoff;
    }

    void report(Role role, long status) {
      if (devices_.write_failed) {
        devices_.write_failed(role, status);
      }
    }

    void reassert(Role role) {
      for (int attempt = 0; attempt < 3; ++attempt) {
        std::uint64_t epoch;
        std::wstring desired;
        {
          std::scoped_lock lock(registry_.mutex);
          epoch = registry_.epoch;
          desired = registry_.desired[index(role)];
        }
        if (desired.empty()) {
          return;
        }
        const auto status = devices_.write(role, desired);
        if (status < 0) {
          report(role, status);
        }
        if (status >= 0 && registry_.current(epoch, role, desired)) {
          return;
        }
      }
    }

    std::optional<long> write(std::uint64_t epoch, Role role, const std::wstring &id) {
      if (!registry_.publish(epoch, role, id)) {
        return std::nullopt;
      }
      const auto status = devices_.write(role, id);
      if (!registry_.current(epoch, role, id)) {
        reassert(role);
        return std::nullopt;
      }
      return status;
    }

    void adopt_worker(Role role, const token_t &token, std::uint64_t epoch) {
      const auto current = devices_.defaults()[index(role)];
      std::scoped_lock lock(registry_.mutex);
      if (registry_.owns_locked(token, epoch)) {
        registry_.desired[index(role)] = current;
      }
    }

    registry_t &registry_;
    device_api_t devices_;
    ids_t captured_;
    bool captured_valid_ {false};
    std::wstring assigned_;
    handoff_t restore_handoff_;
    std::uint64_t assignment_epoch_ {};
    bool assignment_active_ {false};
  };
}  // namespace audio
