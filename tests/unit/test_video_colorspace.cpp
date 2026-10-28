#include "../tests_common.h"
#include "src/video.h"
#include "src/video_colorspace.h"

TEST(VideoColorspace, LiveHdrAndSdrUseNegotiatedMain10Depth) {
  video::config_t config {};
  config.dynamicRange = 1;
  config.encoderCscMode = 2;
  const auto hdr = video::colorspace_from_client_config(config, true);
  const auto sdr = video::colorspace_from_client_config(config, false);
  EXPECT_EQ(hdr.bit_depth, 10u);
  EXPECT_EQ(sdr.bit_depth, 10u);
  EXPECT_EQ(hdr.colorspace, video::colorspace_e::bt2020);
  EXPECT_EQ(sdr.colorspace, video::colorspace_e::rec709);
  const auto hdr_wire = video::avcodec_colorspace_from_sunshine_colorspace(hdr);
  const auto sdr_wire = video::avcodec_colorspace_from_sunshine_colorspace(sdr);
  EXPECT_EQ(hdr_wire.transfer_function, AVCOL_TRC_SMPTE2084);
  EXPECT_EQ(sdr_wire.transfer_function, AVCOL_TRC_BT709);
  EXPECT_EQ(sdr_wire.primaries, AVCOL_PRI_BT709);
}

TEST(VideoColorspace, ExplicitSdrPoliciesTakePrecedenceOverHdrSource) {
  video::config_t config {};
  config.dynamicRange = 1;
  config.encoderCscMode = 2;
  config.prefer_sdr_10bit = true;
  EXPECT_FALSE(video::colorspace_is_hdr(video::colorspace_from_client_config(config, true)));
  config.prefer_sdr_10bit = false;
  config.force_sdr = true;
  const auto sdr = video::colorspace_from_client_config(config, true);
  EXPECT_FALSE(video::colorspace_is_hdr(sdr));
  EXPECT_EQ(sdr.bit_depth, 10u);
}

TEST(VideoColorspace, ClientSdrAndBt2020SdrRemainValid) {
  video::config_t config {};
  config.dynamicRange = 0;
  config.encoderCscMode = 2;
  EXPECT_EQ(video::colorspace_from_client_config(config, true).bit_depth, 8u);
  EXPECT_FALSE(video::colorspace_is_hdr(video::colorspace_from_client_config(config, true)));
  config.dynamicRange = 1;
  config.encoderCscMode = 4;
  const auto sdr = video::colorspace_from_client_config(config, false);
  EXPECT_EQ(sdr.colorspace, video::colorspace_e::bt2020sdr);
  EXPECT_EQ(sdr.bit_depth, 10u);
  EXPECT_EQ(video::avcodec_colorspace_from_sunshine_colorspace(sdr).transfer_function, AVCOL_TRC_BT2020_10);
}
