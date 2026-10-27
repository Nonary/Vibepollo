#ifdef _WIN32

  #include "../tests_common.h"

  #include "src/platform/windows/display_baseline_presence.h"
  #include "src/platform/windows/display_restore_guard.h"

  #include <display_device/windows/win_api_utils.h>
  #include <display_device/windows/win_display_device.h>

  #include <memory>
  #include <optional>
  #include <set>
  #include <string>
  #include <utility>
  #include <vector>

namespace {
  using display_device::DeviceDisplayModeMap;
  using display_device::PathAndModeData;
  using display_device::QueryType;
  using display_device::Rational;

  DISPLAYCONFIG_PATH_INFO make_path(
    const UINT32 target_id,
    const bool active,
    const bool available,
    const Rational refresh_rate,
    const bool has_source_mode) {
    DISPLAYCONFIG_PATH_INFO path {};
    path.flags = DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE;
    if (active) {
      path.flags |= DISPLAYCONFIG_PATH_ACTIVE;
    }
    path.sourceInfo.adapterId = {1, 0};
    path.sourceInfo.id = target_id;
    path.targetInfo.adapterId = {1, 0};
    path.targetInfo.id = target_id;
    path.targetInfo.targetAvailable = available ? TRUE : FALSE;
    path.targetInfo.refreshRate = {refresh_rate.m_numerator, refresh_rate.m_denominator};
    if (has_source_mode) {
      display_device::win_utils::setSourceIndex(path, 0);
    } else {
      display_device::win_utils::setSourceIndex(path, std::nullopt);
    }
    display_device::win_utils::setTargetIndex(path, std::nullopt);
    display_device::win_utils::setDesktopIndex(path, std::nullopt);
    return path;
  }

  DISPLAYCONFIG_MODE_INFO make_source_mode(const UINT32 source_id) {
    DISPLAYCONFIG_MODE_INFO mode {};
    mode.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
    mode.adapterId = {1, 0};
    mode.id = source_id;
    mode.sourceMode = {};
    mode.sourceMode.position = {0, 0};
    mode.sourceMode.width = 3840;
    mode.sourceMode.height = 2160;
    return mode;
  }

  class FakeWinApiLayer final: public display_device::WinApiLayerInterface {
  public:
    FakeWinApiLayer() {
      current_data.m_paths.push_back(make_path(1, true, true, {59997, 1000}, true));
      current_data.m_modes.push_back(make_source_mode(1));
    }

    std::string getErrorString(LONG) const override {
      return "fake error";
    }

    std::optional<PathAndModeData> queryDisplayConfig(const QueryType type) const override {
      queries.push_back(type);
      if (type == QueryType::All) {
        if (fail_all_query) {
          return std::nullopt;
        }
        if (all_data_override) {
          return all_data_override;
        }
      }
      return current_data;
    }

    std::string getDeviceId(const DISPLAYCONFIG_PATH_INFO &path) const override {
      if (path.targetInfo.id == 1) {
        return "saved-display";
      }
      if (path.targetInfo.id == 2) {
        return "other-display";
      }
      return "display-" + std::to_string(path.targetInfo.id);
    }

    std::vector<std::byte> getEdid(const DISPLAYCONFIG_PATH_INFO &) const override {
      return {};
    }

    std::string getMonitorDevicePath(const DISPLAYCONFIG_PATH_INFO &path) const override {
      return "monitor-path-" + std::to_string(path.targetInfo.id);
    }

    std::string getFriendlyName(const DISPLAYCONFIG_PATH_INFO &) const override {
      return "Test monitor";
    }

    std::string getDisplayName(const DISPLAYCONFIG_PATH_INFO &path) const override {
      ++display_name_queries;
      if (display_name_unavailable.contains(path.targetInfo.id)) {
        return {};
      }
      return "\\\\.\\DISPLAY1";
    }

    LONG setDisplayConfig(
      std::vector<DISPLAYCONFIG_PATH_INFO> paths,
      std::vector<DISPLAYCONFIG_MODE_INFO> modes,
      const UINT32 flags) override {
      ++set_config_calls;
      submitted_paths = paths;
      submitted_modes = modes;
      submitted_flags = flags;
      if (set_config_result != ERROR_SUCCESS) {
        return set_config_result;
      }

      current_data.m_paths = std::move(paths);
      current_data.m_modes = std::move(modes);
      if (readback_rate && !current_data.m_paths.empty()) {
        current_data.m_paths.front().targetInfo.refreshRate = {
          readback_rate->m_numerator,
          readback_rate->m_denominator
        };
      }
      return ERROR_SUCCESS;
    }

    std::optional<display_device::HdrState> getHdrState(const DISPLAYCONFIG_PATH_INFO &) const override {
      return std::nullopt;
    }

    bool setHdrState(const DISPLAYCONFIG_PATH_INFO &, display_device::HdrState) override {
      return false;
    }

    std::optional<Rational> getDisplayScale(const std::string &, const DISPLAYCONFIG_SOURCE_MODE &) const override {
      return std::nullopt;
    }

    std::optional<display_device::Resolution> getPreferredResolution(const DISPLAYCONFIG_PATH_INFO &) const override {
      return display_device::Resolution {3840, 2160};
    }

    std::vector<display_device::DisplayMode> getSupportedDisplayModes(const DISPLAYCONFIG_PATH_INFO &) const override {
      ++supported_mode_queries;
      return {{{3840, 2160}, {60, 1}}};
    }

    bool probeGdiDisplayMode(const DISPLAYCONFIG_PATH_INFO &, const display_device::DisplayMode &) const override {
      return false;
    }

    void recoverDisplayStack() override {}

    PathAndModeData current_data;
    std::optional<PathAndModeData> all_data_override;
    mutable std::vector<QueryType> queries;
    std::set<UINT32> display_name_unavailable;
    mutable int display_name_queries = 0;
    mutable int supported_mode_queries = 0;
    int set_config_calls = 0;
    UINT32 submitted_flags = 0;
    std::vector<DISPLAYCONFIG_PATH_INFO> submitted_paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> submitted_modes;
    std::optional<Rational> readback_rate;
    LONG set_config_result = ERROR_SUCCESS;
    bool fail_all_query = false;
  };

  const DeviceDisplayModeMap SAVED_MODES {
    {"saved-display", {{3840, 2160}, {240, 4}}}
  };

  void expect_all_queries_active(const FakeWinApiLayer &api) {
    ASSERT_FALSE(api.queries.empty());
    for (const auto type : api.queries) {
      EXPECT_EQ(type, QueryType::Active);
    }
  }

}  // namespace

TEST(DisplayModesRestore, ExactTemporaryApplyPreservesSavedRationalWithoutPersistence) {
  auto api = std::make_shared<FakeWinApiLayer>();
  display_device::WinDisplayDevice display(api);

  ASSERT_EQ(api->current_data.m_paths.front().targetInfo.refreshRate.Numerator, 59997u);
  ASSERT_EQ(api->current_data.m_paths.front().targetInfo.refreshRate.Denominator, 1000u);
  ASSERT_TRUE(display.setDisplayModesExactTemporary(SAVED_MODES));

  ASSERT_EQ(api->set_config_calls, 1);
  ASSERT_EQ(api->submitted_paths.size(), 1u);
  EXPECT_EQ(api->submitted_paths.front().targetInfo.refreshRate.Numerator, 240u);
  EXPECT_EQ(api->submitted_paths.front().targetInfo.refreshRate.Denominator, 4u);
  EXPECT_EQ(api->submitted_flags, static_cast<UINT32>(SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_VIRTUAL_MODE_AWARE));
  EXPECT_EQ(api->submitted_flags & (SDC_SAVE_TO_DATABASE | SDC_ALLOW_CHANGES), 0u);
  EXPECT_EQ(api->supported_mode_queries, 0);
  expect_all_queries_active(*api);
}

TEST(DisplayModesRestore, ExactTemporaryAcceptsEquivalentNormalizedReadback) {
  auto api = std::make_shared<FakeWinApiLayer>();
  api->readback_rate = Rational {60, 1};
  display_device::WinDisplayDevice display(api);

  EXPECT_TRUE(display.setDisplayModesExactTemporary(SAVED_MODES));
  EXPECT_EQ(api->set_config_calls, 1);
  expect_all_queries_active(*api);
}

TEST(DisplayModesRestore, NumericallyEquivalentFixedRateShortCircuitsDrrRestoreUpstreamLimitation) {
  for (const auto rate : {Rational {60, 1}, Rational {120, 2}}) {
    auto api = std::make_shared<FakeWinApiLayer>();
    api->current_data.m_paths.front().targetInfo.refreshRate = {rate.m_numerator, rate.m_denominator};
    display_device::WinDisplayDevice display(api);

    EXPECT_TRUE(display.setDisplayModesExactTemporary(SAVED_MODES));
    EXPECT_EQ(api->set_config_calls, 0);
    EXPECT_EQ(api->current_data.m_paths.front().targetInfo.refreshRate.Numerator, rate.m_numerator);
    EXPECT_EQ(api->current_data.m_paths.front().targetInfo.refreshRate.Denominator, rate.m_denominator);
  }
}

TEST(DisplayModesRestore, ExactRestoreAppliesWhereNormalFuzzyModeSetterDoesNot) {
  auto api = std::make_shared<FakeWinApiLayer>();
  display_device::WinDisplayDevice display(api);

  EXPECT_TRUE(display.setDisplayModes(SAVED_MODES));
  EXPECT_EQ(api->set_config_calls, 0);
  EXPECT_TRUE(display.setDisplayModesExactTemporary(SAVED_MODES));
  EXPECT_EQ(api->set_config_calls, 1);
  ASSERT_FALSE(api->submitted_paths.empty());
  EXPECT_EQ(api->submitted_paths.front().targetInfo.refreshRate.Numerator, 240u);
  EXPECT_EQ(api->submitted_paths.front().targetInfo.refreshRate.Denominator, 4u);
}

TEST(DisplayModesRestore, ExactTemporaryRejectsMismatchedReadback) {
  auto api = std::make_shared<FakeWinApiLayer>();
  api->readback_rate = Rational {59997, 1000};
  display_device::WinDisplayDevice display(api);

  EXPECT_FALSE(display.setDisplayModesExactTemporary(SAVED_MODES));
  ASSERT_EQ(api->set_config_calls, 1);
  ASSERT_FALSE(api->submitted_paths.empty());
  EXPECT_EQ(api->submitted_paths.front().targetInfo.refreshRate.Numerator, 240u);
  EXPECT_EQ(api->submitted_paths.front().targetInfo.refreshRate.Denominator, 4u);
  expect_all_queries_active(*api);
}

TEST(DisplayModesRestore, ActiveEnumerationQueriesActivePathsOnly) {
  auto api = std::make_shared<FakeWinApiLayer>();
  auto all_data = api->current_data;
  all_data.m_paths.push_back(make_path(2, false, true, {0, 0}, false));
  api->all_data_override = std::move(all_data);
  display_device::WinDisplayDevice display(api);

  const auto devices = display.enumActiveDevices(display_device::DeviceEnumerationDetail::Minimal);

  ASSERT_EQ(devices.size(), 1u);
  EXPECT_EQ(devices.front().m_device_id, "saved-display");
  ASSERT_EQ(api->queries.size(), 1u);
  EXPECT_EQ(api->queries.front(), QueryType::Active);
}

TEST(DisplayBaselinePresence, ConnectedInactivePathWithoutDisplayNameKeepsBaselineProtected) {
  FakeWinApiLayer api;
  PathAndModeData all_data;
  all_data.m_paths.push_back(make_path(1, false, true, {0, 0}, false));
  api.all_data_override = std::move(all_data);
  api.display_name_unavailable.insert(1);

  EXPECT_FALSE(display_helper_integration::disconnected_baseline_devices(api, {"saved-display"}).value_or(std::vector<std::string> {}).size());
  EXPECT_EQ(api.queries, (std::vector<QueryType> {QueryType::All}));
  EXPECT_EQ(api.display_name_queries, 0);
}

TEST(DisplayBaselinePresence, FullyAbsentBaselineDeviceIsPrunable) {
  FakeWinApiLayer api;
  PathAndModeData all_data;
  all_data.m_paths.push_back(make_path(2, false, true, {0, 0}, false));
  api.all_data_override = std::move(all_data);

  EXPECT_EQ(display_helper_integration::disconnected_baseline_devices(api, {"saved-display"}), (std::optional<std::vector<std::string>> {{"saved-display"}}));
  EXPECT_EQ(api.queries, (std::vector<QueryType> {QueryType::All}));
  EXPECT_EQ(api.display_name_queries, 0);
}

TEST(DisplayBaselinePresence, FailedAllPathsQueryConservativelyKeepsBaselineProtected) {
  FakeWinApiLayer api;
  api.fail_all_query = true;

  EXPECT_FALSE(display_helper_integration::disconnected_baseline_devices(api, {"saved-display"}).value_or(std::vector<std::string> {}).size());
  EXPECT_EQ(api.queries, (std::vector<QueryType> {QueryType::All}));
}

#endif  // _WIN32
