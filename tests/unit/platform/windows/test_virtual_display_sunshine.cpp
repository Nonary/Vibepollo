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

TEST(SunshineVirtualDisplay, ResumeRequiresExactVirtualDisplayMatch) {
  const auto rtsp_source = read_source("src/nvhttp.cpp");
  expect_contains(
    rtsp_source,
    "resolveActiveVirtualDisplayDeviceId(launch_session->virtual_display_device_id, launch_session->client_name, false)"
  );

  const auto webrtc_source = read_source("src/webrtc_stream.cpp");
  expect_contains(
    webrtc_source,
    "resolveActiveVirtualDisplayDeviceId(session->virtual_display_device_id, session->client_name, false)"
  );
}

TEST(SunshineVirtualDisplay, ResolverCanDisableGenericFallback) {
  const auto source = read_virtual_display_source();

  expect_contains(source, "No exact virtual display match found and generic fallback is disabled.");
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

TEST(SunshineWgcCapture, HelperDoesNotHoldSharedMutexWhileWaitingForLocalD3DContext) {
  const auto helper_source = read_source("tools/sunshine_wgc_capture.cpp");

  const auto function_start = helper_source.find("void copy_frame_to_shared_texture(");
  ASSERT_NE(function_start, std::string::npos);

  const auto context_lock = helper_source.find("std::unique_lock context_lock(_d3d_context_mutex);", function_start);
  const auto shared_mutex = helper_source.find("get_keyed_mutex()->AcquireSync(0, 200)", function_start);
  ASSERT_NE(context_lock, std::string::npos);
  ASSERT_NE(shared_mutex, std::string::npos);
  EXPECT_LT(context_lock, shared_mutex);

  expect_contains(helper_source, "context_wait_ms=");
  expect_contains(helper_source, "shared_mutex_hold_ms=");
  expect_contains(helper_source, "slow_context_count=");
  expect_contains(helper_source, "slow_shared_hold_count=");
}
#endif
