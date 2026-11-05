/**
 * @file tests/unit/test_nvenc_intra_refresh.cpp
 * @brief Test the intra refresh setup shared by the H.264 and HEVC encoder configs.
 */
#include "src/nvenc/nvenc_intra_refresh.h"

#include <gtest/gtest.h>
#include <vector>

namespace {
  namespace intra_refresh = nvenc::intra_refresh;

  struct caps_t {
    bool intra_refresh = true;
    bool single_slice = true;
    std::vector<NV_ENC_CAPS> queried;

    int operator()(NV_ENC_CAPS cap) {
      queried.push_back(cap);
      if (cap == NV_ENC_CAPS_SUPPORT_INTRA_REFRESH) {
        return intra_refresh;
      }
      if (cap == NV_ENC_CAPS_SINGLE_SLICE_INTRA_REFRESH) {
        return single_slice;
      }
      return 0;
    }
  };

  nvenc::nvenc_config host_option(bool intra_refresh) {
    nvenc::nvenc_config config;
    config.intra_refresh = intra_refresh;
    return config;
  }

  template<typename FormatConfig>
  class NvencIntraRefreshTest: public testing::Test {};

  using format_configs_t = testing::Types<NV_ENC_CONFIG_H264, NV_ENC_CONFIG_HEVC>;
  TYPED_TEST_SUITE(NvencIntraRefreshTest, format_configs_t);

  template<typename FormatConfig>
  void expect_intra_refresh_on(const FormatConfig &format_config) {
    EXPECT_EQ(format_config.enableIntraRefresh, 1u);
    EXPECT_EQ(format_config.intraRefreshPeriod, 300u);
    EXPECT_EQ(format_config.intraRefreshCnt, 299u);
    EXPECT_EQ(format_config.outputRecoveryPointSEI, 1u);
  }

  template<typename FormatConfig>
  void expect_intra_refresh_off(const FormatConfig &format_config) {
    EXPECT_EQ(format_config.enableIntraRefresh, 0u);
    EXPECT_EQ(format_config.intraRefreshPeriod, 0u);
    EXPECT_EQ(format_config.intraRefreshCnt, 0u);
    EXPECT_EQ(format_config.outputRecoveryPointSEI, 0u);
    EXPECT_EQ(format_config.singleSliceIntraRefresh, 0u);
  }

}  // namespace

// The bug this guards: nvenc_intra_refresh was parsed but ignored when the
// client (Moonlight) didn't ask for intra refresh itself.
TYPED_TEST(NvencIntraRefreshTest, HostOptionEnablesIntraRefreshWithoutClientRequest) {
  TypeParam format_config = {};
  caps_t caps;
  EXPECT_EQ(intra_refresh::apply(format_config, 0, host_option(true), caps), intra_refresh::result_e::enabled);
  expect_intra_refresh_on(format_config);
  EXPECT_EQ(format_config.singleSliceIntraRefresh, 1u);
}

TYPED_TEST(NvencIntraRefreshTest, ClientRequestEnablesIntraRefreshWithHostOptionOff) {
  TypeParam format_config = {};
  caps_t caps;
  EXPECT_EQ(intra_refresh::apply(format_config, 1, host_option(false), caps), intra_refresh::result_e::enabled);
  expect_intra_refresh_on(format_config);
  EXPECT_EQ(format_config.singleSliceIntraRefresh, 1u);
}

TYPED_TEST(NvencIntraRefreshTest, NothingChangesWhenNeitherAsks) {
  TypeParam format_config = {};
  caps_t caps;
  EXPECT_EQ(intra_refresh::apply(format_config, 0, host_option(false), caps), intra_refresh::result_e::not_requested);
  expect_intra_refresh_off(format_config);
  EXPECT_TRUE(caps.queried.empty());
}

TYPED_TEST(NvencIntraRefreshTest, UnsupportedEncoderLeavesConfigUnchanged) {
  TypeParam format_config = {};
  caps_t caps;
  caps.intra_refresh = false;
  EXPECT_EQ(intra_refresh::apply(format_config, 0, host_option(true), caps), intra_refresh::result_e::unsupported);
  expect_intra_refresh_off(format_config);
}

TYPED_TEST(NvencIntraRefreshTest, MissingSingleSliceSupportStillEnablesIntraRefresh) {
  TypeParam format_config = {};
  caps_t caps;
  caps.single_slice = false;
  EXPECT_EQ(intra_refresh::apply(format_config, 0, host_option(true), caps), intra_refresh::result_e::enabled_without_single_slice);
  expect_intra_refresh_on(format_config);
  EXPECT_EQ(format_config.singleSliceIntraRefresh, 0u);
}
