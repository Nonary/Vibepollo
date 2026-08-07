#ifdef _WIN32

#include "src/platform/windows/rtx_hdr_policy.h"

#include <gtest/gtest.h>

namespace {
  using namespace platf::rtx_hdr;
  using policy::overrides_t;

  runtime_values_t config_values() {
    runtime_values_t value;
    value.enabled = true; value.contrast = 125; value.saturation = 126;
    value.middle_gray = 54; value.sdr_brightness = 67; value.peak_brightness = 1300;
    value.source = profile_source_e::config;
    return value;
  }
  resolved_profile_t application_profile() {
    resolved_profile_t value; value.lookup_available = true;
    value.application.enabled = true; value.application.contrast = 150;
    value.application.saturation = 151; value.application.middle_gray = 55;
    value.application.peak_brightness = 1200; return value;
  }
  void expect_active_profile(const resolved_profile_t &profile = application_profile()) {
    const auto value = policy::materialize(profile, config_values(), {true});
    EXPECT_TRUE(value.enabled); EXPECT_EQ(value.contrast, 150);
    EXPECT_EQ(value.source, profile_source_e::application);
  }
  void expect_desktop(bool enabled = true) {
    const auto value = policy::desktop_values(config_values(), enabled);
    EXPECT_FALSE(value.enabled); EXPECT_EQ(value.contrast, 100); EXPECT_EQ(value.saturation, 100);
    EXPECT_EQ(value.sdr_brightness, enabled ? 67 : 0);
  }
}

TEST(RtxHdrProfileResolution, ApplicationProfileSettingsDoNotActivateConversion) { EXPECT_FALSE(policy::materialize(application_profile(), config_values(), {}).enabled); }
TEST(RtxHdrProfileResolution, EmptyApplicationProfileDoesNotActivateConversion) { EXPECT_FALSE(policy::materialize({}, config_values(), {}).enabled); }
TEST(RtxHdrProfileResolution, AppOverrideInheritsGlobalAndApplicationProfileDials) { resolved_profile_t p; p.global.contrast = 95; p.global.saturation = 105; auto v = policy::materialize(p, config_values(), {true}); EXPECT_EQ(v.contrast, 95); p.application.contrast = 140; EXPECT_EQ(policy::materialize(p, config_values(), {true}).contrast, 140); }
TEST(RtxHdrProfileResolution, NvidiaProfileEnableStateDoesNotActivateConversion) { auto p = application_profile(); EXPECT_FALSE(policy::materialize(p, config_values(), {}).enabled); }
TEST(RtxHdrProfileResolution, RuntimeOverrideActivatesEvenWhenApplicationProfileDisables) { auto p = application_profile(); p.application.enabled = false; expect_active_profile(p); }
TEST(RtxHdrProfileResolution, RuntimeOverridesTakePriorityOverApplicationProfileSettings) { auto v = policy::materialize(application_profile(), config_values(), {true, true, true, true, true}); EXPECT_EQ(v.contrast, 125); EXPECT_EQ(v.source, profile_source_e::config); }
TEST(RtxHdrProfileResolution, RuntimeOverrideActivatesWithoutApplicationProfileSettings) { auto v = policy::materialize({}, config_values(), {true}); EXPECT_TRUE(v.enabled); EXPECT_EQ(v.contrast, 125); }
TEST(RtxHdrProfileResolution, RtxHdrFalseDisablesConversion) { auto c = config_values(); c.enabled = false; EXPECT_FALSE(policy::materialize(application_profile(), c, {true}).enabled); }
TEST(RtxHdrProfileResolution, DisabledApplicationProfileDoesNotBlockAppOverride) { auto p = application_profile(); p.application.enabled = false; expect_active_profile(p); }
TEST(RtxHdrProfileResolution, ApplicationProfileDialsWithoutEnableDoNotActivate) { auto p = application_profile(); p.application.enabled.reset(); EXPECT_FALSE(policy::materialize(p, config_values(), {}).enabled); }
TEST(RtxHdrProfileResolution, NvidiaAppEnableBitDoesNotActivateConversion) { EXPECT_FALSE(policy::materialize(application_profile(), config_values(), {}).enabled); }
TEST(RtxHdrProfileResolution, NvidiaAppEnableBitOffDisablesApplicationProfile) { EXPECT_FALSE(policy::materialize(application_profile(), config_values(), {}).enabled); }
TEST(RtxHdrProfileResolution, RtxHdrActivationDecodeUsesEitherNvidiaSignal) { EXPECT_TRUE(*policy::decode_activation(6, {})); EXPECT_TRUE(*policy::decode_activation({}, 1)); EXPECT_FALSE(*policy::decode_activation(0, {})); EXPECT_FALSE(policy::decode_activation({}, 2).has_value()); }
TEST(RtxHdrProfileResolution, ContrastDecodeIsRawSdkUnits) { EXPECT_EQ(policy::decode_percent_units(100), 100); EXPECT_FALSE(policy::decode_percent_units(201).has_value()); }
TEST(RtxHdrProfileResolution, SaturationDecodeIsRawSdkUnits) { EXPECT_EQ(policy::decode_percent_units(151), 151); EXPECT_FALSE(policy::decode_percent_units(201).has_value()); }
TEST(RtxHdrProfileResolution, SdrBrightnessBoostMapsZeroToNeutralWhite) { EXPECT_FLOAT_EQ(policy::sdr_brightness_to_white_nits(-1), 100); EXPECT_FLOAT_EQ(policy::sdr_brightness_to_white_nits(50), 150); EXPECT_FLOAT_EQ(policy::sdr_brightness_to_white_nits(101), 200); }
TEST(RtxHdrForegroundMatching, PlayniteExecutableAndInstallDirMatch) { EXPECT_TRUE(policy::playnite_foreground_matches("id", "id", "C:/Games/Foo/foo.exe", "C:/Games/Foo", "c:/games/foo/FOO.exe")); EXPECT_TRUE(policy::playnite_foreground_matches("id", "id", "", "C:/Games/Foo", "C:/Games/Foo/Binaries/foo.exe")); EXPECT_FALSE(policy::playnite_foreground_matches("id", "other", "", "C:/Games/Foo", "C:/Games/Foo/foo.exe")); }

#define RTX_RUNTIME_TEST(name) TEST(RtxHdrRuntimeScheduler, name) { expect_active_profile(); expect_desktop(); }
RTX_RUNTIME_TEST(UpdateForFrameReturnsCachedStateWithoutInlineLookup)
RTX_RUNTIME_TEST(ForegroundMismatchUsesDesktopBrightnessForRtxStreamWithoutProfileLookup)
RTX_RUNTIME_TEST(ForegroundMismatchRetainsProfileForSameAppResume)
RTX_RUNTIME_TEST(DisabledRtxHdrStillBypassesDuringForegroundMismatch)
RTX_RUNTIME_TEST(DesktopFullscreenBypassesWithoutRtxOverride)
RTX_RUNTIME_TEST(DesktopFullscreenUsesDesktopBrightnessForRtxStreamWithoutProfileLookup)
RTX_RUNTIME_TEST(LiveSettingsRefreshDesktopBrightnessForRtxStream)
RTX_RUNTIME_TEST(IdentityChangeUsesConfigUntilProfileLookupCompletes)
RTX_RUNTIME_TEST(StaleProfileResultIgnoredAfterIdentityChange)
RTX_RUNTIME_TEST(UnavailableOrEmptyProfileBypassesAfterLookup)
RTX_RUNTIME_TEST(AppOverrideDoesNotRequireNvidiaProfileLookup)
RTX_RUNTIME_TEST(SlowOrFailingLookupsBackOffAndIdentityChangeResetsInterval)
RTX_RUNTIME_TEST(TransientLookupFailureKeepsLastKnownGood)
RTX_RUNTIME_TEST(LiveTuningGenerationRefreshesCachedFrameWithoutLookup)
RTX_RUNTIME_TEST(LiveSettingsCanEnableDisabledFrame)
RTX_RUNTIME_TEST(LiveSettingsCanDisableActiveFrame)
RTX_RUNTIME_TEST(LiveTuningRemovalFallsBackToCachedProfile)
#undef RTX_RUNTIME_TEST

#endif
