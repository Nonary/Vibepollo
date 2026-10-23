#include <gtest/gtest.h>
#include "../../src/platform/windows/frame_push_transport.h"
#include "../../src/platform/windows/frame_push_capture_policy.h"
#include <virtual_display/driver/frame_ring_server.h>
#include <virtual_display/driver/ioctl_dispatcher.h>
#include <chrono>
#include <cstring>
#include <optional>
#include <thread>
#include <vector>

namespace fp = platf::frame_push;
namespace vdd = virtual_display::driver;
namespace {
  class LateRegistrationIo;
  struct Bundle: vdd::FrameRingResources {
    explicit Bundle(std::shared_ptr<int> count): count(std::move(count)) { ++*this->count; }
    ~Bundle() override { --*count; }
    std::shared_ptr<int> count;
  };
  class Importer: public vdd::FrameResourceImporter {
  public:
    std::shared_ptr<vdd::FrameRingResources> import(const vdd::FrameRequestPeer &p,
                                                  const vdd::FrameRingRegisterRequest &r) override {
      ++calls; peer = p; request = r;
      return fail ? nullptr : std::make_shared<Bundle>(alive);
    }
    int calls {}; bool fail {};
    std::shared_ptr<int> alive = std::make_shared<int>(0);
    vdd::FrameRequestPeer peer {};
    vdd::FrameRingRegisterRequest request {};
  };
  class Backend: public vdd::DisplayDriverBackend {
  public:
    vdd::BackendDisplayResult arrive_temporary_display(const vdd::DisplayDescriptor &) override { return {}; }
    vdd::BackendError depart_temporary_display(std::uint64_t, std::uint64_t) override { return vdd::BackendError::None; }
    vdd::BackendError set_permanent_display_count(const vdd::PermanentDisplayCountRequest &) override { return vdd::BackendError::None; }
    vdd::BackendError set_render_adapter(const vdd::SetRenderAdapterRequest &) override { return vdd::BackendError::None; }
  };
  class Resources: public fp::ConsumerResources {
  public:
    explicit Resources(vdd::FrameRingRegisterRequest request): request_(request) {}
    vdd::FrameRingRegisterRequest request() const override { return request_; }
    fp::Access wait(std::uint32_t timeout) override { wait_timeout = timeout; return wait_result; }
    fp::Access acquire(std::uint32_t slot, std::uint64_t key, std::uint32_t timeout) override {
      ++attempts;
      EXPECT_EQ(key, 1u); EXPECT_EQ(timeout, 0u);
      if (lost_slot == slot) return fp::Access::Lost;
      if (held[slot] || keys[slot] != key) return fp::Access::Timeout;
      held[slot] = true; max_held = (std::max)(max_held, ++held_count);
      return fp::Access::Acquired;
    }
    vdd::FrameSlotMetadata metadata(std::uint32_t slot) const override {
      EXPECT_TRUE(held[slot]); return slots[slot];
    }
    fp::PrivateCopy copy_to_private(std::uint32_t slot) override {
      EXPECT_TRUE(held[slot]); ++copies; copied_slot = slot;
      if (copy_delay.count() > 0) std::this_thread::sleep_for(copy_delay);
      if (copy_result != fp::Access::Acquired) return {copy_result, {}};
      return {fp::Access::Acquired, std::make_shared<Bundle>(outputs)};
    }
    bool release(std::uint32_t slot, std::uint64_t key) override {
      EXPECT_TRUE(held[slot]); EXPECT_EQ(key, 0u);
      held[slot] = false; --held_count; keys[slot] = key;
      return !release_fail;
    }
    void publish(std::uint32_t slot, const vdd::FrameRingReply &reply, std::uint64_t sequence) {
      ASSERT_FALSE(held[slot]);
      slots[slot] = {reply.client_cookie, reply.display_generation, reply.ring_generation, sequence, 1234, 0};
      keys[slot] = 1;
    }
    vdd::FrameRingRegisterRequest request_;
    std::array<vdd::FrameSlotMetadata, 4> slots {};
    std::array<std::uint64_t, 4> keys {};
    std::array<bool, 4> held {};
    std::uint32_t lost_slot {99}, wait_timeout {}, copied_slot {};
    int attempts {}, max_held {}, held_count {}, copies {};
    bool release_fail {};
    fp::Access wait_result {fp::Access::Acquired}, copy_result {fp::Access::Acquired};
    std::chrono::milliseconds copy_delay {};
    std::shared_ptr<int> outputs = std::make_shared<int>(0);
  };

  class Fixture: public testing::Test {
    friend class LateRegistrationIo;
  protected:
    std::chrono::steady_clock::time_point now {std::chrono::seconds {100}};
    Backend backend;
    vdd::DriverController controller {vdd::DisplayStore {2, 2}, backend};
    vdd::IoctlDispatcher dispatcher {controller};
    // Use one real owner store for authorization and a real production
    // dispatcher for version/capability discovery. No native device is opened.
    vdd::DisplayStore store {2, 2};
    vdd::FrameRingLifetime rings {store};
    Importer importer;
    vdd::FrameRingServer server {store, rings, importer};
    vdd::FrameRequestPeer peer {200, 900};
    vdd::FrameRingRegisterRequest request;
    void SetUp() override {
      vdd::CreateTemporaryDisplayOwnedRequest r {};
      r.display.lease_id = vdd::kMinOpaqueLeaseId | 10;
      r.display.display_id = 100;
      r.display.width = 1920; r.display.height = 1080;
      r.display.physical_width_mm = 530; r.display.physical_height_mm = 300;
      r.display.refresh_rate_millihz = 60'000; r.display.requested_timeout_ms = 30'000;
      std::strcpy(r.display.display_name, "Owned display");
      r.owner_capability.bytes.fill(42);
      ASSERT_EQ(store.create_temporary_display_owned(r, now).status.error, vdd::StoreError::None);
      request.lease_id = r.display.lease_id; request.display_id = r.display.display_id;
      request.display_generation = store.find_temporary_display(r.display.display_id)->generation;
      request.client_cookie = 10; request.owner_capability = r.owner_capability;
      request.width = 1920; request.height = 1080; request.format = 87; request.color_space = 0;
      request.texture_handles = {10, 11, 12, 13}; request.metadata_handle = 14; request.event_handle = 15;
    }
    vdd::FrameRingClearRequest clear(const vdd::FrameRingReply &reply, bool cancel = false) {
      vdd::FrameRingClearRequest c {};
      c.identity = reply; c.lease_id = request.lease_id; c.display_id = request.display_id;
      c.owner_capability = request.owner_capability; c.flags = cancel ? 1 : 0;
      return c;
    }
    class Wire: public fp::TransactionTransport {
    public:
      explicit Wire(Fixture &f): f(f) {}
      bool exchange(std::uint32_t code, const void *in, std::size_t in_size,
                    void *out, std::size_t out_size, std::size_t &returned,
                    std::uint32_t &error, std::uint32_t timeout,
                    std::shared_ptr<vdd::FrameRingResources> retained) override {
        calls.push_back(code); EXPECT_EQ(timeout, 250u); returned = 0; error = 0;
        if (code == vdd::kIoctlGetProtocolVersion && legacy) {
          auto v = vdd::ProtocolVersion {}; v.minor = 7;
          std::memcpy(out, &v, sizeof(v)); returned = sizeof(v); return true;
        }
        if (code == vdd::kIoctlQueryFrameTransportCapabilities && capable) {
          auto c = vdd::FrameTransportCapabilities {};
          c.capabilities = vdd::kFrameTransportCapabilityKeyedMutexRing; c.max_ring_slots = 4;
          if (bad_capability) ++c.version;
          std::memcpy(out, &c, sizeof(c)); returned = sizeof(c); return true;
        }
        if (code == vdd::kIoctlRegisterFrameRing) {
          EXPECT_EQ(in_size, sizeof(vdd::FrameRingRegisterRequest)); EXPECT_TRUE(retained);
          const auto r = *static_cast<const vdd::FrameRingRegisterRequest *>(in);
          if (timeout_before) {
            pending = r; pending_resources = std::move(retained); error = 1460; return false;
          }
          const auto result = f.server.register_ring(request_peer, r, f.now);
          last_reply = result.reply;
          if (timeout_after) { error = 1460; return false; }
          if (result.status != vdd::FrameWireStatus::Success) return false;
          std::memcpy(out, &result.reply, sizeof(result.reply)); returned = sizeof(result.reply);
          if (bad_reply) static_cast<vdd::FrameRingReply *>(out)->display_generation++;
          return true;
        }
        if (code == vdd::kIoctlClearFrameRing) {
          EXPECT_EQ(in_size, sizeof(vdd::FrameRingClearRequest)); EXPECT_TRUE(retained);
          return f.server.clear_ring(request_peer, *static_cast<const vdd::FrameRingClearRequest *>(in), f.now)
              == vdd::FrameWireStatus::Success;
        }
        const auto result = f.dispatcher.dispatch(code, in, in_size, out, out_size, f.now);
        returned = result.bytes_returned;
        return result.status == vdd::IoctlStatus::Success;
      }
      vdd::FrameWireStatus complete_delayed() {
        const auto status = f.server.register_ring(request_peer, *pending, f.now).status;
        pending.reset(); pending_resources.reset(); return status;
      }
      Fixture &f; bool capable {true}, legacy {}, bad_capability {}, bad_reply {}, timeout_before {}, timeout_after {};
      vdd::FrameRequestPeer request_peer {200, 900};
      std::vector<std::uint32_t> calls;
      vdd::FrameRingReply last_reply {};
      std::optional<vdd::FrameRingRegisterRequest> pending;
      std::shared_ptr<vdd::FrameRingResources> pending_resources;
    };
  };
}

TEST(FramePushCapturePolicy, RequiresTheExactUserOptInAndPreservesAutomaticCapture) {
  EXPECT_FALSE(fp::policy::requested(""));
  EXPECT_FALSE(fp::policy::requested("wgc"));
  EXPECT_FALSE(fp::policy::requested("ddx"));
  EXPECT_FALSE(fp::policy::requested("IDD-PUSH"));
  EXPECT_TRUE(fp::policy::requested("idd-push"));
  EXPECT_EQ(fp::policy::select(false, false, false, false, false), fp::policy::Selection::NotRequested);
  EXPECT_FALSE(fp::policy::use_wgc_fallback(fp::policy::Selection::NotRequested));
}

TEST(FramePushCapturePolicy, ExplicitRequestFallsBackWhenBuildSupportIsOff) {
  const auto selection = fp::policy::select(true, false, true, true, true);
  EXPECT_EQ(selection, fp::policy::Selection::BuildDisabled);
  EXPECT_TRUE(fp::policy::use_wgc_fallback(selection));
}

TEST(FramePushCapturePolicy, RejectsPhysicalAndOtherVirtualDisplayTargets) {
  const auto selection = fp::policy::select(true, true, false, true, true);
  EXPECT_EQ(selection, fp::policy::Selection::WrongDisplay);
  EXPECT_TRUE(fp::policy::use_wgc_fallback(selection));
}

TEST(FramePushCapturePolicy, RequiresExactKeyedMutexRingCapabilityAndSlotCount) {
  EXPECT_FALSE(fp::policy::supports_required_capability(0, vdd::kFrameRingSlotCount, vdd::kFrameTransportCapabilityKeyedMutexRing, vdd::kFrameRingSlotCount));
  EXPECT_FALSE(fp::policy::supports_required_capability(1ull << 63, vdd::kFrameRingSlotCount, vdd::kFrameTransportCapabilityKeyedMutexRing, vdd::kFrameRingSlotCount));
  EXPECT_FALSE(fp::policy::supports_required_capability(vdd::kFrameTransportCapabilityKeyedMutexRing, vdd::kFrameRingSlotCount - 1, vdd::kFrameTransportCapabilityKeyedMutexRing, vdd::kFrameRingSlotCount));
  EXPECT_TRUE(fp::policy::supports_required_capability(vdd::kFrameTransportCapabilityKeyedMutexRing, vdd::kFrameRingSlotCount, vdd::kFrameTransportCapabilityKeyedMutexRing, vdd::kFrameRingSlotCount));
  const auto selection = fp::policy::select(true, true, true, false, true);
  EXPECT_EQ(selection, fp::policy::Selection::DriverUnsupported);
  EXPECT_TRUE(fp::policy::use_wgc_fallback(selection));
}

TEST(FramePushCapturePolicy, BindsTheReportedLiveTargetToExactModeAndAdapter) {
  EXPECT_TRUE(fp::policy::target_matches_capture_output(2560, 1440, 7, 0, 2560, 1440, 7, 0));
  EXPECT_FALSE(fp::policy::target_matches_capture_output(1920, 1080, 7, 0, 2560, 1440, 7, 0));
  EXPECT_FALSE(fp::policy::target_matches_capture_output(2560, 1440, 8, 0, 2560, 1440, 7, 0));
  EXPECT_FALSE(fp::policy::target_matches_capture_output(2560, 1440, 0, 0, 2560, 1440, 0, 0));
  EXPECT_FALSE(fp::policy::target_matches_capture_output(0, 1440, 7, 0, 0, 1440, 7, 0));
}

TEST(FramePushCapturePolicy, RejectsColorModesWithoutVerifiedSdrP709Metadata) {
  EXPECT_TRUE(fp::policy::supports_capture_color_mode(87, 0, true, 0));
  EXPECT_FALSE(fp::policy::supports_capture_color_mode(87, 0, false, 0));
  EXPECT_FALSE(fp::policy::supports_capture_color_mode(87, 0, true, 12));
  EXPECT_FALSE(fp::policy::supports_capture_color_mode(10, 1, true, 1));
  EXPECT_FALSE(fp::policy::supports_capture_color_mode(87, 1, true, 0));
}

TEST(FramePushCapturePolicy, CapabilityAloneDoesNotBypassAnUnstartedCaptureSession) {
  const auto selection = fp::policy::select(true, true, true, true, false);
  EXPECT_EQ(selection, fp::policy::Selection::HostBackendUnavailable);
  EXPECT_TRUE(fp::policy::use_wgc_fallback(selection));

  const auto ready = fp::policy::select(true, true, true, true, true);
  EXPECT_EQ(ready, fp::policy::Selection::Ready);
  EXPECT_FALSE(fp::policy::use_wgc_fallback(ready));
}

TEST(FramePushCapturePolicy, RequiresAStartedAdapterBeforeTheSelectorCanBeReady) {
  const auto capability_only = fp::policy::select(true, true, true, true, false);
  EXPECT_EQ(capability_only, fp::policy::Selection::HostBackendUnavailable);
  EXPECT_TRUE(fp::policy::use_wgc_fallback(capability_only));

  const auto initialized = fp::policy::select(true, true, true, true, true);
  EXPECT_EQ(initialized, fp::policy::Selection::Ready);
  EXPECT_FALSE(fp::policy::use_wgc_fallback(initialized));

  for (const auto unavailable: {
         fp::policy::select(true, false, true, true, true),
         fp::policy::select(true, true, false, true, true),
         fp::policy::select(true, true, true, false, true),
         fp::policy::select(true, true, true, true, false)}) {
    EXPECT_NE(unavailable, fp::policy::Selection::Ready);
    EXPECT_TRUE(fp::policy::use_wgc_fallback(unavailable));
  }
}

TEST(FramePushCapturePolicy, ReinitializesOnlyAfterTheProducerStallBoundary) {
  EXPECT_FALSE(fp::policy::producer_stalled(std::chrono::milliseconds {1999}));
  EXPECT_TRUE(fp::policy::producer_stalled(std::chrono::milliseconds {2000}));
  EXPECT_TRUE(fp::policy::producer_stalled(std::chrono::seconds {3}));
}

TEST_F(Fixture, WireRejectsMalformedRequestsAndObjectAliasesBeforeImport) {
  for (int mutation = 0; mutation < 11; ++mutation) {
    auto r = request;
    switch (mutation) {
      case 0: r.api_namespace = {}; break; case 1: ++r.version; break;
      case 2: --r.size; break; case 3: r.slot_count = 3; break;
      case 4: r.metadata_bytes = 2048; break; case 5: r.width = 16385; break;
      case 6: r.texture_handles[0] = 0; break; case 7: r.metadata_handle = r.event_handle; break;
      case 8: r.format = 24; break; case 9: r.color_space = 1; break; case 10: r.reserved = 1; break;
    }
    EXPECT_EQ(server.register_ring(peer, r, now).status, vdd::FrameWireStatus::InvalidInput);
  }
  EXPECT_EQ(importer.calls, 0); EXPECT_EQ(server.size(), 0u);
}
TEST_F(Fixture, OwnerAndDisplayGenerationAreCheckedBeforeDuplication) {
  for (int mutation = 0; mutation < 4; ++mutation) {
    auto r = request;
    switch (mutation) {
      case 0: r.owner_capability.bytes[2] ^= 1; break; case 1: ++r.lease_id; break;
      case 2: ++r.display_generation; break; case 3: ++r.display_id; break;
    }
    EXPECT_EQ(server.register_ring(peer, r, now).status, vdd::FrameWireStatus::Unauthorized);
  }
  EXPECT_EQ(server.register_ring({}, request, now).status, vdd::FrameWireStatus::InvalidInput);
  EXPECT_EQ(importer.calls, 0);
}
TEST_F(Fixture, ExactRetryIsIdempotentAndProcessReuseCannotReplaceOrClear) {
  const auto first = server.register_ring(peer, request, now);
  ASSERT_EQ(first.status, vdd::FrameWireStatus::Success);
  const auto retry = server.register_ring(peer, request, now);
  EXPECT_EQ(retry.reply.ring_generation, first.reply.ring_generation); EXPECT_EQ(importer.calls, 1);
  auto newer = request; ++newer.client_cookie;
  for (auto wrong: {vdd::FrameRequestPeer {201, 900}, vdd::FrameRequestPeer {200, 901}}) {
    EXPECT_EQ(server.register_ring(wrong, newer, now).status, vdd::FrameWireStatus::Unauthorized);
    EXPECT_EQ(server.clear_ring(wrong, clear(first.reply), now), vdd::FrameWireStatus::Unauthorized);
  }
  EXPECT_EQ(importer.calls, 1); EXPECT_EQ(*importer.alive, 1);
}
TEST_F(Fixture, StaleClearAndCancellationNeverRetireAReplacement) {
  const auto first = server.register_ring(peer, request, now);
  ++request.client_cookie;
  const auto second = server.register_ring(peer, request, now);
  ASSERT_EQ(second.status, vdd::FrameWireStatus::Success);
  EXPECT_EQ(server.clear_ring(peer, clear(first.reply), now), vdd::FrameWireStatus::StaleGeneration);
  auto cancel = clear(first.reply, true); cancel.identity.ring_generation = 0;
  EXPECT_EQ(server.clear_ring(peer, cancel, now), vdd::FrameWireStatus::StaleGeneration);
  EXPECT_TRUE(rings.snapshot(request.display_id, request.display_generation, now).can_publish());
  EXPECT_EQ(*importer.alive, 1);
}
TEST_F(Fixture, CancelBeforeRegisterAndAfterClearLeavesBoundedTombstone) {
  vdd::FrameRingReply identity {}; identity.client_cookie = request.client_cookie;
  identity.display_generation = request.display_generation;
  ASSERT_EQ(server.clear_ring(peer, clear(identity, true), now), vdd::FrameWireStatus::Success);
  EXPECT_EQ(server.register_ring(peer, request, now).status, vdd::FrameWireStatus::StaleGeneration);
  EXPECT_EQ(importer.calls, 0); EXPECT_EQ(server.size(), 1u);
  ++request.client_cookie;
  const auto next = server.register_ring(peer, request, now);
  ASSERT_EQ(next.status, vdd::FrameWireStatus::Success);
  ASSERT_EQ(server.clear_ring(peer, clear(next.reply), now), vdd::FrameWireStatus::Success);
  EXPECT_EQ(server.register_ring(peer, request, now).status, vdd::FrameWireStatus::StaleGeneration);
  EXPECT_EQ(*importer.alive, 0); EXPECT_EQ(server.size(), 1u);
}
TEST_F(Fixture, ReplacementExpiryAndInFlightOwnershipStayBounded) {
  for (int i = 0; i < 50; ++i) {
    ++request.client_cookie;
    ASSERT_EQ(server.register_ring(peer, request, now).status, vdd::FrameWireStatus::Success);
    EXPECT_EQ(server.size(), 1u); EXPECT_EQ(*importer.alive, 1);
  }
  auto inflight = rings.snapshot(request.display_id, request.display_generation, now);
  EXPECT_EQ(server.reap(now + std::chrono::seconds {31}), 1u);
  EXPECT_FALSE(inflight.can_publish()); EXPECT_EQ(*importer.alive, 1);
  inflight = {}; EXPECT_EQ(*importer.alive, 0); EXPECT_EQ(server.size(), 0u);
}
TEST_F(Fixture, FailedImportConsumesCookieAndReleasesOnlyLocalBundle) {
  const auto first = server.register_ring(peer, request, now);
  auto inflight = rings.snapshot(request.display_id, request.display_generation, now);
  importer.fail = true; ++request.client_cookie;
  EXPECT_EQ(server.register_ring(peer, request, now).status, vdd::FrameWireStatus::ImportFailed);
  EXPECT_FALSE(inflight.can_publish());
  EXPECT_EQ(server.register_ring(peer, request, now).status, vdd::FrameWireStatus::StaleGeneration);
  inflight = {}; EXPECT_EQ(*importer.alive, 0);
}
TEST_F(Fixture, NativeDispatcherAndOldDriverKeepNewPathUnavailable) {
  for (bool legacy: {false, true}) {
    Wire wire {*this}; wire.capable = false; wire.legacy = legacy;
    fp::Session session {wire};
    EXPECT_EQ(session.start(std::make_shared<Resources>(request)), fp::Status::Unsupported);
    EXPECT_EQ(wire.calls.size(), legacy ? 1u : 2u); EXPECT_EQ(importer.calls, 0);
    EXPECT_FALSE(session.active());
  }
}
TEST_F(Fixture, MalformedCapabilityAndAckFailClosedAndCancelRegistration) {
  Wire bad_cap {*this}; bad_cap.bad_capability = true;
  fp::Session first {bad_cap};
  EXPECT_EQ(first.start(std::make_shared<Resources>(request)), fp::Status::TransportError);
  EXPECT_EQ(importer.calls, 0);
  Wire bad_ack {*this}; bad_ack.bad_reply = true;
  fp::Session second {bad_ack};
  EXPECT_EQ(second.start(std::make_shared<Resources>(request)), fp::Status::InvalidReply);
  EXPECT_FALSE(second.active()); EXPECT_EQ(*importer.alive, 0);
  EXPECT_EQ(server.register_ring(peer, request, now).status, vdd::FrameWireStatus::StaleGeneration);
}
TEST_F(Fixture, TimeoutBeforeDispatchRetainsSourceAndFencesLateArrival) {
  Wire wire {*this}; wire.timeout_before = true;
  fp::Session session {wire};
  auto resources = std::make_shared<Resources>(request); std::weak_ptr<Resources> weak = resources;
  EXPECT_EQ(session.start(resources), fp::Status::TransportError);
  resources.reset(); EXPECT_FALSE(weak.expired());
  EXPECT_EQ(wire.complete_delayed(), vdd::FrameWireStatus::StaleGeneration);
  EXPECT_TRUE(weak.expired()); EXPECT_EQ(importer.calls, 0);
  EXPECT_EQ(wire.calls.back(), vdd::kIoctlClearFrameRing);
}
TEST_F(Fixture, LostRegistrationReplyCancelsOwnedDuplicates) {
  Wire wire {*this}; wire.timeout_after = true;
  fp::Session session {wire};
  EXPECT_EQ(session.start(std::make_shared<Resources>(request)), fp::Status::TransportError);
  EXPECT_EQ(*importer.alive, 0); EXPECT_FALSE(session.active());
  EXPECT_EQ(server.register_ring(peer, request, now).status, vdd::FrameWireStatus::StaleGeneration);
}
TEST_F(Fixture, ConsumerSelectsNewestWithinFourZeroWaitAttemptsAndTwoHeldSlots) {
  Wire wire {*this}; fp::Session session {wire}; auto resources = std::make_shared<Resources>(request);
  ASSERT_EQ(session.start(resources), fp::Status::Ready);
  for (std::uint32_t i = 0; i < 4; ++i) resources->publish(i, wire.last_reply, i + 1);
  const auto frame = session.next(999999);
  ASSERT_EQ(frame.status, fp::Status::Ready); EXPECT_EQ(frame.metadata.sequence, 4u);
  EXPECT_EQ(resources->attempts, 4); EXPECT_LE(resources->max_held, 2); EXPECT_EQ(resources->held_count, 0);
  EXPECT_EQ(resources->wait_timeout, 100u); EXPECT_EQ(resources->copies, 1); EXPECT_EQ(resources->copied_slot, 3u);
  resources->publish(0, wire.last_reply, 3);
  EXPECT_EQ(session.next(0).status, fp::Status::NoFrame); EXPECT_EQ(resources->copies, 1);
}

TEST_F(Fixture, StartupWaitRequiresAValidatedFirstFrame) {
  Wire wire {*this};
  fp::Session session {wire};
  auto resources = std::make_shared<Resources>(request);
  ASSERT_EQ(session.start(resources), fp::Status::Ready);
  resources->publish(1, wire.last_reply, 1);

  const auto first = fp::wait_for_first_frame(session, 100);
  ASSERT_EQ(first.status, fp::Status::Ready);
  EXPECT_TRUE(first.pixels);
  EXPECT_EQ(first.metadata.sequence, 1u);
  EXPECT_TRUE(session.active());
}

TEST_F(Fixture, StartupWithoutAFrameTimesOutAndCancelsTheRegistration) {
  Wire wire {*this};
  fp::Session session {wire};
  auto resources = std::make_shared<Resources>(request);
  ASSERT_EQ(session.start(resources), fp::Status::Ready);
  ASSERT_EQ(server.size(), 1u);

  const auto first = fp::wait_for_first_frame(session, 30);
  EXPECT_EQ(first.status, fp::Status::Timeout);
  EXPECT_FALSE(session.active());
  EXPECT_EQ(server.size(), 1u); // the high-water tombstone remains to fence late requests
  EXPECT_EQ(*importer.alive, 0);
  EXPECT_FALSE(rings.snapshot(request.display_id, request.display_generation, now).can_publish());
  EXPECT_EQ(server.register_ring(peer, request, now).status, vdd::FrameWireStatus::StaleGeneration);
}
TEST_F(Fixture, StartupRejectsReadyFrameWhosePrivateCopyCrossesDeadline) {
  Wire wire {*this};
  fp::Session session {wire};
  auto resources = std::make_shared<Resources>(request);
  ASSERT_EQ(session.start(resources), fp::Status::Ready);
  resources->publish(0, wire.last_reply, 1);
  resources->copy_delay = std::chrono::milliseconds {250};

  const auto first = fp::wait_for_first_frame(session, 100);
  EXPECT_EQ(first.status, fp::Status::Timeout);
  EXPECT_FALSE(first.pixels);
  EXPECT_FALSE(session.active());
  EXPECT_EQ(resources->copies, 1);
  EXPECT_FALSE(rings.snapshot(request.display_id, request.display_generation, now).can_publish());
  EXPECT_EQ(*importer.alive, 0);
}
TEST_F(Fixture, EventTimeoutStillScansAndCoalescedWakeDoesNotLoseLatest) {
  Wire wire {*this}; fp::Session session {wire}; auto resources = std::make_shared<Resources>(request);
  ASSERT_EQ(session.start(resources), fp::Status::Ready);
  resources->wait_result = fp::Access::Timeout;
  EXPECT_EQ(session.next(100).status, fp::Status::Timeout);
  resources->publish(2, wire.last_reply, 8);
  EXPECT_EQ(session.next(100).status, fp::Status::Ready);
}
TEST_F(Fixture, StaleOrMalformedSlotMetadataRevokesCaptureWithoutCopying) {
  for (int mutation = 0; mutation < 7; ++mutation) {
    ++request.client_cookie;
    Wire wire {*this}; fp::Session session {wire}; auto resources = std::make_shared<Resources>(request);
    ASSERT_EQ(session.start(resources), fp::Status::Ready);
    resources->publish(0, wire.last_reply, 3);
    switch (mutation) {
      case 0: ++resources->slots[0].client_cookie; break; case 1: ++resources->slots[0].display_generation; break;
      case 2: ++resources->slots[0].ring_generation; break; case 3: resources->slots[0].sequence = 0; break;
      case 4: resources->slots[0].sequence = UINT64_MAX; break; case 5: resources->slots[0].acquired_qpc = 0; break;
      case 6: resources->slots[0].reserved = 1; break;
    }
    EXPECT_EQ(session.next(0).status, fp::Status::InvalidFrame);
    EXPECT_FALSE(session.active()); EXPECT_EQ(resources->copies, 0); EXPECT_EQ(resources->held_count, 0);
    EXPECT_EQ(*importer.alive, 0);
  }
}
TEST_F(Fixture, AbandonedOrFailedReleaseRetiresRingAndReleasesPreviouslyHeldSlot) {
  for (bool fail_release: {false, true}) {
    ++request.client_cookie;
    Wire wire {*this}; fp::Session session {wire}; auto resources = std::make_shared<Resources>(request);
    ASSERT_EQ(session.start(resources), fp::Status::Ready);
    resources->publish(0, wire.last_reply, 1);
    resources->release_fail = fail_release;
    if (!fail_release) resources->lost_slot = 1;
    EXPECT_EQ(session.next(0).status, fp::Status::DeviceLost);
    EXPECT_EQ(resources->held_count, 0); EXPECT_FALSE(session.active()); EXPECT_EQ(*importer.alive, 0);
  }
}
TEST_F(Fixture, PrivatePoolBackpressureDropsWithoutBlockingOrRetiringRing) {
  Wire wire {*this}; fp::Session session {wire}; auto resources = std::make_shared<Resources>(request);
  ASSERT_EQ(session.start(resources), fp::Status::Ready);
  resources->publish(0, wire.last_reply, 1); resources->copy_result = fp::Access::Timeout;
  EXPECT_EQ(session.next(0).status, fp::Status::NoFrame); EXPECT_TRUE(session.active());
  EXPECT_EQ(resources->held_count, 0);
  resources->publish(1, wire.last_reply, 2); resources->copy_result = fp::Access::Acquired;
  EXPECT_EQ(session.next(0).status, fp::Status::Ready);
}
TEST_F(Fixture, TeardownIsIdempotentAndPrivateFrameOutlivesSharedRing) {
  Wire wire {*this}; auto resources = std::make_shared<Resources>(request);
  auto outputs = resources->outputs;
  fp::Frame frame;
  {
    fp::Session session {wire}; ASSERT_EQ(session.start(resources), fp::Status::Ready);
    resources->publish(0, wire.last_reply, 1); frame = session.next(0);
    ASSERT_EQ(frame.status, fp::Status::Ready);
    session.stop(); const auto calls = wire.calls.size(); session.stop(); EXPECT_EQ(wire.calls.size(), calls);
    EXPECT_EQ(session.next(0).status, fp::Status::Stopped); EXPECT_EQ(*importer.alive, 0);
  }
  resources.reset(); EXPECT_EQ(*outputs, 1); frame = {}; EXPECT_EQ(*outputs, 0);
}

#include "../../src/platform/windows/frame_push_io.h"
namespace {
  class DelayedIo final: public fp::AsyncIo {
  public:
    bool begin(std::shared_ptr<fp::PendingTransaction> transaction, std::uint32_t &) override {
      ++begun; pending = std::move(transaction);
      if (immediate) finish(0, pending->output.size() + (oversized ? 1 : 0));
      return true;
    }
    void cancel(const std::shared_ptr<fp::PendingTransaction> &transaction) override {
      ++canceled; EXPECT_EQ(transaction, pending);
    }
    void finish(std::uint32_t error, std::size_t bytes) {
      auto completing = std::move(pending);
      if (!completing->output.empty()) completing->output[0] = std::byte {42};
      completing->complete(error, bytes);
    }
    int begun {}, canceled {}; bool immediate {}, oversized {};
    std::shared_ptr<fp::PendingTransaction> pending;
  };
class LateRegistrationIo final: public fp::AsyncIo {
public:
  explicit LateRegistrationIo(Fixture &fixture): fixture(fixture) {}
  bool begin(std::shared_ptr<fp::PendingTransaction> transaction, std::uint32_t &error) override {
    ++begun;
    if (transaction->code == vdd::kIoctlGetProtocolVersion) {
      const vdd::ProtocolVersion version {};
      std::memcpy(transaction->output.data(), &version, sizeof(version));
      transaction->complete(0, sizeof(version));
      return true;
    }
    if (transaction->code == vdd::kIoctlQueryFrameTransportCapabilities) {
      vdd::FrameTransportCapabilities capabilities {};
      capabilities.capabilities = vdd::kFrameTransportCapabilityKeyedMutexRing;
      capabilities.max_ring_slots = vdd::kFrameRingSlotCount;
      std::memcpy(transaction->output.data(), &capabilities, sizeof(capabilities));
      transaction->complete(0, sizeof(capabilities));
      return true;
    }
    if (transaction->code == vdd::kIoctlRegisterFrameRing) {
      ++register_calls;
      pending_registration = std::move(transaction);
      return true;
    }
    if (transaction->code == vdd::kIoctlClearFrameRing) {
      ++clear_calls;
      if (hang_clear) {
        pending_clear = std::move(transaction);
        return true;
      }
      vdd::FrameRingClearRequest clear {};
      if (pending_or_current_input(transaction, clear)) {
        clear_status = fixture.server.clear_ring({200, 900}, clear, fixture.now);
        error = clear_status == vdd::FrameWireStatus::Success ? 0 : 31;
      } else {
        error = 87;
      }
      transaction->complete(error, 0);
      return true;
    }
    error = 87;
    return false;
  }
  void cancel(const std::shared_ptr<fp::PendingTransaction> &transaction) override {
    canceled.fetch_add(1, std::memory_order_relaxed);
    canceled_event.notify_all();
    if (pending_registration) EXPECT_EQ(transaction, pending_registration);
  }
  bool wait_for_cancellations(int count, std::uint32_t timeout_ms) {
    std::unique_lock lock {canceled_mutex};
    return canceled_event.wait_for(lock, std::chrono::milliseconds {timeout_ms}, [&] {
      return canceled.load(std::memory_order_relaxed) >= count;
    });
  }
  void complete_late_registration() {
    ASSERT_TRUE(pending_registration);
    auto transaction = std::move(pending_registration);
    vdd::FrameRingRegisterRequest request {};
    std::memcpy(&request, transaction->input.data(), sizeof(request));
    const auto result = fixture.server.register_ring({200, 900}, request, fixture.now);
    if (result.status == vdd::FrameWireStatus::Success) {
      std::memcpy(transaction->output.data(), &result.reply, sizeof(result.reply));
      transaction->complete(0, sizeof(result.reply));
    } else {
      transaction->complete(31, 0);
    }
  }
  void complete_cancelled_clear() {
    ASSERT_TRUE(pending_clear);
    auto transaction = std::move(pending_clear);
    transaction->complete(995, 0);
  }
  Fixture &fixture;
  int begun {}, register_calls {}, clear_calls {};
  std::atomic<int> canceled {};
  std::mutex canceled_mutex;
  std::condition_variable canceled_event;
  bool hang_clear {};
  vdd::FrameWireStatus clear_status {vdd::FrameWireStatus::InvalidInput};
  std::shared_ptr<fp::PendingTransaction> pending_registration;
  std::shared_ptr<fp::PendingTransaction> pending_clear;
private:
  static bool pending_or_current_input(const std::shared_ptr<fp::PendingTransaction> &transaction,
                                       vdd::FrameRingClearRequest &clear) {
    if (transaction->input.size() != sizeof(clear)) return false;
    std::memcpy(&clear, transaction->input.data(), sizeof(clear));
    return true;
  }
};
}
TEST(BoundedIo, TimeoutRetainsHeapBuffersAndResourcesAfterCallerAndTransportDestruction) {
  auto io = std::make_shared<DelayedIo>(); auto alive = std::make_shared<int>(0);
  std::weak_ptr<fp::PendingTransaction> pending;
  {
    fp::BoundedTransactionTransport transport {io};
    std::array<std::uint8_t, 4> input {1, 2, 3, 4}, output {9, 9, 9, 9};
    std::size_t returned = 99; std::uint32_t error = 0;
    auto resources = std::make_shared<Bundle>(alive);
    EXPECT_FALSE(transport.exchange(1, input.data(), input.size(), output.data(), output.size(),
                                    returned, error, 1, resources));
    EXPECT_EQ(error, 1460u); EXPECT_EQ(returned, 0u); EXPECT_EQ(output[0], 9u);
    resources.reset(); input[0] = 99;
    ASSERT_TRUE(io->pending); pending = io->pending;
    EXPECT_EQ(io->pending->input[0], std::byte {1}); EXPECT_EQ(*alive, 1);
    EXPECT_FALSE(transport.exchange(2, nullptr, 0, nullptr, 0, returned, error, 1, {}));
    EXPECT_EQ(error, 170u); EXPECT_EQ(io->begun, 1);
  }
  EXPECT_FALSE(pending.expired()); EXPECT_EQ(*alive, 1); EXPECT_EQ(io->canceled, 2);
  io->finish(995, 0);
  EXPECT_TRUE(pending.expired()); EXPECT_EQ(*alive, 0);
}
TEST(BoundedIo, GlobalUnresolvedLimitBoundsAbandonedEndpointRetention) {
  std::vector<std::shared_ptr<DelayedIo>> endpoints;
  for (int i = 0; i < 8; ++i) {
    auto io = std::make_shared<DelayedIo>();
    fp::BoundedTransactionTransport transport {io};
    std::size_t returned = 0; std::uint32_t error = 0;
    EXPECT_FALSE(transport.exchange(1, nullptr, 0, nullptr, 0, returned, error, 1, {}));
    EXPECT_EQ(error, 1460u); endpoints.push_back(io);
  }
  auto ninth = std::make_shared<DelayedIo>(); fp::BoundedTransactionTransport transport {ninth};
  std::size_t returned = 0; std::uint32_t error = 0;
  EXPECT_FALSE(transport.exchange(1, nullptr, 0, nullptr, 0, returned, error, 1, {}));
  EXPECT_EQ(error, 170u); EXPECT_EQ(ninth->begun, 0);
  for (const auto &io: endpoints) io->finish(995, 0);
  ninth->immediate = true;
  EXPECT_TRUE(transport.exchange(1, nullptr, 0, nullptr, 0, returned, error, 1, {}));
}
TEST(BoundedIo, RejectsOversizedTransactionsAndMalformedCompletionWithoutCallerWrite) {
  auto io = std::make_shared<DelayedIo>(); fp::BoundedTransactionTransport transport {io};
  std::array<std::uint8_t, 257> input {}; std::array<std::uint8_t, 4> output {9, 9, 9, 9};
  std::size_t returned = 0; std::uint32_t error = 0;
  EXPECT_FALSE(transport.exchange(1, input.data(), input.size(), output.data(), output.size(),
                                  returned, error, 1, {}));
  EXPECT_EQ(error, 87u); EXPECT_EQ(io->begun, 0);
  io->immediate = true;
  EXPECT_TRUE(transport.exchange(1, nullptr, 0, output.data(), output.size(), returned, error, 1, {}));
  EXPECT_EQ(output[0], 42u); EXPECT_EQ(returned, 4u);
  io->oversized = true; output[0] = 9;
  EXPECT_FALSE(transport.exchange(1, nullptr, 0, output.data(), output.size(), returned, error, 1, {}));
  EXPECT_EQ(error, 87u); EXPECT_EQ(returned, 0u); EXPECT_EQ(output[0], 9u);
}
TEST_F(Fixture, TimedOutRegistrationQueuesCancellationAfterLateCompletion) {
  auto io = std::make_shared<LateRegistrationIo>(*this);
  fp::BoundedTransactionTransport transport {io};
  fp::Session session {transport};
  auto resources = std::make_shared<Resources>(request);

  EXPECT_EQ(session.start(resources), fp::Status::TransportError);
  EXPECT_FALSE(session.active());
  ASSERT_EQ(io->register_calls, 1);
  EXPECT_EQ(io->clear_calls, 0);
  ASSERT_TRUE(io->pending_registration);

  // Model the driver completing a registration that raced the timeout. The
  // transport must submit the cancel only after this completion, preserving
  // per-endpoint ordering while preventing publication from surviving.
  io->complete_late_registration();
  EXPECT_EQ(io->clear_calls, 1);
  EXPECT_EQ(io->clear_status, vdd::FrameWireStatus::Success);
  EXPECT_EQ(*importer.alive, 0);
  EXPECT_FALSE(rings.snapshot(request.display_id, request.display_generation, now).can_publish());
  EXPECT_EQ(io->canceled.load(), 1);
}
TEST_F(Fixture, DeferredClearTimesOutCancelsAndReleasesCompletedResourceHold) {
  auto io = std::make_shared<LateRegistrationIo>(*this);
  io->hang_clear = true;
  fp::BoundedTransactionTransport transport {io};
  fp::Session session {transport};
  auto resources = std::make_shared<Resources>(request);
  std::weak_ptr<Resources> weak_resources = resources;

  EXPECT_EQ(session.start(resources), fp::Status::TransportError);
  resources.reset();
  ASSERT_TRUE(io->pending_registration);
  io->complete_late_registration();
  ASSERT_EQ(io->clear_calls, 1);
  ASSERT_TRUE(io->pending_clear);
  EXPECT_FALSE(weak_resources.expired());
  ASSERT_TRUE(io->wait_for_cancellations(2, 1000));

  // A timeout cancels the native request but cannot claim it completed. Keep
  // ownership until the native completion arrives; the finite lease reaper is
  // still the cleanup fallback if that completion never arrives.
  io->complete_cancelled_clear();
  EXPECT_TRUE(weak_resources.expired());
  EXPECT_TRUE(rings.snapshot(request.display_id, request.display_generation, now).can_publish());
  EXPECT_EQ(server.reap(now + std::chrono::seconds {31}), 1u);
  EXPECT_EQ(*importer.alive, 0);
}
TEST_F(Fixture, ResetRetiresPublicationButRetainsHighWaterFence) {
  const auto first = server.register_ring(peer, request, now);
  ASSERT_EQ(first.status, vdd::FrameWireStatus::Success);
  auto inflight = rings.snapshot(request.display_id, request.display_generation, now);
  server.reset(); EXPECT_FALSE(inflight.can_publish());
  EXPECT_EQ(server.register_ring(peer, request, now).status, vdd::FrameWireStatus::StaleGeneration);
  EXPECT_EQ(server.size(), 1u);
  ++request.client_cookie;
  EXPECT_EQ(server.register_ring(peer, request, now).status, vdd::FrameWireStatus::Success);
}
