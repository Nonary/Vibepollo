#ifdef _WIN32

#include "src/platform/windows/foreground_app.h"
#include "src/platform/windows/rtx_hdr_profile.h"
#include "src/platform/windows/rtx_hdr_runtime.h"

#include "src/config.h"

#include <gtest/gtest.h>

#include <functional>
#include <map>
#include <unordered_map>
#include <vector>

using platf::rtx_hdr::materialize_runtime_values_for_tests;
using platf::rtx_hdr::profile_source_e;
using platf::rtx_hdr::resolved_profile_t;
using platf::rtx_hdr::runtime_t;
using platf::rtx_hdr::runtime_test_hooks_t;
using platf::rtx_hdr::runtime_values_t;

namespace {
  struct rtx_hdr_config_guard_t {
    decltype(config::video.rtx_hdr) original;

    rtx_hdr_config_guard_t():
        original {config::video.rtx_hdr} {
      config::clear_runtime_config_overrides();
      config::video.rtx_hdr.enabled = true;
      config::video.rtx_hdr.force_sdr = false;
      config::video.rtx_hdr.contrast = 0;
      config::video.rtx_hdr.saturation = 0;
      config::video.rtx_hdr.middle_gray = 50;
      config::video.rtx_hdr.peak_brightness = 1000;
    }

    ~rtx_hdr_config_guard_t() {
      config::video.rtx_hdr = original;
      config::clear_runtime_config_overrides();
    }
  };

  platf::foreground_app::state_t matching_foreground(const std::string &executable, const std::string &name = "Game") {
    platf::foreground_app::state_t state;
    state.valid_window = true;
    state.has_active_app = true;
    state.matches_active_app = true;
    state.foreground_pid = 42;
    state.foreground_exe = executable;
    state.active_app_name = name;
    state.active_app_exe = executable;
    state.source = "process";
    return state;
  }

  platf::foreground_app::state_t mismatched_foreground(const std::string &executable = "C:/Windows/explorer.exe") {
    platf::foreground_app::state_t state;
    state.valid_window = true;
    state.has_active_app = true;
    state.matches_active_app = false;
    state.foreground_pid = 100;
    state.foreground_exe = executable;
    state.active_app_name = "Game";
    state.active_app_exe = "C:/Games/Game/game.exe";
    state.source = "foreground-mismatch";
    return state;
  }

  resolved_profile_t enabled_profile(const std::string &executable, int contrast = 140) {
    resolved_profile_t resolved;
    resolved.lookup_available = true;
    resolved.executable = executable;
    resolved.source = profile_source_e::application;
    resolved.profile_name = "application";
    resolved.application.enabled = true;
    resolved.application.contrast = contrast;
    resolved.application.saturation = 120;
    resolved.application.middle_gray = 55;
    resolved.application.peak_brightness = 1200;
    return resolved;
  }

  class fake_runtime_t {
  public:
    fake_runtime_t():
        runtime {runtime_test_hooks_t {
          [this]() {
            return now;
          },
          [this](const std::optional<RECT> &capture_rect) {
            ++foreground_calls;
            last_capture_rect = capture_rect;
            return foreground;
          },
          [this](const std::string &executable) {
            ++resolve_calls;
            resolved_executables.push_back(executable);
            const auto it = profiles.find(executable);
            if (before_resolve_returns) {
              before_resolve_returns(executable);
            }
            now += lookup_duration;
            if (it != profiles.end()) {
              return it->second;
            }
            resolved_profile_t resolved;
            resolved.executable = executable;
            return resolved;
          },
          false,
        }} {
    }

    std::chrono::steady_clock::time_point now {};
    std::chrono::milliseconds lookup_duration {0};
    platf::foreground_app::state_t foreground;
    std::optional<RECT> last_capture_rect;
    int foreground_calls {0};
    int resolve_calls {0};
    std::vector<std::string> resolved_executables;
    std::map<std::string, resolved_profile_t> profiles;
    std::function<void(const std::string &executable)> before_resolve_returns;
    runtime_t runtime;
  };
}  // namespace

TEST(RtxHdrProfileResolution, ApplicationProfileSettingsActivateConversion) {
  rtx_hdr_config_guard_t config_guard;
  resolved_profile_t resolved;
  resolved.lookup_available = true;
  resolved.application.enabled = true;
  resolved.application.contrast = 140;
  resolved.application.peak_brightness = 1200;

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
  EXPECT_EQ(values.saturation, 100);
  EXPECT_EQ(values.middle_gray, 50);
  EXPECT_EQ(values.peak_brightness, 1200);
  EXPECT_EQ(values.source, profile_source_e::application);
}

TEST(RtxHdrProfileResolution, EmptyApplicationProfileDoesNotActivateConversion) {
  rtx_hdr_config_guard_t config_guard;
  resolved_profile_t resolved;
  resolved.lookup_available = true;

  runtime_values_t config;
  config.enabled = true;
  config.contrast = 130;
  config.saturation = 130;
  config.middle_gray = 60;
  config.peak_brightness = 1500;
  config.source = profile_source_e::config;

  const auto values = materialize_runtime_values_for_tests(resolved, config);

  EXPECT_FALSE(values.enabled);
  EXPECT_EQ(values.source, profile_source_e::none);
}

TEST(RtxHdrProfileResolution, GlobalProfileEnabledActivatesAndApplicationProfileRefines) {
  rtx_hdr_config_guard_t config_guard;
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
  config.saturation = 131;
  config.middle_gray = 60;
  config.peak_brightness = 1500;
  config.source = profile_source_e::config;

  // Driver global RTX HDR on => auto-apply even to a game with no profile of its own,
  // using the global profile's dial values.
  const auto values = materialize_runtime_values_for_tests(resolved, config);
  EXPECT_TRUE(values.enabled);
  EXPECT_EQ(values.contrast, 95);
  EXPECT_EQ(values.saturation, 105);
  EXPECT_EQ(values.middle_gray, 55);
  EXPECT_EQ(values.peak_brightness, 900);
  EXPECT_EQ(values.source, profile_source_e::global);

  // A per-game application profile wins for the values it sets, falling back to global.
  resolved.application.enabled = true;
  resolved.application.contrast = 140;
  const auto app_values = materialize_runtime_values_for_tests(resolved, config);
  EXPECT_TRUE(app_values.enabled);
  EXPECT_EQ(app_values.contrast, 140);
  EXPECT_EQ(app_values.saturation, 105);
  EXPECT_EQ(app_values.middle_gray, 55);
  EXPECT_EQ(app_values.peak_brightness, 900);
  EXPECT_EQ(app_values.source, profile_source_e::application);
}

TEST(RtxHdrProfileResolution, ApplicationProfileDisableWinsOverGlobalEnable) {
  rtx_hdr_config_guard_t config_guard;
  resolved_profile_t resolved;
  resolved.lookup_available = true;
  resolved.global.enabled = true;
  resolved.global.contrast = 120;
  resolved.application.enabled = false;
  resolved.application.contrast = 175;

  runtime_values_t config;
  config.enabled = true;
  config.source = profile_source_e::config;

  // NVIDIA inheritance: a per-game profile that disables RTX HDR overrides the global "on".
  const auto values = materialize_runtime_values_for_tests(resolved, config);
  EXPECT_FALSE(values.enabled);
  EXPECT_EQ(values.source, profile_source_e::application);
}

TEST(RtxHdrProfileResolution, RuntimeOverrideActivatesEvenWhenApplicationProfileDisables) {
  rtx_hdr_config_guard_t config_guard;
  config::set_runtime_config_overrides(std::unordered_map<std::string, std::string> {
    {"rtx_hdr", "true"},
  });

  resolved_profile_t resolved;
  resolved.lookup_available = true;
  resolved.application.enabled = false;
  resolved.application.contrast = 175;

  runtime_values_t config;
  config.enabled = true;
  config.source = profile_source_e::config;

  // An explicit Sunshine launch override forces RTX HDR on even past an app-profile disable.
  const auto values = materialize_runtime_values_for_tests(resolved, config);
  EXPECT_TRUE(values.enabled);
  EXPECT_EQ(values.source, profile_source_e::config);
}

TEST(RtxHdrProfileResolution, RuntimeOverridesTakePriorityOverApplicationProfileSettings) {
  rtx_hdr_config_guard_t config_guard;
  config::video.rtx_hdr.contrast = 80;
  config::video.rtx_hdr.saturation = 81;
  config::video.rtx_hdr.middle_gray = 82;
  config::video.rtx_hdr.peak_brightness = 1500;
  config::set_runtime_config_overrides(std::unordered_map<std::string, std::string> {
    {"rtx_hdr_contrast", "80"},
    {"rtx_hdr_saturation", "81"},
    {"rtx_hdr_middle_gray", "82"},
    {"rtx_hdr_peak_brightness", "1500"},
  });

  resolved_profile_t resolved;
  resolved.lookup_available = true;
  resolved.application.enabled = true;
  resolved.application.contrast = 140;
  resolved.application.saturation = 120;
  resolved.application.middle_gray = 55;
  resolved.application.peak_brightness = 1200;

  runtime_values_t config;
  config.enabled = true;
  config.contrast = 180;
  config.saturation = 181;
  config.middle_gray = 82;
  config.peak_brightness = 1500;
  config.source = profile_source_e::config;

  const auto values = materialize_runtime_values_for_tests(resolved, config);

  EXPECT_TRUE(values.enabled);
  EXPECT_EQ(values.contrast, 180);
  EXPECT_EQ(values.saturation, 181);
  EXPECT_EQ(values.middle_gray, 82);
  EXPECT_EQ(values.peak_brightness, 1500);
  EXPECT_EQ(values.source, profile_source_e::application);
}

TEST(RtxHdrProfileResolution, RuntimeOverrideActivatesWithoutApplicationProfileSettings) {
  rtx_hdr_config_guard_t config_guard;
  config::video.rtx_hdr.contrast = 25;
  config::video.rtx_hdr.saturation = 26;
  config::video.rtx_hdr.middle_gray = 54;
  config::video.rtx_hdr.peak_brightness = 1300;
  config::set_runtime_config_overrides(std::unordered_map<std::string, std::string> {
    {"rtx_hdr", "true"},
  });

  resolved_profile_t resolved;
  resolved.lookup_available = true;

  runtime_values_t config;
  config.enabled = true;
  config.contrast = 125;
  config.saturation = 126;
  config.middle_gray = 54;
  config.peak_brightness = 1300;
  config.source = profile_source_e::config;

  const auto values = materialize_runtime_values_for_tests(resolved, config);

  EXPECT_TRUE(values.enabled);
  EXPECT_EQ(values.contrast, 125);
  EXPECT_EQ(values.saturation, 126);
  EXPECT_EQ(values.middle_gray, 54);
  EXPECT_EQ(values.peak_brightness, 1300);
  EXPECT_EQ(values.source, profile_source_e::config);
}

TEST(RtxHdrProfileResolution, RtxHdrFalseDisablesConversion) {
  rtx_hdr_config_guard_t config_guard;
  resolved_profile_t resolved;
  resolved.lookup_available = true;
  resolved.application.enabled = true;
  resolved.application.contrast = 140;

  runtime_values_t config;
  config.enabled = false;
  config.contrast = 111;
  config.saturation = 112;
  config.middle_gray = 53;
  config.peak_brightness = 1300;
  config.source = profile_source_e::config;

  const auto values = materialize_runtime_values_for_tests(resolved, config);

  EXPECT_FALSE(values.enabled);
  EXPECT_EQ(values.source, profile_source_e::config);
}

TEST(RtxHdrProfileResolution, DisabledApplicationProfilePreventsConversion) {
  rtx_hdr_config_guard_t config_guard;
  resolved_profile_t resolved;
  resolved.lookup_available = true;
  resolved.application.enabled = false;
  resolved.application.contrast = 180;
  resolved.application.saturation = 180;
  resolved.application.peak_brightness = 1600;

  runtime_values_t config;
  config.enabled = true;
  config.source = profile_source_e::config;

  const auto values = materialize_runtime_values_for_tests(resolved, config);

  EXPECT_FALSE(values.enabled);
  EXPECT_EQ(values.source, profile_source_e::application);
}

TEST(RtxHdrProfileResolution, ApplicationProfileDialsWithoutEnableDoNotActivate) {
  rtx_hdr_config_guard_t config_guard;
  resolved_profile_t resolved;
  resolved.lookup_available = true;
  resolved.application.contrast = 160;  // dial present, but no explicit enable in any profile

  runtime_values_t config;
  config.enabled = true;
  config.source = profile_source_e::config;

  const auto values = materialize_runtime_values_for_tests(resolved, config);
  EXPECT_FALSE(values.enabled);
  EXPECT_EQ(values.source, profile_source_e::none);
}

TEST(RtxHdrProfileResolution, NvidiaAppEnableBitActivatesWithApplicationDials) {
  rtx_hdr_config_guard_t config_guard;
  resolved_profile_t resolved;
  resolved.lookup_available = true;
  resolved.application.enabled = platf::rtx_hdr::decode_rtx_hdr_activation_for_tests(std::nullopt, 1);
  resolved.application.contrast = 150;
  resolved.application.saturation = 151;
  resolved.application.middle_gray = 64;
  resolved.application.peak_brightness = 1499;
  resolved.global.contrast = 112;
  resolved.global.saturation = 200;
  resolved.global.peak_brightness = 400;

  runtime_values_t config;
  config.enabled = true;
  config.contrast = 180;
  config.saturation = 150;
  config.middle_gray = 82;
  config.peak_brightness = 2000;
  config.source = profile_source_e::config;

  const auto values = materialize_runtime_values_for_tests(resolved, config);

  EXPECT_TRUE(values.enabled);
  EXPECT_EQ(values.source, profile_source_e::application);
  EXPECT_EQ(values.contrast, 150);
  EXPECT_EQ(values.saturation, 151);
  EXPECT_EQ(values.middle_gray, 64);
  EXPECT_EQ(values.peak_brightness, 1499);
}

TEST(RtxHdrProfileResolution, NvidiaAppEnableBitOffDisablesApplicationProfile) {
  rtx_hdr_config_guard_t config_guard;
  resolved_profile_t resolved;
  resolved.lookup_available = true;
  resolved.global.enabled = true;
  resolved.application.enabled = platf::rtx_hdr::decode_rtx_hdr_activation_for_tests(std::nullopt, 0);
  resolved.application.contrast = 150;

  runtime_values_t config;
  config.enabled = true;
  config.source = profile_source_e::config;

  const auto values = materialize_runtime_values_for_tests(resolved, config);

  EXPECT_FALSE(values.enabled);
  EXPECT_EQ(values.source, profile_source_e::application);
}

TEST(RtxHdrProfileResolution, RtxHdrActivationDecodeUsesEitherNvidiaSignal) {
  auto values = platf::rtx_hdr::decode_rtx_hdr_activation_for_tests(0x06, std::nullopt);
  ASSERT_TRUE(values.has_value());
  EXPECT_TRUE(*values);

  values = platf::rtx_hdr::decode_rtx_hdr_activation_for_tests(std::nullopt, 1);
  ASSERT_TRUE(values.has_value());
  EXPECT_TRUE(*values);

  values = platf::rtx_hdr::decode_rtx_hdr_activation_for_tests(0, std::nullopt);
  ASSERT_TRUE(values.has_value());
  EXPECT_FALSE(*values);

  values = platf::rtx_hdr::decode_rtx_hdr_activation_for_tests(std::nullopt, 0);
  ASSERT_TRUE(values.has_value());
  EXPECT_FALSE(*values);

  EXPECT_FALSE(platf::rtx_hdr::decode_rtx_hdr_activation_for_tests(std::nullopt, std::nullopt).has_value());
  EXPECT_FALSE(platf::rtx_hdr::decode_rtx_hdr_activation_for_tests(std::nullopt, 2).has_value());
}

TEST(RtxHdrProfileResolution, ContrastDecodeIsRawSdkUnits) {
  EXPECT_EQ(platf::rtx_hdr::decode_rtx_hdr_contrast_units_for_tests(100), 100);  // neutral
  EXPECT_EQ(platf::rtx_hdr::decode_rtx_hdr_contrast_units_for_tests(0), 0);
  EXPECT_EQ(platf::rtx_hdr::decode_rtx_hdr_contrast_units_for_tests(200), 200);
  EXPECT_FALSE(platf::rtx_hdr::decode_rtx_hdr_contrast_units_for_tests(201).has_value());
  EXPECT_FALSE(platf::rtx_hdr::decode_rtx_hdr_contrast_units_for_tests(0xFFFFFFE7u).has_value());
}

TEST(RtxHdrProfileResolution, SaturationDecodeIsRawSdkUnits) {
  EXPECT_EQ(platf::rtx_hdr::decode_rtx_hdr_saturation_units_for_tests(100), 100);  // neutral
  EXPECT_EQ(platf::rtx_hdr::decode_rtx_hdr_saturation_units_for_tests(0), 0);
  EXPECT_EQ(platf::rtx_hdr::decode_rtx_hdr_saturation_units_for_tests(151), 151);
  EXPECT_EQ(platf::rtx_hdr::decode_rtx_hdr_saturation_units_for_tests(200), 200);
  EXPECT_FALSE(platf::rtx_hdr::decode_rtx_hdr_saturation_units_for_tests(201).has_value());
  EXPECT_FALSE(platf::rtx_hdr::decode_rtx_hdr_saturation_units_for_tests(0xFFFFFFE7u).has_value());
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

TEST(RtxHdrRuntimeScheduler, UpdateForFrameReturnsCachedStateWithoutInlineLookup) {
  rtx_hdr_config_guard_t config_guard;
  fake_runtime_t fake;
  fake.foreground = matching_foreground("C:/Games/Foo/foo.exe");
  fake.profiles[fake.foreground.active_app_exe] = enabled_profile(fake.foreground.active_app_exe);

  const auto frame = fake.runtime.update_for_frame(std::nullopt);

  EXPECT_FALSE(frame.enabled);
  EXPECT_EQ(fake.foreground_calls, 0);
  EXPECT_EQ(fake.resolve_calls, 0);
}

TEST(RtxHdrRuntimeScheduler, ForegroundMismatchDisablesWithoutProfileLookup) {
  rtx_hdr_config_guard_t config_guard;
  fake_runtime_t fake;
  fake.foreground = matching_foreground("C:/Games/Foo/foo.exe");
  fake.profiles[fake.foreground.active_app_exe] = enabled_profile(fake.foreground.active_app_exe);

  fake.runtime.poll_foreground_for_tests();
  ASSERT_TRUE(fake.runtime.run_pending_profile_lookup_for_tests());
  ASSERT_TRUE(fake.runtime.update_for_frame(std::nullopt).enabled);
  ASSERT_EQ(fake.resolve_calls, 1);

  fake.foreground = mismatched_foreground();
  fake.runtime.poll_foreground_for_tests();
  const auto frame = fake.runtime.update_for_frame(std::nullopt);

  EXPECT_FALSE(frame.enabled);
  EXPECT_FALSE(frame.foreground_matches);
  EXPECT_EQ(frame.source, profile_source_e::none);
  EXPECT_EQ(fake.resolve_calls, 1);

  fake.foreground = matching_foreground("C:/Games/Bar/bar.exe", "Bar");
  fake.runtime.poll_foreground_for_tests();
  fake.foreground = mismatched_foreground();
  fake.runtime.poll_foreground_for_tests();
  EXPECT_FALSE(fake.runtime.run_pending_profile_lookup_for_tests());
  EXPECT_EQ(fake.resolve_calls, 1);
}

TEST(RtxHdrRuntimeScheduler, RuntimeOverrideDoesNotActivateDuringForegroundMismatch) {
  rtx_hdr_config_guard_t config_guard;
  config::video.rtx_hdr.contrast = 25;
  config::video.rtx_hdr.saturation = 26;
  config::video.rtx_hdr.middle_gray = 54;
  config::video.rtx_hdr.peak_brightness = 1300;
  config::set_runtime_config_overrides(std::unordered_map<std::string, std::string> {
    {"rtx_hdr", "true"},
  });

  fake_runtime_t fake;
  fake.foreground = mismatched_foreground();

  fake.runtime.poll_foreground_for_tests();
  const auto frame = fake.runtime.update_for_frame(std::nullopt);

  EXPECT_FALSE(frame.enabled);
  EXPECT_FALSE(frame.foreground_matches);
  EXPECT_EQ(frame.source, profile_source_e::none);
  EXPECT_EQ(fake.resolve_calls, 0);
}

TEST(RtxHdrRuntimeScheduler, IdentityChangeBypassesUntilProfileLookupCompletes) {
  rtx_hdr_config_guard_t config_guard;
  fake_runtime_t fake;
  fake.foreground = matching_foreground("C:/Games/Foo/foo.exe");
  fake.profiles[fake.foreground.active_app_exe] = enabled_profile(fake.foreground.active_app_exe, 150);

  fake.runtime.poll_foreground_for_tests();
  auto frame = fake.runtime.update_for_frame(std::nullopt);
  EXPECT_TRUE(frame.foreground_matches);
  EXPECT_FALSE(frame.enabled);
  EXPECT_EQ(frame.source, profile_source_e::none);
  EXPECT_EQ(fake.resolve_calls, 0);

  ASSERT_TRUE(fake.runtime.run_pending_profile_lookup_for_tests());
  frame = fake.runtime.update_for_frame(std::nullopt);
  EXPECT_TRUE(frame.enabled);
  EXPECT_EQ(frame.contrast, 150);
  EXPECT_EQ(frame.source, profile_source_e::application);
}

TEST(RtxHdrRuntimeScheduler, StaleProfileResultIgnoredAfterIdentityChange) {
  rtx_hdr_config_guard_t config_guard;
  fake_runtime_t fake;
  const std::string first_exe = "C:/Games/First/first.exe";
  const std::string second_exe = "C:/Games/Second/second.exe";
  fake.foreground = matching_foreground(first_exe, "First");
  fake.profiles[first_exe] = enabled_profile(first_exe, 130);
  fake.profiles[second_exe] = enabled_profile(second_exe, 170);
  bool changed_identity_during_lookup = false;
  fake.before_resolve_returns = [&](const std::string &executable) {
    if (executable != first_exe || changed_identity_during_lookup) {
      return;
    }

    changed_identity_during_lookup = true;
    fake.foreground = matching_foreground(second_exe, "Second");
    fake.runtime.poll_foreground_for_tests();
  };

  fake.runtime.poll_foreground_for_tests();
  ASSERT_TRUE(fake.runtime.run_pending_profile_lookup_for_tests());
  auto frame = fake.runtime.update_for_frame(std::nullopt);
  EXPECT_TRUE(frame.foreground_matches);
  EXPECT_FALSE(frame.enabled);
  EXPECT_EQ(frame.active_app_exe, second_exe);
  EXPECT_EQ(frame.source, profile_source_e::none);

  ASSERT_TRUE(fake.runtime.run_pending_profile_lookup_for_tests());
  frame = fake.runtime.update_for_frame(std::nullopt);
  EXPECT_TRUE(frame.enabled);
  EXPECT_EQ(frame.contrast, 170);
  EXPECT_EQ(frame.active_app_exe, second_exe);
  EXPECT_EQ(fake.resolve_calls, 2);
}

TEST(RtxHdrRuntimeScheduler, UnavailableOrEmptyProfileBypassesAfterLookup) {
  rtx_hdr_config_guard_t config_guard;
  config::video.rtx_hdr.contrast = 23;
  config::video.rtx_hdr.saturation = 24;
  config::video.rtx_hdr.middle_gray = 52;
  config::video.rtx_hdr.peak_brightness = 1100;

  fake_runtime_t fake;
  fake.foreground = matching_foreground("C:/Games/Foo/foo.exe");
  resolved_profile_t unavailable;
  unavailable.executable = fake.foreground.active_app_exe;
  fake.profiles[fake.foreground.active_app_exe] = unavailable;

  fake.runtime.poll_foreground_for_tests();
  ASSERT_TRUE(fake.runtime.run_pending_profile_lookup_for_tests());
  auto frame = fake.runtime.update_for_frame(std::nullopt);
  EXPECT_FALSE(frame.enabled);
  EXPECT_EQ(frame.source, profile_source_e::none);
  EXPECT_FALSE(frame.lookup_available);

  fake.foreground = matching_foreground("C:/Games/Bar/bar.exe", "Bar");
  resolved_profile_t empty;
  empty.lookup_available = true;
  empty.executable = fake.foreground.active_app_exe;
  fake.profiles[fake.foreground.active_app_exe] = empty;
  fake.runtime.poll_foreground_for_tests();
  ASSERT_TRUE(fake.runtime.run_pending_profile_lookup_for_tests());
  frame = fake.runtime.update_for_frame(std::nullopt);
  EXPECT_FALSE(frame.enabled);
  EXPECT_EQ(frame.source, profile_source_e::none);
  EXPECT_TRUE(frame.lookup_available);
}

TEST(RtxHdrRuntimeScheduler, SlowOrFailingLookupsBackOffAndIdentityChangeResetsInterval) {
  rtx_hdr_config_guard_t config_guard;
  fake_runtime_t fake;
  const std::string first_exe = "C:/Games/First/first.exe";
  const std::string second_exe = "C:/Games/Second/second.exe";
  fake.foreground = matching_foreground(first_exe, "First");
  fake.lookup_duration = std::chrono::milliseconds(101);

  resolved_profile_t unavailable;
  unavailable.executable = first_exe;
  fake.profiles[first_exe] = unavailable;

  fake.runtime.poll_foreground_for_tests();
  ASSERT_TRUE(fake.runtime.run_pending_profile_lookup_for_tests());
  EXPECT_EQ(fake.runtime.profile_refresh_interval_for_tests(), std::chrono::seconds(15));

  fake.foreground = matching_foreground(second_exe, "Second");
  fake.profiles[second_exe] = enabled_profile(second_exe);
  fake.runtime.poll_foreground_for_tests();
  EXPECT_EQ(fake.runtime.profile_refresh_interval_for_tests(), std::chrono::seconds(5));
}

TEST(RtxHdrRuntimeScheduler, TransientLookupFailureKeepsLastKnownGood) {
  rtx_hdr_config_guard_t config_guard;
  fake_runtime_t fake;
  const std::string exe = "C:/Games/Foo/foo.exe";
  fake.foreground = matching_foreground(exe);
  fake.profiles[exe] = enabled_profile(exe, 150);

  fake.runtime.poll_foreground_for_tests();
  ASSERT_TRUE(fake.runtime.run_pending_profile_lookup_for_tests());
  auto frame = fake.runtime.update_for_frame(std::nullopt);
  ASSERT_TRUE(frame.enabled);
  EXPECT_EQ(frame.contrast, 150);

  // A later refresh transiently fails (NvAPI unavailable). The active conversion should keep
  // streaming the last-known-good profile instead of flickering off for one cycle.
  resolved_profile_t unavailable;
  unavailable.executable = exe;
  fake.profiles[exe] = unavailable;
  fake.now += std::chrono::seconds(60);  // advance past next_profile_refresh
  fake.runtime.poll_foreground_for_tests();  // same identity -> schedules a refresh
  ASSERT_TRUE(fake.runtime.run_pending_profile_lookup_for_tests());
  frame = fake.runtime.update_for_frame(std::nullopt);
  EXPECT_TRUE(frame.enabled);
  EXPECT_EQ(frame.contrast, 150);
  EXPECT_EQ(frame.source, profile_source_e::application);
}

#endif
