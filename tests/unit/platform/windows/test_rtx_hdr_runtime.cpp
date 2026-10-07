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

namespace {
  policy::foreground_state_t app(std::string exe = "C:/Games/Foo/foo.exe") { return {true, true, exe, exe, "Game", "process"}; }
  policy::foreground_state_t mismatch() { return {true, false, "C:/Windows/explorer.exe", "C:/Games/Foo/foo.exe", "Game", "mismatch"}; }
  policy::foreground_state_t desktop() { return {false, true, "C:/Games/Foo/foo.exe", "C:/Games/Foo/foo.exe", "", "fullscreen"}; }
  void lookup(policy::scheduler_t &s, const resolved_profile_t &p = application_profile(), int elapsed = 0, int now = 1) { ASSERT_TRUE(s.complete_profile_lookup(p, std::chrono::milliseconds(elapsed), std::chrono::milliseconds(now), config_values(), {true})); }
}
TEST(RtxHdrRuntimeScheduler, UpdateForFrameReturnsCachedStateWithoutInlineLookup) { policy::scheduler_t s; EXPECT_FALSE(s.frame().enabled); EXPECT_FALSE(s.lookup_pending()); }
TEST(RtxHdrRuntimeScheduler, ForegroundMismatchUsesDesktopBrightnessForRtxStreamWithoutProfileLookup) { policy::scheduler_t s; s.observe_foreground(mismatch(), {}, config_values(), {true}); EXPECT_FALSE(s.frame().enabled); EXPECT_EQ(s.frame().sdr_brightness, 67); EXPECT_FALSE(s.lookup_pending()); }
TEST(RtxHdrRuntimeScheduler, ForegroundMismatchRetainsProfileForSameAppResume) { policy::scheduler_t s; s.observe_foreground(app(), {}, config_values(), {true}); lookup(s); s.observe_foreground(mismatch(), std::chrono::milliseconds(2), config_values(), {true}); s.observe_foreground(app(), std::chrono::milliseconds(3), config_values(), {true}); EXPECT_EQ(s.frame().contrast, 150); EXPECT_TRUE(s.frame().lookup_available); }
TEST(RtxHdrRuntimeScheduler, DisabledRtxHdrStillBypassesDuringForegroundMismatch) { policy::scheduler_t s; auto c=config_values(); c.enabled=false; s.observe_foreground(mismatch(), {}, c, {true}); EXPECT_EQ(s.frame().source, profile_source_e::none); }
TEST(RtxHdrRuntimeScheduler, DesktopFullscreenBypassesWithoutRtxOverride) { policy::scheduler_t s; s.observe_foreground(desktop(), {}, config_values(), {}); EXPECT_EQ(s.frame().sdr_brightness, 0); EXPECT_EQ(s.frame().source, profile_source_e::none); }
TEST(RtxHdrRuntimeScheduler, DesktopFullscreenUsesDesktopBrightnessForRtxStreamWithoutProfileLookup) { policy::scheduler_t s; s.observe_foreground(desktop(), {}, config_values(), {true}); EXPECT_EQ(s.frame().sdr_brightness, 67); EXPECT_FALSE(s.lookup_pending()); }
TEST(RtxHdrRuntimeScheduler, LiveSettingsRefreshDesktopBrightnessForRtxStream) { policy::scheduler_t s; s.observe_foreground(mismatch(), {}, config_values(), {true}); auto c=config_values(); c.sdr_brightness=72; s.refresh_live_settings(c, {true}); EXPECT_EQ(s.frame().sdr_brightness, 72); EXPECT_EQ(s.frame().contrast, 100); }
TEST(RtxHdrRuntimeScheduler, IdentityChangeUsesConfigUntilProfileLookupCompletes) { policy::scheduler_t s; s.observe_foreground(app(), {}, config_values(), {true}); EXPECT_EQ(s.frame().contrast, 125); lookup(s); EXPECT_EQ(s.frame().contrast, 150); }
TEST(RtxHdrRuntimeScheduler, StaleProfileResultIgnoredAfterIdentityChange) { policy::scheduler_t s; s.observe_foreground(app("first.exe"), {}, config_values(), {true}); s.observe_foreground(app("second.exe"), std::chrono::milliseconds(1), config_values(), {true}); lookup(s); EXPECT_EQ(s.frame().active_app_exe, "second.exe"); EXPECT_EQ(s.frame().contrast, 150); }
TEST(RtxHdrRuntimeScheduler, UnavailableOrEmptyProfileBypassesAfterLookup) { policy::scheduler_t s; s.observe_foreground(app(), {}, config_values(), {}); resolved_profile_t unavailable; ASSERT_TRUE(s.complete_profile_lookup(unavailable, {}, std::chrono::milliseconds(1), config_values(), {})); EXPECT_FALSE(s.frame().enabled); EXPECT_FALSE(s.frame().lookup_available); s.observe_foreground(app("bar.exe"), std::chrono::milliseconds(2), config_values(), {}); resolved_profile_t empty; empty.lookup_available=true; ASSERT_TRUE(s.complete_profile_lookup(empty, {}, std::chrono::milliseconds(3), config_values(), {})); EXPECT_TRUE(s.frame().lookup_available); }
TEST(RtxHdrRuntimeScheduler, AppOverrideDoesNotRequireNvidiaProfileLookup) { policy::scheduler_t s; s.observe_foreground(app(), {}, config_values(), {true}); resolved_profile_t unavailable; lookup(s, unavailable); EXPECT_TRUE(s.frame().enabled); EXPECT_EQ(s.frame().contrast, 125); }
TEST(RtxHdrRuntimeScheduler, SlowOrFailingLookupsBackOffAndIdentityChangeResetsInterval) { policy::scheduler_t s; s.observe_foreground(app(), {}, config_values(), {true}); lookup(s, {}, 101); EXPECT_EQ(s.refresh_interval(), std::chrono::seconds(15)); s.observe_foreground(app("bar.exe"), std::chrono::milliseconds(2), config_values(), {true}); EXPECT_EQ(s.refresh_interval(), std::chrono::seconds(5)); }
TEST(RtxHdrRuntimeScheduler, TransientLookupFailureKeepsLastKnownGood) { policy::scheduler_t s; s.observe_foreground(app(), {}, config_values(), {true}); lookup(s); s.observe_foreground(app(), std::chrono::milliseconds(7000), config_values(), {true}); lookup(s, {}, 0, 7001); EXPECT_TRUE(s.frame().enabled); EXPECT_EQ(s.frame().contrast, 150); }
TEST(RtxHdrRuntimeScheduler, LiveTuningGenerationRefreshesCachedFrameWithoutLookup) { policy::scheduler_t s; s.observe_foreground(app(), {}, config_values(), {true}); lookup(s); auto c=config_values(); c.contrast=180; s.refresh_live_settings(c, {true, true}); EXPECT_EQ(s.frame().contrast, 180); EXPECT_EQ(s.frame().source, profile_source_e::config); }
TEST(RtxHdrRuntimeScheduler, LiveSettingsCanEnableDisabledFrame) { policy::scheduler_t s; s.observe_foreground(app(), {}, config_values(), {}); ASSERT_TRUE(s.complete_profile_lookup(application_profile(), {}, std::chrono::milliseconds(1), config_values(), {})); EXPECT_FALSE(s.frame().enabled); s.refresh_live_settings(config_values(), {true}); EXPECT_TRUE(s.frame().enabled); }
TEST(RtxHdrRuntimeScheduler, LiveSettingsCanDisableActiveFrame) { policy::scheduler_t s; s.observe_foreground(app(), {}, config_values(), {true}); lookup(s); auto c=config_values(); c.enabled=false; s.refresh_live_settings(c, {true}); EXPECT_FALSE(s.frame().enabled); }
TEST(RtxHdrRuntimeScheduler, LiveTuningRemovalFallsBackToCachedProfile) { policy::scheduler_t s; s.observe_foreground(app(), {}, config_values(), {true, true}); ASSERT_TRUE(s.complete_profile_lookup(application_profile(), {}, std::chrono::milliseconds(1), config_values(), {true, true})); EXPECT_EQ(s.frame().contrast, 125); s.refresh_live_settings(config_values(), {true}); EXPECT_EQ(s.frame().contrast, 150); }

#endif
