/**
 * @file tests/unit/test_nvenc_log_flags.cpp
 * @brief Test the NvEnc created encoder log flags.
 */
#include "src/nvenc/nvenc_log_flags.h"

#include <gtest/gtest.h>

namespace {

  NV_ENC_CONFIG config_with_intra_refresh(int video_format) {
    NV_ENC_CONFIG config = {};
    if (video_format == 0) {
      config.encodeCodecConfig.h264Config.enableIntraRefresh = 1;
    } else if (video_format == 1) {
      config.encodeCodecConfig.hevcConfig.enableIntraRefresh = 1;
    }
    return config;
  }

}  // namespace

TEST(NvencLogFlagsTest, IntraRefreshShownForH264AndHevc) {
  EXPECT_EQ(nvenc::log_flags::intra_refresh(config_with_intra_refresh(0), 0, false), " intra-refresh");
  EXPECT_EQ(nvenc::log_flags::intra_refresh(config_with_intra_refresh(1), 1, false), " intra-refresh");
}

TEST(NvencLogFlagsTest, IntraRefreshUnknownAfterDriverDefaultRetry) {
  EXPECT_EQ(nvenc::log_flags::intra_refresh(config_with_intra_refresh(0), 0, true), " intra-refresh=unknown");
  EXPECT_EQ(nvenc::log_flags::intra_refresh(config_with_intra_refresh(1), 1, true), " intra-refresh=unknown");
}

TEST(NvencLogFlagsTest, NothingShownWhenIntraRefreshNotRequested) {
  const NV_ENC_CONFIG config = {};
  for (int video_format : {0, 1, 2}) {
    EXPECT_TRUE(nvenc::log_flags::intra_refresh(config, video_format, false).empty());
    EXPECT_TRUE(nvenc::log_flags::intra_refresh(config, video_format, true).empty());
  }
}

TEST(NvencLogFlagsTest, Av1NeverShowsIntraRefresh) {
  EXPECT_TRUE(nvenc::log_flags::intra_refresh(config_with_intra_refresh(0), 2, false).empty());
  EXPECT_TRUE(nvenc::log_flags::intra_refresh(config_with_intra_refresh(0), 2, true).empty());
}
