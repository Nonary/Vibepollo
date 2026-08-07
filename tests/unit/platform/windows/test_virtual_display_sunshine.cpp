/**
 * @file tests/unit/platform/windows/test_virtual_display_sunshine.cpp
 * @brief Pure virtual-display identity, session, and capture policy tests.
 */
#include <gtest/gtest.h>

#ifdef _WIN32
  #include <src/platform/windows/virtual_display.h>
  #include <src/platform/windows/wgc_capture_policy.h>

  #include <array>
  #include <cstring>
  #include <filesystem>
  #include <fstream>
  #include <sstream>
  #include <string>

namespace {
  constexpr GUID kClientGuid {
    0x1d6f6f2a,
    0x4f29,
    0x41b2,
    {0x95, 0x8f, 0x6f, 0x01, 0xd7, 0x58, 0x3f, 0x4b}
  };

  constexpr GUID kOtherClientGuid {
    0x9528c3cc,
    0x0ec0,
    0x477a,
    {0x9b, 0x7a, 0x79, 0x45, 0x0b, 0x81, 0x2d, 0x60}
  };

  std::string read_source(const std::filesystem::path &relative_path) {
    const auto path = std::filesystem::path {SUNSHINE_SOURCE_DIR} / relative_path;
    std::ifstream file {path, std::ios::binary};
    if (!file) {
      ADD_FAILURE() << "Failed to open " << path.string();
      return {};
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
  }

  void expect_contains(const std::string &content, const std::string &needle) {
    EXPECT_NE(content.find(needle), std::string::npos) << "missing: " << needle;
  }
}  // namespace

TEST(SunshineVirtualDisplay, ClientUuidDisplayIdIsStableAndNonZero) {
  const auto first = VDISPLAY::client_uuid_to_virtual_display_id(kClientGuid);
  EXPECT_NE(first, 0u);
  EXPECT_EQ(first, VDISPLAY::client_uuid_to_virtual_display_id(kClientGuid));
}

TEST(SunshineVirtualDisplay, RecommendedScaleTracksTheShortResolutionEdge) {
  EXPECT_EQ(VDISPLAY::effective_virtual_display_scale_percent(-1, 1920, 1080), 125u);
  EXPECT_EQ(VDISPLAY::effective_virtual_display_scale_percent(-1, 2560, 1440), 175u);
  EXPECT_EQ(VDISPLAY::effective_virtual_display_scale_percent(-1, 3840, 2160), 250u);
  EXPECT_EQ(VDISPLAY::effective_virtual_display_scale_percent(-1, 3440, 1440), 175u);
  EXPECT_EQ(VDISPLAY::effective_virtual_display_scale_percent(-1, 1440, 2560), 175u);
}

TEST(SunshineVirtualDisplay, ConfiguredScalePreservesAutomaticAndExactValues) {
  EXPECT_EQ(VDISPLAY::effective_virtual_display_scale_percent(0, 3840, 2160), 0u);
  EXPECT_EQ(VDISPLAY::effective_virtual_display_scale_percent(200, 1920, 1080), 200u);
}

TEST(SunshineVirtualDisplay, PerClientDisplayIdsDifferByClientUuid) {
  EXPECT_NE(
    VDISPLAY::client_uuid_to_virtual_display_id(kClientGuid),
    VDISPLAY::client_uuid_to_virtual_display_id(kOtherClientGuid)
  );
}

TEST(SunshineVirtualDisplay, StableVirtualDisplayUuidKeepsCanonicalUuidBytes) {
  const std::string client_uuid = "1d6f6f2a-4f29-41b2-958f-6f01d7583f4b";
  EXPECT_EQ(VDISPLAY::virtualDisplayUuidFromStableId(client_uuid), uuid_util::uuid_t::parse(client_uuid));
}

TEST(SunshineVirtualDisplay, RecoveryJournalRawUuidRoundTripsDriverGuidBytes) {
  constexpr std::string_view raw_guid = "EAADBAA7-AFE9-2232-FF17-F29CD76380DD";
  const auto parsed = uuid_util::uuid_t::parse_raw(std::string {raw_guid});
  EXPECT_EQ(parsed.string(), raw_guid);
  EXPECT_FALSE(parsed == uuid_util::uuid_t::parse(std::string {raw_guid}));
}

TEST(SunshineVirtualDisplay, RecoveryJournalRawUuidRejectsMalformedString) {
  EXPECT_THROW(
    uuid_util::uuid_t::parse_raw("EAADBAA7-AFE9-2232-FF17-F29CD76380DG"),
    std::invalid_argument
  );
}

TEST(SunshineVirtualDisplay, StableVirtualDisplayUuidDerivesNonCanonicalClientId) {
  const auto first = VDISPLAY::virtualDisplayUuidFromStableId("0123456789ABCDEF");
  EXPECT_EQ(first, VDISPLAY::virtualDisplayUuidFromStableId("0123456789ABCDEF"));
  EXPECT_NE(first, VDISPLAY::virtualDisplayUuidFromStableId("FEDCBA9876543210"));

  GUID first_guid {};
  std::memcpy(&first_guid, first.b8, sizeof(first_guid));
  EXPECT_NE(VDISPLAY::client_uuid_to_virtual_display_id(first_guid), 0u);
}

TEST(SunshineVirtualDisplay, PersistentIdentityUsesTheReservedEnsureStableId) {
  const auto persistent = VDISPLAY::persistentVirtualDisplayUuid();
  EXPECT_EQ(
    persistent,
    VDISPLAY::virtualDisplayUuidFromStableId(std::string {VDISPLAY::policy::ensure_display_stable_id})
  );
  EXPECT_NE(persistent, VDISPLAY::virtualDisplayUuidFromStableId("C19912B3-2432-D020-368E-65EC0EDD3C72"));
}

TEST(SunshineVirtualDisplay, SharedPersistentGuidUsesTheReservedUuidBytes) {
  const auto persistent = VDISPLAY::persistentVirtualDisplayUuid();
  GUID expected {};
  std::memcpy(&expected, persistent.b8, sizeof(expected));

  const auto shared = VDISPLAY::sharedVirtualDisplayGuid();
  const auto repeated = VDISPLAY::sharedVirtualDisplayGuid();
  EXPECT_EQ(0, std::memcmp(&shared, &expected, sizeof(shared)));
  EXPECT_EQ(0, std::memcmp(&shared, &repeated, sizeof(shared)));
}

TEST(SunshineVirtualDisplay, EnsureDisplayReservedIdentityNeverCollidesWithClients) {
  const auto reserved = VDISPLAY::virtualDisplayUuidFromStableId(
    std::string {VDISPLAY::policy::ensure_display_stable_id}
  );
  EXPECT_EQ(
    reserved,
    VDISPLAY::virtualDisplayUuidFromStableId(std::string {VDISPLAY::policy::ensure_display_stable_id})
  );
  EXPECT_NE(reserved, VDISPLAY::virtualDisplayUuidFromStableId("C19912B3-2432-D020-368E-65EC0EDD3C72"));
  EXPECT_NE(reserved, VDISPLAY::virtualDisplayUuidFromStableId("2430544F-24C6-860F-B981-B84D70E57BFF"));
}

TEST(SunshineVirtualDisplay, EncoderProbeEnsureDisplaySkippedForPerClientVirtualDisplay) {
  EXPECT_TRUE(VDISPLAY::policy::should_ensure_probe_display(false));
  EXPECT_FALSE(VDISPLAY::policy::should_ensure_probe_display(true));
}

TEST(SunshineVirtualDisplay, EnsureDisplayAppliesConfiguredRenderAdapterBeforeTemporaryCreation) {
  EXPECT_TRUE(VDISPLAY::policy::adapter_preference_allows_creation(true));
  EXPECT_FALSE(VDISPLAY::policy::adapter_preference_allows_creation(false));
}

TEST(SunshineVirtualDisplay, ActivePhysicalDisplayDetectionIsScopedToConfiguredAdapter) {
  // Regression (#265): a display attached to the *other* GPU used to satisfy
  // has_active_physical_display(), so no virtual display was created and capture — which is pinned
  // to adapter_name — then failed with "Failed to locate an output device".
  for (const auto &relative_path : {
         std::string {"src/platform/windows/virtual_display_sunshine.cpp"},
         std::string {"src/platform/windows/virtual_display_sudovda.cpp"},
       }) {
    const auto source = read_source(relative_path);
    const auto detection_pos = source.find("has_active_physical_display() {");
    ASSERT_NE(detection_pos, std::string::npos) << relative_path;
    const auto detection_end = source.find("should_auto_enable_virtual_display", detection_pos);
    ASSERT_NE(detection_end, std::string::npos) << relative_path;
    const auto detection_body = source.substr(detection_pos, detection_end - detection_pos);

    EXPECT_NE(detection_body.find("platf::configured_capture_adapter_has_output(active_physical_displays)"), std::string::npos)
      << relative_path << " does not scope active display detection to the configured adapter";
  }

  const auto misc_source = read_source("src/platform/windows/misc.cpp");
  expect_contains(misc_source, "bool configured_capture_adapter_has_output(");
  expect_contains(misc_source, "adapter_output_match_e adapter_drives_any_output(");
  // Hosts without a configured adapter must keep the legacy adapter-agnostic answer.
  expect_contains(misc_source, "if (config::video.adapter_name.empty()) {");
}

TEST(SunshineVirtualDisplay, ConfiguredRenderAdapterIsNeverSilentlyReplaced) {
  for (const auto &relative_path : {
         std::string {"src/platform/windows/virtual_display_sunshine.cpp"},
         std::string {"src/platform/windows/virtual_display_sudovda.cpp"},
       }) {
    const auto source = read_source(relative_path);
    expect_contains(source, "return VDISPLAY::applyConfiguredRenderAdapterPreference(context);");
  }

  // The driver-specific wrappers delegate to the shared policy, which reports
  // an unusable preference and never substitutes a highest-VRAM adapter.
  const auto shared_source = read_source("src/platform/windows/virtual_display.cpp");
  const auto preference_pos = shared_source.find("bool applyConfiguredRenderAdapterPreference(");
  ASSERT_NE(preference_pos, std::string::npos);
  const auto preference_end = shared_source.find("bool configuredRenderAdapterMatchesVirtualDisplay(", preference_pos);
  ASSERT_NE(preference_end, std::string::npos);
  const auto preference_body = shared_source.substr(preference_pos, preference_end - preference_pos);
  expect_contains(preference_body, "BOOST_LOG(error)");
  expect_contains(preference_body, "No fallback adapter will be used.");
  EXPECT_EQ(preference_body.find("setRenderAdapterWithMostDedicatedMemory"), std::string::npos);

  // The capture path must name the exact reason a pinned adapter could not be honored.
  const auto display_base_source = read_source("src/platform/windows/display_base.cpp");
  expect_contains(display_base_source, "bool configured_adapter_present = false;");
  expect_contains(display_base_source, "bool configured_adapter_has_output = false;");
  expect_contains(display_base_source, "does not match any GPU.");
  expect_contains(display_base_source, "has no display attached to the desktop.");
}

TEST(SunshineVirtualDisplay, ResumeRequiresExactVirtualDisplayMatch) {
  EXPECT_FALSE(VDISPLAY::policy::allow_generic_resume_fallback());
}

TEST(SunshineVirtualDisplay, ActiveRtspJoinSkipsVirtualDisplayPreparation) {
  EXPECT_FALSE(VDISPLAY::policy::should_prepare_display_for_new_session(false));
  EXPECT_TRUE(VDISPLAY::policy::should_prepare_display_for_new_session(true));
}

TEST(SunshineVirtualDisplay, StableIdentityResolverUsesEdidBeforeFriendlyName) {
  using kind = VDISPLAY::policy::identity_match_kind;
  EXPECT_EQ(VDISPLAY::policy::identity_resolution_order.front(), kind::stable_edid);
  EXPECT_EQ(VDISPLAY::policy::identity_resolution_order[1], kind::exact_output);
  EXPECT_EQ(VDISPLAY::policy::identity_resolution_order[2], kind::exact_client_name);
  EXPECT_EQ(VDISPLAY::policy::identity_resolution_order.back(), kind::generic_inactive);
}

TEST(SunshineVirtualDisplay, StreamStartRemovesRetainedProbeDisplayRegardlessOfStreamGuid) {
  EXPECT_TRUE(VDISPLAY::policy::should_release_retained_probe_display(false));
  EXPECT_FALSE(VDISPLAY::policy::should_release_retained_probe_display(true));
}

TEST(SunshineVirtualDisplay, StreamReadinessAllowsHelperToActivateEnumeratedDisplay) {
  EXPECT_FALSE(VDISPLAY::policy::accept_enumerated_target(std::chrono::milliseconds {499}));
  EXPECT_TRUE(VDISPLAY::policy::accept_enumerated_target(std::chrono::milliseconds {500}));
}

TEST(SunshineVirtualDisplay, DetectsDriverIdentityFromDriverSignals) {
  EXPECT_TRUE(VDISPLAY::is_sunshine_virtual_display_identity(
    "\\\\?\\DISPLAY#SunshineVirtualDisplay#5&1", "", "", ""
  ));
  EXPECT_TRUE(VDISPLAY::is_sunshine_virtual_display_identity(
    "", "Sunshine Virtual Display Driver", "", ""
  ));
  EXPECT_TRUE(VDISPLAY::is_sunshine_virtual_display_identity("", "", "SDD", "5001"));
  EXPECT_TRUE(VDISPLAY::is_sunshine_virtual_display_identity("", "", "sdd", "0x4001"));
  EXPECT_FALSE(VDISPLAY::is_sunshine_virtual_display_identity(
    "\\\\?\\DISPLAY#OTHER#5&1", "Physical Display", "DEL", "4096"
  ));
}

TEST(SunshineVirtualDisplay, AcceptsVirtualDisplaySentinel) {
  EXPECT_TRUE(VDISPLAY::policy::is_virtual_display_selection("sunshine:virtual_display", false));
  EXPECT_TRUE(VDISPLAY::policy::is_virtual_display_selection("SUNSHINE:VIRTUAL_DISPLAY", false));
  EXPECT_FALSE(VDISPLAY::policy::is_virtual_display_selection("sunshine:sudovda_virtual_display", false));
  EXPECT_TRUE(VDISPLAY::policy::is_virtual_display_selection("sunshine:sudovda_virtual_display", true));
  EXPECT_FALSE(VDISPLAY::policy::is_virtual_display_selection("DISPLAY1", true));
}

TEST(SunshineVirtualDisplay, HdrActivationRequiresWindowsHdrSupportAndTenBit) {
  using state = VDISPLAY::policy::advanced_color_state_t;
  EXPECT_TRUE(VDISPLAY::policy::hdr_target_ready(state {true, true, true, false, 10}));
  EXPECT_FALSE(VDISPLAY::policy::hdr_target_ready(state {true, false, true, false, 10}));
  EXPECT_FALSE(VDISPLAY::policy::hdr_target_ready(state {true, true, false, false, 10}));
  EXPECT_FALSE(VDISPLAY::policy::hdr_target_ready(state {true, true, true, true, 10}));
  EXPECT_FALSE(VDISPLAY::policy::hdr_target_ready(state {true, true, true, false, 8}));
  EXPECT_EQ(VDISPLAY::policy::hdr_activation_timeout, std::chrono::seconds {3});
}

TEST(SunshineVirtualDisplay, HdrRequestedTemporaryDisplayFallsBackToSdr) {
  using action = VDISPLAY::policy::hdr_activation_failure_action;
  EXPECT_EQ(VDISPLAY::policy::hdr_failure_action(true, false, true), action::continue_sdr);
  EXPECT_EQ(VDISPLAY::policy::hdr_failure_action(true, false, false), action::defer_to_display_helper);
  EXPECT_EQ(VDISPLAY::policy::hdr_failure_action(true, true, true), action::none);
}

TEST(SunshineVirtualDisplay, AvailabilityChecksStayPassive) {
  EXPECT_TRUE(VDISPLAY::policy::passive_install_status(true));
  EXPECT_FALSE(VDISPLAY::policy::passive_install_status(false));
}

TEST(SunshineVirtualDisplay, LeaseAndTransportFailuresKeepProtocolMeaning) {
  constexpr std::uint64_t minimum = 0x1000;
  EXPECT_GE(VDISPLAY::policy::normalize_opaque_lease_id(7, minimum), minimum);
  EXPECT_EQ(VDISPLAY::policy::normalize_opaque_lease_id(0x2000, minimum), 0x2000u);
  EXPECT_TRUE(VDISPLAY::policy::should_reopen_control_transport(false, false));
  EXPECT_TRUE(VDISPLAY::policy::should_reopen_control_transport(true, false));
  EXPECT_FALSE(VDISPLAY::policy::should_reopen_control_transport(true, true));

  using status = VDISPLAY::policy::driver_status_class;
  EXPECT_EQ(VDISPLAY::policy::classify_protocol_query(false, true), status::version_incompatible);
  EXPECT_EQ(VDISPLAY::policy::classify_protocol_query(false, false), status::failed);
  EXPECT_EQ(VDISPLAY::policy::classify_protocol_query(true, false), status::ok);
}

TEST(SunshineWgcCapture, UsesFp16ForAdvancedColorTargets) {
  using namespace platf::dxgi::wgc_policy;
  EXPECT_EQ(select_capture_surface_format(true, false, true, false), capture_surface_format::rgba16_float);
  EXPECT_EQ(select_capture_surface_format(true, false, false, true), capture_surface_format::rgba16_float);
  EXPECT_EQ(select_capture_surface_format(true, true, true, true), capture_surface_format::bgra8);
  EXPECT_EQ(select_capture_surface_format(false, false, true, true), capture_surface_format::bgra8);
}

TEST(SunshineWgcCapture, HelperStartupAndStopAreBounded) {
  EXPECT_EQ(platf::dxgi::wgc_policy::helper_stop_timeout_ms, 3000u);
}

TEST(SunshineWgcCapture, FramePoolStartsLowLatencyAndCanAdapt) {
  using namespace platf::dxgi::wgc_policy;
  EXPECT_EQ(low_latency_initial_buffer_size, 1u);
  EXPECT_EQ(maximum_buffer_size(false), 2u);
  EXPECT_EQ(maximum_buffer_size(true), 1u);
  EXPECT_TRUE(buffer_pool_is_quiet(true, false, false, 1, 2));
  EXPECT_TRUE(buffer_pool_is_quiet(true, false, false, 0, 1));
  EXPECT_FALSE(buffer_pool_is_quiet(true, false, true, 1, 2));
  EXPECT_FALSE(buffer_pool_is_quiet(true, true, false, 1, 2));
}

#endif
