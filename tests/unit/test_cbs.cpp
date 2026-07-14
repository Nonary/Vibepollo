/**
 * @file tests/unit/test_cbs.cpp
 * @brief Tests for coded-bitstream SPS/VUI validation.
 */

// standard includes
#include <array>

// lib includes
#include <gtest/gtest.h>

extern "C" {
#include <libavcodec/avcodec.h>
}

// local includes
#include "src/cbs.h"

namespace {
  // Parameter sets produced by x264/x265 for limited-range BT.709. Keep only
  // SPS-adjacent parameter sets so the fixture contains no frame payload.
  constexpr std::array<std::uint8_t, 36> h264_bt709_limited {
    0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xc0, 0x0a,
    0xda, 0x7b, 0x01, 0x6a, 0x02, 0x02, 0x02, 0x80,
    0x00, 0x00, 0x03, 0x00, 0x80, 0x00, 0x00, 0x3c,
    0x47, 0x89, 0x13, 0x50, 0x00, 0x00, 0x00, 0x01,
    0x68, 0xce, 0x0f, 0xc8,
  };

  constexpr std::array<std::uint8_t, 84> hevc_bt709_limited {
    0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0c, 0x01,
    0xff, 0xff, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00,
    0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00,
    0x1e, 0xba, 0x02, 0x40, 0x00, 0x00, 0x00, 0x01,
    0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03,
    0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
    0x00, 0x1e, 0xa0, 0x20, 0x81, 0x05, 0x96, 0xe9,
    0x29, 0x30, 0xbc, 0x05, 0xa8, 0x08, 0x08, 0x08,
    0x20, 0x00, 0x00, 0x03, 0x00, 0x20, 0x00, 0x00,
    0x07, 0x81, 0x00, 0x00, 0x00, 0x01, 0x44, 0x01,
    0xc0, 0x71, 0x81, 0x12,
  };

  constexpr std::array<std::uint8_t, 27> av1_bt709_limited_8bit {
    0x12, 0x00, 0x0a, 0x09, 0x18, 0x15, 0x7f, 0xfd,
    0xa2, 0x02, 0x02, 0x02, 0x08, 0x32, 0x0c, 0x18,
    0x00, 0x00, 0x00, 0x50, 0x00, 0x00, 0x0a, 0x05,
    0x77, 0x64, 0x80,
  };

  constexpr std::array<std::uint8_t, 27> av1_bt2020_pq_limited_10bit {
    0x12, 0x00, 0x0a, 0x09, 0x18, 0x15, 0x7f, 0xfd,
    0xaa, 0x12, 0x20, 0x12, 0x08, 0x32, 0x0c, 0x18,
    0x00, 0x00, 0x00, 0x50, 0x00, 0x00, 0x0a, 0x05,
    0x77, 0x64, 0x80,
  };

  constexpr cbs::vui_parameters_t bt709_limited {
    .full_range = false,
    .colour_primaries = AVCOL_PRI_BT709,
    .transfer_characteristics = AVCOL_TRC_BT709,
    .matrix_coefficients = AVCOL_SPC_BT709,
  };
}  // namespace

TEST(CodedBitstream, NativeH264VuiMustMatchCompleteColourDescription) {
  EXPECT_TRUE(cbs::validate_sequence_header(
    h264_bt709_limited.data(),
    h264_bt709_limited.size(),
    AV_CODEC_ID_H264,
    bt709_limited));

  auto mismatch = bt709_limited;
  mismatch.full_range = true;
  EXPECT_FALSE(cbs::validate_sequence_header(
    h264_bt709_limited.data(),
    h264_bt709_limited.size(),
    AV_CODEC_ID_H264,
    mismatch));

  mismatch = bt709_limited;
  mismatch.colour_primaries = AVCOL_PRI_BT2020;
  EXPECT_FALSE(cbs::validate_sequence_header(
    h264_bt709_limited.data(),
    h264_bt709_limited.size(),
    AV_CODEC_ID_H264,
    mismatch));
}

TEST(CodedBitstream, LegacySpsValidationStillRequiresAnActiveParameterSet) {
  AVPacket packet {};
  packet.data = const_cast<std::uint8_t *>(h264_bt709_limited.data());
  packet.size = static_cast<int>(h264_bt709_limited.size());

  // The native AMD data API may validate an unambiguous header-only probe,
  // but the established shared API keeps its slice-activated SPS semantics.
  EXPECT_FALSE(cbs::validate_sps(&packet, AV_CODEC_ID_H264));
}

TEST(CodedBitstream, NativeHevcVuiMustMatchCompleteColourDescription) {
  EXPECT_TRUE(cbs::validate_sequence_header(
    hevc_bt709_limited.data(),
    hevc_bt709_limited.size(),
    AV_CODEC_ID_H265,
    bt709_limited));

  auto mismatch = bt709_limited;
  mismatch.transfer_characteristics = AVCOL_TRC_SMPTE2084;
  EXPECT_FALSE(cbs::validate_sequence_header(
    hevc_bt709_limited.data(),
    hevc_bt709_limited.size(),
    AV_CODEC_ID_H265,
    mismatch));

  mismatch = bt709_limited;
  mismatch.matrix_coefficients = AVCOL_SPC_BT2020_NCL;
  EXPECT_FALSE(cbs::validate_sequence_header(
    hevc_bt709_limited.data(),
    hevc_bt709_limited.size(),
    AV_CODEC_ID_H265,
    mismatch));
}

TEST(CodedBitstream, NativeVuiValidationRejectsMalformedInput) {
  constexpr std::array<std::uint8_t, 4> malformed {0x00, 0x00, 0x00, 0x01};
  EXPECT_FALSE(cbs::validate_sequence_header(nullptr, 0, AV_CODEC_ID_H264, bt709_limited));
  EXPECT_FALSE(cbs::validate_sequence_header(
    malformed.data(), malformed.size(), AV_CODEC_ID_H264, bt709_limited));
  EXPECT_FALSE(cbs::validate_sequence_header(
    h264_bt709_limited.data(),
    h264_bt709_limited.size(),
    AV_CODEC_ID_NONE,
    bt709_limited));
}

TEST(CodedBitstream, NativeHevcMustMatchProfileAndBitDepth) {
  auto expected = bt709_limited;
  expected.bit_depth = 8;
  expected.profile = AV_PROFILE_HEVC_MAIN;
  EXPECT_TRUE(cbs::validate_sequence_header(
    hevc_bt709_limited.data(), hevc_bt709_limited.size(), AV_CODEC_ID_H265, expected));

  expected.bit_depth = 10;
  EXPECT_FALSE(cbs::validate_sequence_header(
    hevc_bt709_limited.data(), hevc_bt709_limited.size(), AV_CODEC_ID_H265, expected));

  expected.bit_depth = 8;
  expected.profile = AV_PROFILE_HEVC_MAIN_10;
  EXPECT_FALSE(cbs::validate_sequence_header(
    hevc_bt709_limited.data(), hevc_bt709_limited.size(), AV_CODEC_ID_H265, expected));
}

TEST(CodedBitstream, NativeAv1SequenceHeaderMustMatchSdrContract) {
  auto expected = bt709_limited;
  expected.bit_depth = 8;
  expected.profile = AV_PROFILE_AV1_MAIN;
  EXPECT_TRUE(cbs::validate_sequence_header(
    av1_bt709_limited_8bit.data(), av1_bt709_limited_8bit.size(), AV_CODEC_ID_AV1, expected));

  expected.bit_depth = 10;
  EXPECT_FALSE(cbs::validate_sequence_header(
    av1_bt709_limited_8bit.data(), av1_bt709_limited_8bit.size(), AV_CODEC_ID_AV1, expected));

  expected.bit_depth = 8;
  expected.full_range = true;
  EXPECT_FALSE(cbs::validate_sequence_header(
    av1_bt709_limited_8bit.data(), av1_bt709_limited_8bit.size(), AV_CODEC_ID_AV1, expected));
}

TEST(CodedBitstream, NativeAv1SequenceHeaderMustMatchHdr10Contract) {
  constexpr cbs::vui_parameters_t hdr10 {
    .full_range = false,
    .colour_primaries = AVCOL_PRI_BT2020,
    .transfer_characteristics = AVCOL_TRC_SMPTE2084,
    .matrix_coefficients = AVCOL_SPC_BT2020_NCL,
    .bit_depth = 10,
    .profile = AV_PROFILE_AV1_MAIN,
  };
  EXPECT_TRUE(cbs::validate_sequence_header(
    av1_bt2020_pq_limited_10bit.data(), av1_bt2020_pq_limited_10bit.size(), AV_CODEC_ID_AV1, hdr10));

  auto mismatch = hdr10;
  mismatch.transfer_characteristics = AVCOL_TRC_BT709;
  EXPECT_FALSE(cbs::validate_sequence_header(
    av1_bt2020_pq_limited_10bit.data(), av1_bt2020_pq_limited_10bit.size(), AV_CODEC_ID_AV1, mismatch));
}
