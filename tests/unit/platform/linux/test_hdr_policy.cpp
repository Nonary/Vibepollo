#include "../../../tests_common.h"

#include "src/platform/linux/hdr_policy.h"

#include <array>

namespace hdr = platf::linux_hdr;

namespace {
  void checksum(std::array<std::uint8_t, 256> &edid, const std::size_t offset) {
    std::uint8_t sum = 0;
    for (std::size_t index = offset; index < offset + 127; ++index) {
      sum = static_cast<std::uint8_t>(sum + edid[index]);
    }
    edid[offset + 127] = static_cast<std::uint8_t>(0u - sum);
  }

  std::array<std::uint8_t, 256> hdr_edid(const std::uint8_t eotfs = 0x0d) {
    std::array<std::uint8_t, 256> edid {};
    constexpr std::array<std::uint8_t, 8> header {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    std::ranges::copy(header, edid.begin());
    edid[126] = 1;
    edid[128] = 0x02;
    edid[130] = 8;
    edid[132] = 0xe3;
    edid[133] = 0x06;
    edid[134] = eotfs;
    edid[135] = 0x01;
    checksum(edid, 0);
    checksum(edid, 128);
    return edid;
  }
}

TEST(LinuxHdrPolicy, DetectsHdr10FromCtaStaticMetadata) {
  const auto edid = hdr_edid();
  EXPECT_TRUE(hdr::edid_supports_hdr10(edid));
  EXPECT_FALSE(hdr::edid_supports_hdr10(std::span<const std::uint8_t> {edid}.first(128)));
  EXPECT_FALSE(hdr::edid_supports_hdr10(hdr_edid(0x01)));

  auto corrupt = edid;
  ++corrupt[140];
  EXPECT_FALSE(hdr::edid_supports_hdr10(corrupt));
}

TEST(LinuxHdrPolicy, PipeWireMetadataMatchesPrivateDisplayEdidLuminance) {
  const auto &metadata = hdr::pipewire_mastering_metadata(true);
  EXPECT_EQ(metadata.max_display_luminance, 1000);
  EXPECT_EQ(metadata.max_full_frame_luminance, 590);
  EXPECT_EQ(metadata.min_display_luminance, 1);
  EXPECT_EQ(metadata.max_content_light_level, 0);
  EXPECT_EQ(metadata.max_frame_average_light_level, 0);
}

TEST(LinuxHdrPolicy, PhysicalPipeWireSourceKeepsGenericMetadata) {
  const auto &metadata = hdr::pipewire_mastering_metadata(false);
  EXPECT_EQ(metadata.max_display_luminance, 4000);
  EXPECT_EQ(metadata.max_full_frame_luminance, 0);
  EXPECT_EQ(metadata.min_display_luminance, 1);
}

TEST(LinuxHdrPolicy, KmsHdrAcceptsOnlyPqEotf) {
  EXPECT_FALSE(hdr::is_hdr10_eotf(0));
  EXPECT_FALSE(hdr::is_hdr10_eotf(1));
  EXPECT_TRUE(hdr::is_hdr10_eotf(2));
  EXPECT_FALSE(hdr::is_hdr10_eotf(3));
}

TEST(LinuxHdrPolicy, DisabledDisplayHdrPolicyNeverReenablesFromRawClientIntent) {
  const auto disabled_policy = hdr::resolve_output_state(std::nullopt, true, false);
  EXPECT_FALSE(disabled_policy.command.has_value());
  EXPECT_FALSE(disabled_policy.expected_enabled);

  const auto preserved_hdr = hdr::resolve_output_state(std::nullopt, true, true);
  EXPECT_FALSE(preserved_hdr.command.has_value());
  EXPECT_TRUE(preserved_hdr.expected_enabled);
}

TEST(LinuxHdrPolicy, AutomaticDisplayHdrPolicyHonorsCapability) {
  const auto supported = hdr::resolve_output_state(true, true, false);
  ASSERT_TRUE(supported.command.has_value());
  EXPECT_TRUE(*supported.command);
  EXPECT_TRUE(supported.expected_enabled);

  const auto unsupported = hdr::resolve_output_state(true, false, false);
  EXPECT_FALSE(unsupported.command.has_value());
  EXPECT_FALSE(unsupported.expected_enabled);
}

TEST(LinuxHdrPolicy, OutputStateRequiresConsecutiveStableObservations) {
  hdr::output_state_stabilizer_t stabilizer;

  EXPECT_FALSE(stabilizer.observe(true));
  EXPECT_FALSE(stabilizer.observe(true));
  EXPECT_FALSE(stabilizer.observe(false));
  EXPECT_FALSE(stabilizer.observe(true));
  EXPECT_FALSE(stabilizer.observe(true));
  EXPECT_TRUE(stabilizer.observe(true));
  EXPECT_TRUE(stabilizer.observe(true));
}

TEST(LinuxHdrPolicy, OnlyNewlyConnectedHdrOutputsRequireRearm) {
  EXPECT_TRUE(hdr::requires_hdr_rearm(true, true));
  EXPECT_FALSE(hdr::requires_hdr_rearm(true, false));
  EXPECT_FALSE(hdr::requires_hdr_rearm(false, true));
  EXPECT_FALSE(hdr::requires_hdr_rearm(std::nullopt, true));
}
