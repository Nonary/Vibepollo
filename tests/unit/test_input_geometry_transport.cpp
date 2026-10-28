#include "../tests_common.h"

#include <src/input_geometry_policy.h>
#include <src/platform/windows/wgc_capture_policy.h>
#include <src/thread_safe.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
  struct test_port_t {
    int offset_x = 10;
    int offset_y = 20;
    int env_width = 1920;
    int env_height = 1080;
    int width = 1280;
    int height = 720;
    int logical_width = 1920;
    int logical_height = 1080;
    std::string output_name = "DISPLAY-A";
    float client_offset_x = 0.0f;
    float client_offset_y = 12.5f;
    float scalar_inv = 1.5f;
    float scalar_tpcoords = 1.0f;
    std::uint64_t capture_generation = 0;
    std::uint64_t geometry_sequence = 0;
    bool geometry_valid = true;

    explicit operator bool() const {
      return geometry_valid && width != 0 && height != 0 && env_width != 0 && env_height != 0;
    }
  };

  struct terminal_capture_context_t {
    std::shared_ptr<safe::event_t<test_port_t>> touch_port_event;
    bool live = true;
  };

  platf::input_geometry_snapshot_t snapshot(
    std::uint64_t generation,
    std::uint64_t sequence,
    bool valid,
    int offset_x = 10,
    int offset_y = 20,
    int desktop_width = 1920,
    int desktop_height = 1080
  ) {
    platf::input_geometry_snapshot_t result;
    result.capture_generation = generation;
    result.geometry_sequence = sequence;
    result.valid = valid;
    result.target.adapter_high_part = -7;
    result.target.adapter_low_part = 0x1234;
    result.target.output_name = L"\\\\.\\DISPLAY-A";
    result.target.monitor = 0x5678;
    result.target.attached_to_desktop = true;
    result.target.rect_left = 100;
    result.target.rect_top = 200;
    result.target.rect_right = 2020;
    result.target.rect_bottom = 1280;
    result.target.rotation = 1;
    result.target.hdr_valid = true;
    result.target.hdr = false;
    result.offset_x = offset_x;
    result.offset_y = offset_y;
    result.desktop_width = desktop_width;
    result.desktop_height = desktop_height;
    return result;
  }
}  // namespace

TEST(InputGeometryTransport, GeometryRefreshChangesOnlyAbsoluteMapping) {
  input::geometry::transport_t transport;
  test_port_t port;
  const auto initial = snapshot(4, 1, true);
  EXPECT_EQ(transport.refresh(initial, port, [] { FAIL() << "initial port must not cancel contacts"; }), input::geometry::snapshot_result_e::initial);

  const auto changed = snapshot(4, 2, true, -1280, -120, 3200, 1200);
  EXPECT_EQ(transport.refresh(changed, port, [] {}), input::geometry::snapshot_result_e::updated);
  EXPECT_EQ(port.offset_x, -1280);
  EXPECT_EQ(port.offset_y, -120);
  EXPECT_EQ(port.env_width, 3200);
  EXPECT_EQ(port.env_height, 1200);
  EXPECT_EQ(port.width, 1280);
  EXPECT_EQ(port.height, 720);
  EXPECT_EQ(port.logical_width, 1920);
  EXPECT_EQ(port.logical_height, 1080);
  EXPECT_EQ(port.output_name, "DISPLAY-A");
  EXPECT_FLOAT_EQ(port.client_offset_x, 0.0f);
  EXPECT_FLOAT_EQ(port.client_offset_y, 12.5f);
  EXPECT_FLOAT_EQ(port.scalar_inv, 1.5f);
  EXPECT_FLOAT_EQ(port.scalar_tpcoords, 1.0f);
  EXPECT_EQ(port.capture_generation, 4u);
  EXPECT_EQ(port.geometry_sequence, 2u);
  EXPECT_TRUE(port.geometry_valid);
}

TEST(InputGeometryTransport, FullPortEventInstallsStreamTransformAndGeometryMapping) {
  input::geometry::transport_t transport;
  test_port_t local_port;
  local_port.offset_x = 0;
  local_port.offset_y = 0;
  local_port.env_width = 0;
  local_port.env_height = 0;
  local_port.width = 0;
  local_port.height = 0;
  local_port.logical_width = 0;
  local_port.logical_height = 0;
  local_port.output_name.clear();
  local_port.client_offset_x = 0.0f;
  local_port.client_offset_y = 0.0f;
  local_port.scalar_inv = 1.0f;
  local_port.scalar_tpcoords = 1.0f;
  EXPECT_FALSE(static_cast<bool>(local_port));

  auto initial_event = test_port_t {};
  initial_event.capture_generation = 4;
  initial_event.geometry_sequence = 1;
  const auto initial = snapshot(4, 1, true, -240, 80, 2560, 1440);
  EXPECT_EQ(
    transport.refresh_from_event(initial, local_port, initial_event, [] { FAIL() << "initial port must not cancel contacts"; }),
    input::geometry::snapshot_result_e::initial
  );
  EXPECT_TRUE(static_cast<bool>(local_port));
  EXPECT_EQ(local_port.offset_x, -240);
  EXPECT_EQ(local_port.offset_y, 80);
  EXPECT_EQ(local_port.env_width, 2560);
  EXPECT_EQ(local_port.env_height, 1440);
  EXPECT_EQ(local_port.width, 1280);
  EXPECT_EQ(local_port.height, 720);
  EXPECT_EQ(local_port.logical_width, 1920);
  EXPECT_EQ(local_port.logical_height, 1080);
  EXPECT_EQ(local_port.output_name, "DISPLAY-A");
  EXPECT_FLOAT_EQ(local_port.client_offset_x, 0.0f);
  EXPECT_FLOAT_EQ(local_port.client_offset_y, 12.5f);
  EXPECT_FLOAT_EQ(local_port.scalar_inv, 1.5f);
  EXPECT_FLOAT_EQ(local_port.scalar_tpcoords, 1.0f);

  // A geometry-only event keeps the stream/client transform while replacing
  // the desktop-relative mapping fields.
  auto geometry_event = initial_event;
  geometry_event.geometry_sequence = 2;
  const auto changed_geometry = snapshot(4, 2, true, -400, -60, 3840, 2160);
  EXPECT_EQ(
    transport.refresh_from_event(changed_geometry, local_port, geometry_event, [] {}),
    input::geometry::snapshot_result_e::updated
  );
  EXPECT_EQ(local_port.offset_x, -400);
  EXPECT_EQ(local_port.offset_y, -60);
  EXPECT_EQ(local_port.env_width, 3840);
  EXPECT_EQ(local_port.env_height, 2160);
  EXPECT_EQ(local_port.width, 1280);
  EXPECT_EQ(local_port.height, 720);
  EXPECT_EQ(local_port.logical_width, 1920);
  EXPECT_EQ(local_port.logical_height, 1080);
  EXPECT_EQ(local_port.output_name, "DISPLAY-A");
  EXPECT_FLOAT_EQ(local_port.client_offset_y, 12.5f);
  EXPECT_FLOAT_EQ(local_port.scalar_inv, 1.5f);

  // A new capture generation replaces the entire transform before applying
  // its current geometry snapshot.
  auto next_capture = geometry_event;
  next_capture.capture_generation = 5;
  next_capture.geometry_sequence = 1;
  next_capture.width = 1920;
  next_capture.height = 1080;
  next_capture.logical_width = 2560;
  next_capture.logical_height = 1440;
  next_capture.output_name = "DISPLAY-B";
  next_capture.client_offset_x = 24.0f;
  next_capture.client_offset_y = 36.0f;
  next_capture.scalar_inv = 2.0f;
  next_capture.scalar_tpcoords = 1.25f;
  const auto next_snapshot = snapshot(5, 1, true, -1920, -108, 7680, 2160);
  EXPECT_EQ(
    transport.refresh_from_event(next_snapshot, local_port, next_capture, [] {}),
    input::geometry::snapshot_result_e::updated
  );
  EXPECT_EQ(local_port.offset_x, -1920);
  EXPECT_EQ(local_port.offset_y, -108);
  EXPECT_EQ(local_port.env_width, 7680);
  EXPECT_EQ(local_port.env_height, 2160);
  EXPECT_EQ(local_port.width, 1920);
  EXPECT_EQ(local_port.height, 1080);
  EXPECT_EQ(local_port.logical_width, 2560);
  EXPECT_EQ(local_port.logical_height, 1440);
  EXPECT_EQ(local_port.output_name, "DISPLAY-B");
  EXPECT_FLOAT_EQ(local_port.client_offset_x, 24.0f);
  EXPECT_FLOAT_EQ(local_port.client_offset_y, 36.0f);
  EXPECT_FLOAT_EQ(local_port.scalar_inv, 2.0f);
  EXPECT_FLOAT_EQ(local_port.scalar_tpcoords, 1.25f);
}

TEST(InputGeometryTransport, InvalidFullEventPreservesLastKnownMapAndFailsClosed) {
  input::geometry::transport_t transport;
  test_port_t local_port;
  local_port.width = 0;
  local_port.height = 0;
  local_port.env_width = 0;
  local_port.env_height = 0;

  auto initial_event = test_port_t {};
  initial_event.capture_generation = 12;
  initial_event.geometry_sequence = 3;
  const auto initial = snapshot(12, 3, true, -1280, 120, 3840, 2160);
  EXPECT_EQ(
    transport.refresh_from_event(initial, local_port, initial_event, [] {}),
    input::geometry::snapshot_result_e::initial
  );
  ASSERT_TRUE(static_cast<bool>(local_port));

  auto invalid_event = initial_event;
  invalid_event.geometry_sequence = 4;
  invalid_event.geometry_valid = false;
  invalid_event.offset_x = 900;
  invalid_event.offset_y = 700;
  invalid_event.env_width = 0;
  invalid_event.env_height = 0;
  invalid_event.width = 1920;
  invalid_event.height = 1080;
  bool cancellation_saw_old_map = false;
  const auto invalid = snapshot(12, 4, false, 900, 700, 0, 0);
  EXPECT_EQ(transport.refresh_from_event(invalid, local_port, invalid_event, [&] {
    cancellation_saw_old_map =
      local_port.geometry_valid && local_port.geometry_sequence == 3 &&
      local_port.offset_x == -1280 && local_port.offset_y == 120 &&
      local_port.env_width == 3840 && local_port.env_height == 2160;
  }), input::geometry::snapshot_result_e::updated);

  EXPECT_TRUE(cancellation_saw_old_map);
  EXPECT_FALSE(local_port.geometry_valid);
  EXPECT_FALSE(static_cast<bool>(local_port));
  EXPECT_EQ(local_port.geometry_sequence, 4u);
  EXPECT_EQ(local_port.offset_x, -1280);
  EXPECT_EQ(local_port.offset_y, 120);
  EXPECT_EQ(local_port.env_width, 3840);
  EXPECT_EQ(local_port.env_height, 2160);
  EXPECT_EQ(local_port.width, 1920);
  EXPECT_EQ(local_port.height, 1080);
}

TEST(InputGeometryTransport, ContactsDoNotStartUntilPublishedPortIsUsable) {
  input::geometry::transport_t transport;
  test_port_t port;
  port.width = 0;
  port.height = 0;
  port.env_width = 0;
  port.env_height = 0;
  EXPECT_FALSE(static_cast<bool>(port));
  EXPECT_EQ(transport.touch(input::geometry::contact_event_e::down, 55, static_cast<bool>(port)), input::geometry::contact_result_e::suppress);
  EXPECT_EQ(transport.pen(input::geometry::contact_event_e::down, static_cast<bool>(port)), input::geometry::contact_result_e::suppress);

  auto published = test_port_t {};
  published.capture_generation = 8;
  published.geometry_sequence = 1;
  EXPECT_EQ(
    transport.refresh_from_event(snapshot(8, 1, true), port, published, [] {}),
    input::geometry::snapshot_result_e::initial
  );
  EXPECT_TRUE(static_cast<bool>(port));
  EXPECT_EQ(transport.touch(input::geometry::contact_event_e::down, 55, static_cast<bool>(port)), input::geometry::contact_result_e::accept);
  EXPECT_EQ(transport.pen(input::geometry::contact_event_e::down, static_cast<bool>(port)), input::geometry::contact_result_e::accept);
  EXPECT_EQ(transport.touch(input::geometry::contact_event_e::move, 55, static_cast<bool>(port)), input::geometry::contact_result_e::accept);
  EXPECT_EQ(transport.pen(input::geometry::contact_event_e::move, static_cast<bool>(port)), input::geometry::contact_result_e::accept);
}

TEST(InputGeometryTransport, FullEventWithSameVersionCannotReplaceCurrentMapping) {
  input::geometry::transport_t transport;
  test_port_t local_port;
  local_port.width = 0;
  local_port.height = 0;
  local_port.env_width = 0;
  local_port.env_height = 0;

  auto initial_event = test_port_t {};
  initial_event.capture_generation = 31;
  initial_event.geometry_sequence = 6;
  const auto stable = snapshot(31, 6, true);
  EXPECT_EQ(
    transport.refresh_from_event(stable, local_port, initial_event, [] {}),
    input::geometry::snapshot_result_e::initial
  );
  ASSERT_TRUE(static_cast<bool>(local_port));
  EXPECT_EQ(local_port.offset_x, 10);

  auto same_version_event = initial_event;
  same_version_event.width = 1920;
  same_version_event.offset_x = -500;
  same_version_event.offset_y = 700;
  same_version_event.env_width = 500;
  same_version_event.env_height = 600;
  auto conflicting_snapshot = snapshot(31, 6, true, -500, 700, 500, 600);
  int cancel_calls = 0;
  EXPECT_EQ(
    transport.refresh_from_event(conflicting_snapshot, local_port, same_version_event, [&] { ++cancel_calls; }),
    input::geometry::snapshot_result_e::unchanged
  );
  EXPECT_EQ(cancel_calls, 0);
  EXPECT_EQ(local_port.width, 1920);
  EXPECT_EQ(local_port.offset_x, 10);
  EXPECT_EQ(local_port.offset_y, 20);
  EXPECT_EQ(local_port.env_width, 1920);
  EXPECT_EQ(local_port.env_height, 1080);
}

TEST(InputGeometryTransport, LateDynamicGeometryRefreshCannotReplaceNewerEventPayload) {
  using namespace std::chrono_literals;
  auto event = std::make_shared<safe::event_t<test_port_t>>();

  auto initial = test_port_t {};
  initial.capture_generation = 8;
  initial.geometry_sequence = 10;
  EXPECT_TRUE(input::geometry::publish_monotonic_port(event, initial));

  auto delayed_port = initial;
  delayed_port.geometry_sequence = 11;
  auto newest_port = initial;
  newest_port.geometry_sequence = 12;

  // The older producer computed sequence 11, then pauses while another
  // producer refreshes the event to sequence 12.
  const auto old_view = event->view(0ms);
  ASSERT_TRUE(old_view);
  EXPECT_EQ(old_view->geometry_sequence, 10u);
  input::geometry::publish_port_if_geometry_changed(event, newest_port);
  input::geometry::publish_port_if_geometry_changed(event, delayed_port);

  const auto latest = event->view(0ms);
  ASSERT_TRUE(latest);
  EXPECT_EQ(latest->capture_generation, 8u);
  EXPECT_EQ(latest->geometry_sequence, 12u);
}

TEST(InputGeometryTransport, CaptureTerminalizationDrainsQueuedSessionBeforeRejectingStalePort) {
  using namespace std::chrono_literals;
  auto event = std::make_shared<safe::event_t<test_port_t>>();
  auto old_valid_port = test_port_t {};
  old_valid_port.capture_generation = 52;
  old_valid_port.geometry_sequence = 4;
  auto terminal_port = old_valid_port;
  terminal_port.geometry_sequence = 5;
  terminal_port.geometry_valid = false;

  safe::signal_t reinit_event;
  safe::queue_t<terminal_capture_context_t> pending_contexts {4};
  std::vector<terminal_capture_context_t> active_contexts;
  ASSERT_TRUE(pending_contexts.try_raise(terminal_capture_context_t {event}));

  input::geometry::terminalize_capture_contexts(
    reinit_event,
    pending_contexts,
    active_contexts,
    true,
    [](const auto &context) { return context.live; },
    [&](auto &context) {
      EXPECT_EQ(context.touch_port_event, event);
      EXPECT_TRUE(input::geometry::publish_monotonic_port(context.touch_port_event, terminal_port));
    }
  );

  EXPECT_TRUE(reinit_event.peek());
  EXPECT_FALSE(pending_contexts.pop(0ms));
  ASSERT_EQ(active_contexts.size(), 1u);
  const auto terminal = event->view(0ms);
  ASSERT_TRUE(terminal);
  EXPECT_FALSE(terminal->geometry_valid);
  EXPECT_EQ(terminal->capture_generation, 52u);
  EXPECT_EQ(terminal->geometry_sequence, 5u);

  // A producer that copied the old display map before its capture return can
  // publish only after the terminal fanout. The event's version floor rejects
  // that delayed write.
  EXPECT_FALSE(input::geometry::publish_monotonic_port(event, old_valid_port));
  const auto latest = event->view(0ms);
  ASSERT_TRUE(latest);
  EXPECT_FALSE(latest->geometry_valid);
  EXPECT_EQ(latest->geometry_sequence, 5u);
}

TEST(InputGeometryTransport, TerminalGeometryOnCaptureErrorDoesNotForceReinitTeardown) {
  using namespace std::chrono_literals;
  auto event = std::make_shared<safe::event_t<test_port_t>>();
  safe::signal_t reinit_event;
  safe::queue_t<terminal_capture_context_t> pending_contexts {4};
  std::vector<terminal_capture_context_t> active_contexts;
  ASSERT_TRUE(pending_contexts.try_raise(terminal_capture_context_t {event}));
  auto terminal_port = test_port_t {};
  terminal_port.capture_generation = 7;
  terminal_port.geometry_sequence = 1;
  terminal_port.geometry_valid = false;

  input::geometry::terminalize_capture_contexts(
    reinit_event,
    pending_contexts,
    active_contexts,
    false,
    [](const auto &context) { return context.live; },
    [&](auto &context) {
      EXPECT_EQ(context.touch_port_event, event);
      EXPECT_TRUE(input::geometry::publish_monotonic_port(context.touch_port_event, terminal_port));
    }
  );

  EXPECT_FALSE(reinit_event.peek());
  EXPECT_FALSE(pending_contexts.pop(0ms));
  ASSERT_EQ(active_contexts.size(), 1u);
  const auto latest = event->view(0ms);
  ASSERT_TRUE(latest);
  EXPECT_EQ(latest->capture_generation, 7u);
  EXPECT_EQ(latest->geometry_sequence, 1u);
  EXPECT_FALSE(latest->geometry_valid);
}

TEST(InputGeometryTransport, WorkerGenerationViewReadsNewestGeometryPublication) {
  auto event = std::make_shared<safe::event_t<test_port_t>>();
  std::uint64_t observed_generation {};
  EXPECT_FALSE(event->view_if_newer(observed_generation));

  auto initial = test_port_t {};
  initial.capture_generation = 15;
  initial.geometry_sequence = 1;
  EXPECT_TRUE(input::geometry::publish_monotonic_port(event, initial));
  const auto first = event->view_if_newer(observed_generation);
  ASSERT_TRUE(first);
  EXPECT_EQ(first->geometry_sequence, 1u);
  EXPECT_FALSE(event->view_if_newer(observed_generation));

  auto changed = initial;
  changed.geometry_sequence = 2;
  EXPECT_TRUE(input::geometry::publish_monotonic_port(event, changed));
  const auto latest = event->view_if_newer(observed_generation);
  ASSERT_TRUE(latest);
  EXPECT_EQ(latest->capture_generation, 15u);
  EXPECT_EQ(latest->geometry_sequence, 2u);
  EXPECT_FALSE(event->view_if_newer(observed_generation));
}

TEST(InputGeometryTransport, NewGenerationCancelsBeforePublishingAndQuarantinesOldContacts) {
  input::geometry::transport_t transport;
  test_port_t port;
  EXPECT_EQ(transport.refresh(snapshot(10, 7, true), port, [] {}), input::geometry::snapshot_result_e::initial);
  EXPECT_EQ(transport.touch(input::geometry::contact_event_e::down, 55, true), input::geometry::contact_result_e::accept);
  EXPECT_EQ(transport.pen(input::geometry::contact_event_e::down, true), input::geometry::contact_result_e::accept);

  std::vector<std::string> order;
  const auto next_generation = snapshot(11, 1, true, -1920, 0, 3840, 1080);
  const auto result = transport.refresh(next_generation, port, [&] {
    EXPECT_EQ(port.capture_generation, 10u);
    EXPECT_EQ(port.geometry_sequence, 7u);
    EXPECT_EQ(port.offset_x, 10);
    order.emplace_back("cancel old touch and pen repeat state");
  });
  EXPECT_EQ(result, input::geometry::snapshot_result_e::updated);
  order.emplace_back("publish new input map");
  ASSERT_EQ(order.size(), 2u);
  EXPECT_EQ(order[0], "cancel old touch and pen repeat state");
  EXPECT_EQ(order[1], "publish new input map");
  EXPECT_EQ(port.capture_generation, 11u);
  EXPECT_EQ(port.geometry_sequence, 1u);
  EXPECT_EQ(port.offset_x, -1920);

  EXPECT_EQ(transport.touch(input::geometry::contact_event_e::move, 55, true), input::geometry::contact_result_e::suppress);
  EXPECT_EQ(transport.pen(input::geometry::contact_event_e::move, true), input::geometry::contact_result_e::suppress);
  EXPECT_EQ(transport.touch(input::geometry::contact_event_e::up, 55, true), input::geometry::contact_result_e::suppress);
  EXPECT_EQ(transport.pen(input::geometry::contact_event_e::up, true), input::geometry::contact_result_e::suppress);
  EXPECT_EQ(transport.touch(input::geometry::contact_event_e::down, 55, true), input::geometry::contact_result_e::accept);
  EXPECT_EQ(transport.pen(input::geometry::contact_event_e::down, true), input::geometry::contact_result_e::accept);

  int stale_cancel_calls = 0;
  EXPECT_EQ(transport.refresh(snapshot(10, 99, true, 999, 999, 1, 1), port, [&] { ++stale_cancel_calls; }), input::geometry::snapshot_result_e::stale);
  EXPECT_EQ(stale_cancel_calls, 0);
  EXPECT_EQ(port.capture_generation, 11u);
  EXPECT_EQ(port.offset_x, -1920);
}

TEST(InputGeometryTransport, InvalidSnapshotCancelsOldContactsAndFailsClosed) {
  input::geometry::transport_t transport;
  test_port_t port;
  EXPECT_EQ(transport.refresh(snapshot(22, 4, true), port, [] {}), input::geometry::snapshot_result_e::initial);
  EXPECT_EQ(transport.touch(input::geometry::contact_event_e::down, 9, true), input::geometry::contact_result_e::accept);
  EXPECT_EQ(transport.pen(input::geometry::contact_event_e::down, true), input::geometry::contact_result_e::accept);

  bool cancellation_saw_old_map = false;
  const auto invalid = snapshot(22, 5, false, 0, 0, 0, 0);
  EXPECT_EQ(transport.refresh(invalid, port, [&] {
    cancellation_saw_old_map = port.geometry_valid && port.geometry_sequence == 4 && port.offset_x == 10;
  }), input::geometry::snapshot_result_e::updated);
  EXPECT_TRUE(cancellation_saw_old_map);
  EXPECT_FALSE(port.geometry_valid);
  EXPECT_FALSE(static_cast<bool>(port));
  EXPECT_EQ(port.geometry_sequence, 5u);
  EXPECT_EQ(port.offset_x, 10);
  EXPECT_EQ(port.env_width, 1920);
  EXPECT_EQ(transport.touch(input::geometry::contact_event_e::down, 10, false), input::geometry::contact_result_e::suppress);
  EXPECT_EQ(transport.pen(input::geometry::contact_event_e::down, false), input::geometry::contact_result_e::suppress);
  EXPECT_EQ(transport.touch(input::geometry::contact_event_e::move, 9, false), input::geometry::contact_result_e::suppress);
  EXPECT_EQ(transport.pen(input::geometry::contact_event_e::move, false), input::geometry::contact_result_e::suppress);
}

TEST(InputGeometryTransport, InvalidOrClosedCaptureStillRequestsReinitialization) {
  using namespace platf::dxgi::wgc_policy;
  const input_geometry_t captured {10, 20, 1920, 1080};
  const desktop_bounds_t bounds {0, 0, 1920, 1080};
  for (const auto state : {active_capture_state_e::invalid, active_capture_state_e::item_closed}) {
    const auto decision = assess_verified_geometry_update(
      state, verified_target_state_e::same, true, captured, 10, 20, bounds);
    EXPECT_EQ(decision.action, geometry_update_action_e::reinitialize);
  }
}

TEST(InputGeometryTransport, StableVersionIsANoopAndControlWakeNeedsNoClientPacket) {
  input::geometry::transport_t transport;
  test_port_t port;
  const auto stable = snapshot(31, 6, true);
  EXPECT_EQ(transport.refresh(stable, port, [] {}), input::geometry::snapshot_result_e::initial);

  int cancel_calls = 0;
  auto unchanged_but_conflicting_payload = stable;
  unchanged_but_conflicting_payload.offset_x = -500;
  EXPECT_EQ(transport.refresh(unchanged_but_conflicting_payload, port, [&] { ++cancel_calls; }), input::geometry::snapshot_result_e::unchanged);
  EXPECT_EQ(cancel_calls, 0);
  EXPECT_EQ(port.offset_x, 10);

  std::atomic_bool worker_scheduled {false};
  // This control wake is independent of the input packet queue. The timer uses
  // the same coalescing bit before enqueueing passthrough_next_message().
  EXPECT_TRUE(input::geometry::request_serialized_worker_wake(worker_scheduled));
  EXPECT_FALSE(input::geometry::request_serialized_worker_wake(worker_scheduled));
  worker_scheduled.store(false, std::memory_order_release);
  EXPECT_TRUE(input::geometry::request_serialized_worker_wake(worker_scheduled));
}

TEST(InputGeometryTransport, MissingOutputSampleInvalidatesKnownChangedGeometryBeforeRetry) {
  using namespace platf::dxgi::wgc_policy;
  platf::input_geometry_snapshot_store_t publisher {snapshot(41, 8, true)};
  bool mapping_valid = true;
  std::vector<std::string> order;

  const auto quarantined = invalidate_unverified_target_geometry_before_retry(
    true, mapping_valid, [&] {
      ASSERT_TRUE(publisher.invalidate());
      const auto current = publisher.snapshot();
      ASSERT_TRUE(current.has_value());
      EXPECT_FALSE(current->valid);
      EXPECT_EQ(current->capture_generation, 41u);
      EXPECT_EQ(current->geometry_sequence, 9u);
      order.emplace_back("invalidate old mapping");
    });

  EXPECT_TRUE(quarantined);
  EXPECT_FALSE(mapping_valid);
  order.emplace_back("retry output sampling");
  ASSERT_EQ(order.size(), 2u);
  EXPECT_EQ(order[0], "invalidate old mapping");
  EXPECT_EQ(order[1], "retry output sampling");

  // A verified observation may restore a nonterminal generation after retry.
  EXPECT_TRUE(publisher.publish_mapping(-1920, 0, 3840, 1080));
  const auto recovered = publisher.snapshot();
  ASSERT_TRUE(recovered.has_value());
  EXPECT_TRUE(recovered->valid);
  EXPECT_EQ(recovered->geometry_sequence, 10u);
  EXPECT_EQ(recovered->offset_x, -1920);

  bool invalidation_called = false;
  bool unrelated_mapping_valid = true;
  EXPECT_FALSE(invalidate_unverified_target_geometry_before_retry(false, unrelated_mapping_valid, [&] {
    invalidation_called = true;
    publisher.invalidate();
  }));
  EXPECT_TRUE(unrelated_mapping_valid);
  EXPECT_FALSE(invalidation_called);
  const auto still_valid = publisher.snapshot();
  ASSERT_TRUE(still_valid.has_value());
  EXPECT_TRUE(still_valid->valid);
}

TEST(InputGeometryTransport, MissingSampleQuarantinesMovedTargetInsideUnchangedDesktopWithVerifiedControl) {
  using namespace platf::dxgi::wgc_policy;

  // The selected output can move/reappear at a new rectangle inside the same
  // virtual-desktop envelope. Whole-desktop metrics alone still compare equal.
  const input_geometry_t captured_geometry {0, 0, 3840, 1080};
  const desktop_bounds_t unchanged_desktop {0, 0, 3840, 1080};
  const auto desktop_change = assess_input_geometry(captured_geometry, 0, 0, unchanged_desktop);
  EXPECT_EQ(desktop_change, input_geometry_change_e::unchanged);
  const bool geometry_observation_needed = desktop_change != input_geometry_change_e::unchanged;
  EXPECT_FALSE(geometry_observation_needed);

  output_target_identity_t captured_target {
    -7, 0x1234, L"\\\\.\\DISPLAY-A", 0x5678, true,
    0, 0, 1920, 1080, 1, true, false,
  };
  auto moved_target = captured_target;
  moved_target.rect_left = 1920;
  moved_target.rect_right = 3840;
  EXPECT_FALSE(matches_captured_target(captured_target, moved_target));

  auto old_map = snapshot(61, 4, true, 0, 0, 3840, 1080);
  old_map.target.rect_left = captured_target.rect_left;
  old_map.target.rect_top = captured_target.rect_top;
  old_map.target.rect_right = captured_target.rect_right;
  old_map.target.rect_bottom = captured_target.rect_bottom;
  platf::input_geometry_snapshot_store_t publisher {old_map};
  const auto before_sample_failure = publisher.snapshot();
  ASSERT_TRUE(before_sample_failure.has_value());
  EXPECT_TRUE(before_sample_failure->valid);
  EXPECT_EQ(before_sample_failure->offset_x, 0);
  bool mapping_valid = true;
  std::vector<std::string> order;
  // sample_output() is unavailable, so the monitor cannot distinguish a
  // transient enumeration failure from the selected output moving above.
  const auto quarantined = invalidate_unverified_target_geometry_before_retry(
    true, mapping_valid, [&] {
      EXPECT_TRUE(publisher.invalidate());
      order.emplace_back("invalidate old target map");
    });
  EXPECT_TRUE(quarantined);
  EXPECT_FALSE(mapping_valid);
  const auto invalid = publisher.snapshot();
  ASSERT_TRUE(invalid.has_value());
  EXPECT_FALSE(invalid->valid);
  EXPECT_EQ(invalid->geometry_sequence, 5u);
  order.emplace_back("retry output sampling");
  ASSERT_EQ(order.size(), 2u);
  EXPECT_EQ(order[0], "invalidate old target map");
  EXPECT_EQ(order[1], "retry output sampling");

  // Control: when sampling verifies the same target and unchanged desktop,
  // ordinary policy assessment is a no-op and the valid mapping stays intact.
  EXPECT_TRUE(matches_captured_target(captured_target, captured_target));
  const auto verified = assess_verified_geometry_update(
    active_capture_state_e::valid, verified_target_state_e::same, true,
    captured_geometry, 0, 0, unchanged_desktop);
  EXPECT_EQ(verified.action, geometry_update_action_e::unchanged);
  auto control_map = snapshot(62, 4, true, 0, 0, 3840, 1080);
  control_map.target.rect_left = captured_target.rect_left;
  control_map.target.rect_top = captured_target.rect_top;
  control_map.target.rect_right = captured_target.rect_right;
  control_map.target.rect_bottom = captured_target.rect_bottom;
  platf::input_geometry_snapshot_store_t control {control_map};
  const auto verified_snapshot = control.snapshot();
  ASSERT_TRUE(verified_snapshot.has_value());
  EXPECT_TRUE(verified_snapshot->valid);
  EXPECT_EQ(verified_snapshot->geometry_sequence, 4u);
}

TEST(InputGeometryTransport, TerminalCaptureGenerationRejectsDelayedPublisherAndFreshGenerationWorks) {
  platf::input_geometry_snapshot_store_t old_publisher {snapshot(52, 3, true)};
  std::mutex gate_mutex;
  std::condition_variable gate_cv;
  bool publisher_waiting = false;
  bool release_publisher = false;
  bool stale_publish_succeeded = true;

  std::thread delayed_publisher([&] {
    {
      std::unique_lock lock {gate_mutex};
      publisher_waiting = true;
      gate_cv.notify_all();
      gate_cv.wait(lock, [&] { return release_publisher; });
    }
    stale_publish_succeeded = old_publisher.publish_mapping(-500, -200, 3000, 1400);
  });

  {
    std::unique_lock lock {gate_mutex};
    gate_cv.wait(lock, [&] { return publisher_waiting; });
  }

  EXPECT_TRUE(old_publisher.terminate_generation());
  EXPECT_TRUE(old_publisher.terminal());
  {
    std::lock_guard lock {gate_mutex};
    release_publisher = true;
  }
  gate_cv.notify_all();
  delayed_publisher.join();

  EXPECT_FALSE(stale_publish_succeeded);
  const auto terminal = old_publisher.snapshot();
  ASSERT_TRUE(terminal.has_value());
  EXPECT_FALSE(terminal->valid);
  EXPECT_EQ(terminal->capture_generation, 52u);
  EXPECT_EQ(terminal->geometry_sequence, 4u);
  EXPECT_EQ(terminal->offset_x, 10);
  old_publisher.initialize(snapshot(99, 1, true));
  const auto still_terminal = old_publisher.snapshot();
  ASSERT_TRUE(still_terminal.has_value());
  EXPECT_EQ(still_terminal->capture_generation, 52u);
  EXPECT_FALSE(still_terminal->valid);

  // A replacement capture has a distinct store and generation; it can publish.
  platf::input_geometry_snapshot_store_t new_publisher {snapshot(53, 1, true)};
  EXPECT_FALSE(new_publisher.terminal());
  EXPECT_TRUE(new_publisher.publish_mapping(-1280, -100, 3200, 1300));
  const auto fresh = new_publisher.snapshot();
  ASSERT_TRUE(fresh.has_value());
  EXPECT_TRUE(fresh->valid);
  EXPECT_EQ(fresh->capture_generation, 53u);
  EXPECT_EQ(fresh->geometry_sequence, 2u);
  EXPECT_EQ(fresh->offset_x, -1280);
}
