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
