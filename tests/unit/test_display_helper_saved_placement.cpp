#include <gtest/gtest.h>

#include "src/display_helper_saved_placement.h"

namespace {
  using display_helper_integration::DisplayApplyBuilder;
  using display_device::SingleDisplayConfiguration;
  using Prep = SingleDisplayConfiguration::DevicePreparation;

  nlohmann::json layout(const nlohmann::json &placement) {
    return {{"version", 1}, {"placements", {{"client", placement}}}};
  }

  DisplayApplyBuilder builder_with_live_positions() {
    DisplayApplyBuilder builder;
    SingleDisplayConfiguration config;
    config.m_device_id = "virtual";
    config.m_device_prep = Prep::EnsureActive;
    builder.set_configuration(config);
    builder.mutable_topology().monitor_positions["virtual"] = {2000, 300};
    builder.mutable_topology().monitor_positions["physical"] = {0, 0};
    return builder;
  }
}

TEST(DisplayHelperSavedPlacement, DetachedPrimaryAppliesToActualExtendedRequestWithoutMovingTarget) {
  remote_display_topology::coordinator_t coordinator;
  const nlohmann::json placement {{"anchor_kind", "client"}, {"anchor_id", "old"}, {"edge", "below"}, {"alignment", "center"}, {"gap_px", 0}, {"primary", true}};
  coordinator.set_layout(remote_display_topology::remove_client_from_layout(layout(placement), "old"));
  const remote_display_topology::node_t target {.id = "client", .device_id = "virtual", .active = true, .x = 2000, .y = 300};
  const auto saved = coordinator.saved_stream_placement(target, {});
  ASSERT_FALSE(saved.position);
  ASSERT_TRUE(saved.primary_requested);

  auto builder = builder_with_live_positions();
  display_helper_integration::apply_saved_stream_layout(builder, saved, target.device_id, false);
  const auto request = builder.build();
  ASSERT_TRUE(request.configuration);
  EXPECT_EQ(request.configuration->m_device_prep, Prep::EnsurePrimary);
  EXPECT_EQ(request.topology.monitor_positions.at("virtual").m_x, 2000);
  EXPECT_EQ(request.topology.monitor_positions.at("virtual").m_y, 300);
  EXPECT_EQ(request.topology.monitor_positions.at("physical").m_x, 0);
}

TEST(DisplayHelperSavedPlacement, DetachedFallbackDoesNotOverrideIsolatedOrManualRules) {
  remote_display_topology::coordinator_t coordinator;
  const nlohmann::json detached {{"anchor_kind", "physical"}, {"anchor_id", ""}, {"edge", "right"}, {"alignment", "center"}, {"gap_px", 0}, {"primary", true}, {"preserve", true}, {"automatic_fallback", true}};
  coordinator.set_layout(layout(detached));
  const remote_display_topology::node_t target {.id = "client", .device_id = "virtual", .active = true, .x = 2000, .y = 300};
  auto isolated = builder_with_live_positions();
  display_helper_integration::apply_saved_stream_layout(isolated, coordinator.saved_stream_placement(target, {}), target.device_id, true);
  EXPECT_EQ(isolated.build().configuration->m_device_prep, Prep::EnsureActive);
  EXPECT_EQ(isolated.build().topology.monitor_positions.at("virtual").m_x, 2000);

  auto manual_rule = detached;
  manual_rule.erase("automatic_fallback");
  coordinator.set_layout(layout(manual_rule));
  const auto manual = coordinator.saved_stream_placement(target, {});
  ASSERT_TRUE(manual.position);
  EXPECT_FALSE(manual.primary_requested);
  auto manual_builder = builder_with_live_positions();
  display_helper_integration::apply_saved_stream_layout(manual_builder, manual, target.device_id, true);
  EXPECT_EQ(manual_builder.build().configuration->m_device_prep, Prep::EnsureActive);
  EXPECT_EQ(manual_builder.build().topology.monitor_positions.at("virtual").m_x, 2000);
}

TEST(DisplayHelperSavedPlacement, DirectionalPrimaryStillOverridesPositionInIsolatedRequest) {
  remote_display_topology::coordinator_t coordinator;
  const nlohmann::json placement {{"anchor_kind", "physical"}, {"anchor_id", "physical"}, {"edge", "right"}, {"alignment", "start"}, {"gap_px", 10}, {"primary", true}};
  coordinator.set_layout(layout(placement));
  const remote_display_topology::node_t target {.id = "client", .device_id = "virtual", .active = true, .x = 2000, .y = 300};
  const remote_display_topology::node_t physical {.id = "physical", .physical = true, .active = true, .configured_mode = {1920, 1080, 60}};
  auto builder = builder_with_live_positions();
  display_helper_integration::apply_saved_stream_layout(builder, coordinator.saved_stream_placement(target, {physical}), target.device_id, true);
  const auto request = builder.build();
  EXPECT_EQ(request.configuration->m_device_prep, Prep::EnsurePrimary);
  EXPECT_EQ(request.topology.monitor_positions.at("virtual").m_x, 1930);
}

TEST(DisplayHelperSavedPlacement, DetachedNonPrimaryAndMissingConfigurationDoNotCreatePrimaryRequests) {
  remote_display_topology::coordinator_t coordinator;
  const nlohmann::json detached {{"anchor_kind", "physical"}, {"anchor_id", ""}, {"edge", "right"}, {"alignment", "center"}, {"gap_px", 0}, {"primary", false}, {"preserve", true}, {"automatic_fallback", true}};
  coordinator.set_layout(layout(detached));
  const remote_display_topology::node_t target {.id = "client", .device_id = "virtual", .active = true};
  auto builder = builder_with_live_positions();
  display_helper_integration::apply_saved_stream_layout(builder, coordinator.saved_stream_placement(target, {}), target.device_id, false);
  EXPECT_EQ(builder.build().configuration->m_device_prep, Prep::EnsureActive);
  EXPECT_EQ(builder.build().topology.monitor_positions.at("virtual").m_x, 2000);

  auto primary = detached;
  primary["primary"] = true;
  coordinator.set_layout(layout(primary));
  DisplayApplyBuilder no_config;
  display_helper_integration::apply_saved_stream_layout(no_config, coordinator.saved_stream_placement(target, {}), target.device_id, false);
  EXPECT_FALSE(no_config.build().configuration);
  EXPECT_TRUE(no_config.build().topology.monitor_positions.empty());
}
