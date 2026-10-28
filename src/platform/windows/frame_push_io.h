#pragma once
#include "frame_push_transport.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <vector>
#include <utility>

namespace platf::frame_push {
  class PendingTransaction {
  public:
    ~PendingTransaction();
    static std::shared_ptr<PendingTransaction> create();
    void complete(std::uint32_t error, std::size_t bytes);
    void on_complete(std::function<void()> callback);
    bool wait(std::uint32_t timeout_ms);
    std::pair<std::uint32_t, std::size_t> result() const;
    std::uint32_t code {};
    std::vector<std::byte> input, output;
    std::shared_ptr<vdd::FrameRingResources> resources;
  private:
    PendingTransaction() = default;
    mutable std::mutex mutex_;
    std::condition_variable completion_;
    bool completed_ {};
    std::uint32_t error_ {};
    std::size_t bytes_ {};
    std::vector<std::function<void()>> callbacks_;
    std::atomic<bool> permit_ {true};
    void release_permit();
  };
  class AsyncIo {
  public:
    virtual ~AsyncIo() = default;
    // Success transfers an additional shared_ptr hold to the native operation
    // until completion; begin failure must not leave any asynchronous access.
    virtual bool begin(std::shared_ptr<PendingTransaction> transaction, std::uint32_t &error) = 0;
    virtual void cancel(const std::shared_ptr<PendingTransaction> &transaction) = 0;
  };
  // Production portable transport state machine. One serial control caller,
  // one unresolved operation per endpoint, and eight globally. Timeout never
  // frees input/output/OVERLAPPED-owned resources still referenced by native IO.
  class BoundedTransactionTransport final: public TransactionTransport {
  public:
    explicit BoundedTransactionTransport(std::shared_ptr<AsyncIo> io);
    ~BoundedTransactionTransport() override;
    bool exchange(std::uint32_t code, const void *input, std::size_t input_size,
                  void *output, std::size_t output_size, std::size_t &bytes_returned,
                  std::uint32_t &native_error, std::uint32_t timeout_ms,
                  std::shared_ptr<vdd::FrameRingResources> keep_alive) override;
    void best_effort_after_pending(std::uint32_t code, const void *input,
                                   std::size_t input_size, std::uint32_t timeout_ms,
                                   std::shared_ptr<vdd::FrameRingResources> keep_alive) override;
  private:
    struct DeferredCleanup;
    std::shared_ptr<AsyncIo> io_;
    std::shared_ptr<PendingTransaction> pending_;
    std::shared_ptr<DeferredCleanup> deferred_cleanup_;
  };
}
