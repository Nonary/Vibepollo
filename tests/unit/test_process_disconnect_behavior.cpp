/**
 * @file tests/unit/test_process_disconnect_behavior.cpp
 */
#include "../tests_common.h"

#include <src/process.h>

#include <atomic>
#include <filesystem>
#include <fstream>

namespace {
  class temporary_apps_file_t {
  public:
    temporary_apps_file_t() {
      static std::atomic_uint64_t sequence {0};
      path_ = std::filesystem::temp_directory_path() /
              ("vibepollo-disconnect-behavior-" + std::to_string(++sequence) + ".json");
    }

    ~temporary_apps_file_t() {
      std::error_code error;
      std::filesystem::remove(path_, error);
    }

    void write(std::string_view contents) const {
      std::ofstream output(path_, std::ios::binary | std::ios::trunc);
      ASSERT_TRUE(output.is_open());
      output << contents;
      ASSERT_TRUE(output.good());
    }

    [[nodiscard]] std::string string() const { return path_.string(); }

  private:
    std::filesystem::path path_;
  };

  TEST(ProcessDisconnectBehavior, GlobalChoicesAndFallbacks) {
    EXPECT_EQ(proc::parse_global_disconnect_behavior("keep_running"), proc::disconnect_behavior_e::keep_running);
    EXPECT_EQ(proc::parse_global_disconnect_behavior("suspend"), proc::disconnect_behavior_e::suspend);
    EXPECT_EQ(proc::parse_global_disconnect_behavior("terminate"), proc::disconnect_behavior_e::keep_running);
    EXPECT_EQ(proc::parse_global_disconnect_behavior("invalid"), proc::disconnect_behavior_e::keep_running);
    EXPECT_EQ(proc::parse_global_disconnect_behavior(" SUSPEND "), proc::disconnect_behavior_e::suspend);
  }

  TEST(ProcessDisconnectBehavior, ParsesOverridesAndLegacyTerminateFlag) {
    temporary_apps_file_t file;
    file.write(R"json({
      "env": {},
      "apps": [
        {"name": "Default", "cmd": ""},
        {"name": "Keep", "cmd": "", "disconnect-behavior": "keep_running"},
        {"name": "Suspend", "cmd": "", "disconnect-behavior": "suspend"},
        {"name": "Terminate", "cmd": "", "disconnect-behavior": "terminate"},
        {"name": "Inherit", "cmd": "", "disconnect-behavior": "inherit"},
        {"name": "Legacy true", "cmd": "", "terminate-on-pause": true},
        {"name": "Legacy false", "cmd": "", "terminate-on-pause": false},
        {"name": "New inherit wins", "cmd": "", "disconnect-behavior": "inherit", "terminate-on-pause": true},
        {"name": "New keep wins", "cmd": "", "disconnect-behavior": "keep_running", "terminate-on-pause": true},
        {"name": "New suspend wins", "cmd": "", "disconnect-behavior": "suspend", "terminate-on-pause": true},
        {"name": "New terminate wins", "cmd": "", "disconnect-behavior": "terminate", "terminate-on-pause": false}
      ]
    })json");

    auto parsed = proc::parse(file.string());
    ASSERT_TRUE(parsed.has_value());
    const auto apps = parsed->get_apps();
    ASSERT_GE(apps.size(), 11u);

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
    ASSERT_TRUE(apps[8].disconnect_behavior_override.has_value());
    EXPECT_EQ(*apps[8].disconnect_behavior_override, proc::disconnect_behavior_e::keep_running);
    ASSERT_TRUE(apps[9].disconnect_behavior_override.has_value());
    EXPECT_EQ(*apps[9].disconnect_behavior_override, proc::disconnect_behavior_e::suspend);
    ASSERT_TRUE(apps[10].disconnect_behavior_override.has_value());
    EXPECT_EQ(*apps[10].disconnect_behavior_override, proc::disconnect_behavior_e::terminate);
  }
}  // namespace
