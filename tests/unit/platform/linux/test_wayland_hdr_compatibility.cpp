/**
 * @file tests/unit/platform/linux/test_wayland_hdr_compatibility.cpp
 * @brief Resolved-stream tests for the optional Wayland HDR launch policy.
 */
#include "../../../tests_common.h"

#include <src/platform/linux/wayland_hdr_compatibility.h>

namespace compatibility = platf::wayland_hdr_compatibility;

TEST(WaylandHdrCompatibility, ManagedWaylandSessionDoesNotDependOnHostDisplayEnvironment) {
  EXPECT_TRUE(compatibility::selected_session_is_wayland(false, "wayland"));
  EXPECT_TRUE(compatibility::selected_session_is_wayland(true, ""));
  EXPECT_FALSE(compatibility::selected_session_is_wayland(false, ""));
  EXPECT_FALSE(compatibility::selected_session_is_wayland(false, "x11"));
}

TEST(WaylandHdrCompatibility, DisabledOptionDoesNotChangeHdrLaunches) {
  const auto result = compatibility::resolve(false, true, true);
  EXPECT_FALSE(result.enabled);
  EXPECT_EQ(result.suppression_reason, compatibility::suppression_reason_e::disabled);
}

TEST(WaylandHdrCompatibility, ResolvedWaylandHdrEnablesCompatibility) {
  const auto result = compatibility::resolve(true, true, true);
  EXPECT_TRUE(result.enabled);
  EXPECT_EQ(result.suppression_reason, compatibility::suppression_reason_e::none);
}

TEST(WaylandHdrCompatibility, AutoModeResolvingToSdrDoesNotEnableCompatibility) {
  const auto result = compatibility::resolve(true, true, false);
  EXPECT_FALSE(result.enabled);
  EXPECT_EQ(result.suppression_reason, compatibility::suppression_reason_e::resolved_sdr);
}

TEST(WaylandHdrCompatibility, Prefer10BitSdrDoesNotCountAsHdr) {
  // The caller has already resolved the color-mode policy. A 10-bit SDR
  // result must be supplied as false rather than inferred from its bit depth.
  const auto result = compatibility::resolve(true, true, false);
  EXPECT_FALSE(result.enabled);
  EXPECT_EQ(result.suppression_reason, compatibility::suppression_reason_e::resolved_sdr);
}

TEST(WaylandHdrCompatibility, ForceHdrOffDoesNotEnableCompatibility) {
  // Force HDR off is likewise resolved before this policy is consulted.
  const auto result = compatibility::resolve(true, true, false);
  EXPECT_FALSE(result.enabled);
  EXPECT_EQ(result.suppression_reason, compatibility::suppression_reason_e::resolved_sdr);
}

TEST(WaylandHdrCompatibility, NonWaylandSessionDoesNotChangeLaunchEnvironment) {
  const auto result = compatibility::resolve(true, false, true);
  EXPECT_FALSE(result.enabled);
  EXPECT_EQ(result.suppression_reason, compatibility::suppression_reason_e::not_wayland);
}
