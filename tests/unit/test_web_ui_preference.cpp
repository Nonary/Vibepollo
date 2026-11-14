#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
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
    bool refuse_reads = false;
    int preference_write_calls = 0;
  };

  web_ui_preference::store_t store_for(fake_files_t &files) {
    return {
      "web-ui-preference.conf",
      [&files](const std::filesystem::path &path) {
        if (files.refuse_reads) return std::optional<std::string> {};
        if (path.filename() == "web-ui-preference.conf") return std::optional<std::string> {files.preference_conf};
        return std::optional<std::string> {std::string {}};
      },
      [&files](const std::filesystem::path &path, const std::string_view text) {
        ++files.preference_write_calls;
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

TEST(WebUiPreference, UnreadableExistingFileIsNeverReplaced) {
  fake_files_t files;
  files.preference_conf = "# Keep user comment\nother = keep\nwebui = v2\n";
  const auto original = files.preference_conf;
  files.refuse_reads = true;
  auto store = store_for(files);
  EXPECT_FALSE(store.write("legacy"));
  EXPECT_EQ(files.preference_write_calls, 0);
  EXPECT_EQ(files.preference_conf, original);
}

TEST(WebUiPreference, NativeFileSavePreservesCrLfBytes) {
  const auto path = std::filesystem::temp_directory_path() /
    ("vibepollo-ui-preference-crlf-" +
     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".conf");
  const std::string original = "# User note\r\nother = keep\r\nwebui = legacy  # important\r\n";
  {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output.is_open());
    output.write(original.data(), static_cast<std::streamsize>(original.size()));
    ASSERT_TRUE(output.good());
  }
  web_ui_preference::store_t store {
    path,
    web_ui_preference::read_conf_file,
    // Native binary I/O isolates the Windows CRLF read regression without
    // linking the host file-handler implementation into this component test.
    // Atomic replacement and Windows WriteFile behavior are covered by the
    // independent FileHandlerPolicy / FileHandlerWindowsWrite tests.
    [](const std::filesystem::path &target, const std::string_view data) {
      std::ofstream output(target, std::ios::binary | std::ios::trunc);
      if (!output.is_open()) {
        return false;
      }
      output.write(data.data(), static_cast<std::streamsize>(data.size()));
      output.close();
      return output.good();
    },
  };
  EXPECT_EQ(store.read(), "legacy");
  EXPECT_TRUE(store.write("v2"));
  {
    std::ifstream input(path, std::ios::binary);
    ASSERT_TRUE(input.is_open());
    const std::string bytes {(std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>()};
    EXPECT_EQ(bytes, "# User note\r\nother = keep\r\nwebui = v2  # important\r\n");
  }
  std::error_code ec;
  std::filesystem::remove(path, ec);
}
