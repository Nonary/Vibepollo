/**
 * @file tests/unit/platform/linux/test_private_display_mode_policy.cpp
 * @brief Tests for Linux private-display refresh matching.
 */
#include <gtest/gtest.h>
#include <limits>
#include <src/platform/linux/private_display_configuration_policy.h>
#include <src/platform/linux/private_display_diagnostic_policy.h>
#include <src/platform/linux/private_display_mode_policy.h>
#include <src/video_framerate_policy.h>

namespace policy = platf::linux_private_display::mode_policy;
namespace configuration = platf::linux_private_display::configuration_policy;
namespace diagnostic = platf::linux_private_display::diagnostic_policy;

TEST(VideoFrameratePolicy, ReportsTheEffectiveRateAfterProductionFallbackNormalization) {
  EXPECT_EQ(video::framerate_policy::effective_encoding_framerate_milli_fps(120000, 60), 120000);
  EXPECT_EQ(video::framerate_policy::effective_encoding_framerate_milli_fps(0, 60), 60000);
  EXPECT_EQ(video::framerate_policy::effective_encoding_framerate_milli_fps(-1, 90), 90000);
  EXPECT_EQ(video::framerate_policy::effective_encoding_framerate_milli_fps(0, 0), 60000);
}

TEST(LinuxPrivateDisplayDiagnosticPolicy, TreatsWrongTypedCurrentModeIdAsUnobserved) {
  nlohmann::json output = {
    {"connected", false},
    {"enabled", false},
    {"currentModeId", 42},
    {"modes", nlohmann::json::array()},
  };

  EXPECT_NO_THROW({
    EXPECT_FALSE(diagnostic::current_mode_id(output).has_value());
  });

  output["currentModeId"] = "mode-1";
  ASSERT_EQ(diagnostic::current_mode_id(output), std::optional<std::string> {"mode-1"});
  output["currentModeId"] = "";
  EXPECT_FALSE(diagnostic::current_mode_id(output).has_value());
  output.erase("currentModeId");
  EXPECT_FALSE(diagnostic::current_mode_id(output).has_value());
}

TEST(LinuxPrivateDisplayDiagnosticPolicy, TreatsMalformedModeSizeAsUnobserved) {
  nlohmann::json output = {
    {"currentModeId", "mode-1"},
    {"modes", nlohmann::json::array({
                 {{"id", 7}, {"size", {{"width", 2448}, {"height", 1848}}}},
                 {{"id", "mode-1"}, {"size", {{"width", -1}, {"height", 1848}}}},
               })},
  };

  EXPECT_NO_THROW({
    EXPECT_FALSE(diagnostic::current_mode_size(output, "mode-1").has_value());
  });

  output["modes"][1]["size"]["width"] = 2448;
  const std::pair<std::uint32_t, std::uint32_t> expected_size {2448, 1848};
  ASSERT_EQ(diagnostic::current_mode_size(output, "mode-1"), expected_size);
  output["modes"][1]["size"]["width"] = std::numeric_limits<std::uint64_t>::max();
  EXPECT_FALSE(diagnostic::current_mode_size(output, "mode-1").has_value());
}

TEST(LinuxPrivateDisplayConfigurationPolicy, RejectsKScreenFailureWithSuccessfulExit) {
  EXPECT_FALSE(configuration::command_succeeded(true, "applying config failed! The session is not active\n", "locale warning"));
  EXPECT_FALSE(configuration::command_succeeded(true, "", "applying config failed! rejected"));
  EXPECT_FALSE(configuration::command_succeeded(false, "", ""));
  EXPECT_TRUE(configuration::command_succeeded(true, "Output 1 set mode 1", "locale warning"));
}

TEST(LinuxPrivateDisplayModePolicy, AcceptsGlobalDrmNamesAndRejectsMalformedOrOutOfRangeNames) {
  for (const auto name : {"Virtual-1", "Virtual-2", "Virtual-3", "Virtual-4",
                         "Virtual-5", "Virtual-6", "Virtual-7", "Virtual-8",
                         "Virtual-9", "Virtual-10", "Virtual-2147483647"}) {
    EXPECT_TRUE(policy::managed_connector_name(name)) << name;
  }
  for (const auto name : {"", "Virtual-", "Virtual-0", "Virtual-01", "Virtual-2147483648",
                         "Virtual-5x", "Virtual-8\n", "Virtual-8/../1", "DP-1"}) {
    EXPECT_FALSE(policy::managed_connector_name(name)) << name;
  }
}

TEST(LinuxPrivateDisplayModePolicy, AcceptsFractionalEquivalentRefresh) {
  EXPECT_TRUE(policy::refresh_matches(59.95, 60.0));
  EXPECT_TRUE(policy::refresh_matches(119.88, 120.0));
  EXPECT_TRUE(policy::refresh_matches(240.0 * 1000.0 / 1001.0, 240.0));
  EXPECT_TRUE(policy::refresh_matches(480.0 * 1000.0 / 1001.0, 480.0));
}

TEST(LinuxPrivateDisplayModePolicy, RejectsNearestDifferentRefresh) {
  EXPECT_FALSE(policy::refresh_matches(59.95, 138.0));
  EXPECT_FALSE(policy::refresh_matches(120.0, 138.0));
}

TEST(LinuxPrivateDisplayModePolicy, RejectsInvalidAndToleranceBoundary) {
  EXPECT_FALSE(policy::refresh_matches(59.8, 60.0));
  EXPECT_FALSE(policy::refresh_matches(std::numeric_limits<double>::quiet_NaN(), 60.0));
}

TEST(LinuxPrivateDisplayModePolicy, PreservesExactClientModeIncludingFractionalRefresh) {
  const policy::requested_mode_t ninety_hz {3024, 1890, 90000};
  const policy::requested_mode_t mac {3024, 1890, 120000};
  const policy::requested_mode_t custom {3025, 1891, 119880};
  EXPECT_TRUE(ninety_hz.valid());
  EXPECT_TRUE(mac.valid());
  EXPECT_TRUE(custom.valid());
  EXPECT_EQ(custom.width, 3025u);
  EXPECT_EQ(custom.height, 1891u);
  EXPECT_EQ(custom.refresh_millihz, 119880u);
}

TEST(LinuxPrivateDisplayModePolicy, RefreshesManagedCatalogForEveryExactSessionRequest) {
  EXPECT_TRUE(policy::should_admit_requested_mode(true, true, false, true));
  EXPECT_FALSE(policy::should_admit_requested_mode(false, true, false, true));
  EXPECT_FALSE(policy::should_admit_requested_mode(true, false, false, true));
  EXPECT_FALSE(policy::should_admit_requested_mode(true, true, true, true));
  EXPECT_FALSE(policy::should_admit_requested_mode(true, true, false, false));
}

TEST(LinuxPrivateDisplayModePolicy, RejectsModesOutsideDriverLimits) {
  EXPECT_FALSE((policy::requested_mode_t {63, 1890, 120000}).valid());
  EXPECT_FALSE((policy::requested_mode_t {3024, 8193, 120000}).valid());
  EXPECT_FALSE((policy::requested_mode_t {3024, 1890, 999}).valid());
  EXPECT_FALSE((policy::requested_mode_t {3024, 1890, 1000001}).valid());
  EXPECT_TRUE((policy::requested_mode_t {64, 8192, 1000000}).valid());
}

TEST(LinuxPrivateDisplayModePolicy, RecognizesKScreenFailuresDespiteSuccessfulExitStatus) {
  EXPECT_TRUE(policy::doctor_reported_failure("Unable to parse arguments: output.Virtual-1.addCustomMode.3024.1890.120000.reduced\n"));
  EXPECT_TRUE(policy::doctor_reported_failure("applying config failed! The request was rejected\n"));
  EXPECT_TRUE(policy::doctor_reported_failure("kscreen.doctor: Invalid config.\n"));
  EXPECT_FALSE(policy::doctor_reported_failure("kscreen.doctor: Set output position QPoint(0,0)\n"));
}

TEST(LinuxPrivateDisplayDiagnosticPolicy, SeparatesPrimaryRetryAndHdrOutcomes) {
  diagnostic::trace_t trace;
  diagnostic::record_configuration_result(trace, false, diagnostic::configuration_e::primary);
  diagnostic::record_configuration_result(trace, true, diagnostic::configuration_e::isolated_layout_retry);
  diagnostic::record_configuration_result(trace, false, diagnostic::configuration_e::hdr_activation);

  EXPECT_EQ(trace.primary, diagnostic::configuration_result_e::failed);
  EXPECT_EQ(trace.isolated_layout_retry, diagnostic::configuration_result_e::succeeded);
  EXPECT_EQ(trace.hdr_activation, diagnostic::configuration_result_e::failed);
}

TEST(LinuxPrivateDisplayDiagnosticPolicy, ClassifiesCurrentSettingsFallbackFromObservedOutput) {
  diagnostic::readback_facts_t facts {
    .query_available = true,
    .requested_mode_known = true,
    .output_present = true,
    .connection_observed = true,
    .output_connected = true,
    .enabled_observed = true,
    .output_enabled = true,
    .mode_id_observed = true,
    .size_required = true,
    .size_observed = true,
    .refresh_required = true,
    .refresh_expected = true,
    .refresh_observed = true,
    .hdr_required = true,
    .hdr_observed = true,
    .mode_id_matches = false,
  };
  EXPECT_EQ(diagnostic::classify_readback(facts), diagnostic::readback_result_e::mode_mismatch);
  EXPECT_EQ(diagnostic::fallback_choice(true), diagnostic::fallback_e::current_settings);

  facts.mode_id_matches = true;
  facts.size_matches = false;
  EXPECT_EQ(diagnostic::classify_readback(facts), diagnostic::readback_result_e::size_mismatch);
  facts.size_matches = true;
  facts.refresh_matches = false;
  EXPECT_EQ(diagnostic::classify_readback(facts), diagnostic::readback_result_e::refresh_mismatch);
  facts.refresh_matches = true;
  facts.hdr_matches = false;
  EXPECT_EQ(diagnostic::classify_readback(facts), diagnostic::readback_result_e::hdr_mismatch);
}

TEST(LinuxPrivateDisplayDiagnosticPolicy, FailedFreshReadbackReplacesAnEarlierMatch) {
  diagnostic::trace_t trace;
  diagnostic::readback_facts_t matched {
    .query_available = true,
    .requested_mode_known = true,
    .output_present = true,
    .connection_observed = true,
    .output_connected = true,
    .enabled_observed = true,
    .output_enabled = true,
    .mode_id_observed = true,
  };
  diagnostic::record_readback(trace, matched);
  EXPECT_EQ(trace.readback, diagnostic::readback_result_e::matched);

  // Negative control for the old stale-snapshot bug: the next failed query
  // must describe unavailable fresh data, not preserve the previous match.
  diagnostic::begin_fallback(trace);
  EXPECT_EQ(trace.pre_fallback_readback, diagnostic::readback_result_e::matched);
  EXPECT_EQ(trace.readback, diagnostic::readback_result_e::not_observed);
  diagnostic::readback_facts_t unavailable;
  diagnostic::record_readback(trace, unavailable, true);
  EXPECT_EQ(trace.fallback_readback, diagnostic::readback_result_e::unavailable);
  EXPECT_EQ(trace.readback, diagnostic::readback_result_e::unavailable);
  EXPECT_EQ(trace.pre_fallback_readback, diagnostic::readback_result_e::matched);
}

TEST(LinuxPrivateDisplayDiagnosticPolicy, DistinguishesNoCaptureFromUnappliedMode) {
  diagnostic::readback_facts_t facts {
    .query_available = true,
    .requested_mode_known = false,
    .output_present = true,
    .connection_observed = true,
    .output_connected = true,
    .enabled_observed = true,
    .output_enabled = true,
  };
  EXPECT_EQ(diagnostic::classify_readback(facts), diagnostic::readback_result_e::not_observed);
  EXPECT_EQ(diagnostic::fallback_choice(false), diagnostic::fallback_e::no_capture_output);
}

TEST(LinuxPrivateDisplayDiagnosticPolicy, MatchesSelectedModeWithoutRequestedResolution) {
  diagnostic::readback_facts_t facts {
    .query_available = true,
    .requested_mode_known = true,
    .output_present = true,
    .connection_observed = true,
    .output_connected = true,
    .enabled_observed = true,
    .output_enabled = true,
    .mode_id_observed = true,
  };

  EXPECT_EQ(diagnostic::classify_readback(facts), diagnostic::readback_result_e::matched);
}

TEST(LinuxPrivateDisplayDiagnosticPolicy, UsesSelectedRateInsteadOfHighestRefreshSentinel) {
  const auto expected = diagnostic::expected_refresh_hz(10000.0, 143.98, true);

  ASSERT_TRUE(expected.has_value());
  EXPECT_DOUBLE_EQ(*expected, 143.98);
  EXPECT_FALSE(policy::refresh_matches(143.98, 10000.0));
  EXPECT_TRUE(policy::refresh_matches(143.98, *expected));
}

TEST(LinuxPrivateDisplayDiagnosticPolicy, MarksMissingExpectedFieldsIncomplete) {
  diagnostic::readback_facts_t facts {
    .query_available = true,
    .requested_mode_known = true,
    .output_present = true,
    .connection_observed = true,
    .output_connected = true,
    .enabled_observed = true,
    .output_enabled = true,
    .mode_id_observed = true,
    .size_required = true,
    .size_observed = true,
    .refresh_required = true,
    .refresh_expected = true,
    .refresh_observed = true,
    .scale_required = true,
    .scale_observed = false,
    .hdr_required = true,
    .hdr_observed = false,
  };

  EXPECT_EQ(diagnostic::classify_readback(facts), diagnostic::readback_result_e::incomplete);
  facts.scale_observed = true;
  facts.hdr_observed = true;
  EXPECT_EQ(diagnostic::classify_readback(facts), diagnostic::readback_result_e::matched);
  facts.refresh_expected = false;
  EXPECT_EQ(diagnostic::classify_readback(facts), diagnostic::readback_result_e::incomplete);
}
