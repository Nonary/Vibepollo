/**
 * @file tests/unit/platform/windows/test_virtual_display_sunshine.cpp
 * @brief Tests for Windows virtual display identity helpers.
 */
#include "../../../tests_common.h"

#ifdef _WIN32
  #include <src/platform/windows/virtual_display.h>

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

  std::string read_virtual_display_source() {
    return read_source("src/platform/windows/virtual_display_sunshine.cpp");
  }

  void expect_contains(const std::string &content, const std::string &needle) {
    EXPECT_NE(content.find(needle), std::string::npos) << "missing: " << needle;
  }
}  // namespace

TEST(SunshineVirtualDisplay, ClientUuidDisplayIdIsStableAndNonZero) {
  const auto first = VDISPLAY::client_uuid_to_virtual_display_id(kClientGuid);
  const auto second = VDISPLAY::client_uuid_to_virtual_display_id(kClientGuid);

  EXPECT_NE(first, 0u);
  EXPECT_EQ(first, second);
}

TEST(SunshineVirtualDisplay, PerClientDisplayIdsDifferByClientUuid) {
  EXPECT_NE(
    VDISPLAY::client_uuid_to_virtual_display_id(kClientGuid),
    VDISPLAY::client_uuid_to_virtual_display_id(kOtherClientGuid)
  );
}

TEST(SunshineVirtualDisplay, SharedDisplayIdentityUsesPersistentGuid) {
  const auto shared_guid = VDISPLAY::sharedVirtualDisplayGuid();

  EXPECT_NE(VDISPLAY::client_uuid_to_virtual_display_id(shared_guid), 0u);
  EXPECT_NE(
    VDISPLAY::client_uuid_to_virtual_display_id(shared_guid),
    VDISPLAY::client_uuid_to_virtual_display_id(kClientGuid)
  );
}

TEST(SunshineVirtualDisplay, StableVirtualDisplayUuidKeepsCanonicalUuidBytes) {
  const std::string client_uuid = "1d6f6f2a-4f29-41b2-958f-6f01d7583f4b";

  EXPECT_EQ(
    VDISPLAY::virtualDisplayUuidFromStableId(client_uuid),
    uuid_util::uuid_t::parse(client_uuid)
  );
}

TEST(SunshineVirtualDisplay, StableVirtualDisplayUuidDerivesNonCanonicalClientId) {
  const auto first = VDISPLAY::virtualDisplayUuidFromStableId("0123456789ABCDEF");
  const auto second = VDISPLAY::virtualDisplayUuidFromStableId("0123456789ABCDEF");
  const auto different = VDISPLAY::virtualDisplayUuidFromStableId("FEDCBA9876543210");

  EXPECT_EQ(first, second);
  EXPECT_NE(first, different);

  GUID first_guid {};
  GUID second_guid {};
  std::memcpy(&first_guid, first.b8, sizeof(first_guid));
  std::memcpy(&second_guid, second.b8, sizeof(second_guid));
  EXPECT_NE(VDISPLAY::client_uuid_to_virtual_display_id(first_guid), 0u);
  EXPECT_EQ(
    VDISPLAY::client_uuid_to_virtual_display_id(first_guid),
    VDISPLAY::client_uuid_to_virtual_display_id(second_guid)
  );
}

TEST(SunshineVirtualDisplay, TemporaryCreationDoesNotPersistSessionGuidAsSharedIdentity) {
  const auto source = read_virtual_display_source();

  EXPECT_EQ(source.find("write_guid_to_state_locked(requested_uuid)"), std::string::npos);
}

TEST(SunshineVirtualDisplay, EnsureDisplayReservedIdentityNeverCollidesWithClients) {
  // The headless encoder-probe / "Sunshine Temporary" display is identified by the reserved
  // "sunshine-ensure" sentinel. It must never resolve to the same identity as a real per-client
  // virtual display. Regression: a client whose stable id mirrored a contaminated
  // root.virtual_display_guid became an identity twin of the temporary display, which then
  // displaced the per-client display and streamed at the wrong resolution.
  const auto reserved = VDISPLAY::virtualDisplayUuidFromStableId("sunshine-ensure");

  // Deterministic across calls (stable identity, no state-file dependency).
  EXPECT_EQ(reserved, VDISPLAY::virtualDisplayUuidFromStableId("sunshine-ensure"));

  // Distinct from canonical-GUID client identities, including the exact stable ids from the
  // reported incident (TV and Mac clients).
  EXPECT_NE(reserved, VDISPLAY::virtualDisplayUuidFromStableId("C19912B3-2432-D020-368E-65EC0EDD3C72"));
  EXPECT_NE(reserved, VDISPLAY::virtualDisplayUuidFromStableId("2430544F-24C6-860F-B981-B84D70E57BFF"));

  GUID reserved_guid {};
  std::memcpy(&reserved_guid, reserved.b8, sizeof(reserved_guid));
  EXPECT_NE(VDISPLAY::client_uuid_to_virtual_display_id(reserved_guid), 0u);
  EXPECT_NE(
    VDISPLAY::client_uuid_to_virtual_display_id(reserved_guid),
    VDISPLAY::client_uuid_to_virtual_display_id(kClientGuid)
  );
}

TEST(SunshineVirtualDisplay, PersistentVirtualDisplayUuidUsesReservedSentinel) {
  const auto source = read_virtual_display_source();

  const auto fn_pos = source.find("VDISPLAY_SUNSHINE::persistentVirtualDisplayUuid()");
  ASSERT_NE(fn_pos, std::string::npos);
  const auto fn_body = source.substr(fn_pos, source.find('}', fn_pos) - fn_pos);

  // The ensure/shared display identity must be the reserved sentinel, not the mutable,
  // state-backed GUID that could be contaminated with a client's display identity.
  expect_contains(fn_body, "virtualDisplayUuidFromStableId(\"sunshine-ensure\")");
  EXPECT_EQ(fn_body.find("ensure_persistent_guid()"), std::string::npos);
}

TEST(SunshineVirtualDisplay, EncoderProbeEnsureDisplaySkippedForPerClientVirtualDisplay) {
  // A session that owns a per-client virtual display must never be served the generic
  // encoder-probe temporary display: creating it would tear down the per-client display and make
  // the 1080p temp the capture target (wrong resolution).
  const auto rtsp_source = read_source("src/nvhttp.cpp");
  expect_contains(rtsp_source, "VDISPLAY::ensure_display_result ensure_result {};");
  expect_contains(rtsp_source, "if (!launch_session->virtual_display) {");

  const auto webrtc_source = read_source("src/webrtc_stream.cpp");
  expect_contains(webrtc_source, "VDISPLAY::ensure_display_result ensure_result {};");
  expect_contains(webrtc_source, "if (!launch_session->virtual_display) {");
}

TEST(SunshineVirtualDisplay, EnsureDisplayAppliesConfiguredRenderAdapterBeforeTemporaryCreation) {
  const auto sunshine_source = read_virtual_display_source();
  const auto sunshine_ensure_pos = sunshine_source.find("VDISPLAY_SUNSHINE::ensure_display_result VDISPLAY_SUNSHINE::ensure_display()");
  ASSERT_NE(sunshine_ensure_pos, std::string::npos);
  const auto sunshine_ensure_body = sunshine_source.substr(sunshine_ensure_pos, sunshine_source.find("void VDISPLAY_SUNSHINE::cleanup_ensure_display", sunshine_ensure_pos) - sunshine_ensure_pos);
  const auto sunshine_preference_pos = sunshine_ensure_body.find("apply_configured_render_adapter_preference();");
  const auto sunshine_create_pos = sunshine_ensure_body.find("createVirtualDisplay(");
  ASSERT_NE(sunshine_preference_pos, std::string::npos);
  ASSERT_NE(sunshine_create_pos, std::string::npos);
  EXPECT_LT(sunshine_preference_pos, sunshine_create_pos);
  expect_contains(sunshine_source, "if (!config::video.adapter_name.empty())");
  expect_contains(sunshine_source, "setRenderAdapterByName(platf::from_utf8(config::video.adapter_name))");

  const auto sudo_source = read_source("src/platform/windows/virtual_display_sudovda.cpp");
  const auto sudo_ensure_pos = sudo_source.find("VDISPLAY_SUDOVDA::ensure_display_result VDISPLAY_SUDOVDA::ensure_display()");
  ASSERT_NE(sudo_ensure_pos, std::string::npos);
  const auto sudo_ensure_body = sudo_source.substr(sudo_ensure_pos, sudo_source.find("void VDISPLAY_SUDOVDA::cleanup_ensure_display", sudo_ensure_pos) - sudo_ensure_pos);
  const auto sudo_preference_pos = sudo_ensure_body.find("apply_configured_render_adapter_preference();");
  const auto sudo_create_pos = sudo_ensure_body.find("createVirtualDisplay(");
  ASSERT_NE(sudo_preference_pos, std::string::npos);
  ASSERT_NE(sudo_create_pos, std::string::npos);
  EXPECT_LT(sudo_preference_pos, sudo_create_pos);
  expect_contains(sudo_source, "if (!config::video.adapter_name.empty())");
  expect_contains(sudo_source, "setRenderAdapterByName(platf::from_utf8(config::video.adapter_name))");
}

TEST(SunshineVirtualDisplay, ResumeRequiresExactVirtualDisplayMatch) {
  const auto rtsp_source = read_source("src/nvhttp.cpp");
  expect_contains(
    rtsp_source,
    "resolveActiveVirtualDisplayDeviceIdForStableId(virtual_display_stable_id, launch_session->virtual_display_device_id, launch_session->client_name, false)"
  );

  const auto webrtc_source = read_source("src/webrtc_stream.cpp");
  expect_contains(
    webrtc_source,
    "resolveActiveVirtualDisplayDeviceIdForStableId(virtual_display_stable_id, session->virtual_display_device_id, session->client_name, false)"
  );
}

TEST(SunshineVirtualDisplay, ActiveRtspJoinSkipsVirtualDisplayPreparation) {
  const auto source = read_source("src/nvhttp.cpp");

  const auto skip_pos = source.find("another session is active; joining existing capture target without display changes");
  ASSERT_NE(skip_pos, std::string::npos);

  const auto recreate_pos = source.find("resume requested virtual display capture but no active virtual display was found");
  ASSERT_NE(recreate_pos, std::string::npos);
  EXPECT_LT(skip_pos, recreate_pos);

  expect_contains(source, "if (!no_active_sessions) {");
  expect_contains(source, "launch_session->virtual_display = false;");
}

TEST(SunshineVirtualDisplay, ResolverCanDisableGenericFallback) {
  const auto source = read_virtual_display_source();

  expect_contains(source, "No exact virtual display match found and generic fallback is disabled.");
}

TEST(SunshineVirtualDisplay, StableIdentityResolverUsesEdidBeforeFriendlyName) {
  const auto source = read_virtual_display_source();

  expect_contains(source, "matches_virtual_display_id_edid(device, expected_display_id)");
  expect_contains(source, "Resolved active virtual display by stable EDID identity");
  expect_contains(source, "falling back to exact display/client names");
}

TEST(SunshineVirtualDisplay, StreamStartRemovesRetainedProbeDisplayRegardlessOfStreamGuid) {
  const auto source = read_virtual_display_source();

  const auto cleanup_pos = source.find("void release_retained_ensure_display_for_stream");
  ASSERT_NE(cleanup_pos, std::string::npos);
  const auto cleanup_body = source.substr(cleanup_pos, source.find("bool adopt_existing_driver_lease", cleanup_pos) - cleanup_pos);

  expect_contains(cleanup_body, "if (g_ensure_display_retained)");
  expect_contains(cleanup_body, "resolve_virtual_display_name_from_devices_for_client(\"Sunshine Temporary\")");
  expect_contains(cleanup_body, "Removing encoder-probe virtual display before creating stream display");
  EXPECT_EQ(cleanup_body.find("!guid_equal(g_ensure_display_guid, guid)"), std::string::npos);
}

TEST(SunshineVirtualDisplay, StreamReadinessAllowsHelperToActivateEnumeratedDisplay) {
  const auto source = read_virtual_display_source();

  const auto wait_pos = source.find("bool wait_for_virtual_display_ready(");
  ASSERT_NE(wait_pos, std::string::npos);
  const auto wait_body = source.substr(wait_pos, source.find("bool wait_for_virtual_display_teardown", wait_pos) - wait_pos);

  expect_contains(wait_body, "if (enumerated_at && now - *enumerated_at >= activation_grace)");
  expect_contains(wait_body, "continuing so the display helper can apply the session mode");
  EXPECT_EQ(wait_body.find("allow_inactive_success"), std::string::npos);
  expect_contains(source, "wait_for_virtual_display_ready(display_name, device_id, width, height)");
  expect_contains(source, "wait_for_virtual_display_ready(resolved_display_name, device_id, width, height, display_config_ptr)");
}

TEST(SunshineVirtualDisplay, DetectsDriverIdentityFromDriverSignals) {
  EXPECT_TRUE(VDISPLAY::is_sunshine_virtual_display_identity(
    "\\\\?\\DISPLAY#SunshineVirtualDisplay#5&1",
    "",
    "",
    ""
  ));
  EXPECT_TRUE(VDISPLAY::is_sunshine_virtual_display_identity(
    "",
    "Sunshine Virtual Display Driver",
    "",
    ""
  ));
  EXPECT_TRUE(VDISPLAY::is_sunshine_virtual_display_identity(
    "",
    "",
    "SDD",
    "5001"
  ));
  EXPECT_TRUE(VDISPLAY::is_sunshine_virtual_display_identity(
    "",
    "",
    "sdd",
    "0x5001"
  ));
  EXPECT_TRUE(VDISPLAY::is_sunshine_virtual_display_identity(
    "",
    "",
    "SDD",
    "4001"
  ));
  EXPECT_FALSE(VDISPLAY::is_sunshine_virtual_display_identity(
    "\\\\?\\DISPLAY#OTHER#5&1",
    "Physical Display",
    "DEL",
    "4096"
  ));
}

TEST(SunshineVirtualDisplay, AcceptsVirtualDisplaySentinel) {
  EXPECT_TRUE(VDISPLAY::is_virtual_display_selection(VDISPLAY::VIRTUAL_DISPLAY_SELECTION));
  EXPECT_TRUE(VDISPLAY::is_virtual_display_selection("SUNSHINE:VIRTUAL_DISPLAY"));
  EXPECT_FALSE(VDISPLAY::is_virtual_display_selection(""));
  EXPECT_FALSE(VDISPLAY::is_virtual_display_selection("DISPLAY1"));
}

TEST(SunshineVirtualDisplay, HdrActivationRequiresWindowsHdrSupportAndTenBit) {
  const auto source = read_virtual_display_source();

  expect_contains(source, "const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);");
  expect_contains(source, "info->supported && info->hdr_supported && info->hdr_enabled && !info->limited_by_policy && ten_bit_or_better");
  EXPECT_EQ(source.find("info->hdr_supported ? info->hdr_enabled : info->active"), std::string::npos);
  expect_contains(source, "Windows did not report HDR support/enabled at 10-bit");
}

TEST(SunshineVirtualDisplay, HdrRequestedTemporaryDisplayFallsBackToSdr) {
  const auto source = read_virtual_display_source();

  const auto activation_failure = source.find("if (hdr_requested && !request_hdr10_advanced_color(output))");
  ASSERT_NE(activation_failure, std::string::npos);

  const auto fallback = source.find("continuing with SDR capture", activation_failure);
  ASSERT_NE(fallback, std::string::npos);

  const auto success = source.find("Virtual display added successfully", activation_failure);
  ASSERT_NE(success, std::string::npos);
  EXPECT_LT(fallback, success);

  const auto destructive_revert = source.find("(void) removeVirtualDisplay(guid);", activation_failure);
  EXPECT_TRUE(destructive_revert == std::string::npos || destructive_revert > success);
}

TEST(SunshineWgcCapture, UsesFp16ForAdvancedColorTargets) {
  const auto helper_source = read_source("tools/sunshine_wgc_capture.cpp");
  const auto display_source = read_source("src/platform/windows/display_wgc.cpp");
  const auto ipc_source = read_source("src/platform/windows/ipc/ipc_session.cpp");

  expect_contains(helper_source, "g_config.dynamic_range || g_config.advanced_color_capture");
  expect_contains(helper_source, "DXGI_FORMAT_R16G16B16A16_FLOAT");
  expect_contains(display_source, "const bool advanced_color_capture = is_hdr();");
  expect_contains(display_source, "device.get(), advanced_color_capture");
  expect_contains(ipc_source, "config_data.advanced_color_capture = _advanced_color_capture ? 1u : 0u;");
}

TEST(SunshineWgcCapture, HelperStartupAndStopAreBounded) {
  const auto ipc_source = read_source("src/platform/windows/ipc/ipc_session.cpp");
  const auto process_header = read_source("src/platform/windows/ipc/process_handler.h");

  const auto create_pipe = ipc_source.find("auto control_pipe = anon_connector->create_server(pipe_guid);");
  const auto start_helper = ipc_source.find("_process_helper->start(exe_path.wstring(), arguments)");
  ASSERT_NE(create_pipe, std::string::npos);
  ASSERT_NE(start_helper, std::string::npos);
  EXPECT_LT(create_pipe, start_helper);

  expect_contains(ipc_source, "_process_helper->wait_for(exit_code, 3000)");
  expect_contains(ipc_source, "WGC helper did not exit within 3000ms");
  expect_contains(process_header, "bool wait_for(DWORD &exit_code, DWORD timeout_ms);");
}

TEST(SunshineWgcCapture, FramePoolStartsLowLatencyAndCanAdapt) {
  const auto helper_source = read_source("tools/sunshine_wgc_capture.cpp");
  const auto ipc_source = read_source("src/platform/windows/ipc/ipc_session.cpp");

  expect_contains(ipc_source, "constexpr uint32_t kWgcLowLatencyInitialBufferSize = 1;");
  expect_contains(ipc_source, "constexpr uint32_t kWgcAdaptiveMaxBufferSize = 2;");
  expect_contains(ipc_source, "WGC_IPC_FLAG_DRAIN_TO_LATEST");
  expect_contains(ipc_source, "WGC_IPC_FLAG_ALLOW_BUFFER_DECREASE");
  expect_contains(ipc_source, "return kWgcLowLatencyInitialBufferSize;");
  expect_contains(ipc_source, "return kWgcAdaptiveMaxBufferSize;");

  expect_contains(helper_source, "_current_buffer_size > _initial_buffer_size");
  expect_contains(helper_source, "allow_buffer_decrease: ");
  expect_contains(helper_source, "recent_pool_pressure");
  expect_contains(helper_source, "WGC drained ");
  expect_contains(helper_source, "WGC capture diagnostics: interval_s=");
  expect_contains(helper_source, "approx_extra_pool_latency_ms=");
  expect_contains(helper_source, "capture_fps=");
  expect_contains(helper_source, "publish_fps=");
}

#endif
