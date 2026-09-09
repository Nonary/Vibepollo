/**
 * @file tests/unit/test_log_sessions.cpp
 * @brief Exercise retained session logs and support ZIPs using temporary files.
 */
#include "../tests_common.h"

#include <chrono>
#include <fstream>
#include <src/log_export.h>
#include <src/logging.h>

namespace fs = std::filesystem;

class LogSessionsTest: public testing::Test {
protected:
  fs::path root;

  void SetUp() override {
    root = fs::temp_directory_path() / ("vibeshine-log-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root / "logs");
  }

  void TearDown() override {
    fs::remove_all(root);
  }
};

TEST_F(LogSessionsTest, RetainsPreviousLaunchesAndPurgesOldestSessionWithRollovers) {
  for (int i = 0; i < 31; ++i) {
    const auto label = "vibeshine-20000101-0000" + std::to_string(10 + i);
    std::ofstream(root / "logs" / (label + ".log")) << "previous launch";
    std::ofstream(root / "logs" / (label + ".log.1")) << "previous rollover";
  }
  auto guard = logging::init(2, root / "vibeshine.log");
  const auto paths = logging::recent_session_logs(30);
  EXPECT_EQ(paths.size(), 59u);  // 29 retained launches with rollover, plus active file.
  EXPECT_FALSE(fs::exists(root / "logs/vibeshine-20000101-000010.log"));
  EXPECT_FALSE(fs::exists(root / "logs/vibeshine-20000101-000010.log.1"));
  EXPECT_TRUE(fs::exists(root / "logs/vibeshine-20000101-000040.log.1"));
}

TEST_F(LogSessionsTest, RotatesAtTwoMiBAndKeepsFourRollovers) {
  auto guard = logging::init(2, root / "vibeshine.log");
  const std::string payload(1024, 'x');
  for (int i = 0; i < 11000; ++i) {
    BOOST_LOG(info) << payload;
  }
  logging::log_flush();
  const auto paths = logging::recent_session_logs(30);
  ASSERT_EQ(paths.size(), 5u);
  EXPECT_TRUE(fs::exists(logging::current_log_file()));
  EXPECT_FALSE(fs::exists(logging::current_log_file().string() + ".1"));
  for (const auto &path : paths) {
    EXPECT_LE(fs::file_size(path), 2u * 1024u * 1024u + 3u);
  }
}

TEST(LogExportTest, ZipPreservesMultipleFilesAndSanitizesConsistently) {
  log_export::export_log_sanitizer_t sanitizer;
  std::vector<log_export::ZipDataEntry> entries;
  entries.push_back(log_export::make_export_log_entry(sanitizer, "previous.log.1", "from /home/alice/game 192.168.1.2", std::nullopt));
  entries.push_back(log_export::make_export_log_entry(sanitizer, "current.log", "to /home/alice/game 192.168.1.2", std::nullopt));
  EXPECT_EQ(entries[0].data.substr(5), entries[1].data.substr(3));
  EXPECT_EQ(entries[0].data.find("alice"), std::string::npos);
  EXPECT_EQ(entries[0].data.find("192.168.1.2"), std::string::npos);
  entries.push_back({"compressed.log", std::string(4096, 'x'), std::nullopt});
  const auto zip = log_export::build_zip_from_entries(entries);
  auto u16 = [&](std::size_t p) {
    return static_cast<unsigned char>(zip[p]) | (static_cast<unsigned char>(zip[p + 1]) << 8);
  };
  auto u32 = [&](std::size_t p) {
    return static_cast<std::uint32_t>(u16(p)) | (static_cast<std::uint32_t>(u16(p + 2)) << 16);
  };
  std::size_t offset = 0;
  for (const auto &entry : entries) {
    ASSERT_EQ(u32(offset), 0x04034b50u);
    const auto size = u32(offset + 18);
    const auto name_size = u16(offset + 26);
    EXPECT_EQ(zip.substr(offset + 30, name_size), entry.name);
    const auto payload = zip.substr(offset + 30 + name_size, size);
    if (u16(offset + 8) == 8) {
      std::string decoded(entry.data.size(), '\0');
      z_stream stream {};
      ASSERT_EQ(inflateInit2(&stream, -MAX_WBITS), Z_OK);
      stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(payload.data()));
      stream.avail_in = payload.size();
      stream.next_out = reinterpret_cast<Bytef *>(decoded.data());
      stream.avail_out = decoded.size();
      EXPECT_EQ(inflate(&stream, Z_FINISH), Z_STREAM_END);
      inflateEnd(&stream);
      EXPECT_EQ(decoded, entry.data);
    } else {
      EXPECT_EQ(payload, entry.data);
    }
    offset += 30 + name_size + size;
  }
  EXPECT_EQ(u32(offset), 0x02014b50u);
  EXPECT_EQ(u16(zip.size() - 12), entries.size());
}
