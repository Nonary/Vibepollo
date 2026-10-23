/**
 * @file tests/unit/test_file_handler.cpp
 * @brief Deterministic file-handler policy tests.
 */
#include "../tests_common.h"

#include <src/file_handler.h>
#include <src/file_handler_windows.h>

#include <map>
#include <set>
#include <vector>

namespace {
  class fake_filesystem_t: public file_handler::filesystem_t {
  public:
    bool exists(const std::filesystem::path &path) const override {
      const auto key = path.generic_string();
      return files.contains(key) || directories.contains(key);
    }

    bool create_directories(const std::filesystem::path &path) override {
      if (fail_create_directories) {
        return false;
      }
      directories.insert(path.generic_string());
      return true;
    }

    std::string read(const std::filesystem::path &path) const override {
      const auto entry = files.find(path.generic_string());
      return entry == files.end() ? std::string {} : entry->second;
    }

    bool write(const std::filesystem::path &path, std::string_view contents) override {
      if (fail_write) {
        return false;
      }
      files[path.generic_string()] = std::string(contents);
      return true;
    }

    file_handler::replace_result_e replace(const std::filesystem::path &temporary,
                                           const std::filesystem::path &target) override {
      ++replace_calls;
      const auto target_key = target.generic_string();
      if (read_only_files.contains(target_key)) {
        return file_handler::replace_result_e::access_denied;
      }
      if (fail_replace) {
        return file_handler::replace_result_e::error;
      }
      files[target_key] = files[temporary.generic_string()];
      files.erase(temporary.generic_string());
      return file_handler::replace_result_e::success;
    }

    std::optional<bool> read_only(const std::filesystem::path &path) const override {
      return read_only_files.contains(path.generic_string());
    }

    bool set_read_only(const std::filesystem::path &path, bool read_only) override {
      ++attribute_writes;
      if (fail_attribute_write) {
        return false;
      }
      if (read_only) {
        read_only_files.insert(path.generic_string());
      } else {
        read_only_files.erase(path.generic_string());
      }
      return true;
    }

    void remove(const std::filesystem::path &path) override {
      files.erase(path.generic_string());
      removed.insert(path.generic_string());
    }

    std::filesystem::path temporary_path(const std::filesystem::path &target) override {
      return target.generic_string() + ".tmp";
    }

    std::map<std::string, std::string> files;
    std::set<std::string> directories;
    std::set<std::string> read_only_files;
    std::set<std::string> removed;
    bool fail_create_directories = false;
    bool fail_write = false;
    bool fail_replace = false;
    bool fail_attribute_write = false;
    int replace_calls = 0;
    int attribute_writes = 0;
  };

  enum class windows_write_failure_e {
    none,
    create,
    write,
    zero_write,
    flush,
    close,
  };

  struct fake_windows_file_api_t {
    explicit fake_windows_file_api_t(std::map<std::string, std::string> &files):
        files(files) {}

    std::optional<int> create(const std::filesystem::path &path) {
      events.emplace_back("create");
      if (failure == windows_write_failure_e::create) {
        return std::nullopt;
      }
      temporary = path.generic_string();
      files[temporary].clear();
      open = true;
      return handle;
    }

    bool write(int file, const char *data, uint32_t amount, uint32_t &written) {
      EXPECT_EQ(file, handle);
      EXPECT_TRUE(open);
      events.emplace_back("write");
      if (failure == windows_write_failure_e::write && !files[temporary].empty()) {
        return false;
      }
      written = failure == windows_write_failure_e::zero_write ? 0 : (std::min) (amount, max_write);
      files[temporary].append(data, written);
      return true;
    }

    bool flush(int file) {
      EXPECT_EQ(file, handle);
      EXPECT_TRUE(open);
      events.emplace_back("flush");
      if (failure == windows_write_failure_e::flush) {
        return false;
      }
      flushed_contents = files[temporary];
      return true;
    }

    bool close(int file) {
      EXPECT_EQ(file, handle);
      EXPECT_TRUE(open);
      events.emplace_back("close");
      open = false;
      return failure != windows_write_failure_e::close;
    }

    std::map<std::string, std::string> &files;
    windows_write_failure_e failure = windows_write_failure_e::none;
    uint32_t max_write = 3;
    static constexpr int handle = 17;
    bool open = false;
    std::string temporary;
    std::optional<std::string> flushed_contents;
    std::vector<std::string> events;
  };

  class windows_write_filesystem_t: public fake_filesystem_t {
  public:
    bool write(const std::filesystem::path &path, std::string_view contents) override {
      return file_handler::detail::write_windows_file(api, path, contents);
    }

    file_handler::replace_result_e replace(const std::filesystem::path &temporary, const std::filesystem::path &target) override {
      EXPECT_FALSE(api.open);
      EXPECT_EQ(api.flushed_contents, files[temporary.generic_string()]);
      api.events.emplace_back("replace");
      return fake_filesystem_t::replace(temporary, target);
    }

    void remove(const std::filesystem::path &path) override {
      api.events.emplace_back("remove");
      fake_filesystem_t::remove(path);
    }

    fake_windows_file_api_t api {files};
  };
}  // namespace

class FileHandlerParentDirectoryTest: public testing::TestWithParam<std::tuple<std::string, std::string>> {};

TEST_P(FileHandlerParentDirectoryTest, ReturnsTrimmedParent) {
  fake_filesystem_t filesystem;
  const file_handler::handler_t handler {filesystem};
  const auto &[input, expected] = GetParam();
  EXPECT_EQ(handler.get_parent_directory(input), expected);
}

INSTANTIATE_TEST_SUITE_P(
  FileHandlerTests,
  FileHandlerParentDirectoryTest,
  testing::Values(
    std::make_tuple("/path/to/file.txt", "/path/to"),
    std::make_tuple("/path/to/directory", "/path/to"),
    std::make_tuple("/path/to/directory/", "/path/to")
  )
);

TEST(FileHandlerPolicy, CreatesMissingDirectoryAndAcceptsExistingDirectory) {
  fake_filesystem_t filesystem;
  file_handler::handler_t handler {filesystem};

  EXPECT_TRUE(handler.make_directory("sandbox/path"));
  EXPECT_TRUE(filesystem.directories.contains("sandbox/path"));
  EXPECT_TRUE(handler.make_directory("sandbox/path"));
}

class FileHandlerContentTest: public testing::TestWithParam<std::string> {};

TEST_P(FileHandlerContentTest, WritesAtomicallyAndReadsBackContent) {
  fake_filesystem_t filesystem;
  file_handler::handler_t handler {filesystem};

  ASSERT_EQ(handler.write_file("sandbox/value.txt", GetParam()), 0);
  EXPECT_EQ(handler.read_file("sandbox/value.txt"), GetParam());
  EXPECT_FALSE(filesystem.files.contains("sandbox/value.txt.tmp"));
}

INSTANTIATE_TEST_SUITE_P(
  FileHandlerTests,
  FileHandlerContentTest,
  testing::Values(
    std::string {},
    std::string {"a"},
    std::string {"Mr. Blue Sky - Electric Light Orchestra"},
    std::string {"first line\nsecond line\n"}
  )
);

TEST(FileHandlerPolicy, MissingFileReturnsEmpty) {
  fake_filesystem_t filesystem;
  const file_handler::handler_t handler {filesystem};
  EXPECT_TRUE(handler.read_file("missing.txt").empty());
}

TEST(FileHandlerPolicy, WriteFailureRemovesTemporaryFile) {
  fake_filesystem_t filesystem;
  filesystem.fail_write = true;
  file_handler::handler_t handler {filesystem};

  EXPECT_EQ(handler.write_file("value.txt", "content"), -1);
  EXPECT_TRUE(filesystem.removed.contains("value.txt.tmp"));
}

TEST(FileHandlerPolicy, ReadOnlyTargetIsClearedAndRetried) {
  fake_filesystem_t filesystem;
  filesystem.files["value.txt"] = "original";
  filesystem.read_only_files.insert("value.txt");
  file_handler::handler_t handler {filesystem};

  EXPECT_EQ(handler.write_file("value.txt", "updated"), 0);
  EXPECT_EQ(handler.read_file("value.txt"), "updated");
  EXPECT_FALSE(filesystem.read_only_files.contains("value.txt"));
  EXPECT_EQ(filesystem.replace_calls, 2);
  EXPECT_EQ(filesystem.attribute_writes, 1);
}

TEST(FileHandlerPolicy, FailedRetryRestoresReadOnlyAttributeAndCleansTemporary) {
  fake_filesystem_t filesystem;
  filesystem.files["value.txt"] = "original";
  filesystem.read_only_files.insert("value.txt");
  file_handler::handler_t handler {filesystem};

  // First replacement is denied by the attribute. After it is cleared, make
  // every subsequent replacement fail to exercise rollback.
  filesystem.fail_replace = true;
  EXPECT_EQ(handler.write_file("value.txt", "updated"), -1);
  EXPECT_TRUE(filesystem.read_only_files.contains("value.txt"));
  EXPECT_TRUE(filesystem.removed.contains("value.txt.tmp"));
  EXPECT_EQ(handler.read_file("value.txt"), "original");
}

TEST(FileHandlerWindowsWrite, PartialWritesFlushAndCloseBeforeReplacement) {
  windows_write_filesystem_t filesystem;
  filesystem.files["state.json"] = "original";
  filesystem.files["state.json.bak"] = "backup";
  file_handler::handler_t handler {filesystem};

  ASSERT_EQ(handler.write_file("state.json", "updated"), 0);

  EXPECT_EQ(filesystem.files["state.json"], "updated");
  EXPECT_EQ(filesystem.files["state.json.bak"], "backup");
  EXPECT_FALSE(filesystem.files.contains("state.json.tmp"));
  EXPECT_EQ(filesystem.api.events, (std::vector<std::string> {"create", "write", "write", "write", "flush", "close", "replace"}));
}

TEST(FileHandlerWindowsWrite, EmptyFileStillFlushesAndClosesBeforeReplacement) {
  windows_write_filesystem_t filesystem;
  filesystem.files["state.json"] = "original";
  file_handler::handler_t handler {filesystem};

  ASSERT_EQ(handler.write_file("state.json", {}), 0);

  EXPECT_TRUE(filesystem.files["state.json"].empty());
  EXPECT_EQ(filesystem.api.events, (std::vector<std::string> {"create", "flush", "close", "replace"}));
}

class FileHandlerWindowsFailureTest: public testing::TestWithParam<std::tuple<windows_write_failure_e, std::string>> {};

TEST_P(FileHandlerWindowsFailureTest, FailurePreservesPrimaryAndBackupAndRemovesOnlyTemporary) {
  windows_write_filesystem_t filesystem;
  // These snapshots stand for complete auxiliary state, including credentials
  // and fields unknown to an individual writer. Failures must preserve bytes.
  filesystem.files["state.json"] = R"({"root":{"session_tokens":[{"hash":"keep"}],"unknown":"primary"}})";
  filesystem.files["state.json.bak"] = R"({"root":{"app_id_aliases":{"a":"b"},"unknown":"backup"}})";
  const auto before = filesystem.files;
  const auto &[failure, target] = GetParam();
  filesystem.api.failure = failure;
  file_handler::handler_t handler {filesystem};

  EXPECT_EQ(handler.write_file(target, "updated state"), -1);

  EXPECT_EQ(filesystem.files, before);
  EXPECT_EQ(filesystem.replace_calls, 0);
  EXPECT_EQ(filesystem.attribute_writes, 0);
  EXPECT_EQ(filesystem.removed, (std::set<std::string> {target + ".tmp"}));
  EXPECT_FALSE(filesystem.api.open);
  if (failure == windows_write_failure_e::create) {
    EXPECT_EQ(filesystem.api.events, (std::vector<std::string> {"create", "remove"}));
  } else {
    ASSERT_GE(filesystem.api.events.size(), 3);
    EXPECT_EQ(filesystem.api.events[filesystem.api.events.size() - 2], "close");
    EXPECT_EQ(filesystem.api.events.back(), "remove");
  }
}

INSTANTIATE_TEST_SUITE_P(
  FileHandlerTests,
  FileHandlerWindowsFailureTest,
  testing::Combine(
    testing::Values(
      windows_write_failure_e::create,
      windows_write_failure_e::write,
      windows_write_failure_e::zero_write,
      windows_write_failure_e::flush,
      windows_write_failure_e::close
    ),
    testing::Values(std::string {"state.json"}, std::string {"state.json.bak"})
  )
);

TEST(FileHandlerWindowsWrite, ReplaceFailurePreservesPrimaryAndBackupAfterSuccessfulFlush) {
  windows_write_filesystem_t filesystem;
  filesystem.files["state.json"] = "original";
  filesystem.files["state.json.bak"] = "backup";
  const auto before = filesystem.files;
  filesystem.fail_replace = true;
  file_handler::handler_t handler {filesystem};

  EXPECT_EQ(handler.write_file("state.json", "updated"), -1);

  EXPECT_EQ(filesystem.files, before);
  EXPECT_EQ(filesystem.api.flushed_contents, "updated");
  EXPECT_EQ(filesystem.removed, (std::set<std::string> {"state.json.tmp"}));
  EXPECT_EQ(filesystem.replace_calls, 1);
}
