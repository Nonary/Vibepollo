#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "src/web_ui_preference.h"

namespace {
  struct fake_files_t {
    std::string preference_conf;
    std::string main_config = "# Keep this comment\nport = 47989 # restart pending\n";
    int host_config_reload_calls = 0;
    bool refuse_writes = false;
  };

  web_ui_preference::store_t store_for(fake_files_t &files) {
    return {
      "web-ui-preference.conf",
      [&files](const std::filesystem::path &path) {
        if (path.filename() == "web-ui-preference.conf") return files.preference_conf;
        return std::string {};
      },
      [&files](const std::filesystem::path &path, const std::string_view text) {
        if (path.filename() != "web-ui-preference.conf" || files.refuse_writes) return false;
        files.preference_conf = std::string(text);
        return true;
      },
    };
  }
}

TEST(WebUiPreference, DefaultsToClassicAndRejectsInvalidValues) {
  fake_files_t files;
  auto store = store_for(files);
  EXPECT_EQ(store.read(), "legacy");
  EXPECT_FALSE(store.write("not-an-interface"));
  EXPECT_FALSE(store.write(""));
  EXPECT_TRUE(files.preference_conf.empty());
  EXPECT_TRUE(store.write("v2"));
  EXPECT_EQ(store.read(), "v2");
  EXPECT_NE(files.preference_conf.find("webui = v2\n"), std::string::npos);
  EXPECT_TRUE(store.write("legacy"));
  EXPECT_EQ(store.read(), "legacy");
  EXPECT_NE(files.preference_conf.find("webui = legacy\n"), std::string::npos);
}

TEST(WebUiPreference, SavingChoiceDoesNotRewriteMainConfigOrApplyPendingRestart) {
  fake_files_t files;
  auto store = store_for(files);
  const auto original_config = files.main_config;
  EXPECT_TRUE(store.write("v2"));
  EXPECT_EQ(store.read(), "v2");
  EXPECT_EQ(files.main_config, original_config);
  EXPECT_EQ(files.host_config_reload_calls, 0);
  EXPECT_NE(files.main_config.find("# Keep this comment"), std::string::npos);
  EXPECT_NE(files.main_config.find("port = 47989 # restart pending"), std::string::npos);
}

TEST(WebUiPreference, PreservesPreferenceCommentsUnknownSettingsAndCrLf) {
  fake_files_t files;
  files.preference_conf = "# User annotation\r\nother = keep\r\nwebui = legacy  # trailing comment\r\n# more notes\r\n";
  const auto original = files.preference_conf;
  auto store = store_for(files);
  EXPECT_EQ(store.read(), "legacy");
  EXPECT_TRUE(store.write("v2"));
  EXPECT_EQ(files.preference_conf,
            "# User annotation\r\nother = keep\r\nwebui = v2  # trailing comment\r\n# more notes\r\n");
  EXPECT_EQ(store.read(), "v2");
  EXPECT_TRUE(store.write("legacy"));
  EXPECT_EQ(files.preference_conf, original);
}

TEST(WebUiPreference, OnlyRecognizesExplicitWebuiSetting) {
  fake_files_t files;
  files.preference_conf = "# webui = v2\nwebui_extra = v2\n";
  auto store = store_for(files);
  EXPECT_EQ(store.read(), "legacy");
  EXPECT_TRUE(store.write("v2"));
  EXPECT_EQ(files.preference_conf,
            "# webui = v2\nwebui_extra = v2\nwebui = v2\n");
  EXPECT_EQ(store.read(), "v2");

  files.preference_conf = "webui = v2 # leading value\nwebui=legacy\n";
  EXPECT_TRUE(store.write("legacy"));
  EXPECT_EQ(files.preference_conf, "webui = legacy # leading value\nwebui=legacy\n");
  files.preference_conf = "webui = invalid\n";
  EXPECT_EQ(store.read(), "legacy");
}

TEST(WebUiPreference, ConcurrentTrayReadsAndPreferenceWritesAreSerialized) {
  fake_files_t files;
  auto store = store_for(files);
  std::atomic<bool> valid_read {true};
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back([&store, &valid_read, i] {
      for (int j = 0; j < 250; ++j) {
        if ((i % 2) == 0) {
          if (!store.write((j % 2) == 0 ? "v2" : "legacy")) {
            valid_read = false;
          }
        } else {
          const auto choice = store.read();
          if (!web_ui_preference::valid(choice)) {
            valid_read = false;
          }
        }
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  EXPECT_TRUE(valid_read.load());
  EXPECT_TRUE(web_ui_preference::valid(store.read()));
}

TEST(WebUiPreference, FailedPersistenceDoesNotClaimSuccess) {
  fake_files_t files;
  files.preference_conf = "# Keep this text\nwebui = v2\n";
  files.refuse_writes = true;
  auto store = store_for(files);
  EXPECT_FALSE(store.write("legacy"));
  EXPECT_EQ(files.preference_conf, "# Keep this text\nwebui = v2\n");
  EXPECT_EQ(store.read(), "v2");
}
