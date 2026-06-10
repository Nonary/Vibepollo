/**
 * @file tests/unit/test_display_helper_v2_restore_engine.cpp
 * @brief Invariant tests for the display helper v2 restore engine internals
 *        (snapshot codec, filtering rules, golden health) ported from the
 *        battle-tested legacy helper.
 */
#ifdef _WIN32

  #include "../tests_common.h"

  #include "src/platform/windows/display_helper_v2/golden_health.h"
  #include "src/platform/windows/display_helper_v2/operations.h"
  #include "src/platform/windows/display_helper_v2/snapshot.h"
  #include "src/platform/windows/display_helper_v2/snapshot_codec.h"

  #include <chrono>
  #include <filesystem>
  #include <fstream>

  #include <nlohmann/json.hpp>

namespace codec = display_helper::v2::codec;

namespace {
  struct TempDir {
    std::filesystem::path path;

    TempDir() {
      std::error_code ec;
      const auto base = std::filesystem::temp_directory_path(ec);
      const auto token = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
      path = (ec ? std::filesystem::path(".") : base) / ("sunshine_display_helper_v2_restore_test_" + token);
      std::filesystem::create_directories(path, ec);
    }

    ~TempDir() {
      std::error_code ec;
      std::filesystem::remove_all(path, ec);
    }
  };

  display_device::DisplaySettingsSnapshot make_snapshot(const std::vector<std::vector<std::string>> &topology) {
    display_device::DisplaySettingsSnapshot snapshot;
    snapshot.m_topology = topology;
    for (const auto &group : topology) {
      for (const auto &id : group) {
        display_device::DisplayMode mode;
        mode.m_resolution.m_width = 1920;
        mode.m_resolution.m_height = 1080;
        mode.m_refresh_rate.m_numerator = 60000;
        mode.m_refresh_rate.m_denominator = 1000;
        snapshot.m_modes[id] = mode;
        snapshot.m_hdr_states[id] = std::nullopt;
      }
    }
    if (!topology.empty() && !topology.front().empty()) {
      snapshot.m_primary_device = topology.front().front();
    }
    return snapshot;
  }

  display_device::EnumeratedDevice make_device(
    const std::string &device_id,
    const std::string &display_name,
    bool active,
    const std::string &edid_manufacturer = {},
    const std::string &friendly_name = "Generic Monitor") {
    display_device::EnumeratedDevice device;
    device.m_device_id = device_id;
    device.m_display_name = display_name;
    device.m_friendly_name = friendly_name;
    if (!edid_manufacturer.empty()) {
      display_device::EdidData edid {};
      edid.m_manufacturer_id = edid_manufacturer;
      device.m_edid = edid;
    }
    if (active) {
      device.m_info = display_device::EnumeratedDevice::Info {};
    }
    return device;
  }
}  // namespace

// --- canonical topology comparison (fd98755b: MPO-scrambling endless restore loop) ---

TEST(DisplayHelperV2Codec, CanonicalTopologyEquality) {
  auto a = make_snapshot({{"A"}, {"B", "C"}});
  auto b = make_snapshot({{"C", "B"}, {"A"}});
  b.m_primary_device = a.m_primary_device;

  EXPECT_TRUE(codec::equal_snapshots_strict(a, b));

  auto c = make_snapshot({{"A", "B"}, {"C"}});  // different grouping
  c.m_primary_device = a.m_primary_device;
  EXPECT_FALSE(codec::equal_snapshots_strict(a, c));
}

TEST(DisplayHelperV2Codec, SignatureIsOrderIndependent) {
  auto a = make_snapshot({{"A"}, {"B", "C"}});
  auto b = make_snapshot({{"C", "B"}, {"A"}});
  b.m_primary_device = a.m_primary_device;

  EXPECT_EQ(codec::signature(a), codec::signature(b));
}

TEST(DisplayHelperV2Codec, EqualSnapshotsToleratesMissingOrigins) {
  auto a = make_snapshot({{"A"}});
  auto b = a;
  a.m_origins["A"] = display_device::Point {100, 200};
  // b has no origins (older snapshot): still equal
  EXPECT_TRUE(codec::equal_snapshots_strict(a, b));

  b.m_origins["A"] = display_device::Point {0, 0};
  EXPECT_FALSE(codec::equal_snapshots_strict(a, b));
}

// --- legacy on-disk format compatibility ---

TEST(DisplayHelperV2Codec, SerializeParseRoundTrip) {
  auto snap = make_snapshot({{"A"}, {"B"}});
  snap.m_hdr_states["A"] = display_device::HdrState::Enabled;
  snap.m_hdr_states["B"] = display_device::HdrState::Disabled;
  snap.m_origins["A"] = display_device::Point {0, 0};
  snap.m_origins["B"] = display_device::Point {-1920, 0};

  codec::layout_rotation_map_t layouts {{"A", 0}, {"B", 90}};
  const auto text = codec::serialize_snapshot(snap, layouts);
  const auto loaded = codec::parse_snapshot_text(text);

  EXPECT_EQ(loaded.snapshot_version, codec::kSnapshotLayoutVersionLatest);
  EXPECT_TRUE(loaded.has_layout_data);
  EXPECT_EQ(loaded.layout_rotations, layouts);
  EXPECT_TRUE(codec::equal_snapshots_strict(loaded.snapshot, snap));
  EXPECT_EQ(loaded.snapshot.m_origins, snap.m_origins);
}

TEST(DisplayHelperV2Codec, LegacyV1SchemaParsesWithoutLayouts) {
  // Hand-written fixture in the original (pre-layouts, pre-origins) schema.
  const std::string fixture = R"({
  "topology": [["A"],["B"]],
  "modes": {
    "A": { "w": 2560, "h": 1440, "num": 240000, "den": 1000 },
    "B": { "w": 1920, "h": 1080, "num": 60000, "den": 1000 }
  },
  "hdr": {
    "A": "on",
    "B": null
  },
  "primary": "A"
})";

  const auto loaded = codec::parse_snapshot_text(fixture);
  EXPECT_EQ(loaded.snapshot_version, 1);
  EXPECT_FALSE(loaded.has_layout_data);
  EXPECT_TRUE(loaded.layout_rotations.empty());
  ASSERT_EQ(loaded.snapshot.m_topology.size(), 2u);
  EXPECT_EQ(loaded.snapshot.m_primary_device, "A");
  ASSERT_EQ(loaded.snapshot.m_modes.count("A"), 1u);
  EXPECT_EQ(loaded.snapshot.m_modes.at("A").m_resolution.m_width, 2560u);
  EXPECT_EQ(loaded.snapshot.m_modes.at("A").m_refresh_rate.m_numerator, 240000u);
  ASSERT_EQ(loaded.snapshot.m_hdr_states.count("A"), 1u);
  EXPECT_EQ(loaded.snapshot.m_hdr_states.at("A"), display_device::HdrState::Enabled);
  EXPECT_EQ(loaded.snapshot.m_hdr_states.at("B"), std::nullopt);
  EXPECT_TRUE(codec::snapshot_text_has_restore_payload(fixture));
}

// --- virtual display classification (f3841ad8: baseline poisoning) ---

TEST(DisplayHelperV2Codec, VirtualDisplayClassifier) {
  EXPECT_TRUE(codec::is_virtual_display_device(make_device("ID1", "\\\\.\\DISPLAY9", true, "", "SunshineVirtualDisplay 4K")));
  EXPECT_TRUE(codec::is_virtual_display_device(make_device("ID2", "\\\\.\\DISPLAY9", true, "", "Sunshine Virtual Display Driver")));
  EXPECT_TRUE(codec::is_virtual_display_device(make_device("ID3", "\\\\.\\DISPLAY9", true, "SMK")));  // SudoVDA EDID
  EXPECT_TRUE(codec::is_virtual_display_device(make_device("ID4", "\\\\.\\DISPLAY9", true, "SDD")));  // bundled driver EDID
  EXPECT_FALSE(codec::is_virtual_display_device(make_device("ID5", "\\\\.\\DISPLAY1", true, "DEL", "DELL U2723QE")));
}

// --- load-side filtering (f3841ad8 / 8f2d0632) ---

TEST(DisplayHelperV2Codec, FilterLoadRejectsSnapshotReferencingActiveVirtual) {
  codec::ParsedSnapshot loaded;
  loaded.snapshot = make_snapshot({{"PHYS", "VDD"}});

  display_device::EnumeratedDeviceList devices {
    make_device("PHYS", "\\\\.\\DISPLAY1", true),
    make_device("VDD", "\\\\.\\DISPLAY2", true, "SMK"),
  };

  auto result = codec::filter_loaded_snapshot(std::move(loaded), devices, {}, "test");
  EXPECT_FALSE(result.has_value());
}

TEST(DisplayHelperV2Codec, FilterLoadRejectsAllExcluded) {
  codec::ParsedSnapshot loaded;
  loaded.snapshot = make_snapshot({{"A"}});

  display_device::EnumeratedDeviceList devices {
    make_device("A", "\\\\.\\DISPLAY1", true),
  };

  // Exclusions are matched case-insensitively via normalize_device_id.
  auto result = codec::filter_loaded_snapshot(std::move(loaded), devices, {"a"}, "test");
  EXPECT_FALSE(result.has_value());
}

TEST(DisplayHelperV2Codec, FilterLoadRejectsWhenNoValidDevices) {
  codec::ParsedSnapshot loaded;
  loaded.snapshot = make_snapshot({{"A"}});

  auto result = codec::filter_loaded_snapshot(std::move(loaded), {}, {}, "test");
  EXPECT_FALSE(result.has_value());
}

TEST(DisplayHelperV2Codec, FilterLoadDropsAbsentDevicesButKeepsRest) {
  codec::ParsedSnapshot loaded;
  loaded.snapshot = make_snapshot({{"A"}, {"B"}});
  loaded.has_layout_data = true;
  loaded.layout_rotations = {{"A", 0}, {"B", 90}};

  display_device::EnumeratedDeviceList devices {
    make_device("A", "\\\\.\\DISPLAY1", true),
    // B not present anymore
  };

  auto result = codec::filter_loaded_snapshot(std::move(loaded), devices, {}, "test");
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->snapshot.m_topology.size(), 1u);
  EXPECT_EQ(result->snapshot.m_topology.front().front(), "A");
  EXPECT_EQ(result->snapshot.m_modes.count("B"), 0u);
  EXPECT_EQ(result->snapshot.m_hdr_states.count("B"), 0u);
  EXPECT_EQ(result->layout_rotations.count("B"), 0u);
  EXPECT_TRUE(result->has_layout_data);
}

TEST(DisplayHelperV2Codec, FilterLoadAllowsInactiveButConnectedDevices) {
  // m_display_name is only populated for active displays; restore must still
  // target inactive-but-connected monitors by device id alone.
  codec::ParsedSnapshot loaded;
  loaded.snapshot = make_snapshot({{"A"}});

  display_device::EnumeratedDeviceList devices {
    make_device("A", "", false),  // connected but inactive
  };

  auto result = codec::filter_loaded_snapshot(std::move(loaded), devices, {}, "test");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->snapshot.m_topology.front().front(), "A");
}

// --- save-side filtering (f3841ad8) ---

TEST(DisplayHelperV2Codec, FilterSaveRejectsWhenActiveVirtualPresent) {
  auto snap = make_snapshot({{"PHYS"}});
  display_device::EnumeratedDeviceList devices {
    make_device("PHYS", "\\\\.\\DISPLAY1", true),
    make_device("VDD", "\\\\.\\DISPLAY2", true, "SDD"),
  };

  std::string reason;
  auto result = codec::filter_snapshot_for_save(snap, devices, {}, reason);
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(reason, "active virtual display present");
}

TEST(DisplayHelperV2Codec, FilterSaveDropsDevicesWithoutDisplayName) {
  auto snap = make_snapshot({{"A", "B"}});
  display_device::EnumeratedDeviceList devices {
    make_device("A", "\\\\.\\DISPLAY1", true),
    make_device("B", "", false),  // no display_name: unsafe restore target
  };

  std::string reason;
  auto result = codec::filter_snapshot_for_save(snap, devices, {}, reason);
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->m_topology.size(), 1u);
  EXPECT_EQ(result->m_topology.front().size(), 1u);
  EXPECT_EQ(result->m_topology.front().front(), "A");
  EXPECT_EQ(result->m_modes.count("B"), 0u);
}

TEST(DisplayHelperV2Codec, FilterSaveRejectsAllExcluded) {
  auto snap = make_snapshot({{"A"}});
  display_device::EnumeratedDeviceList devices {
    make_device("A", "\\\\.\\DISPLAY1", true),
  };

  std::string reason;
  auto result = codec::filter_snapshot_for_save(snap, devices, {"a"}, reason);
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(reason, "all devices are excluded");
}

// --- golden health (8f062f99: stale warnings only after failures persist) ---

TEST(DisplayHelperV2GoldenHealth, WarnsOnlyAfterThresholdAndWindow) {
  TempDir temp;
  const auto status_path = temp.path / "display_golden_restore_status.json";

  long long fake_now_ms = 1'000'000;
  display_helper::v2::GoldenHealth health(status_path, [&]() {
    return fake_now_ms;
  });

  auto read_status = [&]() {
    std::ifstream file(status_path, std::ios::binary);
    return nlohmann::json::parse(file, nullptr, false);
  };

  // No issue noted: nothing written.
  health.register_unresolved("noop");
  EXPECT_FALSE(std::filesystem::exists(status_path));

  // Two failures inside the window: marker exists but no out-of-date warning.
  health.note_issue("restore_not_confirmed");
  health.register_unresolved("test");
  health.note_issue("restore_not_confirmed");
  health.register_unresolved("test");

  ASSERT_TRUE(std::filesystem::exists(status_path));
  auto status = read_status();
  ASSERT_TRUE(status.is_object());
  EXPECT_FALSE(status["snapshot_out_of_date"].get<bool>());
  EXPECT_EQ(status["unresolved_restore_attempts"].get<int>(), 2);

  // Third failure past the 72h window: now flagged out of date.
  fake_now_ms += std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::hours(73)).count();
  health.note_issue("restore_not_confirmed");
  health.register_unresolved("test");

  status = read_status();
  EXPECT_TRUE(status["snapshot_out_of_date"].get<bool>());
  EXPECT_EQ(status["unresolved_restore_attempts"].get<int>(), 3);

  // Confirmed restore clears the marker.
  health.clear_status("restore confirmed");
  EXPECT_FALSE(std::filesystem::exists(status_path));
}

// --- recovery engine semantics (legacy try_restore_once_if_valid) ---

namespace {
  class EngineClock final : public display_helper::v2::IClock {
  public:
    std::chrono::steady_clock::time_point now() override {
      return now_;
    }

    void sleep_for(std::chrono::milliseconds duration) override {
      now_ += duration;
    }

    void advance(std::chrono::milliseconds duration) {
      now_ += duration;
    }

  private:
    std::chrono::steady_clock::time_point now_ {std::chrono::steady_clock::now() + std::chrono::hours(1)};
  };

  /// Stateful display fake: apply_snapshot takes effect on `current` unless the
  /// snapshot's first device id is in `ineffective_ids` (simulates Windows
  /// accepting the calls without the state actually sticking).
  class EngineDisplayFake final : public display_helper::v2::IDisplaySettings {
  public:
    using ApplyStatus = display_helper::v2::ApplyStatus;

    ApplyStatus apply(const display_device::SingleDisplayConfiguration &) override {
      return ApplyStatus::Ok;
    }

    ApplyStatus apply_topology(const display_device::ActiveTopology &) override {
      return ApplyStatus::Ok;
    }

    display_device::EnumeratedDeviceList enumerate(display_device::DeviceEnumerationDetail) override {
      return devices;
    }

    display_device::ActiveTopology capture_topology() override {
      return current.m_topology;
    }

    bool validate_topology(const display_device::ActiveTopology &) override {
      return true;
    }

    display_device::DisplaySettingsSnapshot capture_snapshot() override {
      return current;
    }

    bool apply_snapshot(const display_device::DisplaySettingsSnapshot &snapshot) override {
      ++apply_calls;
      const auto id = first_id(snapshot);
      apply_order.push_back(id);
      if (!ineffective_ids.contains(id)) {
        current = snapshot;
      }
      return true;
    }

    bool snapshot_matches_current(const display_device::DisplaySettingsSnapshot &snapshot) override {
      return codec::equal_snapshots_strict(current, snapshot);
    }

    bool configuration_matches(const display_device::SingleDisplayConfiguration &) override {
      return true;
    }

    bool set_display_origin(const std::string &, const display_device::Point &) override {
      return true;
    }

    std::optional<display_device::ActiveTopology> compute_expected_topology(
      const display_device::SingleDisplayConfiguration &,
      const std::optional<display_device::ActiveTopology> &) override {
      return std::nullopt;
    }

    bool is_topology_same(const display_device::ActiveTopology &lhs, const display_device::ActiveTopology &rhs) override {
      return codec::canonical_topology(lhs) == codec::canonical_topology(rhs);
    }

    static std::string first_id(const display_device::DisplaySettingsSnapshot &snapshot) {
      if (!snapshot.m_topology.empty() && !snapshot.m_topology.front().empty()) {
        return snapshot.m_topology.front().front();
      }
      return {};
    }

    display_device::DisplaySettingsSnapshot current;
    display_device::EnumeratedDeviceList devices;
    std::set<std::string> ineffective_ids;
    std::vector<std::string> apply_order;
    int apply_calls = 0;
  };

  struct RecoveryHarness {
    EngineClock clock;
    EngineDisplayFake display;
    display_helper::v2::InMemorySnapshotStorage storage;
    TempDir temp;
    display_helper::v2::GoldenHealth golden_health {temp.path / "golden_status.json"};
    display_helper::v2::RestoreState state;
    display_helper::v2::RecoveryOperation recovery {display, storage, golden_health, state, clock};
    display_helper::v2::CancellationSource cancellation;

    void add_device(const std::string &id) {
      display.devices.push_back(make_device(id, "\\\\.\\DISPLAY_" + id, true));
    }
  };
}  // namespace

// fd98755b: once the current state matches the baseline (order-independently),
// the restore must confirm WITHOUT touching the display stack again.
TEST(DisplayHelperV2RecoveryEngine, TerminatesWithoutApplyOnFirstConfirmedMatch) {
  RecoveryHarness harness;
  harness.add_device("A");
  harness.add_device("B");

  auto baseline = make_snapshot({{"A"}, {"B"}});
  ASSERT_TRUE(harness.storage.save(display_helper::v2::SnapshotTier::Current, baseline));

  // Same state, but topology groups enumerated in a different order.
  harness.display.current = make_snapshot({{"B"}, {"A"}});
  harness.display.current.m_primary_device = baseline.m_primary_device;

  auto outcome = harness.recovery.run(harness.cancellation.token());

  EXPECT_TRUE(outcome.success);
  EXPECT_EQ(harness.display.apply_calls, 0);
}

// 79681014 / 8f2d0632: when the current session snapshot is missing, golden is
// preferred over the previous session snapshot.
TEST(DisplayHelperV2RecoveryEngine, PrefersGoldenWhenCurrentMissing) {
  RecoveryHarness harness;
  harness.add_device("G");
  harness.add_device("P");

  ASSERT_TRUE(harness.storage.save(display_helper::v2::SnapshotTier::Previous, make_snapshot({{"P"}})));
  ASSERT_TRUE(harness.storage.save(display_helper::v2::SnapshotTier::Golden, make_snapshot({{"G"}})));
  harness.display.current = make_snapshot({{"X"}});

  auto outcome = harness.recovery.run(harness.cancellation.token());

  EXPECT_TRUE(outcome.success);
  ASSERT_TRUE(outcome.snapshot.has_value());
  EXPECT_EQ(EngineDisplayFake::first_id(*outcome.snapshot), "G");
  ASSERT_FALSE(harness.display.apply_order.empty());
  EXPECT_EQ(harness.display.apply_order.front(), "G");
  // Confirmed golden restore clears the session snapshot chain.
  EXPECT_FALSE(harness.storage.exists(display_helper::v2::SnapshotTier::Previous));
}

// d1c2230e / 59802f34: in golden-first mode, a confirmed session fallback is only
// accepted after three consecutive golden-first attempts keep failing.
TEST(DisplayHelperV2RecoveryEngine, GoldenFirstAcceptsSessionFallbackAfterThreeMisses) {
  RecoveryHarness harness;
  harness.add_device("G");
  harness.add_device("C");

  harness.state.always_restore_from_golden.store(true);

  // Golden applies without effect (never confirms); session snapshots work.
  harness.display.ineffective_ids.insert("G");
  ASSERT_TRUE(harness.storage.save(display_helper::v2::SnapshotTier::Golden, make_snapshot({{"G"}})));
  ASSERT_TRUE(harness.storage.save(display_helper::v2::SnapshotTier::Current, make_snapshot({{"C"}})));
  harness.display.current = make_snapshot({{"X"}});

  auto first = harness.recovery.run(harness.cancellation.token());
  EXPECT_FALSE(first.success);  // session fallback applied, golden still pending
  EXPECT_EQ(harness.state.golden_pending_session_fallbacks.load(), 1u);

  // The session restore landed and current was promoted to previous.
  EXPECT_FALSE(harness.storage.exists(display_helper::v2::SnapshotTier::Current));
  EXPECT_TRUE(harness.storage.exists(display_helper::v2::SnapshotTier::Previous));

  harness.display.current = make_snapshot({{"X"}});  // drift again before next attempt
  auto second = harness.recovery.run(harness.cancellation.token());
  EXPECT_FALSE(second.success);
  EXPECT_EQ(harness.state.golden_pending_session_fallbacks.load(), 2u);

  harness.display.current = make_snapshot({{"X"}});
  auto third = harness.recovery.run(harness.cancellation.token());
  EXPECT_TRUE(third.success);
  EXPECT_EQ(harness.state.golden_pending_session_fallbacks.load(), 0u);
}

// --- storage round trip in the legacy file format ---

TEST(DisplayHelperV2FileStorage, LegacyFormatRoundTripWithLayouts) {
  TempDir temp;
  display_helper::v2::SnapshotPaths paths {
    temp.path / "display_session_current.json",
    temp.path / "display_session_previous.json",
    temp.path / "display_golden_restore.json",
  };
  display_helper::v2::FileSnapshotStorage storage(paths);

  auto snap = make_snapshot({{"A", "B"}});
  snap.m_hdr_states["A"] = display_device::HdrState::Enabled;
  snap.m_origins["A"] = display_device::Point {0, 0};
  snap.m_origins["B"] = display_device::Point {1920, -200};

  codec::layout_rotation_map_t layouts {{"B", 270}};
  EXPECT_TRUE(storage.save(display_helper::v2::SnapshotTier::Golden, snap, layouts));
  EXPECT_TRUE(storage.exists(display_helper::v2::SnapshotTier::Golden));

  auto loaded = storage.load_with_metadata(display_helper::v2::SnapshotTier::Golden);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->snapshot_version, codec::kSnapshotLayoutVersionLatest);
  EXPECT_TRUE(loaded->has_layout_data);
  EXPECT_EQ(loaded->layout_rotations, layouts);
  EXPECT_TRUE(codec::equal_snapshots_strict(loaded->snapshot, snap));
  EXPECT_EQ(loaded->snapshot.m_origins, snap.m_origins);

  EXPECT_TRUE(storage.remove(display_helper::v2::SnapshotTier::Golden));
  EXPECT_FALSE(storage.exists(display_helper::v2::SnapshotTier::Golden));
}

#endif  // _WIN32
