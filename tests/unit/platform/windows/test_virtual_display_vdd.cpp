/**
 * @file tests/unit/platform/windows/test_virtual_display_vdd.cpp
 * @brief Tests for Windows virtual display identity helpers.
 */
#include "../../../tests_common.h"

#ifdef _WIN32
  #include <src/platform/windows/virtual_display.h>

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
}  // namespace

TEST(VddVirtualDisplay, ClientUuidDisplayIdIsStableAndNonZero) {
  const auto first = VDISPLAY::client_uuid_to_vdd_display_id(kClientGuid);
  const auto second = VDISPLAY::client_uuid_to_vdd_display_id(kClientGuid);

  EXPECT_NE(first, 0u);
  EXPECT_EQ(first, second);
}

TEST(VddVirtualDisplay, PerClientDisplayIdsDifferByClientUuid) {
  EXPECT_NE(
    VDISPLAY::client_uuid_to_vdd_display_id(kClientGuid),
    VDISPLAY::client_uuid_to_vdd_display_id(kOtherClientGuid)
  );
}

TEST(VddVirtualDisplay, SharedDisplayIdentityUsesPersistentGuid) {
  const auto shared_guid = VDISPLAY::sharedVirtualDisplayGuid();

  EXPECT_NE(VDISPLAY::client_uuid_to_vdd_display_id(shared_guid), 0u);
  EXPECT_NE(
    VDISPLAY::client_uuid_to_vdd_display_id(shared_guid),
    VDISPLAY::client_uuid_to_vdd_display_id(kClientGuid)
  );
}

TEST(VddVirtualDisplay, DetectsVddIdentityFromDriverSignals) {
  EXPECT_TRUE(VDISPLAY::is_vdd_virtual_display_identity(
    "\\\\?\\DISPLAY#MttVDD#5&1",
    "",
    "",
    ""
  ));
  EXPECT_TRUE(VDISPLAY::is_vdd_virtual_display_identity(
    "",
    "Virtual Display Driver",
    "",
    ""
  ));
  EXPECT_TRUE(VDISPLAY::is_vdd_virtual_display_identity(
    "",
    "",
    "MTT",
    "1337"
  ));
  EXPECT_TRUE(VDISPLAY::is_vdd_virtual_display_identity(
    "",
    "",
    "mtt",
    "0x1337"
  ));
  EXPECT_FALSE(VDISPLAY::is_vdd_virtual_display_identity(
    "\\\\?\\DISPLAY#OTHER#5&1",
    "Physical Display",
    "DEL",
    "4096"
  ));
}

TEST(VddVirtualDisplay, AcceptsNewAndLegacyVirtualDisplaySentinels) {
  EXPECT_TRUE(VDISPLAY::is_virtual_display_selection(VDISPLAY::VIRTUAL_DISPLAY_SELECTION));
  EXPECT_TRUE(VDISPLAY::is_virtual_display_selection(VDISPLAY::SUDOVDA_VIRTUAL_DISPLAY_SELECTION));
  EXPECT_TRUE(VDISPLAY::is_virtual_display_selection("SUNSHINE:SUDOVDA_VIRTUAL_DISPLAY"));
  EXPECT_FALSE(VDISPLAY::is_virtual_display_selection(""));
  EXPECT_FALSE(VDISPLAY::is_virtual_display_selection("DISPLAY1"));
}
#endif
