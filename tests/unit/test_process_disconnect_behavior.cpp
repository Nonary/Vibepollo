/**
 * @file tests/unit/test_process_disconnect_behavior.cpp
 */
#include "../tests_common.h"

#include <src/process.h>

#include <filesystem>
#include <fstream>

namespace {
  TEST(ProcessDisconnectBehavior, ParsesOverridesAndLegacyTerminateFlag) {
    const auto path = std::filesystem::temp_directory_path() / "sunshine-disconnect-behavior-apps.json";
    {
      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      out << R"json({
        "env": {},
        "apps": [
          {"name": "Default", "cmd": ""},
          {"name": "Keep", "cmd": "", "disconnect-behavior": "keep_running"},
          {"name": "Suspend", "cmd": "", "disconnect-behavior": "suspend"},
          {"name": "Terminate", "cmd": "", "disconnect-behavior": "terminate"},
          {"name": "Inherit", "cmd": "", "disconnect-behavior": "inherit"},
          {"name": "Legacy true", "cmd": "", "terminate-on-pause": true},
          {"name": "Legacy false", "cmd": "", "terminate-on-pause": false},
          {"name": "New inherit wins", "cmd": "", "disconnect-behavior": "inherit", "terminate-on-pause": true}
        ]
      })json";
    }

    auto parsed = proc::parse(path.string());
    ASSERT_TRUE(parsed.has_value());
    const auto apps = parsed->get_apps();
    ASSERT_GE(apps.size(), 8u);

    EXPECT_FALSE(apps[0].disconnect_behavior_override.has_value());
    ASSERT_TRUE(apps[1].disconnect_behavior_override.has_value());
    EXPECT_EQ(*apps[1].disconnect_behavior_override, proc::disconnect_behavior_e::keep_running);
    ASSERT_TRUE(apps[2].disconnect_behavior_override.has_value());
    EXPECT_EQ(*apps[2].disconnect_behavior_override, proc::disconnect_behavior_e::suspend);
    ASSERT_TRUE(apps[3].disconnect_behavior_override.has_value());
    EXPECT_EQ(*apps[3].disconnect_behavior_override, proc::disconnect_behavior_e::terminate);
    EXPECT_FALSE(apps[4].disconnect_behavior_override.has_value());
    ASSERT_TRUE(apps[5].disconnect_behavior_override.has_value());
    EXPECT_EQ(*apps[5].disconnect_behavior_override, proc::disconnect_behavior_e::terminate);
    EXPECT_FALSE(apps[6].disconnect_behavior_override.has_value());
    EXPECT_FALSE(apps[7].disconnect_behavior_override.has_value());

    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
}  // namespace
