#include "../tests_common.h"
#include "src/video_encoder_probe_policy.h"

#include <optional>
#include <string>
#include <utility>

namespace {
  using video::encoder_probe_policy::cache_key_matches;
  using video::encoder_probe_policy::cache_key_t;
  using video::encoder_probe_policy::own_successful_cache_key;
  using video::encoder_probe_policy::probe_observation_t;

  cache_key_t key(std::string adapter) {
    return {
      .encoder_configuration = "encoder=nvenc|hevc=auto|av1=auto",
      .adapter_identity = std::move(adapter),
      .adapter_identity_resolved = true,
    };
  }
}  // namespace

TEST(EncoderProbePolicy, MatchingObservedAdapterReusesItsCacheEntry) {
  const auto pending_key = key("luid=nvidia");
  const auto owned_key = own_successful_cache_key(
    pending_key,
    probe_observation_t {
      .required_adapter = "luid=nvidia",
      .observed_adapter = "luid=nvidia",
    }
  );

  ASSERT_TRUE(owned_key);
  EXPECT_TRUE(cache_key_matches(key("luid=nvidia"), owned_key));
}

TEST(EncoderProbePolicy, DifferentObservedAdapterMisses) {
  const auto cached_key = std::optional<cache_key_t> {key("luid=amd")};

  EXPECT_FALSE(cache_key_matches(key("luid=nvidia"), cached_key));
}

TEST(EncoderProbePolicy, PendingHintCannotOwnMismatchedObservedProbe) {
  const auto pending_key = key("luid=nvidia");
  const auto owned_key = own_successful_cache_key(
    pending_key,
    probe_observation_t {
      .required_adapter = "luid=nvidia",
      .observed_adapter = "luid=amd",
    }
  );

  EXPECT_FALSE(owned_key);
  EXPECT_FALSE(cache_key_matches(pending_key, owned_key));
}

TEST(EncoderProbePolicy, UnresolvedProbeCannotPublishPositiveEntry) {
  const auto pending_key = key("luid=nvidia");

  EXPECT_FALSE(own_successful_cache_key(
    pending_key,
    probe_observation_t {
      .required_adapter = "luid=nvidia",
      .observed_adapter = std::nullopt,
    }
  ));
  EXPECT_FALSE(own_successful_cache_key(
    pending_key,
    probe_observation_t {
      .required_adapter = std::nullopt,
      .observed_adapter = std::string {},
    }
  ));
}

TEST(EncoderProbePolicy, CapabilitiesRemainAssociatedWithObservedKey) {
  struct cached_capabilities_t {
    int hevc_mode;
    bool hdr;
  };

  const auto owned_key = own_successful_cache_key(
    key("luid=nvidia"),
    probe_observation_t {
      .required_adapter = "luid=nvidia",
      .observed_adapter = "luid=nvidia",
    }
  );
  ASSERT_TRUE(owned_key);
  const auto cached = std::pair {*owned_key, cached_capabilities_t {3, true}};

  EXPECT_TRUE(cache_key_matches(key("luid=nvidia"), cached.first));
  EXPECT_EQ(cached.second.hevc_mode, 3);
  EXPECT_TRUE(cached.second.hdr);
  EXPECT_FALSE(cache_key_matches(key("luid=amd"), cached.first));
}
