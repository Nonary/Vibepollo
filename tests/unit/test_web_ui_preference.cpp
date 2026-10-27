#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "src/web_ui_preference.h"

namespace {
  struct fake_files_t {
    std::string preference_file;
    std::string main_config = "# Keep this comment\nport = 47989 # restart pending\n";
    int host_config_reload_calls = 0;
  };

  web_ui_preference::store_t store_for(fake_files_t &files) {
    return {
      "web-ui-preference",
      [&files](const std::filesystem::path &) { return files.preference_file; },
      [&files](const std::filesystem::path &, const std::string_view text) {
        files.preference_file = std::string(text);
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
  EXPECT_TRUE(files.preference_file.empty());
  EXPECT_TRUE(store.write("v2"));
  EXPECT_EQ(store.read(), "v2");
  EXPECT_TRUE(store.write("legacy"));
  EXPECT_EQ(store.read(), "legacy");
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
  web_ui_preference::store_t store {
    "unwritable",
    [](const std::filesystem::path &) { return std::string {}; },
    [](const std::filesystem::path &, std::string_view) { return false; },
  };
  EXPECT_FALSE(store.write("v2"));
  EXPECT_EQ(store.read(), "legacy");
}
