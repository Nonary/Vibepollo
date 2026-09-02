/**
 * @file tests/unit/platform/linux/test_secure_open.cpp
 * @brief Tests for the openat()-based secure image fallback.
 */

#include "../../../../src/platform/linux/secure_open.h"

#include <array>
#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  namespace fs = std::filesystem;

  class SecureOpenTest: public ::testing::Test {
  protected:
    void SetUp() override {
      root = fs::temp_directory_path() /
             ("vibeshine-secure-open-" + std::to_string(
                                           std::chrono::steady_clock::now().time_since_epoch().count()
                                         ));
      fs::create_directories(root);
      ASSERT_EQ(chmod(root.c_str(), 0700), 0);
      ASSERT_EQ(stat(root.c_str(), &root_attributes), 0);
      root_descriptor = open(root.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
      ASSERT_GE(root_descriptor, 0);
    }

    void TearDown() override {
      if (root_descriptor >= 0) {
        close(root_descriptor);
      }
      std::error_code error;
      fs::remove_all(root, error);
    }

    void write_png(const fs::path &path) {
      static constexpr std::array<unsigned char, 9> bytes {
        0x89,
        0x50,
        0x4e,
        0x47,
        0x0d,
        0x0a,
        0x1a,
        0x0a,
        0x00
      };
      std::ofstream output(path, std::ios::binary);
      output.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
      output.close();
      ASSERT_EQ(chmod(path.c_str(), 0600), 0);
    }

    int secure_open(const fs::path &relative, bool private_directories = true) const {
      return platf::linux_security::open_readonly_beneath(
        root_descriptor,
        relative,
        root_attributes.st_dev,
        geteuid(),
        private_directories
      );
    }

    fs::path root;
    struct stat root_attributes {};
    int root_descriptor {-1};
  };

  TEST_F(SecureOpenTest, OpensTrustedNestedFile) {
    fs::create_directory(root / "covers");
    ASSERT_EQ(chmod((root / "covers").c_str(), 0700), 0);
    write_png(root / "covers/desktop.png");

    const int descriptor = secure_open("covers/desktop.png");
    ASSERT_GE(descriptor, 0);
    std::array<unsigned char, 8> signature {};
    EXPECT_EQ(read(descriptor, signature.data(), signature.size()), static_cast<ssize_t>(signature.size()));
    close(descriptor);
    EXPECT_EQ(signature[0], 0x89);
    EXPECT_EQ(signature[1], 0x50);
  }

  TEST_F(SecureOpenTest, RejectsTraversalAndAbsolutePaths) {
    EXPECT_LT(secure_open("../desktop.png"), 0);
    EXPECT_LT(secure_open(root / "desktop.png"), 0);
    EXPECT_LT(secure_open("covers//desktop.png"), 0);
    EXPECT_LT(secure_open("covers/"), 0);
    EXPECT_LT(secure_open(fs::path {std::string {"desktop\0.png", 12}}), 0);
    EXPECT_LT(secure_open(std::string(NAME_MAX + 1, 'x')), 0);
  }

  TEST_F(SecureOpenTest, RejectsLeafAndIntermediateSymlinks) {
    fs::create_directory(root / "real");
    ASSERT_EQ(chmod((root / "real").c_str(), 0700), 0);
    write_png(root / "real/desktop.png");
    fs::create_symlink(root / "real/desktop.png", root / "leaf.png");
    fs::create_directory_symlink(root / "real", root / "linked");

    EXPECT_LT(secure_open("leaf.png"), 0);
    EXPECT_LT(secure_open("linked/desktop.png"), 0);
  }

  TEST_F(SecureOpenTest, RejectsWritableWrongOwnerAndNonPrivateDirectories) {
    fs::create_directory(root / "covers");
    write_png(root / "covers/desktop.png");

    ASSERT_EQ(chmod((root / "covers").c_str(), 0770), 0);
    EXPECT_LT(secure_open("covers/desktop.png", false), 0);

    ASSERT_EQ(chmod((root / "covers").c_str(), 0755), 0);
    EXPECT_LT(secure_open("covers/desktop.png", true), 0);

    ASSERT_EQ(chmod((root / "covers").c_str(), 0700), 0);
    EXPECT_LT(
      platf::linux_security::open_readonly_beneath(
        root_descriptor,
        "covers/desktop.png",
        root_attributes.st_dev,
        static_cast<uid_t>(geteuid() + 1),
        true
      ),
      0
    );

    EXPECT_LT(
      platf::linux_security::open_readonly_beneath(
        root_descriptor,
        "covers/desktop.png",
        root_attributes.st_dev + 1,
        geteuid(),
        true
      ),
      0
    );
  }
}  // namespace
