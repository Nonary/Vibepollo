/**
 * @file tests/unit/platform/windows/test_virtual_display_sunshine.cpp
 * @brief Tests for Windows virtual display identity helpers.
 */
#include "../../../tests_common.h"

#ifdef _WIN32
  #include <src/platform/windows/virtual_display.h>

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

  std::string read_virtual_display_source() {
    const auto path = std::filesystem::path {SUNSHINE_SOURCE_DIR} /
                      "src/platform/windows/virtual_display_sunshine.cpp";
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

TEST(SunshineVirtualDisplay, HdrRequestedTemporaryDisplayDoesNotContinueAsSdr) {
  const auto source = read_virtual_display_source();

  const auto activation_failure = source.find("if (hdr_requested && !request_hdr10_advanced_color(output))");
  ASSERT_NE(activation_failure, std::string::npos);

  const auto revert = source.find("(void) removeVirtualDisplay(guid);", activation_failure);
  ASSERT_NE(revert, std::string::npos);

  const auto fail_create = source.find("return std::nullopt;", activation_failure);
  ASSERT_NE(fail_create, std::string::npos);
  EXPECT_LT(revert, fail_create);
}
#endif
