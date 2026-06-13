#ifdef _WIN32

#include "src/platform/windows/foreground_app.h"
#include "src/platform/windows/rtx_hdr_profile.h"

#include <gtest/gtest.h>

using platf::rtx_hdr::materialize_runtime_values_for_tests;
using platf::rtx_hdr::profile_source_e;
using platf::rtx_hdr::resolved_profile_t;
using platf::rtx_hdr::runtime_values_t;

TEST(RtxHdrProfileResolution, PerGameSettingsOverrideGlobalSettings) {
  resolved_profile_t resolved;
  resolved.lookup_available = true;
  resolved.application.enabled = true;
  resolved.application.contrast = 140;
  resolved.application.peak_brightness = 1200;
  resolved.global.enabled = true;
  resolved.global.contrast = 90;
  resolved.global.saturation = 80;
  resolved.global.middle_gray = 45;
  resolved.global.peak_brightness = 800;

  runtime_values_t config;
  config.enabled = true;
  config.contrast = 100;
  config.saturation = 100;
  config.middle_gray = 50;
  config.peak_brightness = 1000;
  config.source = profile_source_e::config;

  const auto values = materialize_runtime_values_for_tests(resolved, config);

  EXPECT_TRUE(values.enabled);
  EXPECT_EQ(values.contrast, 140);
  EXPECT_EQ(values.saturation, 80);
  EXPECT_EQ(values.middle_gray, 45);
  EXPECT_EQ(values.peak_brightness, 1200);
  EXPECT_EQ(values.source, profile_source_e::application);
}

TEST(RtxHdrProfileResolution, MissingPerGameSettingsFallBackToGlobalSettings) {
  resolved_profile_t resolved;
  resolved.lookup_available = true;
  resolved.global.enabled = true;
  resolved.global.contrast = 95;
  resolved.global.saturation = 105;
  resolved.global.middle_gray = 55;
  resolved.global.peak_brightness = 900;

  runtime_values_t config;
  config.enabled = true;
  config.contrast = 130;
  config.saturation = 130;
  config.middle_gray = 60;
  config.peak_brightness = 1500;
  config.source = profile_source_e::config;

  const auto values = materialize_runtime_values_for_tests(resolved, config);

  EXPECT_TRUE(values.enabled);
  EXPECT_EQ(values.contrast, 95);
  EXPECT_EQ(values.saturation, 105);
  EXPECT_EQ(values.middle_gray, 55);
  EXPECT_EQ(values.peak_brightness, 900);
  EXPECT_EQ(values.source, profile_source_e::global);
}

TEST(RtxHdrProfileResolution, DisabledPerGameSettingPreventsConversion) {
  resolved_profile_t resolved;
  resolved.lookup_available = true;
  resolved.application.enabled = false;
  resolved.application.contrast = 180;
  resolved.application.saturation = 180;
  resolved.application.peak_brightness = 1600;
  resolved.global.enabled = true;

  runtime_values_t config;
  config.enabled = true;
  config.source = profile_source_e::config;

  const auto values = materialize_runtime_values_for_tests(resolved, config);

  EXPECT_FALSE(values.enabled);
  EXPECT_EQ(values.source, profile_source_e::application);
}

TEST(RtxHdrProfileResolution, UnavailableNvapiFallsBackToConfig) {
  resolved_profile_t resolved;
  resolved.lookup_available = false;

  runtime_values_t config;
  config.enabled = true;
  config.contrast = 123;
  config.saturation = 124;
  config.middle_gray = 52;
  config.peak_brightness = 1100;
  config.source = profile_source_e::config;

  const auto values = materialize_runtime_values_for_tests(resolved, config);

  EXPECT_TRUE(values.enabled);
  EXPECT_EQ(values.contrast, 123);
  EXPECT_EQ(values.saturation, 124);
  EXPECT_EQ(values.middle_gray, 52);
  EXPECT_EQ(values.peak_brightness, 1100);
  EXPECT_EQ(values.source, profile_source_e::config);
}

TEST(RtxHdrProfileResolution, MissingAllRtxHdrProfileDataFallsBackToConfig) {
  resolved_profile_t resolved;
  resolved.lookup_available = true;

  runtime_values_t config;
  config.enabled = true;
  config.contrast = 111;
  config.saturation = 112;
  config.middle_gray = 53;
  config.peak_brightness = 1300;
  config.source = profile_source_e::config;

  const auto values = materialize_runtime_values_for_tests(resolved, config);

  EXPECT_TRUE(values.enabled);
  EXPECT_EQ(values.contrast, 111);
  EXPECT_EQ(values.saturation, 112);
  EXPECT_EQ(values.middle_gray, 53);
  EXPECT_EQ(values.peak_brightness, 1300);
  EXPECT_EQ(values.source, profile_source_e::config);
}

TEST(RtxHdrForegroundMatching, PlayniteExecutableAndInstallDirMatch) {
  EXPECT_TRUE(platf::foreground_app::playnite_foreground_matches_for_tests(
    "game-id",
    "game-id",
    "C:/Games/Foo/foo.exe",
    "C:/Games/Foo",
    "c:\\games\\foo\\FOO.exe"
  ));

  EXPECT_TRUE(platf::foreground_app::playnite_foreground_matches_for_tests(
    "game-id",
    "game-id",
    "",
    "C:/Games/Foo",
    "C:/Games/Foo/Binaries/Win64/foo-win64-shipping.exe"
  ));

  EXPECT_FALSE(platf::foreground_app::playnite_foreground_matches_for_tests(
    "game-id",
    "other-id",
    "C:/Games/Foo/foo.exe",
    "C:/Games/Foo",
    "C:/Games/Foo/foo.exe"
  ));

  EXPECT_FALSE(platf::foreground_app::playnite_foreground_matches_for_tests(
    "game-id",
    "game-id",
    "",
    "C:/Games/Foo",
    "C:/Windows/explorer.exe"
  ));
}

#endif
