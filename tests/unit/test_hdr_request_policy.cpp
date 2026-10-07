#include "../tests_common.h"

#include "src/hdr_request_policy.h"

namespace policy = rtsp_stream::hdr_request_policy;
using override_e = config::video_t::dd_t::hdr_request_override_e;

TEST(HdrRequestOverride, AutomaticPreservesClientPolicy) {
  EXPECT_EQ(policy::apply({true, true, false}, override_e::automatic), (policy::state_t {true, true, false}));
}

TEST(HdrRequestOverride, ForceOnRequestsHdrAndClearsSdrPolicies) {
  EXPECT_EQ(policy::apply({false, true, true}, override_e::force_on), (policy::state_t {true, false, false}));
}

TEST(HdrRequestOverride, ForceOffSuppressesHdr) {
  EXPECT_EQ(policy::apply({true, true, false}, override_e::force_off), (policy::state_t {false, true, true}));
}

TEST(AppSdrPreference, UnsetApplicationInheritsEachDevicePreference) {
  EXPECT_FALSE(policy::resolve_prefer_10bit_sdr(false, std::nullopt));
  EXPECT_TRUE(policy::resolve_prefer_10bit_sdr(true, std::nullopt));
}

TEST(AppSdrPreference, ApplicationCanChooseSdrOrHdrRegardlessOfDeviceDefault) {
  for (const bool client_preference : {false, true}) {
    EXPECT_TRUE(policy::resolve_prefer_10bit_sdr(client_preference, true));
    EXPECT_FALSE(policy::resolve_prefer_10bit_sdr(client_preference, false));
  }
}

TEST(AppSdrPreference, SdrPreferenceDoesNotUpgradeClientDecoderRequest) {
  const auto state = policy::apply(
    {false, policy::resolve_prefer_10bit_sdr(false, true), false},
    override_e::automatic
  );
  EXPECT_FALSE(state.enable_hdr);
  EXPECT_TRUE(state.prefer_sdr_10bit);
  EXPECT_FALSE(state.force_sdr);
}

TEST(AppSdrPreference, ExplicitHdrRequestOverrideStillWinsOverApplicationSdr) {
  const policy::state_t requested {true, policy::resolve_prefer_10bit_sdr(false, true), false};
  EXPECT_EQ(policy::apply(requested, override_e::automatic), requested);
  EXPECT_EQ(policy::apply(requested, override_e::force_on), (policy::state_t {true, false, false}));
  EXPECT_EQ(policy::apply(requested, override_e::force_off), (policy::state_t {false, true, true}));
}
