/**
 * @file src/nvhttp_handoff.h
 * @brief Single-flight NVHTTP mutations with one bounded post-cancel start.
 */
#pragma once

#include "nvhttp_pending_peer.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

namespace nvhttp::handoff {
  enum class operation_e { start, cancel, other };
  enum class result_e { admitted, waiting, busy };
  enum class end_e { disconnected, expired, stopping };

  using task_t = std::function<void()>;
  using submit_t = std::function<void(task_t)>;
  using clock_t = std::chrono::steady_clock;

  class admission_t {
    struct job_t;
    struct state_t: std::enable_shared_from_this<state_t> {
      std::mutex mutex;
      bool stopping = false;
      std::uint64_t next_id = 0;
      std::uint64_t active_id = 0;
      operation_e active_operation = operation_e::other;
      std::shared_ptr<job_t> pending;
      std::shared_ptr<job_t> reserved;

      void finish(std::uint64_t id);
      void abort(const std::shared_ptr<job_t> &job, end_e reason);
      void prepared(const std::shared_ptr<job_t> &job, bool ready);
      void run(const std::shared_ptr<job_t> &job);
    };

    struct job_t {
      enum class phase_e { pending, reserved, running, done };
      phase_e phase = phase_e::pending;  // protected by state_t::mutex
      std::uint64_t id = 0;
      clock_t::time_point deadline;
      submit_t submit;
      task_t task;
      std::function<void(end_e)> on_end;
      std::shared_ptr<pending_peer_t> peer;
    };

    struct lease_t {
      std::shared_ptr<state_t> state;
      std::uint64_t id;
      ~lease_t() { state->finish(id); }
    };

    std::shared_ptr<state_t> state_ = std::make_shared<state_t>();

  public:
    result_e submit(operation_e operation, submit_t submit, task_t task,
                    pending_peer_factory_t make_peer = {}, std::function<void(end_e)> on_end = {},
                    clock_t::time_point deadline = {}) {
      auto state = state_;
      std::unique_lock lock(state->mutex);
      if (state->stopping) return result_e::busy;
      if (state->active_id == 0) {
        const auto id = ++state->next_id;
        state->active_id = id;
        state->active_operation = operation;
        std::shared_ptr<lease_t> lease;
        try {
          lease = std::make_shared<lease_t>(state, id);
        } catch (...) {
          state->active_id = 0;
          throw;
        }
        lock.unlock();
        submit([lease = std::move(lease), task = std::move(task)]() mutable {
          auto running = std::move(lease);
          task();
        });
        return result_e::admitted;
      }
      if (operation != operation_e::start || state->active_operation != operation_e::cancel || state->pending || !make_peer) {
        return result_e::busy;
      }
      auto peer = make_peer();
      if (!peer) return result_e::busy;
      auto job = std::make_shared<job_t>();
      job->deadline = deadline;
      job->submit = std::move(submit);
      job->task = std::move(task);
      job->on_end = std::move(on_end);
      job->peer = std::move(peer);
      state->pending = job;
      try {
        // Post the read before the current cancel can reserve the handoff.
        // The watcher serializes its start/prepare operations on one strand.
        job->peer->start(deadline, [weak_state = std::weak_ptr<state_t>(state), weak_job = std::weak_ptr<job_t>(job)](pending_peer_end_e reason) {
          if (auto current = weak_state.lock()) {
            if (auto waiting = weak_job.lock()) {
              current->abort(waiting, reason == pending_peer_end_e::expired ? end_e::expired : end_e::disconnected);
            }
          }
        });
      } catch (...) {
        state->pending.reset();
        throw;
      }
      return result_e::waiting;
    }

    void stop() {
      auto state = state_;
      std::shared_ptr<job_t> pending, reserved;
      {
        std::lock_guard lock(state->mutex);
        state->stopping = true;
        pending = state->pending;
        reserved = state->reserved;
      }
      if (pending) state->abort(pending, end_e::stopping);
      if (reserved) state->abort(reserved, end_e::stopping);
    }
  };

  inline void admission_t::state_t::finish(std::uint64_t id) {
    std::shared_ptr<job_t> next;
    {
      std::lock_guard lock(mutex);
      if (active_id != id) return;
      active_id = 0;
      if (!stopping && pending) {
        next = std::move(pending);
        next->phase = job_t::phase_e::reserved;
        next->id = ++next_id;
        active_id = next->id;
        active_operation = operation_e::start;
        reserved = next;
      }
    }
    if (next) {
      try {
        next->peer->prepare([state = shared_from_this(), next](bool ready) { state->prepared(next, ready); });
      } catch (...) {
        abort(next, end_e::stopping);
      }
    }
  }

  inline void admission_t::state_t::abort(const std::shared_ptr<job_t> &job, end_e reason) {
    task_t discarded;
    std::function<void(end_e)> on_end;
    {
      std::lock_guard lock(mutex);
      if (job->phase == job_t::phase_e::pending && pending == job) {
        pending.reset();
      } else if (job->phase == job_t::phase_e::reserved && reserved == job && active_id == job->id) {
        reserved.reset();
        active_id = 0;
      } else {
        return;
      }
      job->phase = job_t::phase_e::done;
      discarded = std::move(job->task);
      on_end = std::move(job->on_end);
    }
    job->peer->stop();
    if (on_end) on_end(reason);
  }

  inline void admission_t::state_t::prepared(const std::shared_ptr<job_t> &job, bool ready) {
    if (!ready) {
      abort(job, end_e::disconnected);
      return;
    }
    {
      std::lock_guard lock(mutex);
      if (job->phase != job_t::phase_e::reserved || reserved != job || stopping) return;
    }
    try {
      job->submit([state = shared_from_this(), job] { state->run(job); });
    } catch (...) {
      abort(job, end_e::stopping);
    }
  }

  inline void admission_t::state_t::run(const std::shared_ptr<job_t> &job) {
    task_t task;
    end_e rejection = end_e::disconnected;
    bool rejected = false;
    {
      std::lock_guard lock(mutex);
      if (job->phase != job_t::phase_e::reserved || reserved != job || active_id != job->id) return;
      if (stopping) {
        rejected = true;
        rejection = end_e::stopping;
      } else if (clock_t::now() >= job->deadline) {
        rejected = true;
        rejection = end_e::expired;
      } else if (!job->peer->connected()) {
        rejected = true;
        if (clock_t::now() >= job->deadline) rejection = end_e::expired;
      } else if (clock_t::now() >= job->deadline) {
        rejected = true;
        rejection = end_e::expired;
      } else {
        job->phase = job_t::phase_e::running;
        reserved.reset();
        task = std::move(job->task);
        job->on_end = {};
      }
    }
    if (rejected) {
      abort(job, rejection);
      return;
    }
    job->peer->stop();
    lease_t lease {shared_from_this(), job->id};
    task();
  }
}  // namespace nvhttp::handoff
