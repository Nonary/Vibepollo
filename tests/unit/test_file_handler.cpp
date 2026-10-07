/**
 * @file tests/unit/test_file_handler.cpp
 * @brief Deterministic file-handler policy tests.
 */
#include "../tests_common.h"

#include <src/file_handler.h>

#include <map>
#include <set>

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
