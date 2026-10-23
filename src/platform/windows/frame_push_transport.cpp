#include "frame_push_transport.h"
#include "frame_push_capture_policy.h"
#include <algorithm>
#include <chrono>
#include <thread>

namespace platf::frame_push {
  namespace {
    constexpr std::uint32_t control_timeout_ms = 250;
    constexpr std::uint32_t max_frame_wait_ms = 100;
    class QueryTransport final: public vdd::ControlTransport {
    public:
      explicit QueryTransport(TransactionTransport &transport): transport_(transport) {}
      bool ioctl(std::uint32_t code, const void *input, std::size_t input_size, void *output, std::size_t output_size,
                 std::size_t &returned, std::uint32_t &error) override {
        return transport_.exchange(code, input, input_size, output, output_size, returned, error,
                                   control_timeout_ms, {});
      }
    private:
      TransactionTransport &transport_;
    };
    vdd::FrameRingClearRequest clear_request(const vdd::FrameRingRegisterRequest &r,
                                            const vdd::FrameRingReply &reply, bool cancel) {
      vdd::FrameRingClearRequest clear {};
      clear.identity = reply;
      clear.identity.client_cookie = r.client_cookie;
      clear.identity.display_generation = r.display_generation;
      clear.lease_id = r.lease_id; clear.display_id = r.display_id;
      clear.owner_capability = r.owner_capability;
      clear.flags = cancel ? 1 : 0;
      return clear;
    }
  }
  Session::Session(TransactionTransport &transport): transport_(transport) {}
  Session::~Session() { stop(); }

  void Session::cancel_unacknowledged(const std::shared_ptr<ConsumerResources> &resources,
                                     const vdd::FrameRingRegisterRequest &request) {
    const auto clear = clear_request(request, {}, true);
    // Best effort, bounded cancellation. Even if the reply is lost, the native
    // producer must retain a finite lease and reap it. Source objects remain
    // owned by any transport transaction which has not completed cancellation.
    transport_.best_effort_after_pending(vdd::kIoctlClearFrameRing, &clear, sizeof(clear),
                                         control_timeout_ms, resources);
  }

  Status Session::start(std::shared_ptr<ConsumerResources> resources) {
    stop();
    if (!resources) return Status::InvalidTarget;
    const auto request = resources->request();
    if (!vdd::valid_frame_ring_request(request)) return Status::InvalidTarget;
    QueryTransport query {transport_};
    vdd::ControlClient client {query};
    const auto capabilities = client.query_frame_transport_capabilities();
    if (!capabilities.ok()) return Status::TransportError;
    if (!policy::supports_required_capability(capabilities.value.capabilities, capabilities.value.max_ring_slots, vdd::kFrameTransportCapabilityKeyedMutexRing, vdd::kFrameRingSlotCount)) {
      return Status::Unsupported;
    }
    vdd::FrameRingReply reply {};
    std::size_t returned = 0; std::uint32_t error = 0;
    if (!transport_.exchange(vdd::kIoctlRegisterFrameRing, &request, sizeof(request), &reply, sizeof(reply),
                             returned, error, control_timeout_ms, resources)) {
      cancel_unacknowledged(resources, request);
      return Status::TransportError;
    }
    if (returned != sizeof(reply) || !vdd::valid_frame_ring_reply(reply, request)) {
      cancel_unacknowledged(resources, request);
      return Status::InvalidReply;
    }
    resources_ = std::move(resources); request_ = request; reply_ = reply; last_sequence_ = 0;
    return Status::Ready;
  }

  void Session::stop() {
    if (!resources_) return;
    // Revoke local capture first. The clear targets only this exact generation.
    auto resources = std::move(resources_);
    const auto clear = clear_request(request_, reply_, false);
    request_ = {}; reply_ = {}; last_sequence_ = 0;
    std::size_t returned = 0; std::uint32_t error = 0;
    transport_.exchange(vdd::kIoctlClearFrameRing, &clear, sizeof(clear), nullptr, 0,
                        returned, error, control_timeout_ms, resources);
  }

  Frame Session::next(const std::uint32_t timeout_ms) {
    if (!resources_) return {Status::Stopped, {}, {}};
    const auto wait = resources_->wait((std::min)(timeout_ms, max_frame_wait_ms));
    if (wait == Access::Lost) { stop(); return {Status::DeviceLost, {}, {}}; }
    std::uint32_t chosen = vdd::kFrameRingSlotCount;
    vdd::FrameSlotMetadata newest {};
    bool lost = false; bool invalid = false;
    // Four attempts, zero-timeout keyed mutex acquisition. Keep at most two
    // slots during comparison, then at most one during the private copy.
    for (std::uint32_t slot = 0; slot < vdd::kFrameRingSlotCount; ++slot) {
      const auto acquired = resources_->acquire(slot, 1, 0);
      if (acquired == Access::Timeout) continue;
      if (acquired == Access::Lost) { lost = true; break; }
      const auto metadata = resources_->metadata(slot);
      const bool valid = metadata.client_cookie == request_.client_cookie &&
        metadata.display_generation == reply_.display_generation && metadata.ring_generation == reply_.ring_generation &&
        metadata.sequence != 0 && metadata.sequence != (std::numeric_limits<std::uint64_t>::max)() &&
        metadata.acquired_qpc != 0 && metadata.reserved == 0;
      if (!valid) {
        invalid = true;
        if (!resources_->release(slot, 0)) lost = true;
        break;
      }
      if (metadata.sequence > last_sequence_ && (chosen == vdd::kFrameRingSlotCount || metadata.sequence > newest.sequence)) {
        if (chosen != vdd::kFrameRingSlotCount && !resources_->release(chosen, 0)) lost = true;
        chosen = slot; newest = metadata;
      } else if (!resources_->release(slot, 0)) lost = true;
      if (lost) break;
    }
    if (lost || invalid) {
      if (chosen != vdd::kFrameRingSlotCount) resources_->release(chosen, 0);
      stop(); return {lost ? Status::DeviceLost : Status::InvalidFrame, {}, {}};
    }
    if (chosen == vdd::kFrameRingSlotCount) return {wait == Access::Timeout ? Status::Timeout : Status::NoFrame, {}, {}};
    auto pixels = resources_->copy_to_private(chosen);
    const bool released = resources_->release(chosen, 0);
    if (!released || pixels.status == Access::Lost || (pixels.status == Access::Acquired && !pixels.pixels)) {
      stop(); return {Status::DeviceLost, {}, {}};
    }
    if (pixels.status == Access::Timeout) return {Status::NoFrame, {}, {}};
    last_sequence_ = newest.sequence;
    return {Status::Ready, newest, std::move(pixels.pixels)};
  }

  Frame wait_for_first_frame(Session &session, const std::uint32_t timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds {timeout_ms};
    while (session.active()) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        session.stop();
        return {Status::Timeout, {}, {}};
      }
      const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
      const auto wait_ms = static_cast<std::uint32_t>((std::clamp<std::int64_t>)(remaining.count(), 1, max_frame_wait_ms));
      auto frame = session.next(wait_ms);
      // Session::next() may scan the ring and copy a private frame after its
      // event wait. Do not accept a Ready result that completed past the
      // startup cutoff, even when the wait itself began before the deadline.
      if (std::chrono::steady_clock::now() >= deadline) {
        session.stop();
        return {Status::Timeout, {}, {}};
      }
      if (frame.status != Status::Timeout && frame.status != Status::NoFrame) return frame;
      if (std::chrono::steady_clock::now() >= deadline) {
        session.stop();
        return {Status::Timeout, {}, {}};
      }
      // Native event waits consume this interval. Keep the portable/test path
      // bounded too when an implementation reports timeout immediately.
      std::this_thread::sleep_for(std::chrono::milliseconds {1});
    }
    return {Status::Stopped, {}, {}};
  }
}
