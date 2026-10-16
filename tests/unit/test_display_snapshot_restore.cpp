/**
 * @file tests/unit/test_display_snapshot_restore.cpp
 * @brief Portable tests for strict display-mode matching and ordered restore.
 */

#include <display_device/mode_verification.h>

#include "src/platform/windows/display_snapshot_restore.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {
  using display_device::Rational;

  struct Mode {
    display_device::Resolution m_resolution;
    Rational m_refresh_rate;
  };

  using ModeMap = std::map<std::string, Mode>;
  using HdrMap = std::map<std::string, bool>;

  Mode make_mode(unsigned int width, unsigned int height, unsigned int numerator, unsigned int denominator) {
    return {
      .m_resolution = {.m_width = width, .m_height = height},
      .m_refresh_rate = {.m_numerator = numerator, .m_denominator = denominator},
    };
  }

  struct Origin {
    int x = 0;
    int y = 0;

    friend bool operator==(const Origin &, const Origin &) = default;
  };

  struct Snapshot {
    ModeMap m_modes;
    HdrMap m_hdr_states;
    std::string m_primary_device;
    std::map<std::string, Origin> m_origins;
  };

  class FakeDevice {
  public:
    bool setDisplayModesExactTemporary(const ModeMap &requested) {
      calls.emplace_back("modes");
      requested_modes = requested;
      return display_device::equalDisplayModes(requested, readback_modes);
    }

    bool setHdrStates(const HdrMap &states) {
      calls.emplace_back("hdr");
      requested_hdr_states = states;
      return hdr_succeeds;
    }

    bool setAsPrimary(const std::string &device_id) {
      calls.emplace_back("primary");
      requested_primary = device_id;
      return primary_succeeds;
    }

    bool setDisplayOrigin(const std::string &device_id, const Origin &origin) {
      calls.emplace_back("origin:" + device_id);
      requested_origins[device_id] = origin;
      return failing_origin.empty() || failing_origin != device_id;
    }

    std::vector<std::string> calls;
    ModeMap readback_modes;
    ModeMap requested_modes;
    HdrMap requested_hdr_states;
    std::string requested_primary;
    std::map<std::string, Origin> requested_origins;
    bool hdr_succeeds = true;
    bool primary_succeeds = true;
    std::string failing_origin;
  };

  Snapshot complete_snapshot() {
    Snapshot snapshot;
    snapshot.m_modes.emplace("DISPLAY-A", make_mode(2560, 1440, 120, 1));
    snapshot.m_hdr_states.emplace("DISPLAY-A", true);
    snapshot.m_primary_device = "DISPLAY-A";
    snapshot.m_origins.emplace("DISPLAY-A", Origin {100, 200});
    snapshot.m_origins.emplace("DISPLAY-B", Origin {-1920, 0});
    return snapshot;
  }

  void set_matching_readback(FakeDevice &device, const Snapshot &snapshot) {
    device.readback_modes = snapshot.m_modes;
  }
}  // namespace

TEST(DisplayModeVerification, RefreshRatesRequireExactFrequencyEquality) {
  EXPECT_TRUE(display_device::equalRefreshRates(Rational {120, 2}, Rational {60, 1}));
  EXPECT_FALSE(display_device::equalRefreshRates(Rational {120, 1}, Rational {60, 1}));
  EXPECT_FALSE(display_device::equalRefreshRates(Rational {60000, 1001}, Rational {60, 1}));
  EXPECT_FALSE(display_device::equalRefreshRates(Rational {60, 0}, Rational {60, 1}));
  EXPECT_FALSE(display_device::equalRefreshRates(Rational {60, 1}, Rational {60, 0}));
  EXPECT_FALSE(display_device::equalRefreshRates(Rational {0, 1}, Rational {0, 2}));
  EXPECT_FALSE(display_device::equalRefreshRates(Rational {0, 1}, Rational {60, 1}));
}

TEST(DisplayModeVerification, CrossProductsUseEnoughBitsForMaximumInputs) {
  constexpr auto max = std::numeric_limits<std::uint32_t>::max();

  // The first cross-products are close to UINT64_MAX. A 32-bit multiplication
  // wraps max * max and 1 * 1 to the same value, falsely accepting the rates.
  EXPECT_FALSE(display_device::equalRefreshRates(Rational {max, 1}, Rational {1, max}));
  EXPECT_FALSE(display_device::equalRefreshRates(Rational {max, max - 1}, Rational {max - 1, max - 2}));
  EXPECT_TRUE(display_device::equalRefreshRates(Rational {max, max}, Rational {2, 2}));
}

TEST(DisplayModeVerification, DisplayModesRequireMatchingDeviceResolutionAndRefresh) {
  const ModeMap expected { {"DISPLAY-A", make_mode(2560, 1440, 120, 2)} };
  EXPECT_TRUE(display_device::equalDisplayModes(
    expected,
    ModeMap {{"DISPLAY-A", make_mode(2560, 1440, 60, 1)}}));
  EXPECT_FALSE(display_device::equalDisplayModes(
    expected,
    ModeMap {{"DISPLAY-B", make_mode(2560, 1440, 60, 1)}}));
  EXPECT_FALSE(display_device::equalDisplayModes(
    expected,
    ModeMap {{"DISPLAY-A", make_mode(1920, 1080, 60, 1)}}));
  EXPECT_FALSE(display_device::equalDisplayModes(
    expected,
    ModeMap {{"DISPLAY-A", make_mode(2560, 1440, 120, 1)}}));
}

TEST(DisplaySnapshotRestore, RejectedModeReadbackStopsLaterSettingsWrites) {
  const auto snapshot = complete_snapshot();
  FakeDevice device;
  device.readback_modes = {{"DISPLAY-A", make_mode(2560, 1440, 60, 1)}};

  EXPECT_FALSE(display_helper::restore_snapshot_settings(device, snapshot));
  EXPECT_EQ(device.calls, (std::vector<std::string> {"modes"}));
}

TEST(DisplaySnapshotRestore, HdrFailureStopsPrimaryAndOriginWrites) {
  const auto snapshot = complete_snapshot();
  FakeDevice device;
  set_matching_readback(device, snapshot);
  device.hdr_succeeds = false;

  EXPECT_FALSE(display_helper::restore_snapshot_settings(device, snapshot));
  EXPECT_EQ(device.calls, (std::vector<std::string> {"modes", "hdr"}));
}

TEST(DisplaySnapshotRestore, PrimaryFailureStopsOriginWrites) {
  const auto snapshot = complete_snapshot();
  FakeDevice device;
  set_matching_readback(device, snapshot);
  device.primary_succeeds = false;

  EXPECT_FALSE(display_helper::restore_snapshot_settings(device, snapshot));
  EXPECT_EQ(device.calls, (std::vector<std::string> {"modes", "hdr", "primary"}));
}

TEST(DisplaySnapshotRestore, OriginFailureStopsRemainingOriginWrites) {
  const auto snapshot = complete_snapshot();
  FakeDevice device;
  set_matching_readback(device, snapshot);
  device.failing_origin = "DISPLAY-A";

  EXPECT_FALSE(display_helper::restore_snapshot_settings(device, snapshot));
  EXPECT_EQ(device.calls, (std::vector<std::string> {"modes", "hdr", "primary", "origin:DISPLAY-A"}));
}

TEST(DisplaySnapshotRestore, AppliesAllSettingsInOrderAndSkipsEmptyOptionalFields) {
  const auto snapshot = complete_snapshot();
  FakeDevice device;
  set_matching_readback(device, snapshot);

  EXPECT_TRUE(display_helper::restore_snapshot_settings(device, snapshot));
  EXPECT_EQ(device.calls,
            (std::vector<std::string> {"modes", "hdr", "primary", "origin:DISPLAY-A", "origin:DISPLAY-B"}));
  EXPECT_TRUE(display_device::equalDisplayModes(device.requested_modes, snapshot.m_modes));
  EXPECT_EQ(device.requested_hdr_states, snapshot.m_hdr_states);
  EXPECT_EQ(device.requested_primary, "DISPLAY-A");
  EXPECT_EQ(device.requested_origins.size(), 2u);

  Snapshot empty_snapshot;
  FakeDevice empty_device;
  EXPECT_TRUE(display_helper::restore_snapshot_settings(empty_device, empty_snapshot));
  EXPECT_TRUE(empty_device.calls.empty());
}
