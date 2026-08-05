#include "../tests_common.h"

#include "src/video_policy.h"

#include <array>
#include <map>

namespace {
  class FakeEncoderProvider: public video::policy::encoder_capability_provider_t {
  public:
    std::map<std::string, video::policy::encoder_capabilities_t> values;
    video::policy::encoder_capabilities_t capabilities(std::string_view encoder) const override {
      const auto found = values.find(std::string(encoder));
      return found == values.end() ? video::policy::encoder_capabilities_t {} : found->second;
    }
  };
}

TEST(EncoderPolicy, SelectsFirstAvailableCapableEncoderWithoutHardwareProbe) {
  FakeEncoderProvider provider;
  provider.values["nvenc"] = {false, true, true};
  provider.values["software"] = {true, true, false};
  const std::array<std::string_view, 2> preference {"nvenc", "software"};
  EXPECT_EQ(video::policy::select_encoder(preference, {.hdr = true}, provider), "software");
}

TEST(EncoderPolicy, RejectsEncoderThatCannotMeetRequestedFormat) {
  FakeEncoderProvider provider;
  provider.values["software"] = {true, true, false};
  const std::array<std::string_view, 1> preference {"software"};
  EXPECT_FALSE(video::policy::select_encoder(preference, {.hdr = true, .yuv444 = true}, provider));
}

struct FramerateX100Test: testing::TestWithParam<std::tuple<std::int32_t, video::policy::rational_t>> {};
TEST_P(FramerateX100Test, Run) {
  const auto &[value, expected] = GetParam();
  EXPECT_EQ(video::policy::framerate_x100_to_rational(value), expected);
}
INSTANTIATE_TEST_SUITE_P(
  FramerateX100Tests,
  FramerateX100Test,
  testing::Values(
    std::make_tuple(2397, video::policy::rational_t {24000, 1001}),
    std::make_tuple(2398, video::policy::rational_t {24000, 1001}),
    std::make_tuple(2500, video::policy::rational_t {25, 1}),
    std::make_tuple(2997, video::policy::rational_t {30000, 1001}),
    std::make_tuple(6000, video::policy::rational_t {60, 1}),
    std::make_tuple(11988, video::policy::rational_t {120000, 1001}),
    std::make_tuple(23976, video::policy::rational_t {240000, 1001}),
    std::make_tuple(9498, video::policy::rational_t {4749, 50})
  )
);
