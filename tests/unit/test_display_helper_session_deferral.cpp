/**
 * @file tests/unit/test_display_helper_session_deferral.cpp
 * @brief Unit tests for Sunshine display helper session deferral.
 */
#include "../tests_common.h"
#include "src/platform/windows/display_helper_request_helpers.h"
#include "src/platform/windows/display_helper_session_deferral.h"
#include "src/rtsp.h"

namespace {
  class FakeClock {
  public:
    std::chrono::steady_clock::time_point now() const {
      return now_;
    }

    void advance(std::chrono::milliseconds duration) {
      now_ += duration;
    }

  private:
    std::chrono::steady_clock::time_point now_ {std::chrono::steady_clock::now()};
  };

  display_helper_integration::DisplayApplyRequest make_request(rtsp_stream::launch_session_t &session) {
    display_helper_integration::DisplayApplyRequest request;
    request.action = display_helper_integration::DisplayApplyAction::Apply;
    request.session = &session;
    request.configuration = display_device::SingleDisplayConfiguration {};
    return request;
  }

  rtsp_stream::launch_session_t make_virtual_display_session() {
    rtsp_stream::launch_session_t session {};
    session.virtual_display = true;
    session.virtual_display_device_id = "{virtual-device}";
    session.width = 1920;
    session.height = 1080;
    session.fps = 60;
    return session;
  }
}  // namespace

TEST(DisplayHelperSessionDeferral, DelaysAndRestoresSessionSnapshot) {
  FakeClock clock;
  display_helper_integration::SessionDeferralManager manager([&clock]() {
    return clock.now();
  });

  rtsp_stream::launch_session_t session {};
  session.id = 42;
  session.width = 1920;
  session.height = 1080;
  session.fps = 60;
  session.enable_hdr = true;
  session.enable_sops = true;
  session.virtual_display = true;
  session.virtual_display_device_id = "VD";
  session.framegen_refresh_rate = 120;
  session.gen1_framegen_fix = true;
  session.gen2_framegen_fix = false;

  manager.set_pending(make_request(session));

  auto result = manager.take_ready(false);
  EXPECT_EQ(result.status, display_helper_integration::SessionDeferralManager::TakeStatus::SessionNotReady);

  result = manager.take_ready(true);
  EXPECT_EQ(result.status, display_helper_integration::SessionDeferralManager::TakeStatus::DelayStarted);

  clock.advance(display_helper_integration::SessionDeferralManager::initial_delay() - std::chrono::milliseconds(1));
  result = manager.take_ready(true);
  EXPECT_EQ(result.status, display_helper_integration::SessionDeferralManager::TakeStatus::DelayPending);

  clock.advance(std::chrono::milliseconds(1));
  result = manager.take_ready(true);
  ASSERT_EQ(result.status, display_helper_integration::SessionDeferralManager::TakeStatus::Ready);
  ASSERT_TRUE(result.pending.has_value());

  const auto &snapshot = result.pending->session_snapshot;
  EXPECT_EQ(snapshot.width, 1920);
  EXPECT_EQ(snapshot.height, 1080);
  EXPECT_EQ(snapshot.fps, 60);
  EXPECT_TRUE(snapshot.enable_hdr);
  EXPECT_TRUE(snapshot.enable_sops);
  EXPECT_TRUE(snapshot.virtual_display);
  EXPECT_EQ(snapshot.virtual_display_device_id, "VD");
  ASSERT_TRUE(snapshot.framegen_refresh_rate.has_value());
  EXPECT_EQ(*snapshot.framegen_refresh_rate, 120);
  EXPECT_TRUE(snapshot.gen1_framegen_fix);
  EXPECT_FALSE(snapshot.gen2_framegen_fix);
}

TEST(DisplayHelperSessionDeferral, ReschedulesAndDropsForNewerPending) {
  FakeClock clock;
  display_helper_integration::SessionDeferralManager manager([&clock]() {
    return clock.now();
  });

  rtsp_stream::launch_session_t session {};
  session.id = 1;
  manager.set_pending(make_request(session));

  auto result = manager.take_ready(true);
  EXPECT_EQ(result.status, display_helper_integration::SessionDeferralManager::TakeStatus::DelayStarted);

  clock.advance(display_helper_integration::SessionDeferralManager::initial_delay());
  result = manager.take_ready(true);
  ASSERT_EQ(result.status, display_helper_integration::SessionDeferralManager::TakeStatus::Ready);
  ASSERT_TRUE(result.pending.has_value());

  auto reschedule = manager.reschedule(*result.pending);
  EXPECT_TRUE(reschedule.requeued);
  EXPECT_EQ(reschedule.delay, display_helper_integration::SessionDeferralManager::retry_delay(1));

  clock.advance(reschedule.delay);
  result = manager.take_ready(true);
  ASSERT_EQ(result.status, display_helper_integration::SessionDeferralManager::TakeStatus::Ready);
  ASSERT_TRUE(result.pending.has_value());

  rtsp_stream::launch_session_t newer_session {};
  newer_session.id = 2;
  manager.set_pending(make_request(newer_session));

  reschedule = manager.reschedule(*result.pending);
  EXPECT_TRUE(reschedule.dropped_for_newer);
}

TEST(DisplayHelperRequestHelpers, SkipsPhysicalOutputWhenDisplayConfigurationDisabled) {
  config::video_t video_config {};
  video_config.dd.configuration_option = config::video_t::dd_t::config_option_e::disabled;

  rtsp_stream::launch_session_t session {};
  session.output_name_override = "{physical-monitor-guid}";

  auto request = display_helper_integration::helpers::build_request_from_session(video_config, session);

  EXPECT_FALSE(request.has_value());
}

TEST(DisplayHelperRequestHelpers, AppliesExclusiveVirtualDisplayWhenDisplayConfigurationDisabled) {
  config::video_t video_config {};
  video_config.dd.configuration_option = config::video_t::dd_t::config_option_e::disabled;
  video_config.output_name = "{virtual-device}";
  video_config.virtual_display_layout = config::video_t::virtual_display_layout_e::exclusive;

  rtsp_stream::launch_session_t session {};
  session.virtual_display = true;
  session.virtual_display_device_id = "{virtual-device}";
  session.width = 1920;
  session.height = 1080;
  session.fps = 60;

  display_helper_integration::helpers::SessionDisplayConfigurationHelper helper(video_config, session);

  auto request = display_helper_integration::helpers::build_request_from_session(video_config, session);

  ASSERT_TRUE(request.has_value());
  ASSERT_TRUE(request->configuration.has_value());
  EXPECT_EQ(
    request->configuration->m_device_prep,
    display_device::SingleDisplayConfiguration::DevicePreparation::EnsureOnlyDisplay
  );
}

TEST(DisplayHelperRequestHelpers, InitialVirtualDisplayConfigurationPreservesHdrWhenDisabled) {
  config::video_t video_config {};
  video_config.dd.configuration_option = config::video_t::dd_t::config_option_e::disabled;
  video_config.dd.hdr_option = config::video_t::dd_t::hdr_option_e::disabled;
  video_config.virtual_display_layout = config::video_t::virtual_display_layout_e::exclusive;

  auto session = make_virtual_display_session();
  session.virtual_display = false;
  session.enable_hdr = true;

  display_helper_integration::helpers::SessionDisplayConfigurationHelper helper(video_config, session, true);
  const auto initial_configuration = helper.initial_virtual_display_configuration();

  ASSERT_TRUE(initial_configuration.has_value());
  EXPECT_FALSE(initial_configuration->m_hdr_state.has_value());
}

TEST(DisplayHelperRequestHelpers, SkipsExtendedVirtualDisplayWhenDisplayConfigurationDisabled) {
  config::video_t video_config {};
  video_config.dd.configuration_option = config::video_t::dd_t::config_option_e::disabled;
  video_config.output_name = "{virtual-device}";
  video_config.virtual_display_layout = config::video_t::virtual_display_layout_e::extended;

  rtsp_stream::launch_session_t session {};
  session.virtual_display = true;
  session.virtual_display_device_id = "{virtual-device}";
  session.width = 1920;
  session.height = 1080;
  session.fps = 60;

  display_helper_integration::helpers::SessionDisplayConfigurationHelper helper(video_config, session);
  EXPECT_FALSE(helper.initial_virtual_display_resolution().has_value());

  auto request = display_helper_integration::helpers::build_request_from_session(video_config, session);

  EXPECT_FALSE(request.has_value());
}

TEST(DisplayHelperRequestHelpers, PhysicalOutputDoesNotPinSingleDisplayTopology) {
  config::video_t video_config {};
  video_config.dd.configuration_option = config::video_t::dd_t::config_option_e::ensure_active;
  video_config.output_name = "{physical-monitor-guid}";

  rtsp_stream::launch_session_t session {};
  session.output_name_override = "{physical-monitor-guid}";

  auto request = display_helper_integration::helpers::build_request_from_session(video_config, session);

  ASSERT_TRUE(request.has_value());
  EXPECT_TRUE(request->topology.topology.empty());
}

TEST(DisplayHelperRequestHelpers, PhysicalOutputEnsureOnlyDisplayPinsTopology) {
  config::video_t video_config {};
  video_config.dd.configuration_option = config::video_t::dd_t::config_option_e::ensure_only_display;
  video_config.output_name = "{physical-monitor-guid}";

  rtsp_stream::launch_session_t session {};
  session.output_name_override = "{physical-monitor-guid}";

  auto request = display_helper_integration::helpers::build_request_from_session(video_config, session);

  ASSERT_TRUE(request.has_value());
  const std::vector<std::vector<std::string>> expected_topology {{"{physical-monitor-guid}"}};
  EXPECT_EQ(request->topology.topology, expected_topology);
}

TEST(DisplayHelperRequestHelpers, ExclusiveVirtualDisplayWithoutSnapshotPinsSingleDisplayTopology) {
  config::video_t video_config {};
  video_config.dd.configuration_option = config::video_t::dd_t::config_option_e::ensure_active;
  video_config.output_name = "{virtual-device}";
  video_config.virtual_display_layout = config::video_t::virtual_display_layout_e::exclusive;

  auto session = make_virtual_display_session();

  auto request = display_helper_integration::helpers::build_request_from_session(video_config, session);

  ASSERT_TRUE(request.has_value());
  const std::vector<std::vector<std::string>> expected_topology {{"{virtual-device}"}};
  EXPECT_EQ(request->topology.topology, expected_topology);
}

TEST(DisplayHelperRequestHelpers, ExtendedVirtualDisplayWithoutSnapshotDoesNotPinSingleDisplayTopology) {
  config::video_t video_config {};
  video_config.dd.configuration_option = config::video_t::dd_t::config_option_e::ensure_active;
  video_config.output_name = "{virtual-device}";

  const config::video_t::virtual_display_layout_e layouts[] = {
    config::video_t::virtual_display_layout_e::extended,
    config::video_t::virtual_display_layout_e::extended_primary,
    config::video_t::virtual_display_layout_e::extended_isolated,
    config::video_t::virtual_display_layout_e::extended_primary_isolated,
  };

  for (const auto layout : layouts) {
    video_config.virtual_display_layout = layout;
    auto session = make_virtual_display_session();

    auto request = display_helper_integration::helpers::build_request_from_session(video_config, session);

    ASSERT_TRUE(request.has_value());
    EXPECT_TRUE(request->topology.topology.empty()) << "layout=" << static_cast<int>(layout);
  }
}

TEST(DisplayHelperRequestHelpers, ExtendedVirtualDisplayMergesSnapshotWithVirtualDisplay) {
  config::video_t video_config {};
  video_config.dd.configuration_option = config::video_t::dd_t::config_option_e::ensure_active;
  video_config.output_name = "{virtual-device}";
  video_config.virtual_display_layout = config::video_t::virtual_display_layout_e::extended;

  auto session = make_virtual_display_session();
  session.virtual_display_topology_snapshot = std::vector<std::vector<std::string>> {{"{physical-monitor-guid}"}};

  auto request = display_helper_integration::helpers::build_request_from_session(video_config, session);

  ASSERT_TRUE(request.has_value());
  const std::vector<std::vector<std::string>> expected_topology {
    {"{physical-monitor-guid}"},
    {"{virtual-device}"},
  };
  EXPECT_EQ(request->topology.topology, expected_topology);
}

TEST(DisplayHelperRequestHelpers, UsesRemappedVirtualDisplayResolutionForSessionOverrides) {
  config::video_t video_config {};
  video_config.dd.configuration_option = config::video_t::dd_t::config_option_e::ensure_only_display;
  video_config.dd.resolution_option = config::video_t::dd_t::resolution_option_e::automatic;
  video_config.dd.refresh_rate_option = config::video_t::dd_t::refresh_rate_option_e::automatic;
  video_config.dd.mode_remapping.mixed = {
    {
      .requested_resolution = "2560x1440",
      .requested_fps = "",
      .final_resolution = "3840x2160",
      .final_refresh_rate = ""
    }
  };

  rtsp_stream::launch_session_t session {};
  session.virtual_display = true;
  session.virtual_display_device_id = "{virtual-device}";
  session.width = 2560;
  session.height = 1440;
  session.fps = 120;

  display_helper_integration::helpers::SessionDisplayConfigurationHelper helper(video_config, session);
  auto initial_resolution = helper.initial_virtual_display_resolution();
  ASSERT_TRUE(initial_resolution.has_value());
  EXPECT_EQ(initial_resolution->m_width, 3840);
  EXPECT_EQ(initial_resolution->m_height, 2160);

  auto request = display_helper_integration::helpers::build_request_from_session(video_config, session);

  ASSERT_TRUE(request.has_value());
  ASSERT_TRUE(request->session_overrides.width_override.has_value());
  ASSERT_TRUE(request->session_overrides.height_override.has_value());
  EXPECT_EQ(*request->session_overrides.width_override, 3840);
  EXPECT_EQ(*request->session_overrides.height_override, 2160);
  ASSERT_TRUE(request->configuration.has_value());
  ASSERT_TRUE(request->configuration->m_resolution.has_value());
  EXPECT_EQ(request->configuration->m_resolution->m_width, 3840);
  EXPECT_EQ(request->configuration->m_resolution->m_height, 2160);
}
