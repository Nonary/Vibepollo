/**
 * @file src/platform/windows/gpu_priority.h
 * @brief Capture-scoped ownership of the process-wide GPU scheduling boost.
 */
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace platf::dxgi {
  class gpu_priority_manager_t {
    struct state_t {
      std::mutex mutex;
      std::function<std::optional<int>()> get;
      std::function<bool(int)> set;
      std::size_t users = 0;
      std::optional<int> original;
      std::optional<int> applied;

      void release() {
        const auto lock = std::lock_guard(mutex);
        if (--users != 0 || !original) {
          return;
        }
        // Do not overwrite a priority changed by another component. Retain
        // ownership on an API failure so the next capture can retry cleanup.
        const auto current = get();
        if (current && (current != applied || set(*original))) {
          original.reset();
          applied.reset();
        }
      }
    };

  public:
    class lease_t {
    public:
      ~lease_t() {
        if (armed) {
          state->release();
        }
      }
      lease_t(const lease_t &) = delete;
      lease_t &operator=(const lease_t &) = delete;

    private:
      friend class gpu_priority_manager_t;
      explicit lease_t(std::shared_ptr<state_t> state): state(std::move(state)) {}
      std::shared_ptr<state_t> state;
      bool armed = false;
    };

    gpu_priority_manager_t(std::function<std::optional<int>()> get, std::function<bool(int)> set):
        state(std::make_shared<state_t>()) {
      state->get = std::move(get);
      state->set = std::move(set);
    }

    std::unique_ptr<lease_t> acquire(int priority) {
      // Allocate before changing process state, including the first lease.
      auto lease = std::unique_ptr<lease_t>(new lease_t(state));
      const auto lock = std::lock_guard(state->mutex);
      const auto current = state->get();
      if (!current) {
        return {};
      }
      // Reuse an outstanding baseline after a failed restore, but adopt any
      // intervening external change, even while another capture is alive.
      const auto original = !state->original || current != state->applied ? current : state->original;
      if (!state->set(priority)) {
        return {};
      }
      state->original = original;
      state->applied = priority;
      ++state->users;
      lease->armed = true;
      return lease;
    }

  private:
    std::shared_ptr<state_t> state;
  };
}  // namespace platf::dxgi
