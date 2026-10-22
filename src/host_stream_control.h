/** @file src/host_stream_control.h
 * @brief Admission boundary for the explicit local end-all-streams action.
 */
#pragma once
#include <atomic>
#include <cstdint>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

namespace host_stream_control {
  // One counter covers startup and teardown through their last in-process
  // runtime/config tail. Entry never waits: lifecycle-held cleanup callers
  // must be able to defer while terminal is draining, rather than deadlock.
  struct state_t {
    std::mutex mutex;
    std::condition_variable idle;
    std::timed_mutex terminal;
    std::size_t active = 0;
    std::atomic_uint64_t epoch {0};
  };

  inline state_t &state() {
    static state_t value;
    return value;
  }

  struct local_t {
    unsigned depth = 0;
    bool registered = false;
    bool terminal = false;
  };

  inline local_t &local() {
    static thread_local local_t value;
    return value;
  }

  using ticket_t = std::uint64_t;
  inline std::atomic_uint64_t &epoch() { return state().epoch; }
  inline ticket_t request_ticket() { return epoch().load(std::memory_order_acquire); }

  class activity_guard_t {
    bool entered = false;

  public:
    activity_guard_t() {
      auto &thread = local();
      if (thread.depth != 0 || thread.terminal) {
        ++thread.depth;
        entered = true;
        return;
      }
      std::lock_guard lock {state().mutex};
      if (request_ticket() & 1) return;
      ++state().active;
      thread.registered = true;
      ++thread.depth;
      entered = true;
    }

    activity_guard_t(const activity_guard_t &) = delete;
    activity_guard_t &operator=(const activity_guard_t &) = delete;
    explicit operator bool() const { return entered; }

    void unlock() {
      if (!std::exchange(entered, false)) return;
      auto &thread = local();
      if (--thread.depth == 0 && std::exchange(thread.registered, false)) {
        std::lock_guard lock {state().mutex};
        --state().active;
        state().idle.notify_all();
      }
    }

    ~activity_guard_t() { unlock(); }
  };

  // Unlike activity_guard_t's same-thread nesting, a callback reservation can
  // cross SDK threads. Its last shared owner retires the counted operation.
  // A missing completion therefore makes terminal's bounded drain fail closed.
  class callback_guard_t {
    struct reservation_t {
      ~reservation_t() {
        std::lock_guard lock {state().mutex};
        --state().active;
        state().idle.notify_all();
      }
    };
    std::shared_ptr<reservation_t> reservation;

  public:
    callback_guard_t() {
      std::lock_guard lock {state().mutex};
      if ((request_ticket() & 1) && !local().registered) return;
      // Allocate before publishing the counter so allocation failure cannot
      // strand an activity that no callback owns.
      reservation = std::make_shared<reservation_t>();
      ++state().active;
    }
    explicit operator bool() const { return static_cast<bool>(reservation); }
  };

  class start_guard_t {
    activity_guard_t activity;
    bool admitted;

  public:
    explicit start_guard_t(ticket_t ticket):
        admitted(activity && (ticket & 1) == 0 && ticket == request_ticket()) {}
    explicit operator bool() const { return admitted; }
    void unlock() { admitted = false; activity.unlock(); }
  };

  template<class Authorize, class Action>
  bool terminate(Authorize authorized, Action action,
                 std::chrono::milliseconds drain_timeout = std::chrono::seconds(30)) {
    // No recursive terminal action or terminal-from-lifecycle callback: it
    // would otherwise wait for its own activity reservation.
    if (local().depth != 0 || local().terminal) return false;
    const auto deadline = std::chrono::steady_clock::now() + drain_timeout;
    std::unique_lock terminal {state().terminal, std::defer_lock};
    if (!terminal.try_lock_until(deadline) || !authorized()) return false;
    {
      std::lock_guard lock {state().mutex};
      epoch().fetch_add(1, std::memory_order_acq_rel);
    }
    struct reopen_t {
      ~reopen_t() {
        local().terminal = false;
        std::lock_guard lock {state().mutex};
        epoch().fetch_add(1, std::memory_order_release);
      }
    } reopen;
    {
      std::unique_lock lock {state().mutex};
      if (!state().idle.wait_until(lock, deadline, [] { return state().active == 0; })) return false;
    }
    if (!authorized()) return false;
    local().terminal = true;
    action();
    return true;
  }

  // Shared by the actual WebRTC route adapter and NVHTTP single-flight adapter.
  // Rejection must write the protocol response without entering startup code.
  template<class Submit, class Task, class Reject>
  void submit_start(ticket_t ticket, Submit &&submit, Task &&task, Reject &&reject) {
    std::forward<Submit>(submit)([ticket, task = std::forward<Task>(task), reject = std::forward<Reject>(reject)]() mutable {
      start_guard_t admission {ticket};
      if (!admission) {
        reject();
        return;
      }
      task();
    });
  }

  template<class Submit, class Task, class Reject>
  void submit_start(Submit &&submit, Task &&task, Reject &&reject) {
    submit_start(request_ticket(), std::forward<Submit>(submit), std::forward<Task>(task), std::forward<Reject>(reject));
  }

  // Queued RTSP workers keep the server alive for shutdown, but only workers
  // from the current open epoch own capture runtime. Terminal exclusivity has
  // drained executing workers before it invalidates the older queued owners.
  class startup_dispatch_t {
    struct state_t {
      std::mutex mutex;
      std::map<ticket_t, int> pending;
      int responses = 0;
    };

    std::shared_ptr<state_t> state = std::make_shared<state_t>();

    struct reservation_t {
      std::shared_ptr<state_t> state;
      ticket_t ticket;
      bool pending = false;
      bool runtime;

      reservation_t(std::shared_ptr<state_t> state, ticket_t ticket, bool runtime = true):
          state(std::move(state)),
          ticket(ticket),
          runtime(runtime) {
        std::lock_guard lock {this->state->mutex};
        if (runtime) {
          ++this->state->pending[ticket];
        } else {
          ++this->state->responses;
        }
        pending = true;
      }

      bool retire() {
        std::lock_guard lock {state->mutex};
        if (!std::exchange(pending, false)) {
          return false;
        }
        if (runtime) {
          const auto it = state->pending.find(ticket);
          if (--it->second == 0) {
            state->pending.erase(it);
          }
        } else {
          --state->responses;
        }
        return true;
      }

      ~reservation_t() {
        retire();
      }
    };

  public:
    int count() const {
      std::lock_guard lock {state->mutex};
      const auto current = request_ticket();
      if (current & 1) {
        return 0;
      }
      const auto it = state->pending.find(current);
      return it == state->pending.end() ? 0 : it->second;
    }

    int total_count() const {
      std::lock_guard lock {state->mutex};
      int total = state->responses;
      for (const auto &[ticket, count] : state->pending) {
        total += count;
      }
      return total;
    }

    // Socket completions still keep the I/O loop alive during server shutdown,
    // but cannot defer runtime cleanup and carry no runtime finalizer callback.
    template<class Post, class Response>
    void post_response(Post &&post, Response &&response) {
      auto completion = std::make_shared<reservation_t>(state, 0, false);
      try {
        std::forward<Post>(post)([completion, response = std::forward<Response>(response)]() mutable {
          try {
            response();
          } catch (...) {
            completion->retire();
            throw;
          }
          completion->retire();
        });
      } catch (...) {
        completion->retire();
        throw;
      }
    }

    template<class Submit, class Task, class Finalize>
    void submit(Submit &&submit, Task &&task, Finalize finalize) {
      submit_with_ticket(request_ticket(), std::forward<Submit>(submit), std::forward<Task>(task), std::move(finalize));
    }

    template<class Submit, class Task, class Finalize>
    void submit_with_ticket(ticket_t ticket, Submit &&submit, Task &&task, Finalize finalize) {
      auto reservation = std::make_shared<reservation_t>(state, ticket);
      try {
        std::forward<Submit>(submit)([reservation, task = std::forward<Task>(task), finalize]() mutable {
          start_guard_t admission {reservation->ticket};
          try {
            task(static_cast<bool>(admission));
          } catch (...) {
            if (reservation->retire() && admission) {
              finalize();
            }
            throw;
          }
          // Retire/finalize before releasing admission, never on a posted I/O
          // completion. An invalidated worker may only send a canceled response.
          if (reservation->retire() && admission) {
            finalize();
          }
        });
      } catch (...) {
        start_guard_t admission {ticket};
        if (reservation->retire() && admission) {
          finalize();
        }
        throw;
      }
    }
  };
}  // namespace host_stream_control
