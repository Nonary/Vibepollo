#include "src/platform/windows/display_helper_request_policy.h"

#include <gtest/gtest.h>

namespace policy = display_helper_integration::request_policy;

TEST(DisplayHelperRequestPolicy, VerifyOnlyExtendedTargetsNeverPrepareTopologyOrPrimary) {
  for (const auto layout : {policy::VirtualDisplayLayout::Extended, policy::VirtualDisplayLayout::ExtendedPrimary, policy::VirtualDisplayLayout::ExtendedIsolated, policy::VirtualDisplayLayout::ExtendedPrimaryIsolated}) {
    const auto result = policy::evaluate({
      .configuration_option = policy::ConfigurationOption::VerifyOnly,
      .layout = layout,
      .virtual_display = true,
      .target_device_id = "tablet",
      .topology_snapshot = {{"physical"}},
    });
    EXPECT_TRUE(result.dispatch);
    EXPECT_EQ(result.device_preparation, policy::DevicePreparation::VerifyOnly);
    EXPECT_TRUE(result.topology.empty());
  }
}

TEST(DisplayHelperRequestPolicy, ExplicitExclusiveAndAutomaticExtendedKeepTheirPolicies) {
  const auto exclusive = policy::evaluate({
    .configuration_option = policy::ConfigurationOption::VerifyOnly,
    .virtual_display = true,
    .target_device_id = "tablet",
  });
  EXPECT_EQ(exclusive.device_preparation, policy::DevicePreparation::EnsureOnlyDisplay);
  EXPECT_EQ(exclusive.topology, (std::vector<std::vector<std::string>> {{"tablet"}}));
  const auto extended = policy::evaluate({
    .configuration_option = policy::ConfigurationOption::EnsureActive,
    .layout = policy::VirtualDisplayLayout::Extended,
    .virtual_display = true,
    .target_device_id = "tablet",
    .topology_snapshot = {{"physical"}, {"peer"}},
  });
  EXPECT_EQ(extended.device_preparation, policy::DevicePreparation::EnsureActive);
  EXPECT_EQ(extended.topology, (std::vector<std::vector<std::string>> {{"physical"}, {"peer"}, {"tablet"}}));
}

TEST(DisplayHelperRequestPolicy, ExtendedMergeRetainsPeersAndRestoresMissingCloneMembers) {
  const auto merged = policy::merge_extended_topology(
    {{"PHYSICAL"}, {"peer"}}, {{"physical", "cloned"}, {"second"}}, "tablet"
  );
  EXPECT_EQ(merged, (std::vector<std::vector<std::string>> {{"PHYSICAL", "cloned"}, {"peer"}, {"second"}, {"tablet"}}));
  EXPECT_EQ(policy::merge_extended_topology(merged, {{"physical", "cloned"}, {"second"}}, "TABLET"), merged);
}
