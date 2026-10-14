#include "src/client_hdr_peak.h"

#include <gtest/gtest.h>

#include <tuple>
#include <utility>

namespace peak = client_hdr_peak;
using override_e = config::video_t::dd_t::hdr_request_override_e;

TEST(ClientHdrPeak, RejectsMalformedAndUntrustedIntegers) {
  for (const auto value : {"", "0", "100001", "12.5", "1e3", "-1", "+1", " 900", "900 ", "900nits", "4294967296"}) {
    EXPECT_FALSE(peak::parse_nits(value)) << value;
  }
  EXPECT_FALSE(peak::parse_nits(std::string_view {"900\0ignored", 11}));
}

TEST(ClientHdrPeak, AcceptsReportedBounds) {
  EXPECT_EQ(peak::parse_nits("1"), 1u);
  EXPECT_EQ(peak::parse_nits("100000"), 100000u);
}

TEST(ClientHdrPeak, ExplicitOverrideRemainsAuthoritative) {
  EXPECT_FALSE(peak::resolve({true, 1100u, true, true, "1600", "900"}));
}

TEST(ClientHdrPeak, HostCalibrationPrecedesClientReports) {
  const auto selected = peak::resolve({false, 1100u, true, true, "1600", "900"});
  ASSERT_TRUE(selected);
  EXPECT_EQ(selected->peak_nits, 1100u);
  EXPECT_EQ(selected->source, peak::source_e::host_profile);
}

TEST(ClientHdrPeak, CalibratedClientPrecedesDisplayReport) {
  const auto selected = peak::resolve({false, std::nullopt, true, true, "1600", "900"});
  ASSERT_TRUE(selected);
  EXPECT_EQ(selected->peak_nits, 1600u);
  EXPECT_EQ(selected->source, peak::source_e::calibrated);
}

TEST(ClientHdrPeak, InvalidCalibrationFallsBackToDisplayThenConfiguration) {
  const auto selected = peak::resolve({false, std::nullopt, true, true, "invalid", "900"});
  ASSERT_TRUE(selected);
  EXPECT_EQ(selected->peak_nits, 900u);
  EXPECT_EQ(selected->source, peak::source_e::display_reported);
  EXPECT_FALSE(peak::resolve({false, std::nullopt, true, true, "invalid", "100001"}));
}

TEST(ClientHdrPeak, ClientReportsRequireRealAndEffectiveHdrRequest) {
  for (const auto &[real_hdr, effective_hdr] : {std::pair {false, false}, {false, true}, {true, false}}) {
    EXPECT_FALSE(peak::resolve({false, std::nullopt, real_hdr, effective_hdr, "1600", "900"}));
  }
}

TEST(ClientHdrPeak, ClampsAllAutomaticSourcesToSupportedHostRange) {
  EXPECT_EQ(peak::resolve({false, 300u, false, false, "", ""})->peak_nits, 400u);
  EXPECT_EQ(peak::resolve({false, 3000u, false, false, "", ""})->peak_nits, 2000u);
  EXPECT_EQ(peak::resolve({false, std::nullopt, true, true, "1", ""})->peak_nits, 400u);
  EXPECT_EQ(peak::resolve({false, std::nullopt, true, true, "", "100000"})->peak_nits, 2000u);
}

TEST(ClientHdrPeak, RequestedHdrOverrideIsResolvedBeforeRuntimeConfigApply) {
  const auto off = peak::effective_request(true, false, std::nullopt, override_e::force_on, "force_off");
  EXPECT_FALSE(off.enable_hdr);
  EXPECT_TRUE(off.force_sdr);
  const auto on = peak::effective_request(true, true, true, override_e::force_off, "force_on");
  EXPECT_TRUE(on.enable_hdr);
  EXPECT_FALSE(on.prefer_sdr_10bit);
  EXPECT_FALSE(on.force_sdr);
  EXPECT_TRUE(peak::effective_request(true, false, std::nullopt, override_e::force_off, "auto").enable_hdr);
}

TEST(ClientHdrPeak, AppTenBitSdrPreferenceMatchesLaunchSessionPolicy) {
  EXPECT_TRUE(peak::effective_request(true, false, true, override_e::automatic).prefer_sdr_10bit);
  EXPECT_FALSE(peak::effective_request(true, true, false, override_e::automatic).prefer_sdr_10bit);
  EXPECT_TRUE(peak::effective_request(true, true, std::nullopt, override_e::automatic).prefer_sdr_10bit);
  EXPECT_TRUE(peak::effective_request(true, false, std::nullopt, override_e::force_off).force_sdr);
}

TEST(ClientHdrPeak, TenBitSdrAndForcedOffRequestsCannotInheritClientPeak) {
  for (const auto &[client_hdr, device_sdr, app_sdr, override] : {
         std::tuple {true, false, std::optional<bool> {true}, override_e::automatic},
         std::tuple {true, true, std::optional<bool> {}, override_e::automatic},
         std::tuple {true, false, std::optional<bool> {}, override_e::force_off},
         std::tuple {false, false, std::optional<bool> {}, override_e::force_on},
       }) {
    const auto request = peak::effective_request(client_hdr, device_sdr, app_sdr, override);
    const bool effective_hdr = request.enable_hdr && !request.prefer_sdr_10bit && !request.force_sdr;
    EXPECT_FALSE(peak::resolve({false, std::nullopt, client_hdr, effective_hdr, "1600", "900"}));
  }
}

TEST(ClientHdrPeak, UnreadableHostProfileAllowsClientFallback) {
  const auto selected = peak::resolve({false, 0u, true, true, "1600", "900"});
  ASSERT_TRUE(selected);
  EXPECT_EQ(selected->peak_nits, 1600u);
  EXPECT_EQ(selected->source, peak::source_e::calibrated);
}

TEST(ClientHdrPeak, RemovingPausedForceOffReturnsToAutomaticBase) {
  const auto paused = peak::effective_request(true, false, std::nullopt, override_e::automatic, "force_off");
  EXPECT_TRUE(paused.force_sdr);
  const auto resumed = peak::effective_request(true, false, std::nullopt, override_e::automatic);
  const auto selected = peak::resolve({false, std::nullopt, true,
    resumed.enable_hdr && !resumed.prefer_sdr_10bit && !resumed.force_sdr, "1600", "900"});
  ASSERT_TRUE(selected);
  EXPECT_EQ(selected->peak_nits, 1600u);
}

TEST(ClientHdrPeak, RemovingPausedForceOnReturnsToForcedOffBase) {
  const auto paused = peak::effective_request(true, false, std::nullopt, override_e::force_off, "force_on");
  EXPECT_FALSE(paused.force_sdr);
  const auto resumed = peak::effective_request(true, false, std::nullopt, override_e::force_off);
  EXPECT_FALSE(peak::resolve({false, std::nullopt, true,
    resumed.enable_hdr && !resumed.prefer_sdr_10bit && !resumed.force_sdr, "1600", "900"}));
}
