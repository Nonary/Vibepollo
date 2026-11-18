#include "../tests_common.h"
#include "src/posture_size.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace {
  using namespace posture;

  hints_t hints_for(client_class_e client_class) {
    hints_t hints;
    hints.posture = client_class;
    return hints;
  }

  double diagonal_of(const physical_size_t &size) {
    return std::hypot(static_cast<double>(size.width_mm), static_cast<double>(size.height_mm));
  }

  // The size exactly as Windows sees it: whole centimetres from EDID bytes 0x15/0x16, in millimetres.
  double edid_diagonal_mm(const physical_size_t &size) {
    return 10.0 * std::hypot(static_cast<double>(edid_size_cm(size.width_mm)), static_cast<double>(edid_size_cm(size.height_mm)));
  }

  // 356 mm diagonal on a 16:9 pixel grid.
  const physical_size_t default_16x9 {310, 175};
}  // namespace

TEST(PostureParse, PostureAcceptsOnlyKnownLowercaseClasses) {
  EXPECT_EQ(parse_posture("desktop"), client_class_e::desktop);
  EXPECT_EQ(parse_posture("laptop"), client_class_e::laptop);
  EXPECT_EQ(parse_posture("tablet"), client_class_e::tablet);
  EXPECT_EQ(parse_posture("handheld"), client_class_e::handheld);
  EXPECT_EQ(parse_posture("phone"), client_class_e::phone);
  EXPECT_FALSE(parse_posture("Tablet"));
  EXPECT_FALSE(parse_posture("tv"));
  EXPECT_FALSE(parse_posture(""));
}

TEST(PostureParse, TouchAcceptsOnlyZeroAndOne) {
  EXPECT_EQ(parse_touch("1"), true);
  EXPECT_EQ(parse_touch("0"), false);
  EXPECT_FALSE(parse_touch("2"));
  EXPECT_FALSE(parse_touch("true"));
  EXPECT_FALSE(parse_touch(""));
}

TEST(PostureParse, TouchDiagonalOptionRangeIsInclusiveAndStrict) {
  EXPECT_FALSE(parse_touch_diagonal_mm("99"));
  EXPECT_EQ(parse_touch_diagonal_mm("100"), 100);
  EXPECT_EQ(parse_touch_diagonal_mm("356"), 356);
  EXPECT_EQ(parse_touch_diagonal_mm("419"), 419);
  EXPECT_FALSE(parse_touch_diagonal_mm("420"));
  EXPECT_FALSE(parse_touch_diagonal_mm("-356"));
  EXPECT_FALSE(parse_touch_diagonal_mm("+356"));
  EXPECT_FALSE(parse_touch_diagonal_mm("356.5"));
  EXPECT_FALSE(parse_touch_diagonal_mm(" 356"));
  EXPECT_FALSE(parse_touch_diagonal_mm("356mm"));
  EXPECT_FALSE(parse_touch_diagonal_mm("0x164"));
  EXPECT_FALSE(parse_touch_diagonal_mm("99999999999999999999"));
  EXPECT_FALSE(parse_touch_diagonal_mm(""));
}

TEST(PostureParse, SanitizeFallsBackToTheDefaultOutsideTheRange) {
  EXPECT_EQ(sanitize_touch_diagonal_mm(100), 100);
  EXPECT_EQ(sanitize_touch_diagonal_mm(419), 419);
  EXPECT_EQ(sanitize_touch_diagonal_mm(99), default_touch_diagonal_mm);
  EXPECT_EQ(sanitize_touch_diagonal_mm(420), default_touch_diagonal_mm);
  EXPECT_EQ(sanitize_touch_diagonal_mm(0), default_touch_diagonal_mm);
  EXPECT_EQ(sanitize_touch_diagonal_mm(-5), default_touch_diagonal_mm);
  EXPECT_EQ(sanitize_touch_diagonal_mm(INT_MAX), default_touch_diagonal_mm);
  EXPECT_EQ(sanitize_touch_diagonal_mm(INT_MIN), default_touch_diagonal_mm);
}

TEST(PostureParse, HintsKeepValidFieldsAndDropInvalidOnes) {
  const auto hints = parse_hints("phone", "2");
  EXPECT_EQ(hints.posture, client_class_e::phone);
  EXPECT_FALSE(hints.touch);
  EXPECT_TRUE(hints.has_client_hints());
  EXPECT_EQ(parse_hints("phone", "1").touch, true);
}

TEST(PostureParse, UnknownPostureDiscardsEveryHint) {
  const auto hints = parse_hints("hologram", "1", true);
  EXPECT_FALSE(hints.posture);
  EXPECT_FALSE(hints.touch);
  EXPECT_FALSE(hints.has_client_hints());
  // The host-side flag is not a client hint and survives.
  EXPECT_TRUE(hints.paired_touch);
}

TEST(PostureParse, NothingSentMeansNoHints) {
  const auto hints = parse_hints("", "");
  EXPECT_FALSE(hints.has_client_hints());
  EXPECT_FALSE(hints.paired_touch);
  EXPECT_EQ(hints, hints_t {});
}

TEST(PostureClass, TouchClassesApplyAndNonTouchClassesDoNot) {
  EXPECT_EQ(effective_class(hints_for(client_class_e::phone)), client_class_e::phone);
  EXPECT_EQ(effective_class(hints_for(client_class_e::handheld)), client_class_e::handheld);
  EXPECT_EQ(effective_class(hints_for(client_class_e::tablet)), client_class_e::tablet);
  EXPECT_FALSE(effective_class(hints_for(client_class_e::laptop)));
  EXPECT_FALSE(effective_class(hints_for(client_class_e::desktop)));
}

TEST(PostureClass, TouchWithoutPostureIsTablet) {
  EXPECT_EQ(effective_class(parse_hints("", "1")), client_class_e::tablet);
  EXPECT_FALSE(effective_class(parse_hints("", "0")));
  EXPECT_FALSE(effective_class(hints_t {}));
}

TEST(PostureClass, ExplicitPostureBeatsTouchAndPairedFlag) {
  EXPECT_FALSE(effective_class(parse_hints("laptop", "1", true)));
  EXPECT_FALSE(effective_class(parse_hints("desktop", "", true)));
  EXPECT_EQ(effective_class(parse_hints("phone", "", true)), client_class_e::phone);
}

TEST(PostureClass, PairedTouchFlagAppliesTabletWhenNoPostureIsSent) {
  EXPECT_EQ(effective_class(parse_hints("", "", true)), client_class_e::tablet);
  // clientTouch=0 is informational and does not cancel the host flag.
  EXPECT_EQ(effective_class(parse_hints("", "0", true)), client_class_e::tablet);
  // An unusable posture is ignored, so the host flag still applies.
  EXPECT_EQ(effective_class(parse_hints("hologram", "", true)), client_class_e::tablet);
}

TEST(PostureSize, ContractConstants) {
  EXPECT_EQ(default_touch_diagonal_mm, 356);
  EXPECT_EQ(min_touch_diagonal_mm, 100);
  EXPECT_EQ(max_touch_diagonal_mm, 419);
  EXPECT_EQ(min_side_mm, 10);
  EXPECT_NEAR(docking_limit_diagonal_mm, 431.8, 1e-9);
  EXPECT_NEAR(default_touch_diagonal_mm / 25.4, 14.0, 0.02);
  EXPECT_NEAR(max_touch_diagonal_mm / 25.4, 16.5, 0.01);
}

TEST(PostureSize, NoHintMeansNoSize) {
  EXPECT_FALSE(derive_physical_size(hints_t {}, 1920, 1080));
  EXPECT_FALSE(derive_physical_size(parse_hints("", "0"), 1920, 1080));
  EXPECT_FALSE(derive_physical_size(hints_for(client_class_e::laptop), 1920, 1080));
  EXPECT_FALSE(derive_physical_size(hints_for(client_class_e::desktop), 1920, 1080));
}

TEST(PostureSize, EveryTouchClassGetsTheSameSize) {
  // Phone, handheld, tablet, clientTouch=1 and the paired flag are all "touch-primary": one 14 in size.
  const auto expected = derive_physical_size(hints_for(client_class_e::tablet), 1920, 1080);
  ASSERT_TRUE(expected);
  EXPECT_EQ(*expected, default_16x9);
  EXPECT_NEAR(diagonal_of(*expected), 356.0, 1.0);
  EXPECT_EQ(derive_physical_size(hints_for(client_class_e::phone), 1920, 1080), expected);
  EXPECT_EQ(derive_physical_size(hints_for(client_class_e::handheld), 1920, 1080), expected);
  EXPECT_EQ(derive_physical_size(parse_hints("", "1"), 1920, 1080), expected);
  EXPECT_EQ(derive_physical_size(parse_hints("", "", true), 1920, 1080), expected);
}

TEST(PostureSize, PhoneAndTvClientGetTheSameSize) {
  // The same touch-primary posture from a 6 in phone and a 65 in TV client, each streaming at its own
  // resolution: the size depends on neither, only on the shared 16:9 stream aspect.
  const auto phone = derive_physical_size(parse_hints("phone", "1"), 2400, 1080);
  const auto tv = derive_physical_size(parse_hints("phone", "1"), 3840, 2160);
  const auto tablet = derive_physical_size(parse_hints("tablet", "1"), 1920, 1080);
  ASSERT_TRUE(phone);
  ASSERT_TRUE(tv);
  EXPECT_EQ(tv, tablet);
  EXPECT_NEAR(diagonal_of(*phone), diagonal_of(*tv), 1.0);
  EXPECT_NEAR(diagonal_of(*tv), 356.0, 1.0);
}

TEST(PostureSize, ConfiguredDiagonalIsUsedAndSplitByTheStreamAspect) {
  const auto size = derive_physical_size(hints_for(client_class_e::tablet), 1920, 1080, 300);
  ASSERT_TRUE(size);
  EXPECT_EQ(*size, (physical_size_t {261, 147}));
  EXPECT_NEAR(diagonal_of(*size), 300.0, 1.0);
  // The boundaries of the range are valid as is.
  EXPECT_NEAR(diagonal_of(*derive_physical_size(hints_for(client_class_e::tablet), 1920, 1080, 100)), 100.0, 1.0);
  EXPECT_NEAR(diagonal_of(*derive_physical_size(hints_for(client_class_e::tablet), 1920, 1080, 419)), 419.0, 1.0);
}

TEST(PostureSize, InvalidConfiguredDiagonalFallsBackToTheDefault) {
  const auto expected = derive_physical_size(hints_for(client_class_e::tablet), 1920, 1080);
  for (const int invalid : {INT_MIN, -1, 0, 99, 420, 1000, INT_MAX}) {
    EXPECT_EQ(derive_physical_size(hints_for(client_class_e::tablet), 1920, 1080, invalid), expected) << invalid;
  }
}

TEST(PostureSize, AspectRatioFollowsTheStreamNotTheClientScreen) {
  const auto portrait = derive_physical_size(hints_for(client_class_e::phone), 1080, 2400);
  const auto ultrawide = derive_physical_size(hints_for(client_class_e::tablet), 3440, 1440);
  const auto square = derive_physical_size(hints_for(client_class_e::tablet), 1000, 1000);
  ASSERT_TRUE(portrait);
  ASSERT_TRUE(ultrawide);
  ASSERT_TRUE(square);
  EXPECT_LT(portrait->width_mm, portrait->height_mm);
  EXPECT_GT(ultrawide->width_mm, 2 * ultrawide->height_mm);
  EXPECT_EQ(square->width_mm, square->height_mm);
  // Portrait is the landscape size turned, never rescaled.
  const auto landscape = derive_physical_size(hints_for(client_class_e::phone), 2400, 1080);
  ASSERT_TRUE(landscape);
  EXPECT_EQ(portrait->width_mm, landscape->height_mm);
  EXPECT_EQ(portrait->height_mm, landscape->width_mm);
}

TEST(PostureSize, SameAspectAtAnyResolutionGivesTheSameSize) {
  const auto reference = derive_physical_size(hints_for(client_class_e::phone), 1280, 720);
  for (const auto &[width, height] : {std::pair {1920, 1080}, std::pair {2560, 1440}, std::pair {3840, 2160}, std::pair {7680, 4320}, std::pair {16, 9}}) {
    EXPECT_EQ(derive_physical_size(hints_for(client_class_e::phone), width, height), reference) << width << 'x' << height;
  }
}

TEST(PostureSize, UnknownOrInvalidStreamSizeAssumes16x9) {
  const auto expected = derive_physical_size(hints_for(client_class_e::tablet), 1920, 1080);
  ASSERT_TRUE(expected);
  for (const auto &[width, height] : {std::pair {0, 0}, std::pair {-1, 1080}, std::pair {1920, -1}, std::pair {0, 1080}, std::pair {1920, 0}, std::pair {INT_MIN, INT_MIN}, std::pair {-5, -5}}) {
    EXPECT_EQ(derive_physical_size(hints_for(client_class_e::tablet), width, height), expected) << width << 'x' << height;
  }
}

TEST(PostureSize, ExtremeAspectKeepsEverySideAtLeastOneCentimetre) {
  for (const auto &[width, height] : {std::pair {7680, 200}, std::pair {200, 7680}, std::pair {INT_MAX, 1}, std::pair {1, INT_MAX}, std::pair {INT_MAX, INT_MAX}, std::pair {1, 1}}) {
    const auto size = derive_physical_size(hints_for(client_class_e::tablet), width, height);
    ASSERT_TRUE(size) << width << 'x' << height;
    EXPECT_GE(size->width_mm, static_cast<std::uint32_t>(min_side_mm)) << width << 'x' << height;
    EXPECT_GE(size->height_mm, static_cast<std::uint32_t>(min_side_mm)) << width << 'x' << height;
    EXPECT_GE(edid_size_cm(size->width_mm), 1u);
    EXPECT_GE(edid_size_cm(size->height_mm), 1u);
  }
}

TEST(PostureSize, EdidCentimetresMirrorTheDriver) {
  // libvirtualdisplay: (mm + 5) / 10, clamped to [1, 255].
  EXPECT_EQ(edid_size_cm(0), 1u);
  EXPECT_EQ(edid_size_cm(4), 1u);
  EXPECT_EQ(edid_size_cm(10), 1u);
  EXPECT_EQ(edid_size_cm(14), 1u);
  EXPECT_EQ(edid_size_cm(15), 2u);
  EXPECT_EQ(edid_size_cm(356), 36u);
  EXPECT_EQ(edid_size_cm(419), 42u);
  EXPECT_EQ(edid_size_cm(2550), 255u);
  EXPECT_EQ(edid_size_cm(100000), 255u);
}

TEST(PostureSize, SizeDiagonalRoundsToWholeMillimetres) {
  EXPECT_EQ(size_diagonal_mm(physical_size_t {310, 175}), 356u);
  EXPECT_EQ(size_diagonal_mm(physical_size_t {3, 4}), 5u);
}

namespace {
  // Resolutions the property tests sweep: everything from a 64x64 stub to 15360x8640, portrait,
  // square, ultrawide and super ultrawide, odd sizes, and non-positive values.
  std::vector<std::pair<int, int>> resolution_grid() {
    std::vector<std::pair<int, int>> grid;
    for (const int width : {320, 480, 640, 720, 800, 1080, 1280, 1366, 1440, 1600, 1920, 2160, 2340, 2400, 2560, 3200, 3440, 3840, 5120, 7680, 15360}) {
      for (const int height : {240, 360, 480, 720, 900, 1080, 1200, 1280, 1440, 1600, 2160, 2340, 2400, 3200, 4320, 8640}) {
        grid.emplace_back(width, height);
      }
    }
    grid.insert(
      grid.end(),
      {
        {1080, 2340},  // phone portrait
        {720, 1280},
        {9, 32},  // 9:32 portrait ratio
        {900, 3200},
        {32, 9},
        {5120, 1440},  // 32:9
        {3440, 1440},  // 21:9
        {1000, 1000},
        {64, 64},
        {1, 1},
        {2, 7000},
        {7000, 2},
        {1919, 1079},  // odd sizes
        {1081, 2401},
        {4097, 33},
        {0, 0},
        {0, 1080},
        {1920, 0},
        {-1, -1},
        {-1920, 1080},
        {1920, -1080},
        {INT_MIN, INT_MIN},
        {INT_MAX, 1},
        {1, INT_MAX},
        {INT_MAX, INT_MAX},
      }
    );
    return grid;
  }
}  // namespace

TEST(PostureSizeProperty, EdidSizeStaysDockedForEveryResolutionAndPosture) {
  const auto grid = resolution_grid();
  std::size_t checked = 0;
  for (const auto client_class : {client_class_e::phone, client_class_e::handheld, client_class_e::tablet}) {
    for (const auto &[width, height] : grid) {
      const auto size = derive_physical_size(hints_for(client_class), width, height);
      ASSERT_TRUE(size) << width << 'x' << height;
      const double encoded = edid_diagonal_mm(*size);
      EXPECT_LT(encoded, docking_limit_diagonal_mm) << width << 'x' << height << " encodes " << encoded << " mm";
      EXPECT_GE(edid_size_cm(size->width_mm), 1u) << width << 'x' << height;
      EXPECT_GE(edid_size_cm(size->height_mm), 1u) << width << 'x' << height;
      EXPECT_LE(edid_size_cm(size->width_mm), 255u) << width << 'x' << height;
      EXPECT_LE(edid_size_cm(size->height_mm), 255u) << width << 'x' << height;
      // Millimetres as sent to the driver stay inside what it accepts (10..2550).
      EXPECT_GE(size->width_mm, 10u);
      EXPECT_GE(size->height_mm, 10u);
      EXPECT_LE(size->width_mm, 2550u);
      EXPECT_LE(size->height_mm, 2550u);
      ++checked;
    }
  }
  EXPECT_GT(checked, 1000u);
}

TEST(PostureSizeProperty, UnroundedDiagonalEqualsTheConfiguredDiagonal) {
  for (const auto &[width, height] : resolution_grid()) {
    const auto size = derive_physical_size(hints_for(client_class_e::phone), width, height);
    ASSERT_TRUE(size);
    // Whole-millimetre rounding moves each side by at most 0.5 mm. Only an extreme aspect, where a
    // side is raised to the 10 mm minimum, adds more (at most about 0.2 mm for a 356 mm diagonal).
    EXPECT_NEAR(diagonal_of(*size), 356.0, 1.0) << width << 'x' << height;
  }
}

TEST(PostureSizeProperty, EveryConfigurableDiagonalStaysDockedWithWorstCaseRounding) {
  // The configurable maximum must hold for every diagonal of the range against every stream aspect:
  // 1 pixel steps of the aspect from very tall through square to very wide.
  const auto hints = hints_for(client_class_e::tablet);
  double worst_encoded = 0.0;
  for (int diagonal = min_touch_diagonal_mm; diagonal <= max_touch_diagonal_mm; ++diagonal) {
    for (int step = 0; step <= 2000; ++step) {
      // Angle sweep covers the aspect range [1:64, 64:1] on a log scale.
      const double ratio = std::pow(64.0, (step - 1000) / 1000.0);
      const int width = static_cast<int>(std::lround(1000.0 * ratio));
      const auto size = derive_physical_size(hints, width, 1000, diagonal);
      ASSERT_TRUE(size) << diagonal << ' ' << width;
      const double encoded = edid_diagonal_mm(*size);
      worst_encoded = std::max(worst_encoded, encoded);
      ASSERT_LT(encoded, docking_limit_diagonal_mm) << diagonal << " mm at " << width << ":1000 encodes " << encoded << " mm";
      ASSERT_NEAR(diagonal_of(*size), diagonal, 1.0) << diagonal << " mm at " << width << ":1000";
    }
  }
  // The largest encoded diagonal, with the margin to the 17.0 in limit, is 42.x cm.
  EXPECT_LT(worst_encoded, docking_limit_diagonal_mm - 5.0);
}

TEST(PostureSizeProperty, TheDefaultDiagonalEncodesBelowTheLimitForEveryResolution) {
  for (const auto &[width, height] : resolution_grid()) {
    const auto size = derive_physical_size(hints_for(client_class_e::tablet), width, height, default_touch_diagonal_mm);
    ASSERT_TRUE(size);
    EXPECT_LT(edid_diagonal_mm(*size), 17.0 * 25.4) << width << 'x' << height;
  }
}

TEST(PostureTouchCapability, TouchClassesAndFlagsAreCapable) {
  EXPECT_TRUE(touch_capable(hints_for(client_class_e::phone)));
  EXPECT_TRUE(touch_capable(hints_for(client_class_e::tablet)));
  EXPECT_TRUE(touch_capable(hints_for(client_class_e::handheld)));
  EXPECT_TRUE(touch_capable(parse_hints("", "1")));
  EXPECT_TRUE(touch_capable(parse_hints("", "", true)));
}

TEST(PostureTouchCapability, KeyboardStateDoesNotRemoveCapability) {
  // A 2-in-1 with its keyboard attached: laptop state, still a touch device.
  EXPECT_TRUE(touch_capable(parse_hints("laptop", "1")));
  EXPECT_TRUE(touch_capable(parse_hints("desktop", "1")));
  EXPECT_TRUE(touch_capable(parse_hints("laptop", "", true)));
  EXPECT_FALSE(effective_class(parse_hints("laptop", "1")));
}

TEST(PostureTouchCapability, NoTouchMeansNotCapable) {
  EXPECT_FALSE(touch_capable(hints_for(client_class_e::laptop)));
  EXPECT_FALSE(touch_capable(hints_for(client_class_e::desktop)));
  EXPECT_FALSE(touch_capable(parse_hints("laptop", "0")));
  EXPECT_FALSE(touch_capable(hints_t {}));
}

TEST(PosturePhysicalSize, SameSizeWithKeyboardAttachedAndDetached) {
  const auto attached = derive_physical_size(parse_hints("laptop", "1"), 1920, 1280);
  const auto detached = derive_physical_size(parse_hints("tablet", "1"), 1920, 1280);
  ASSERT_TRUE(attached);
  ASSERT_TRUE(detached);
  EXPECT_EQ(*attached, *detached);
  EXPECT_FALSE(derive_physical_size(parse_hints("laptop", "0"), 1920, 1280));
}

TEST(PostureClientOverride, ParseAndToStringRoundTrip) {
  for (const auto value : {client_override_e::automatic, client_override_e::desktop, client_override_e::tablet, client_override_e::handheld}) {
    EXPECT_EQ(parse_client_override(to_string(value)), value);
  }
  EXPECT_EQ(to_string(client_override_e::automatic), "auto");
  EXPECT_EQ(parse_client_override("auto"), client_override_e::automatic);
  EXPECT_FALSE(parse_client_override("automatic"));
  EXPECT_FALSE(parse_client_override("Tablet"));
  EXPECT_FALSE(parse_client_override(""));
}

TEST(PostureClientOverride, AutomaticLeavesHintsUnchanged) {
  const auto hints = parse_hints("laptop", "1", true);
  EXPECT_EQ(apply_client_override(hints, client_override_e::automatic), hints);
  EXPECT_EQ(apply_client_override(hints_t {}, client_override_e::automatic), hints_t {});
}

TEST(PostureClientOverride, TabletAndHandheldForcePostureAndTouch) {
  const auto tablet = apply_client_override(parse_hints("laptop", "0"), client_override_e::tablet);
  EXPECT_EQ(tablet.posture, client_class_e::tablet);
  EXPECT_EQ(tablet.touch, std::optional<bool> {true});
  const auto handheld = apply_client_override(hints_t {}, client_override_e::handheld);
  EXPECT_EQ(handheld.posture, client_class_e::handheld);
  EXPECT_EQ(handheld.touch, std::optional<bool> {true});
}

TEST(PostureClientOverride, DesktopKeepsTouchCapabilityAndDropsTabletState) {
  const auto touch_device = apply_client_override(parse_hints("tablet", "1"), client_override_e::desktop);
  EXPECT_EQ(touch_device.posture, client_class_e::laptop);
  EXPECT_EQ(touch_device.touch, std::optional<bool> {true});
  EXPECT_FALSE(effective_class(touch_device));
  EXPECT_TRUE(touch_capable(touch_device));

  const auto paired = apply_client_override(parse_hints("", "", true), client_override_e::desktop);
  EXPECT_EQ(paired.posture, client_class_e::laptop);
  EXPECT_TRUE(paired.paired_touch);
  EXPECT_TRUE(touch_capable(paired));

  const auto non_touch = apply_client_override(parse_hints("laptop", "0"), client_override_e::desktop);
  EXPECT_EQ(non_touch.posture, client_class_e::desktop);
  EXPECT_FALSE(touch_capable(non_touch));
  EXPECT_EQ(apply_client_override(hints_t {}, client_override_e::desktop).posture, client_class_e::desktop);
}

TEST(PostureHostMode, ParseAndToStringRoundTrip) {
  for (const auto policy : {handheld_policy_e::tablet, handheld_policy_e::xbox_fullscreen, handheld_policy_e::desktop}) {
    EXPECT_EQ(parse_handheld_policy(to_string(policy)), policy);
  }
  EXPECT_EQ(to_string(handheld_policy_e::xbox_fullscreen), "xbox_fullscreen");
  EXPECT_FALSE(parse_handheld_policy("xbox"));
  EXPECT_FALSE(parse_handheld_policy(""));
  EXPECT_EQ(to_string(host_mode_e::desktop), "desktop");
  EXPECT_EQ(to_string(host_mode_e::tablet), "tablet");
  EXPECT_EQ(to_string(host_mode_e::handheld), "handheld");
}

TEST(PostureHostMode, ClassAndPolicyDecideTheMode) {
  for (const auto policy : {handheld_policy_e::tablet, handheld_policy_e::xbox_fullscreen, handheld_policy_e::desktop}) {
    EXPECT_EQ(host_mode(hints_t {}, policy), host_mode_e::desktop);
    EXPECT_EQ(host_mode(hints_for(client_class_e::desktop), policy), host_mode_e::desktop);
    EXPECT_EQ(host_mode(parse_hints("laptop", "1"), policy), host_mode_e::desktop);
    EXPECT_EQ(host_mode(hints_for(client_class_e::tablet), policy), host_mode_e::tablet);
    EXPECT_EQ(host_mode(hints_for(client_class_e::phone), policy), host_mode_e::tablet);
    EXPECT_EQ(host_mode(parse_hints("", "1"), policy), host_mode_e::tablet);
    EXPECT_EQ(host_mode(parse_hints("", "", true), policy), host_mode_e::tablet);
  }
  EXPECT_EQ(host_mode(hints_for(client_class_e::handheld), handheld_policy_e::tablet), host_mode_e::tablet);
  EXPECT_EQ(host_mode(hints_for(client_class_e::handheld), handheld_policy_e::xbox_fullscreen), host_mode_e::handheld);
  EXPECT_EQ(host_mode(hints_for(client_class_e::handheld), handheld_policy_e::desktop), host_mode_e::desktop);
}
