/** Exercise production KMS metadata observations at the libdrm boundary. */
#include "../../../tests_common.h"
#include "src/platform/linux/kmsgrab_hdr.h"

#include <cerrno>
#include <cstdlib>
#include <map>
#include <set>
#include <thread>
#include <vector>
namespace hdr = platf::kms::hdr;

namespace {
  hdr_output_metadata pq_metadata() {
    hdr_output_metadata raw {};
    auto &info = raw.hdmi_metadata_type1;
    info.eotf = 2;
    info.display_primaries[0] = {35400, 14600};
    info.white_point = {15635, 16450};
    info.max_display_mastering_luminance = 1000;
    info.max_cll = 900;
    info.max_fall = 590;
    return raw;
  }

  std::map<uint32_t, std::vector<std::byte>> blobs;
  std::set<drmModePropertyBlobPtr> allocations;
  unsigned reads, frees;
  bool fail_reads;

  void publish(uint32_t id, const hdr_output_metadata &raw) {
    const auto begin = reinterpret_cast<const std::byte *>(&raw);
    blobs[id] = {begin, begin + sizeof(raw)};
  }

  class KmsHdr: public testing::Test {
  protected:
    void SetUp() override {
      blobs.clear();
      reads = frees = 0;
      fail_reads = false;
      publish(10, pq_metadata());
    }

    void TearDown() override {
      EXPECT_TRUE(allocations.empty());
      EXPECT_EQ(reads, frees);
    }
  };
}  // namespace

extern "C" drmModePropertyBlobPtr drmModeGetPropertyBlob(int fd, uint32_t id) {
  EXPECT_EQ(fd, 7);
  if (fail_reads || !blobs.contains(id)) {
    errno = ENOENT;
    return nullptr;
  }
  auto *blob = new drmModePropertyBlobRes {};
  blob->length = blobs[id].size();
  blob->data = std::malloc(blob->length);
  std::memcpy(blob->data, blobs[id].data(), blob->length);
  allocations.insert(blob);
  ++reads;
  return blob;
}

extern "C" void drmModeFreePropertyBlob(drmModePropertyBlobPtr blob) {
  EXPECT_EQ(allocations.erase(blob), 1u);
  std::free(blob->data);
  delete blob;
  ++frees;
}

TEST_F(KmsHdr, EquivalentReplacementDoesNotRestart) {
  hdr::generation_t generation;
  ASSERT_TRUE(generation.selected(7, 10)->is_hdr());
  publish(11, pq_metadata());
  blobs.erase(10);
  EXPECT_EQ(generation.refresh(7, 10, 11), platf::capture_e::ok);
  EXPECT_TRUE(generation.selected(7, 10)->is_hdr());
  EXPECT_EQ(reads, 2);
}

TEST_F(KmsHdr, RecycledReplacementIdCannotHideChangedPayload) {
  hdr::generation_t generation;
  ASSERT_TRUE(generation.selected(7, 10));
  publish(11, pq_metadata());
  ASSERT_EQ(generation.refresh(7, 10, 11), platf::capture_e::ok);
  auto changed = pq_metadata();
  ++changed.hdmi_metadata_type1.max_cll;
  publish(11, changed);
  EXPECT_EQ(generation.refresh(7, 10, 11), platf::capture_e::reinit);
}

TEST_F(KmsHdr, RecycledInitialIdCannotHideChangedPayload) {
  hdr::generation_t generation;
  ASSERT_TRUE(generation.selected(7, 10));
  publish(11, pq_metadata());
  ASSERT_EQ(generation.refresh(7, 10, 11), platf::capture_e::ok);
  auto changed = pq_metadata();
  changed.hdmi_metadata_type1.eotf = 0;
  publish(10, changed);
  EXPECT_EQ(generation.refresh(7, 10, 10), platf::capture_e::reinit);
}

TEST_F(KmsHdr, UnreadableCurrentObservationPreservesSelectedHdrThenDetectsRecovery) {
  hdr::generation_t generation;
  ASSERT_TRUE(generation.selected(7, 10)->is_hdr());
  fail_reads = true;
  for (int i = 0; i < 100; ++i) {
    EXPECT_EQ(generation.refresh(7, 10, 11), platf::capture_e::ok);
  }
  EXPECT_TRUE(generation.selected(7, 10)->is_hdr());
  fail_reads = false;
  auto changed = pq_metadata();
  ++changed.hdmi_metadata_type1.max_fall;
  publish(11, changed);
  EXPECT_EQ(generation.refresh(7, 10, 11), platf::capture_e::reinit);
}

TEST_F(KmsHdr, FailedSelectedObservationStaysSdrUntilReadableHdrRequiresNewGeneration) {
  hdr::generation_t generation;
  fail_reads = true;
  ASSERT_FALSE(generation.selected(7, 10));
  fail_reads = false;
  ASSERT_FALSE(generation.selected(7, 10));  // The encoder selection remains immutable.
  EXPECT_EQ(generation.refresh(7, 10, 0), platf::capture_e::ok);
  EXPECT_EQ(generation.refresh(7, 10, 10), platf::capture_e::reinit);
}

TEST_F(KmsHdr, ExplicitSdrTransitionRequiresReinitWhileMissingPropertyIsUnknown) {
  hdr::generation_t generation;
  ASSERT_TRUE(generation.selected(7, 10));
  EXPECT_EQ(generation.refresh(7, 10, std::nullopt), platf::capture_e::ok);
  EXPECT_EQ(generation.refresh(7, 10, 0), platf::capture_e::reinit);
  hdr::generation_t sdr;
  ASSERT_FALSE(sdr.selected(7, 0)->is_hdr());
  EXPECT_EQ(sdr.refresh(7, 0, 0), platf::capture_e::ok);
  EXPECT_EQ(sdr.refresh(7, 0, 10), platf::capture_e::reinit);
}

TEST_F(KmsHdr, InvalidPayloadIsUnknownAndNeverDereferenced) {
  auto raw = pq_metadata();
  EXPECT_FALSE(hdr::parse_blob(nullptr, sizeof(raw)));
  EXPECT_FALSE(hdr::parse_blob(&raw, offsetof(hdr_output_metadata, hdmi_metadata_type1) + sizeof(hdr_metadata_infoframe) - 1));
  raw.metadata_type = 1;
  EXPECT_FALSE(hdr::parse_blob(&raw, sizeof(raw)));
  raw.metadata_type = 0;
  raw.hdmi_metadata_type1.metadata_type = 1;
  EXPECT_FALSE(hdr::parse_blob(&raw, sizeof(raw)));
  EXPECT_FALSE(hdr::read_blob(7, std::numeric_limits<uint64_t>::max()));
}

TEST_F(KmsHdr, SemanticComparisonCoversAllConsumedFields) {
  const auto raw = pq_metadata();
  const auto baseline = hdr::parse_blob(&raw, sizeof(raw));
  ASSERT_TRUE(baseline);
  for (int field = 0; field < 13; ++field) {
    auto changed = raw;
    auto &info = changed.hdmi_metadata_type1;
    if (field < 6) {
      if (field % 2) {
        ++info.display_primaries[field / 2].y;
      } else {
        ++info.display_primaries[field / 2].x;
      }
    } else if (field == 6) {
      ++info.white_point.x;
    } else if (field == 7) {
      ++info.white_point.y;
    } else if (field == 8) {
      ++info.max_display_mastering_luminance;
    } else if (field == 9) {
      ++info.min_display_mastering_luminance;
    } else if (field == 10) {
      ++info.max_cll;
    } else if (field == 11) {
      ++info.max_fall;
    } else {
      ++info.eotf;
    }
    EXPECT_NE(baseline, hdr::parse_blob(&changed, sizeof(changed)));
  }
}

TEST_F(KmsHdr, NonHdrFieldsRemainDistinctButDoNotRestartEncoders) {
  for (const auto eotf : {0, 1, 3}) {
    auto raw = pq_metadata();
    raw.hdmi_metadata_type1.eotf = eotf;
    publish(10, raw);
    hdr::generation_t generation;
    const auto selected = generation.selected(7, 10);
    ASSERT_TRUE(selected);
    ASSERT_FALSE(selected->is_hdr());
    for (int observation = 0; observation < 100; ++observation) {
      ++raw.hdmi_metadata_type1.max_cll;
      ++raw.hdmi_metadata_type1.max_display_mastering_luminance;
      publish(10, raw);
      EXPECT_NE(*selected, *hdr::parse_blob(&raw, sizeof(raw)));
      EXPECT_EQ(generation.refresh(7, 10, 10), platf::capture_e::ok);
      EXPECT_FALSE(generation.commit_pending());
      EXPECT_EQ(generation.selected(7, 10), selected);
    }
  }
}

TEST_F(KmsHdr, AllKnownNonHdrStatesHaveEquivalentEncoderSignaling) {
  hdr::generation_t generation;
  ASSERT_FALSE(generation.selected(7, 0)->is_hdr());
  for (const auto eotf : {0, 1, 3, 255}) {
    auto raw = pq_metadata();
    raw.hdmi_metadata_type1.eotf = eotf;
    publish(10, raw);
    EXPECT_EQ(generation.refresh(7, 0, 10), platf::capture_e::ok);
  }
  EXPECT_EQ(generation.refresh(7, 0, 0), platf::capture_e::ok);
  EXPECT_FALSE(generation.commit_pending());
}

TEST_F(KmsHdr, EffectiveComparisonStillRestartsForHdrAndEveryHdrMetadataField) {
  auto sdr = pq_metadata();
  sdr.hdmi_metadata_type1.eotf = 3;
  publish(10, sdr);
  hdr::generation_t generation;
  ASSERT_FALSE(generation.selected(7, 10)->is_hdr());
  publish(10, pq_metadata());
  ASSERT_EQ(generation.refresh(7, 10, 10), platf::capture_e::reinit);
  ASSERT_TRUE(generation.commit_pending());
  ASSERT_TRUE(generation.selected(7, 10)->is_hdr());
  const auto baseline = pq_metadata();
  for (int field = 0; field < 12; ++field) {
    auto changed = baseline;
    auto &info = changed.hdmi_metadata_type1;
    if (field < 6) {
      if (field % 2) {
        ++info.display_primaries[field / 2].y;
      } else {
        ++info.display_primaries[field / 2].x;
      }
    } else if (field == 6) {
      ++info.white_point.x;
    } else if (field == 7) {
      ++info.white_point.y;
    } else if (field == 8) {
      ++info.max_display_mastering_luminance;
    } else if (field == 9) {
      ++info.min_display_mastering_luminance;
    } else if (field == 10) {
      ++info.max_cll;
    } else {
      ++info.max_fall;
    }
    publish(10, changed);
    EXPECT_EQ(generation.refresh(7, 10, 10), platf::capture_e::reinit);
    // No commit: each observation is compared with the same selected HDR.
  }
  publish(10, sdr);
  ASSERT_EQ(generation.refresh(7, 10, 10), platf::capture_e::reinit);
  ASSERT_TRUE(generation.commit_pending());
  EXPECT_FALSE(generation.selected(7, 10)->is_hdr());
  EXPECT_EQ(generation.refresh(7, 10, 0), platf::capture_e::ok);
}

TEST_F(KmsHdr, ConcurrentConsumersShareOneSelection) {
  hdr::generation_t generation;
  std::array<const hdr::state_t *, 16> results {};
  std::vector<std::thread> threads;
  for (std::size_t i = 0; i < results.size(); ++i) {
    threads.emplace_back([&, i] {
      results[i] = generation.selected(7, 10).get();
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  for (auto result : results) {
    EXPECT_EQ(result, results[0]);
  }
  EXPECT_EQ(reads, 1u);
}

TEST_F(KmsHdr, SuccessfulRecoveredObservationSurvivesRepeatedSelectionFaults) {
  hdr::generation_t generation;
  fail_reads = true;
  ASSERT_FALSE(generation.selected(7, 10));
  fail_reads = false;
  ASSERT_EQ(generation.refresh(7, 10, 10), platf::capture_e::reinit);
  ASSERT_FALSE(generation.selected(7, 10));  // The old encoder still sees SDR.
  ASSERT_TRUE(generation.commit_pending());
  fail_reads = true;
  ASSERT_TRUE(generation.selected(7, 10)->is_hdr());
  ASSERT_FALSE(generation.commit_pending());
}

TEST_F(KmsHdr, HandoffNeverChangesAnOldConsumersOwnedSnapshot) {
  hdr::generation_t generation;
  const auto old = generation.selected(7, 10);
  auto changed = pq_metadata();
  changed.hdmi_metadata_type1.max_display_mastering_luminance = 1200;
  publish(10, changed);
  ASSERT_EQ(generation.refresh(7, 10, 10), platf::capture_e::reinit);
  ASSERT_TRUE(generation.commit_pending());
  EXPECT_EQ(old->max_display_luminance, 1000);
  EXPECT_EQ(generation.selected(7, 10)->max_display_luminance, 1200);
  hdr::generation_t other_capture;
  EXPECT_FALSE(other_capture.commit_pending());
}

TEST_F(KmsHdr, AnUnknownNewObservationCannotCommitAnOlderPendingPayload) {
  hdr::generation_t generation;
  ASSERT_TRUE(generation.selected(7, 10));
  auto changed = pq_metadata();
  ++changed.hdmi_metadata_type1.max_cll;
  publish(11, changed);
  ASSERT_EQ(generation.refresh(7, 10, 11), platf::capture_e::reinit);
  fail_reads = true;
  ASSERT_EQ(generation.refresh(7, 10, 11), platf::capture_e::ok);
  EXPECT_FALSE(generation.commit_pending());
}

TEST_F(KmsHdr, StrictSdrObservationNeverStagesUnknownOrNonSdr) {
  hdr::generation_t generation;
  auto sdr = pq_metadata();
  sdr.hdmi_metadata_type1.eotf = 0;
  publish(10, sdr);
  const auto selected = generation.selected(7, 10);
  ASSERT_TRUE(selected);
  for (auto eotf : {1u, 2u, 3u, 255u}) {
    auto changed = pq_metadata();
    changed.hdmi_metadata_type1.eotf = eotf;
    publish(11, changed);
    EXPECT_EQ(generation.refresh(7, 10, 11, true), platf::capture_e::reinit);
    EXPECT_FALSE(generation.commit_pending());
    EXPECT_EQ(generation.selected(7, 10), selected);
  }
  fail_reads = true;
  EXPECT_EQ(generation.refresh(7, 10, 11, true), platf::capture_e::reinit);
  EXPECT_FALSE(generation.commit_pending());
  fail_reads = false;
  EXPECT_EQ(generation.refresh(7, 10, std::nullopt, true), platf::capture_e::reinit);
  EXPECT_FALSE(generation.commit_pending());
  EXPECT_EQ(generation.refresh(7, 10, 0, true), platf::capture_e::ok);
  EXPECT_FALSE(generation.commit_pending());
  publish(11, sdr);
  EXPECT_EQ(generation.refresh(7, 10, 11, true), platf::capture_e::ok);
  EXPECT_FALSE(generation.commit_pending());
}

TEST_F(KmsHdr, StrictRejectedObservationClearsEarlierPendingState) {
  hdr::generation_t generation;
  auto sdr = pq_metadata();
  sdr.hdmi_metadata_type1.eotf = 0;
  publish(10, sdr);
  const auto selected = generation.selected(7, 10);
  publish(11, pq_metadata());
  ASSERT_EQ(generation.refresh(7, 10, 11), platf::capture_e::reinit);
  fail_reads = true;
  EXPECT_EQ(generation.refresh(7, 10, 11, true), platf::capture_e::reinit);
  EXPECT_FALSE(generation.commit_pending());
  EXPECT_EQ(generation.selected(7, 10), selected);
}

TEST_F(KmsHdr, InitialStrictSdrSeedOwnsTheValidatedObservation) {
  hdr::generation_t generation;
  hdr::state_t observed {};
  observed.present = true;
  observed.eotf = 0;
  observed.max_content_light_level = 123;
  ASSERT_TRUE(generation.initialize_sdr(observed));
  observed.eotf = 2;
  observed.max_content_light_level = 456;
  const auto selected = generation.selected(7, 10); // Cached ID 10 is readable PQ.
  ASSERT_TRUE(selected);
  EXPECT_FALSE(selected->is_hdr());
  EXPECT_EQ(selected->max_content_light_level, 123);
  EXPECT_EQ(reads, 0u);
  EXPECT_FALSE(generation.commit_pending());
}

TEST_F(KmsHdr, InitialStrictSdrSeedCannotReplaceAnExposedGeneration) {
  for (auto eotf : {1u, 2u, 3u, 255u}) {
    hdr::generation_t fresh;
    hdr::state_t rejected {};
    rejected.present = true;
    rejected.eotf = eotf;
    EXPECT_FALSE(fresh.initialize_sdr(rejected));
    EXPECT_TRUE(fresh.initialize_sdr(hdr::state_t {}));
  }
  hdr::generation_t exposed;
  const auto original = exposed.selected(7, 10);
  ASSERT_TRUE(original->is_hdr());
  EXPECT_FALSE(exposed.initialize_sdr(hdr::state_t {}));
  EXPECT_EQ(exposed.selected(7, 10), original);
}
