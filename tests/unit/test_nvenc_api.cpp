/**
 * @file tests/unit/test_nvenc_api.cpp
 * @brief Test NVENC API version helpers.
 */
#include "../tests_common.h"

#include <gmock/gmock.h>

#include "src/nvenc/nvenc_api.h"

namespace {

  constexpr uint32_t kApi11_0 = nvenc::api::make_api_version(11U, 0U);
  constexpr uint32_t kApi12_0 = nvenc::api::make_api_version(12U, 0U);
  constexpr uint32_t kApi12_1 = nvenc::api::make_api_version(12U, 1U);
  constexpr uint32_t kApi12_2 = nvenc::api::make_api_version(12U, 2U);
  constexpr uint32_t kApi13_0 = nvenc::api::make_api_version(13U, 0U);

}  // namespace

TEST(NvencApiTest, CodecApiCandidatesMatchCompatibilityPlan) {
  EXPECT_THAT(nvenc::api::codec_api_candidates(0), testing::ElementsAre(kApi13_0, kApi12_2, kApi12_1, kApi12_0, kApi11_0));
  EXPECT_THAT(nvenc::api::codec_api_candidates(1), testing::ElementsAre(kApi13_0, kApi12_2, kApi12_1, kApi12_0, kApi11_0));
  EXPECT_THAT(nvenc::api::codec_api_candidates(2), testing::ElementsAre(kApi13_0, kApi12_2, kApi12_1));
  EXPECT_TRUE(nvenc::api::codec_api_candidates(99).empty());
}

TEST(NvencApiTest, SemanticVersionComparisonUsesMajorThenMinor) {
  EXPECT_TRUE(kApi12_2 > kApi13_0);

  EXPECT_GT(nvenc::api::compare_api_versions(kApi13_0, kApi12_2), 0);
  EXPECT_LT(nvenc::api::compare_api_versions(kApi12_2, kApi13_0), 0);
  EXPECT_EQ(nvenc::api::compare_api_versions(kApi12_1, kApi12_1), 0);

  EXPECT_TRUE(nvenc::api::api_version_less(kApi12_2, kApi13_0));
  EXPECT_TRUE(nvenc::api::api_version_less_or_equal(kApi12_2, kApi13_0));
  EXPECT_TRUE(nvenc::api::api_version_less_or_equal(kApi13_0, kApi13_0));
  EXPECT_TRUE(nvenc::api::api_version_greater(kApi13_0, kApi12_2));
}

TEST(NvencApiTest, FilterToApiVersionRetainsReviewedFallbacksForSdk130) {
  EXPECT_THAT(
    nvenc::api::filter_to_api_version({kApi13_0, kApi12_2, kApi12_1, kApi12_0, kApi11_0}, kApi13_0),
    testing::ElementsAre(kApi13_0, kApi12_2, kApi12_1, kApi12_0, kApi11_0)
  );

  EXPECT_THAT(
    nvenc::api::filter_to_api_version({kApi13_0, kApi12_2, kApi12_1, kApi12_0, kApi11_0}, kApi12_1),
    testing::ElementsAre(kApi12_1, kApi12_0, kApi11_0)
  );

  EXPECT_THAT(
    nvenc::api::filter_to_api_version({kApi13_0, kApi12_2, kApi12_1}, kApi13_0),
    testing::ElementsAre(kApi13_0, kApi12_2, kApi12_1)
  );
}

TEST(NvencApiTest, DriverCompatibilityUsesSemanticVersionOrdering) {
  EXPECT_TRUE(nvenc::api::driver_supports_api_version(kApi13_0, kApi12_2));
  EXPECT_TRUE(nvenc::api::driver_supports_api_version(kApi12_2, kApi12_1));
  EXPECT_TRUE(nvenc::api::driver_supports_api_version(kApi12_1, kApi12_1));

  EXPECT_FALSE(nvenc::api::driver_supports_api_version(kApi12_2, kApi13_0));
  EXPECT_FALSE(nvenc::api::driver_supports_api_version(kApi11_0, kApi12_0));
}

TEST(NvencApiTest, ReviewedApiVersionsIncludeSupportedFallbacks) {
  EXPECT_TRUE(nvenc::api::is_reviewed_api_version(kApi11_0));
  EXPECT_TRUE(nvenc::api::is_reviewed_api_version(kApi12_0));
  EXPECT_TRUE(nvenc::api::is_reviewed_api_version(kApi12_1));
  EXPECT_TRUE(nvenc::api::is_reviewed_api_version(kApi12_2));
  EXPECT_TRUE(nvenc::api::is_reviewed_api_version(kApi13_0));
  EXPECT_FALSE(nvenc::api::is_reviewed_api_version(nvenc::api::make_api_version(14U, 0U)));
}

TEST(NvencApiTest, SplitFrameSupportStartsAtSdk121) {
  EXPECT_FALSE(nvenc::api::supports_implicit_split_frame(kApi11_0));
  EXPECT_FALSE(nvenc::api::supports_implicit_split_frame(kApi12_0));
  EXPECT_TRUE(nvenc::api::supports_implicit_split_frame(kApi12_1));
  EXPECT_TRUE(nvenc::api::supports_implicit_split_frame(kApi12_2));
  EXPECT_TRUE(nvenc::api::supports_implicit_split_frame(kApi13_0));
}

TEST(NvencApiTest, SeparateBitDepthFieldsStartAtSdk121) {
  EXPECT_FALSE(nvenc::api::supports_separate_bit_depth_fields(kApi11_0));
  EXPECT_FALSE(nvenc::api::supports_separate_bit_depth_fields(kApi12_0));
  EXPECT_TRUE(nvenc::api::supports_separate_bit_depth_fields(kApi12_1));
  EXPECT_TRUE(nvenc::api::supports_separate_bit_depth_fields(kApi12_2));
  EXPECT_TRUE(nvenc::api::supports_separate_bit_depth_fields(kApi13_0));
}

TEST(NvencApiTest, StructVersionMappingsMatchReviewedValues) {
  EXPECT_EQ(nvenc::api::function_list_version(kApi11_0), nvenc::api::make_struct_version(kApi11_0, 2U));
  EXPECT_EQ(nvenc::api::function_list_version(kApi13_0), nvenc::api::make_struct_version(kApi13_0, 2U));

  EXPECT_EQ(nvenc::api::initialize_params_version(kApi11_0), nvenc::api::make_struct_version(kApi11_0, 5U, true));
  EXPECT_EQ(nvenc::api::initialize_params_version(kApi12_0), nvenc::api::make_struct_version(kApi12_0, 5U, true));
  EXPECT_EQ(nvenc::api::initialize_params_version(kApi12_1), nvenc::api::make_struct_version(kApi12_1, 6U, true));
  EXPECT_EQ(nvenc::api::initialize_params_version(kApi13_0), nvenc::api::make_struct_version(kApi13_0, 7U, true));

  EXPECT_EQ(nvenc::api::config_version(kApi11_0), nvenc::api::make_struct_version(kApi11_0, 7U, true));
  EXPECT_EQ(nvenc::api::config_version(kApi12_0), nvenc::api::make_struct_version(kApi12_0, 8U, true));
  EXPECT_EQ(nvenc::api::config_version(kApi13_0), nvenc::api::make_struct_version(kApi13_0, 9U, true));

  EXPECT_EQ(nvenc::api::preset_config_version(kApi11_0), nvenc::api::make_struct_version(kApi11_0, 4U, true));
  EXPECT_EQ(nvenc::api::preset_config_version(kApi12_1), nvenc::api::make_struct_version(kApi12_1, 4U, true));
  EXPECT_EQ(nvenc::api::preset_config_version(kApi13_0), nvenc::api::make_struct_version(kApi13_0, 5U, true));

  EXPECT_EQ(nvenc::api::reconfigure_params_version(kApi11_0), nvenc::api::make_struct_version(kApi11_0, 1U, true));
  EXPECT_EQ(nvenc::api::reconfigure_params_version(kApi12_1), nvenc::api::make_struct_version(kApi12_1, 1U, true));
  EXPECT_EQ(nvenc::api::reconfigure_params_version(kApi13_0), nvenc::api::make_struct_version(kApi13_0, 2U, true));

  EXPECT_EQ(nvenc::api::pic_params_version(kApi11_0), nvenc::api::make_struct_version(kApi11_0, 4U, true));
  EXPECT_EQ(nvenc::api::pic_params_version(kApi12_0), nvenc::api::make_struct_version(kApi12_0, 6U, true));
  EXPECT_EQ(nvenc::api::pic_params_version(kApi13_0), nvenc::api::make_struct_version(kApi13_0, 7U, true));

  EXPECT_EQ(nvenc::api::register_resource_version(kApi11_0), nvenc::api::make_struct_version(kApi11_0, 3U));
  EXPECT_EQ(nvenc::api::register_resource_version(kApi12_0), nvenc::api::make_struct_version(kApi12_0, 4U));
  EXPECT_EQ(nvenc::api::register_resource_version(kApi13_0), nvenc::api::make_struct_version(kApi13_0, 5U));
}

TEST(NvencApiTest, LockBitstreamVersionHandlesSdkSpecificShapeChanges) {
  EXPECT_EQ(nvenc::api::lock_bitstream_version(kApi11_0), nvenc::api::make_struct_version(kApi11_0, 1U));
  EXPECT_EQ(nvenc::api::lock_bitstream_version(kApi12_0), nvenc::api::make_struct_version(kApi12_0, 2U));
  EXPECT_EQ(nvenc::api::lock_bitstream_version(kApi12_1), nvenc::api::make_struct_version(kApi12_1, 1U, true));
  EXPECT_EQ(nvenc::api::lock_bitstream_version(kApi13_0), nvenc::api::make_struct_version(kApi13_0, 2U, true));
}

TEST(NvencApiTest, EventParamsVersionOnlyBumpsAtSdk130) {
  EXPECT_EQ(nvenc::api::event_params_version(kApi11_0), nvenc::api::make_struct_version(kApi11_0, 1U));
  EXPECT_EQ(nvenc::api::event_params_version(kApi12_0), nvenc::api::make_struct_version(kApi12_0, 1U));
  EXPECT_EQ(nvenc::api::event_params_version(kApi12_1), nvenc::api::make_struct_version(kApi12_1, 1U));
  EXPECT_EQ(nvenc::api::event_params_version(kApi12_2), nvenc::api::make_struct_version(kApi12_2, 1U));
  EXPECT_EQ(nvenc::api::event_params_version(kApi13_0), nvenc::api::make_struct_version(kApi13_0, 2U));
}
