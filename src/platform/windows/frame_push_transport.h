#pragma once

#include <virtual_display/driver/control_client.h>
#include <virtual_display/driver/frame_ring_lifetime.h>
#include <virtual_display/driver/frame_transport_protocol.h>
#include <memory>

namespace platf::frame_push {
  namespace vdd = virtual_display::driver;
  // A native implementation must use heap-owned OVERLAPPED/input/output/event
  // and retain keep_alive until cancellation COMPLETES, including after timeout
  // or destruction. At most one transaction may remain pending per endpoint;
  // reject new transactions while pending. The existing WindowsControlTransport
  // has a different timeout-lifetime contract and is not used by this API.
  class TransactionTransport {
  public:
    virtual ~TransactionTransport() = default;
    virtual bool exchange(std::uint32_t code, const void *input, std::size_t input_size,
                          void *output, std::size_t output_size, std::size_t &bytes_returned,
                          std::uint32_t &native_error, std::uint32_t timeout_ms,
                          std::shared_ptr<vdd::FrameRingResources> keep_alive) = 0;
    // Cleanup may need to follow a timed-out request that is still completing.
    // Simple transports keep this synchronous; native transports may queue it.
    virtual void best_effort_after_pending(std::uint32_t code, const void *input,
                                          std::size_t input_size, std::uint32_t timeout_ms,
                                          std::shared_ptr<vdd::FrameRingResources> keep_alive) {
      std::size_t returned = 0; std::uint32_t error = 0;
      (void) exchange(code, input, input_size, nullptr, 0, returned, error, timeout_ms,
                      std::move(keep_alive));
    }
  };
  enum class Access { Acquired, Timeout, Lost };
  struct PrivateCopy { Access status {Access::Timeout}; std::shared_ptr<vdd::FrameRingResources> pixels {}; };
  class ConsumerResources: public vdd::FrameRingResources {
  public:
    virtual vdd::FrameRingRegisterRequest request() const = 0;
    virtual Access wait(std::uint32_t timeout_ms) = 0;
    virtual Access acquire(std::uint32_t slot, std::uint64_t key, std::uint32_t timeout_ms) = 0;
    virtual vdd::FrameSlotMetadata metadata(std::uint32_t slot) const = 0;
    // Enqueue a copy into an independently retained private texture on the same
    // immediate context before ReleaseSync(0). Returning an alias of the ring is
    // forbidden. The returned resource owns its GPU-work lifetime/pool lease.
    virtual PrivateCopy copy_to_private(std::uint32_t slot) = 0;
    virtual bool release(std::uint32_t slot, std::uint64_t key) = 0;
  };
  enum class Status { Ready, Unsupported, InvalidTarget, TransportError, InvalidReply,
                      NoFrame, Timeout, InvalidFrame, DeviceLost, Stopped };
  struct Frame {
    Status status {Status::NoFrame};
    vdd::FrameSlotMetadata metadata {};
    std::shared_ptr<vdd::FrameRingResources> pixels {};
  };
  // One serial capture caller. No capture selector/config flag or default is
  // changed. Native backend activation remains gated on driver qualification.
  class Session {
  public:
    explicit Session(TransactionTransport &transport);
    ~Session();
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;
    Status start(std::shared_ptr<ConsumerResources> resources);
    Frame next(std::uint32_t timeout_ms);
    void stop();
    bool active() const { return resources_ != nullptr; }
  private:
    void cancel_unacknowledged(const std::shared_ptr<ConsumerResources> &resources,
                               const vdd::FrameRingRegisterRequest &request);
    TransactionTransport &transport_;
    std::shared_ptr<ConsumerResources> resources_;
    vdd::FrameRingRegisterRequest request_ {};
    vdd::FrameRingReply reply_ {};
    std::uint64_t last_sequence_ {};
  };

  // Registration is not readiness: wait for one validated, private frame before
  // allowing the display selector to return this adapter as usable.
  Frame wait_for_first_frame(Session &session, std::uint32_t timeout_ms);
}
