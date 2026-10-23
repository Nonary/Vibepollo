#pragma once

#include "src/display_target_lease.h"
#include "src/platform/windows/display_helper_borrow_gate.h"

#include <atomic>
#include <cstdlib>
#include <memory>
#include <vector>

namespace display_helper::borrow {
  // Registered only after the real IPC acquire has initialized its statics.
  // It therefore runs before IPC/core static destruction. Late lease owners
  // retain the kernel/resource protection until OS process death and never
  // call an already destroyed session singleton during CRT teardown.
  inline std::atomic<bool> &process_teardown_started() {
    static auto *value = new std::atomic<bool> {false};
    return *value;
  }
  inline bool register_process_teardown_guard() {
    static const bool registered = [] {
      (void) process_teardown_started();
      return std::atexit([] { process_teardown_started().store(true, std::memory_order_release); }) == 0;
    }();
    return registered;
  }


  struct acquire_outcome_t {
    std::optional<result_t> reply;
    bool completion_unknown = false;
  };

  struct consumer_ports_t {
    std::function<process_context_t()> context;
    std::function<acquire_outcome_t(const request_t &, std::chrono::steady_clock::time_point)> acquire;
    std::function<bool(const token_t &, std::chrono::steady_clock::time_point)> release;
    std::function<std::optional<std::shared_ptr<void>>(const result_t &)> pin_native_resource;
    std::function<std::shared_ptr<void>(const result_t &, std::chrono::steady_clock::time_point)> inhibit_kernel;
    std::function<std::optional<bool>(const process_context_t &)> alive;
    std::function<std::optional<display_target::observation_t>(const display_target::descriptor_t &)> observe;
    std::function<void()> fence_unknown;
  };

  // Actual host admission/lifetime algorithm; Windows supplies only native
  // process, handle, IPC transport and read-only observation leaves.
  class consumer_t {
    struct retained_native_t {
      std::shared_ptr<void> kernel, resource;
    };
    static void retain_unknown_native(std::shared_ptr<void> kernel, std::shared_ptr<void> resource) {
      // Intentionally process-lifetime retained after unknown RELEASE. Static
      // destruction must not unblock helper work during host teardown either.
      static auto *mutex = new std::mutex;
      static auto *retained = new std::vector<retained_native_t>;
      std::lock_guard lock(*mutex);
      retained->push_back({std::move(kernel), std::move(resource)});
    }
    struct context_hold_t {
      gate_t *gate;
      consumer_ports_t ports;
      request_t request;
      token_t token;
      std::shared_ptr<void> kernel, resource;
      context_hold_t(gate_t &value, consumer_ports_t input, request_t acquired, token_t acknowledged)
        : gate(&value), ports(std::move(input)), request(acquired), token(acknowledged) {}
      context_hold_t(const context_hold_t &) = delete;
      context_hold_t &operator=(const context_hold_t &) = delete;
      ~context_hold_t() noexcept {
        if (process_teardown_started().load(std::memory_order_acquire)) {
          // The local request stays inhibited. Do not access any other
          // singleton in this path; retain only the independent native hold.
          retain_unknown_native(std::move(kernel), std::move(resource));
          return;
        }
        bool released = false;
        try { released = ports.release(token, std::chrono::steady_clock::now() + std::chrono::milliseconds(750)); }
        catch (...) {}
        if (!released) {
          gate->retain_unknown();
          try { ports.fence_unknown(); } catch (...) {}
          retain_unknown_native(std::move(kernel), std::move(resource));
          return;
        }
        // All role/pending/capture owners are gone before this destructor.
        // Keep the host/core borrow fence closed through resource close and
        // synchronous kernel-owner-thread release, then reopen native writes.
        resource.reset();
        kernel.reset();
        gate->release(request);
      }
    };
  public:
    consumer_t(gate_t &gate, consumer_ports_t ports) : gate_(gate), ports_(std::move(ports)) {}
    std::optional<display_target::observation_t> observe(const display_target::descriptor_t &target) {
      if (process_teardown_started().load(std::memory_order_acquire)) return std::nullopt;
      std::lock_guard serial(mutex_);
      if (gate_.permanently_inhibited()) return std::nullopt;
      auto hold = current_.lock();
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
      if (hold) {
        const auto alive = ports_.alive(hold->token.helper);
        if (!alive || !*alive) { unknown(); return std::nullopt; }
      } else {
        const request_t request {ports_.context(), next_id_.fetch_add(1, std::memory_order_relaxed)};
        if (!request.valid() || !gate_.acquire(request, deadline)) return std::nullopt;
        acquire_outcome_t outcome;
        try { outcome = ports_.acquire(request, deadline); } catch (...) { outcome.completion_unknown = true; }
        if (!outcome.reply || !outcome.reply->success || !outcome.reply->token.valid() || outcome.reply->token.request != request) {
          if (outcome.completion_unknown) unknown();
          else gate_.release(request);
          return std::nullopt;
        }
        // Install RAII immediately after the exact ACK, including all native
        // failure paths before publication. Failure must retire that token.
        try { hold = std::make_shared<context_hold_t>(gate_, ports_, request, outcome.reply->token); }
        catch (...) { unknown(); return std::nullopt; }
        const auto pin = ports_.pin_native_resource(*outcome.reply);
        if (!pin) return std::nullopt;
        hold->resource = *pin;
        hold->kernel = ports_.inhibit_kernel(*outcome.reply, deadline);
        if (!hold->kernel) return std::nullopt;
        if (!register_process_teardown_guard()) { unknown(); return std::nullopt; }
        current_ = hold;
      }
      const auto observed = ports_.observe(target);
      const auto alive = ports_.alive(hold->token.helper);
      if (!observed || !alive || !*alive || process_teardown_started().load(std::memory_order_acquire)) {
        if (!alive || !*alive) unknown();
        return std::nullopt;
      }
      auto result = *observed;
      result.runtime_hold = std::move(hold);
      return result;
    }
  private:
    void unknown() noexcept { gate_.retain_unknown(); try { ports_.fence_unknown(); } catch (...) {} }
    gate_t &gate_;
    consumer_ports_t ports_;
    std::mutex mutex_;
    std::weak_ptr<context_hold_t> current_;
    std::atomic<std::uint64_t> next_id_ {1};
  };
}
