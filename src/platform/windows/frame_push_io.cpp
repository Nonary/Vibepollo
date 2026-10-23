#include "frame_push_io.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

namespace platf::frame_push {
  namespace {
    std::atomic<unsigned> unresolved {0};
    constexpr unsigned max_unresolved = 8;
    constexpr std::size_t max_control_bytes = 256;
    constexpr std::uint32_t error_busy = 170, error_invalid_parameter = 87, error_timeout = 1460;
  }
  std::shared_ptr<PendingTransaction> PendingTransaction::create() {
    auto count = unresolved.load(std::memory_order_relaxed);
    do { if (count >= max_unresolved) return {}; }
    while (!unresolved.compare_exchange_weak(count, count + 1, std::memory_order_relaxed));
    PendingTransaction *transaction = nullptr;
    try { transaction = new PendingTransaction; }
    catch (...) { unresolved.fetch_sub(1, std::memory_order_relaxed); throw; }
    // shared_ptr allocation failure deletes transaction, whose destructor owns
    // the permit release; do not decrement it a second time in a catch block.
    return std::shared_ptr<PendingTransaction>(transaction);
  }
  PendingTransaction::~PendingTransaction() { release_permit(); }
  void PendingTransaction::release_permit() {
    if (permit_.exchange(false, std::memory_order_relaxed))
      unresolved.fetch_sub(1, std::memory_order_relaxed);
  }
  void PendingTransaction::complete(std::uint32_t error, std::size_t bytes) {
    std::vector<std::function<void()>> callbacks;
    std::shared_ptr<vdd::FrameRingResources> completed_resources;
    {
      std::lock_guard lock {mutex_};
      if (completed_) return;
      error_ = error; bytes_ = bytes; completed_ = true;
      callbacks = std::move(callbacks_);
      completed_resources = std::move(resources);
    }
    release_permit();
    (void) completed_resources; // Keep the source alive through all completion continuations.
    completion_.notify_all();
    for (auto &callback: callbacks) {
      try { callback(); } catch (...) { /* async cleanup must not escape completion */ }
    }
  }
  void PendingTransaction::on_complete(std::function<void()> callback) {
    if (!callback) return;
    bool invoke = false;
    {
      std::lock_guard lock {mutex_};
      if (completed_) invoke = true;
      else callbacks_.push_back(std::move(callback));
    }
    if (invoke) {
      try { callback(); } catch (...) { /* async cleanup must not escape completion */ }
    }
  }
  bool PendingTransaction::wait(std::uint32_t timeout_ms) {
    std::unique_lock lock {mutex_};
    return completion_.wait_for(lock, std::chrono::milliseconds {timeout_ms}, [this] { return completed_; });
  }
  std::pair<std::uint32_t, std::size_t> PendingTransaction::result() const {
    std::lock_guard lock {mutex_}; return {error_, bytes_};
  }
  BoundedTransactionTransport::BoundedTransactionTransport(std::shared_ptr<AsyncIo> io): io_(std::move(io)) {}
  struct BoundedTransactionTransport::DeferredCleanup {
    std::atomic<bool> finished {false};
    std::mutex mutex;
    std::shared_ptr<PendingTransaction> transaction;
  };
  BoundedTransactionTransport::~BoundedTransactionTransport() {
    if (pending_ && io_) io_->cancel(pending_);
    if (deferred_cleanup_ && io_) {
      std::shared_ptr<PendingTransaction> transaction;
      { std::lock_guard lock {deferred_cleanup_->mutex}; transaction = deferred_cleanup_->transaction; }
      if (transaction) io_->cancel(transaction);
    }
  }
  bool BoundedTransactionTransport::exchange(
    std::uint32_t code, const void *input, std::size_t input_size, void *output, std::size_t output_size,
    std::size_t &returned, std::uint32_t &error, std::uint32_t timeout_ms,
    std::shared_ptr<vdd::FrameRingResources> resources) {
    returned = 0; error = 0;
    if (!io_ || input_size > max_control_bytes || output_size > max_control_bytes ||
        (input_size && !input) || (output_size && !output)) { error = error_invalid_parameter; return false; }
    if (deferred_cleanup_) {
      if (!deferred_cleanup_->finished.load(std::memory_order_acquire)) { error = error_busy; return false; }
      deferred_cleanup_.reset();
    }
    if (pending_) {
      if (!pending_->wait(0)) { error = error_busy; return false; }
      pending_.reset();
    }
    pending_ = PendingTransaction::create();
    if (!pending_) { error = error_busy; return false; }
    pending_->code = code;
    pending_->input.resize(input_size); pending_->output.resize(output_size);
    if (input_size) std::memcpy(pending_->input.data(), input, input_size);
    pending_->resources = std::move(resources);
    if (!io_->begin(pending_, error)) { pending_.reset(); return false; }
    if (!pending_->wait((std::min)(timeout_ms, 250u))) {
      io_->cancel(pending_); error = error_timeout;
      return false; // native completion retains heap buffers and resource hold
    }
    const auto [completion_error, bytes] = pending_->result();
    if (completion_error || bytes > output_size) {
      error = completion_error ? completion_error : error_invalid_parameter;
      pending_.reset(); return false;
    }
    if (bytes) std::memcpy(output, pending_->output.data(), bytes);
    returned = bytes; pending_.reset(); return true;
  }

  void BoundedTransactionTransport::best_effort_after_pending(
    std::uint32_t code, const void *input, std::size_t input_size, std::uint32_t timeout_ms,
    std::shared_ptr<vdd::FrameRingResources> resources) {
    if (!io_ || input_size > max_control_bytes || (input_size && !input)) return;
    if (pending_ && !pending_->wait(0)) {
      if (deferred_cleanup_ && !deferred_cleanup_->finished.load(std::memory_order_acquire)) return;
      std::shared_ptr<DeferredCleanup> deferred;
      try {
        deferred = std::make_shared<DeferredCleanup>();
        std::vector<std::byte> owned_input(input_size);
        if (input_size) std::memcpy(owned_input.data(), input, input_size);
        auto io = io_;
        const auto cleanup_timeout_ms = timeout_ms > 250u ? 250u : timeout_ms;
        auto start_cleanup = [deferred, io, code, cleanup_timeout_ms, input = std::move(owned_input),
                              resources = std::move(resources)]() mutable {
          try {
            auto transaction = PendingTransaction::create();
            if (!transaction) {
              deferred->finished.store(true, std::memory_order_release);
              return;
            }
            transaction->code = code;
            transaction->input = std::move(input);
            transaction->resources = std::move(resources);
            {
              std::lock_guard lock {deferred->mutex};
              deferred->transaction = transaction;
            }
            std::weak_ptr<DeferredCleanup> weak_deferred = deferred;
            transaction->on_complete([weak_deferred] {
              if (auto state = weak_deferred.lock()) {
                std::lock_guard lock {state->mutex};
                state->transaction.reset();
                state->finished.store(true, std::memory_order_release);
              }
            });
            std::uint32_t error = 0;
            if (!io->begin(transaction, error)) {
              transaction->complete(error, 0);
              return;
            }
            // This continuation runs on the earlier operation's completion
            // callback. Watch the queued cleanup on a short-lived worker so a
            // hung clear is cancelled without blocking that callback thread.
            if (!transaction->wait(0)) {
              try {
                std::thread([io, transaction, cleanup_timeout_ms] {
                  if (!transaction->wait(cleanup_timeout_ms)) {
                    try { io->cancel(transaction); } catch (...) { }
                  }
                }).detach();
              } catch (...) {
                try { io->cancel(transaction); } catch (...) { }
              }
            }
          } catch (...) {
            std::lock_guard lock {deferred->mutex};
            deferred->transaction.reset();
            deferred->finished.store(true, std::memory_order_release);
          }
        };
        deferred_cleanup_ = deferred;
        try { pending_->on_complete(std::move(start_cleanup)); }
        catch (...) {
          deferred_cleanup_.reset();
          deferred->finished.store(true, std::memory_order_release);
        }
      } catch (...) {
        if (deferred) deferred->finished.store(true, std::memory_order_release);
      }
      return;
    }
    std::size_t returned = 0; std::uint32_t error = 0;
    (void) exchange(code, input, input_size, nullptr, 0, returned, error, timeout_ms,
                    std::move(resources));
  }
}
