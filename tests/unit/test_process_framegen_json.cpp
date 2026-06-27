/**
 * @file tests/unit/test_process_framegen_json.cpp
 */
#include "../tests_common.h"

#include <src/process.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

  std::filesystem::path write_apps_json(const std::string &name, const std::string &json) {
    auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << json;
    out.close();
    return path;
  }

  TEST(ProcessFramegenJson, LegacyAndCurrentCaptureFixKeysStillParse) {
    const auto path = write_apps_json(
      "sunshine-framegen-legacy-apps.json",
      R"json({
        "env": {},
        "apps": [
          {
            "name": "Legacy",
            "cmd": "",
            "dlss-framegen-capture-fix": true,
            "gen2-framegen-fix": false
          }
        ]
      })json"
    );

    auto parsed = proc::parse(path.string());
    ASSERT_TRUE(parsed.has_value());
    const auto apps = parsed->get_apps();
    ASSERT_EQ(apps.size(), 1u);
    EXPECT_TRUE(apps[0].gen1_framegen_fix);
    EXPECT_FALSE(apps[0].gen2_framegen_fix);
    EXPECT_TRUE(apps[0].frame_generation_enabled);

    std::error_code ec;
    std::filesystem::remove(path, ec);
  }

  TEST(ProcessFramegenJson, CurrentGen1CaptureFixKeyParses) {
    const auto path = write_apps_json(
      "sunshine-framegen-gen1-apps.json",
      R"json({
        "env": {},
        "apps": [
          {
            "name": "Gen1",
            "cmd": "",
            "gen1-framegen-fix": true
          }
        ]
      })json"
    );

    auto parsed = proc::parse(path.string());
    ASSERT_TRUE(parsed.has_value());
    const auto apps = parsed->get_apps();
    ASSERT_EQ(apps.size(), 1u);
    EXPECT_TRUE(apps[0].gen1_framegen_fix);
    EXPECT_TRUE(apps[0].frame_generation_enabled);

    std::error_code ec;
    std::filesystem::remove(path, ec);
  }

  TEST(ProcessFramegenJson, FrameGenerationModeOffOverridesStaleProvider) {
    const auto path = write_apps_json(
      "sunshine-framegen-off-apps.json",
      R"json({
        "env": {},
        "apps": [
          {
            "name": "Off",
            "cmd": "",
            "frame-generation-mode": "off",
            "frame-generation-provider": "nvidia-smooth-motion",
            "lossless-scaling-framegen": true
          }
        ]
      })json"
    );

    auto parsed = proc::parse(path.string());
    ASSERT_TRUE(parsed.has_value());
    const auto apps = parsed->get_apps();
    ASSERT_EQ(apps.size(), 1u);
    EXPECT_FALSE(apps[0].frame_generation_enabled);
    EXPECT_FALSE(apps[0].lossless_scaling_framegen);
    EXPECT_EQ(apps[0].frame_generation_provider, "lossless-scaling");

    std::error_code ec;
    std::filesystem::remove(path, ec);
  }

  TEST(ProcessFramegenJson, FrameGenerationModeSelectsProvider) {
    const auto path = write_apps_json(
      "sunshine-framegen-mode-apps.json",
      R"json({
        "env": {},
        "apps": [
          {
            "name": "Smooth",
            "cmd": "",
            "frame-generation-mode": "nvidia-smooth-motion"
          }
        ]
      })json"
    );

    auto parsed = proc::parse(path.string());
    ASSERT_TRUE(parsed.has_value());
    const auto apps = parsed->get_apps();
    ASSERT_EQ(apps.size(), 1u);
    EXPECT_TRUE(apps[0].frame_generation_enabled);
    EXPECT_FALSE(apps[0].lossless_scaling_framegen);
    EXPECT_EQ(apps[0].frame_generation_provider, "nvidia-smooth-motion");

    std::error_code ec;
    std::filesystem::remove(path, ec);
  }

}  // namespace
