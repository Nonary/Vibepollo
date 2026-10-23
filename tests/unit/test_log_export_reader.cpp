#include "src/log_export.h"

#include <chrono>
#include <fstream>
#include <functional>
#include <gtest/gtest.h>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;

class LogExportReaderTest: public testing::Test {
protected:
  fs::path root;

  void SetUp() override {
    root = fs::temp_directory_path() / ("log-export-reader-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
  }

  void TearDown() override {
    fs::remove_all(root);
  }

  fs::path write(const std::string &name, const std::string &data) {
    const auto path = root / name;
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << data;
    return path;
  }
};

// Mutate the real file after the production reader has snapshotted its length,
// just before its first read. No timing/sleep race and no copied reader logic.
class MutatingFileBuffer: public std::filebuf {
public:
  std::function<void()> mutate;
  std::size_t bytes_returned = 0;

protected:
  std::streamsize xsgetn(char *data, std::streamsize count) override {
    if (mutate) {
      auto once = std::move(mutate);
      mutate = {};
      once();
    }
    const auto received = std::filebuf::xsgetn(data, count);
    bytes_returned += static_cast<std::size_t>(received);
    return received;
  }
};

struct CapturedArchiveResponse {
  int status = 0;
  std::string body;
  std::multimap<std::string, std::string> headers;

  template<typename Status, typename HeaderMap>
  void write(Status response_status, const std::string &response_body, const HeaderMap &response_headers) {
    status = static_cast<int>(response_status);
    body = response_body;
    headers.insert(response_headers.begin(), response_headers.end());
  }
};

TEST_F(LogExportReaderTest, SmallFilesKeepContentTimestampsAndSharedSanitization) {
  const auto first = write("session.log", "user=alice from 192.168.1.2\n");
  const auto second = write("helper.log", "user=alice from 192.168.1.2\n");
  log_export::support_log_collection_t collection;
  ASSERT_TRUE(collection.add_file(first, "session.log"));
  ASSERT_TRUE(collection.add_file(second, "helper.log"));
  ASSERT_EQ(collection.entries().size(), 2u);
  EXPECT_EQ(collection.entries()[0].data, collection.entries()[1].data);
  EXPECT_EQ(collection.entries()[0].data.find("alice"), std::string::npos);
  EXPECT_EQ(collection.entries()[0].data.find("192.168.1.2"), std::string::npos);
  EXPECT_EQ(collection.entries()[0].data.find("truncated"), std::string::npos);
  EXPECT_EQ(collection.entries()[0].write_time, fs::last_write_time(first));
  EXPECT_EQ(collection.bytes_read(), fs::file_size(first) + fs::file_size(second));
}

TEST_F(LogExportReaderTest, SharedBudgetCoversSessionHelperFallbackAndInstallerFiles) {
  log_export::support_log_collection_t collection {100};
  ASSERT_TRUE(collection.add_file(write("session.log", std::string(20, 's')), "session.log"));
  ASSERT_TRUE(collection.add_file(write("helper/logs/helper.log", std::string(30, 'h')), "helper.log"));
  ASSERT_TRUE(collection.add_file(write("fallback.log", std::string(30, 'f')), "fallback.log"));
  const auto installer = write("installer/install.log", std::string(80, 'i') + "\nrecent result\n");
  ASSERT_TRUE(collection.add_file(installer, "installer/install.log"));
  EXPECT_EQ(collection.bytes_read(), 100u);
  ASSERT_EQ(collection.entries().size(), 4u);
  EXPECT_NE(collection.entries().back().data.find("Log export truncated"), std::string::npos);
  EXPECT_NE(collection.entries().back().data.find("recent result\n"), std::string::npos);
  ASSERT_TRUE(collection.add_file(write("another-helper.log", "not read\n"), "another-helper.log"));
  EXPECT_EQ(collection.bytes_read(), 100u);
  EXPECT_NE(collection.entries().back().data.find("read 0 of 9 snapshot bytes"), std::string::npos);
  EXPECT_EQ(collection.entries().back().data.find("not read"), std::string::npos);
  EXPECT_EQ(fs::file_size(installer), 95u);
}

TEST_F(LogExportReaderTest, OversizedFileUsesDefaultBudgetAndKeepsRecentSanitizedLines) {
  const auto path = write("oversized.log", "");
  // A sparse regular file exercises the real default limit without writing a
  // large fixture or spending time sanitizing a huge repetitive string.
  constexpr auto size = log_export::support_log_read_limit + 1024u * 1024u;
  fs::resize_file(path, size);
  const std::string tail = "\nlatest user=alice from 192.168.1.2\n";
  {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(static_cast<std::streamoff>(size - tail.size()));
    file << tail;
  }
  log_export::support_log_collection_t collection;
  ASSERT_TRUE(collection.add_file(path, "oversized.log"));
  EXPECT_EQ(collection.bytes_read(), log_export::support_log_read_limit);
  ASSERT_EQ(collection.entries().size(), 1u);
  const auto &data = collection.entries().front().data;
  EXPECT_NE(data.find("Log export truncated"), std::string::npos);
  EXPECT_NE(data.find("latest user="), std::string::npos);
  EXPECT_EQ(data.find("alice"), std::string::npos);
  EXPECT_EQ(data.find("192.168.1.2"), std::string::npos);
  EXPECT_LT(data.size(), 1024u);
  EXPECT_EQ(fs::file_size(path), size);
}

TEST_F(LogExportReaderTest, DuplicateZipNamesDoNotConsumeTheReadBudgetAgain) {
  const auto path = write("first.log", "first\n");
  log_export::support_log_collection_t collection {12};
  ASSERT_TRUE(collection.add_file(path, "same.log"));
  ASSERT_TRUE(collection.add_file(write("second.log", "other\n"), "same.log"));
  ASSERT_TRUE(collection.add_file(path, "same.log"));
  ASSERT_EQ(collection.entries().size(), 1u);
  EXPECT_EQ(collection.entries().front().data, "first\n");
  EXPECT_EQ(collection.bytes_read(), 6u);
  ASSERT_TRUE(collection.add_file(write("last.log", "last!\n"), "last.log"));
  EXPECT_EQ(collection.bytes_read(), 12u);
  EXPECT_EQ(collection.entries().back().data, "last!\n");
}

TEST_F(LogExportReaderTest, GrowthDuringReadCannotEscapeTheSnapshotOrBudget) {
  const auto path = write("growing.log", "old prefix\noriginal tail\n");
  MutatingFileBuffer buffer;
  ASSERT_NE(buffer.open(path, std::ios::in | std::ios::binary), nullptr);
  buffer.mutate = [&] {
    std::ofstream(path, std::ios::binary | std::ios::app) << std::string(100000, 'x') << "\nlater tail\n";
  };
  std::istream input {&buffer};
  auto result = log_export::read_log_tail(input, 20);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->snapshot_bytes, 25u);
  EXPECT_EQ(result->bytes_read, 20u);
  EXPECT_EQ(buffer.bytes_returned, 20u);
  EXPECT_TRUE(result->changed);
  EXPECT_TRUE(result->truncated);
  EXPECT_EQ(result->data, "original tail\n");
  EXPECT_EQ(result->data.find("later tail"), std::string::npos);
  buffer.close();
  std::ifstream next(path, std::ios::binary);
  auto later = log_export::read_log_tail(next, 20);
  ASSERT_TRUE(later);
  EXPECT_EQ(later->data, "later tail\n");
  EXPECT_FALSE(later->changed);
}

TEST_F(LogExportReaderTest, TruncationDuringReadIsReportedWithoutUnsignedUnderflow) {
  const auto path = write("shrinking.log", std::string(200, 'x'));
  MutatingFileBuffer buffer;
  ASSERT_NE(buffer.open(path, std::ios::in | std::ios::binary), nullptr);
  buffer.mutate = [&] {
    fs::resize_file(path, 0);
  };
  std::istream input {&buffer};
  auto result = log_export::read_log_tail(input, 100);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->bytes_read, 0u);
  EXPECT_TRUE(result->truncated);
  EXPECT_TRUE(result->changed);
  EXPECT_TRUE(result->data.empty());
}

TEST_F(LogExportReaderTest, GrowthIsNotFollowedEvenWhenTheSharedBudgetHasRoom) {
  const auto path = write("growing.log", "original snapshot\n");
  MutatingFileBuffer buffer;
  ASSERT_NE(buffer.open(path, std::ios::in | std::ios::binary), nullptr);
  buffer.mutate = [&] {
    std::ofstream(path, std::ios::binary | std::ios::app) << "later write\n";
  };
  std::istream input {&buffer};
  auto result = log_export::read_log_tail(input, 1000);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->data, "original snapshot\n");
  EXPECT_EQ(result->bytes_read, 18u);
  EXPECT_EQ(buffer.bytes_returned, 18u);
  EXPECT_TRUE(result->changed);
  EXPECT_FALSE(result->truncated);
}

TEST_F(LogExportReaderTest, PartialBoundaryLinesAreDiscardedBeforeSanitization) {
  const auto path = write("boundary.log", "C:\\Users\\alice\\private\nlatest 192.168.1.2\nunfinished secret");
  log_export::support_log_collection_t collection {45};
  ASSERT_TRUE(collection.add_file(path, "boundary.log"));
  const auto &data = collection.entries().front().data;
  EXPECT_EQ(data.find("alice"), std::string::npos);
  EXPECT_EQ(data.find("private"), std::string::npos);
  EXPECT_EQ(data.find("unfinished secret"), std::string::npos);
  EXPECT_EQ(data.find("192.168.1.2"), std::string::npos);
  EXPECT_NE(data.find("latest <IP_"), std::string::npos);
}

TEST_F(LogExportReaderTest, EmptyExactLimitAndUnavailableFilesDoNotInventTruncation) {
  log_export::support_log_collection_t collection {12};
  EXPECT_FALSE(collection.add_file(root / "missing.log", "missing.log"));
  EXPECT_FALSE(collection.add_file(root, "directory.log"));
  ASSERT_TRUE(collection.add_file(write("empty.log", ""), "empty.log"));
  ASSERT_TRUE(collection.add_file(write("exact.log", "last message"), "exact.log"));
  EXPECT_EQ(collection.bytes_read(), 12u);
  EXPECT_TRUE(collection.entries().front().data.empty());
  EXPECT_EQ(collection.entries().back().data, "last message");
}

TEST_F(LogExportReaderTest, TruncatedAndOmittedFilesStillProduceValidZipEntries) {
  log_export::support_log_collection_t collection {128};
  ASSERT_TRUE(collection.add_file(write("large.log", std::string(200, 'x') + "\nrecent line\n"), "large.log"));
  ASSERT_TRUE(collection.add_file(write("omitted.log", "not exported\n"), "omitted.log"));
  const auto zip = log_export::build_zip_from_entries(collection.entries());
  auto u16 = [&](std::size_t pos) {
    return static_cast<unsigned char>(zip[pos]) | (static_cast<unsigned char>(zip[pos + 1]) << 8);
  };
  auto u32 = [&](std::size_t pos) {
    return static_cast<std::uint32_t>(u16(pos)) | (static_cast<std::uint32_t>(u16(pos + 2)) << 16);
  };
  std::size_t offset = 0;
  for (const auto &entry : collection.entries()) {
    ASSERT_EQ(u32(offset), 0x04034b50u);
    const auto length = u32(offset + 18);
    const auto name_length = u16(offset + 26);
    EXPECT_EQ(zip.substr(offset + 30, name_length), entry.name);
    const auto payload = zip.substr(offset + 30 + name_length, length);
    std::string decoded;
    if (u16(offset + 8) == 8) {
      decoded.resize(entry.data.size());
      z_stream stream {};
      ASSERT_EQ(inflateInit2(&stream, -MAX_WBITS), Z_OK);
      stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(payload.data()));
      stream.avail_in = payload.size();
      stream.next_out = reinterpret_cast<Bytef *>(decoded.data());
      stream.avail_out = decoded.size();
      const auto status = inflate(&stream, Z_FINISH);
      inflateEnd(&stream);
      ASSERT_EQ(status, Z_STREAM_END);
    } else {
      decoded = payload;
    }
    EXPECT_EQ(decoded, entry.data);
    boost::crc_32_type crc;
    crc.process_bytes(decoded.data(), decoded.size());
    EXPECT_EQ(u32(offset + 14), crc.checksum());
    offset += 30 + name_length + length;
  }
  EXPECT_EQ(u32(offset), 0x02014b50u);
  EXPECT_EQ(u32(zip.size() - 22), 0x06054b50u);
  EXPECT_EQ(u16(zip.size() - 12), collection.entries().size());
}

TEST_F(LogExportReaderTest, DuplicateZipNameDoesNotHideUnavailableRequestedPaths) {
  log_export::support_log_collection_t collection {64};
  ASSERT_TRUE(collection.add_file(write("session/launcher.log", "first\n"), "launcher.log"));
  const auto removed = write("removed/launcher.log", "removed\n");
  ASSERT_TRUE(fs::remove(removed));
  fs::create_directories(root / "directory.log");

  EXPECT_FALSE(collection.add_file(root / "missing/launcher.log", "launcher.log"));
  EXPECT_FALSE(collection.add_file(removed, "launcher.log"));
  EXPECT_FALSE(collection.add_file(root / "directory.log", "launcher.log"));
  EXPECT_EQ(collection.bytes_read(), 6u);
  ASSERT_EQ(collection.entries().size(), 1u);
  EXPECT_EQ(collection.entries().front().data, "first\n");
}

TEST_F(LogExportReaderTest, MissingKnownFolderDuplicateStillSelectsFallbackLogs) {
  const auto session_log = write("session/playnite.log", "session\n");
  const auto known_folder = root / "known/Playnite";
  fs::create_directories(known_folder);
  const auto fallback_log = write("csidl/Playnite/extensions.log", "fallback diagnostics\n");
  log_export::support_log_collection_t collection {128};
  ASSERT_TRUE(collection.add_file(session_log, "playnite.log"));
  int known_folder_resolutions = 0;
  int csidl_resolutions = 0;
  const auto resolve_known_folders = [&](auto &&visit_root) {
    ++known_folder_resolutions;
    visit_root(root / "known" / "Playnite");
  };
  const auto resolve_csidl_folders = [&](auto &&visit_root) {
    ++csidl_resolutions;
    visit_root(root / "csidl" / "Playnite");
  };

  // This calls the same discovery policy and add-file path used by the
  // Windows collector. The duplicate ZIP name is unavailable in the Known
  // Folder path, so CSIDL must still contribute its extension log.
  EXPECT_TRUE(log_export::add_playnite_logs_from_known_or_csidl(resolve_known_folders, resolve_csidl_folders, collection));
  EXPECT_EQ(known_folder_resolutions, 1);
  EXPECT_EQ(csidl_resolutions, 1);
  ASSERT_EQ(collection.entries().size(), 2u);
  EXPECT_EQ(collection.entries().back().name, "extensions.log");
  EXPECT_EQ(collection.entries().back().data, "fallback diagnostics\n");
  EXPECT_EQ(collection.bytes_read(), fs::file_size(session_log) + fs::file_size(fallback_log));
}

TEST_F(LogExportReaderTest, KnownFolderLogsSkipCsidlFallback) {
  const auto known_log = write("known/Playnite/launcher.log", "known folder diagnostics\n");
  log_export::support_log_collection_t collection {128};
  int known_folder_resolutions = 0;
  int csidl_resolutions = 0;
  const auto resolve_known_folders = [&](auto &&visit_root) {
    ++known_folder_resolutions;
    visit_root(root / "known" / "Playnite");
  };
  const auto resolve_csidl_folders = [&](auto &&visit_root) {
    ++csidl_resolutions;
    visit_root(root / "csidl" / "Playnite");
  };

  EXPECT_TRUE(log_export::add_playnite_logs_from_known_or_csidl(resolve_known_folders, resolve_csidl_folders, collection));
  EXPECT_EQ(known_folder_resolutions, 1);
  EXPECT_EQ(csidl_resolutions, 0);
  ASSERT_EQ(collection.entries().size(), 1u);
  EXPECT_EQ(collection.entries().front().name, "launcher.log");
  EXPECT_EQ(collection.entries().front().data, "known folder diagnostics\n");
  EXPECT_EQ(collection.bytes_read(), fs::file_size(known_log));
}

TEST_F(LogExportReaderTest, CsidlRootFailureDoesNotSkipTheNextRoot) {
  std::vector<std::string> events;

  struct ThrowingCollection {
    fs::path throwing_parent;
    fs::path available_log;
    std::vector<std::string> &events;

    bool add_file(const fs::path &path, const std::string &) {
      events.push_back("add:" + path.parent_path().parent_path().filename().string() + "/" + path.filename().string());
      if (path.parent_path() == throwing_parent) {
        throw std::runtime_error("injected path failure");
      }
      return path == available_log;
    }
  } collection {root / "broken/Playnite", root / "healthy/Playnite/extensions.log", events};

  const auto resolve_known_folders = [](auto &&) {
  };
  const auto resolve_csidl_folders = [&](auto &&visit_root) {
    events.emplace_back("resolve:broken");
    visit_root(root / "broken" / "Playnite");
    events.emplace_back("resolve:healthy");
    visit_root(root / "healthy" / "Playnite");
  };

  EXPECT_TRUE(log_export::add_playnite_logs_from_known_or_csidl(resolve_known_folders, resolve_csidl_folders, collection));
  EXPECT_EQ(events, (std::vector<std::string> {
                      "resolve:broken",
                      "add:broken/playnite.log",
                      "resolve:healthy",
                      "add:healthy/playnite.log",
                      "add:healthy/extensions.log",
                      "add:healthy/launcher.log",
                    }));
}

TEST_F(LogExportReaderTest, KnownFolderReadsInterleaveWithRootResolution) {
  write("roaming/Playnite/playnite.log", "roaming diagnostics\n");
  write("local/Playnite/extensions.log", "local diagnostics\n");
  std::vector<std::string> events;

  struct RecordingCollection {
    std::vector<std::string> &events;
    log_export::support_log_collection_t inner {128};

    bool add_file(const fs::path &path, const std::string &name) {
      events.push_back("add:" + path.parent_path().parent_path().filename().string() + "/" + path.filename().string());
      return inner.add_file(path, name);
    }
  } collection {events};

  const auto visit_known_folders = [&](auto &&visit_root) {
    events.emplace_back("resolve:roaming");
    visit_root(root / "roaming" / "Playnite");
    events.emplace_back("resolve:local");
    visit_root(root / "local" / "Playnite");
  };
  const auto visit_csidl_folders = [&](auto &&) {
    events.emplace_back("resolve:csidl");
  };

  EXPECT_TRUE(log_export::add_playnite_logs_from_known_or_csidl(visit_known_folders, visit_csidl_folders, collection));
  EXPECT_EQ(events, (std::vector<std::string> {
                      "resolve:roaming",
                      "add:roaming/playnite.log",
                      "add:roaming/extensions.log",
                      "add:roaming/launcher.log",
                      "resolve:local",
                      "add:local/playnite.log",
                      "add:local/extensions.log",
                      "add:local/launcher.log",
                    }));
}

TEST_F(LogExportReaderTest, KnownFolderReadFailureFallsBackToCsidlRoots) {
  std::vector<std::string> events;

  struct ThrowingCollection {
    fs::path throwing_parent;
    fs::path available_log;
    std::vector<std::string> &events;

    bool add_file(const fs::path &path, const std::string &) {
      events.push_back("add:" + path.parent_path().parent_path().filename().string() + "/" + path.filename().string());
      if (path.parent_path() == throwing_parent) {
        throw std::runtime_error("injected Known Folder read failure");
      }
      return path == available_log;
    }
  } collection {root / "known/Playnite", root / "csidl/Playnite/extensions.log", events};

  const auto visit_known_folders = [&](auto &&visit_root) {
    events.emplace_back("resolve:known");
    visit_root(root / "known" / "Playnite");
    events.emplace_back("unexpected:next-known-root");
  };
  const auto visit_csidl_folders = [&](auto &&visit_root) {
    events.emplace_back("resolve:csidl");
    visit_root(root / "csidl" / "Playnite");
  };

  EXPECT_TRUE(log_export::add_playnite_logs_from_known_or_csidl(visit_known_folders, visit_csidl_folders, collection));
  EXPECT_EQ(events, (std::vector<std::string> {
                      "resolve:known",
                      "add:known/playnite.log",
                      "resolve:csidl",
                      "add:csidl/playnite.log",
                      "add:csidl/extensions.log",
                      "add:csidl/launcher.log",
                    }));
}

TEST_F(LogExportReaderTest, EmptyExportUsesArchiveHttpResponsePath) {
  const auto response = std::make_shared<CapturedArchiveResponse>();
  const std::vector<log_export::ZipDataEntry> entries;
  std::vector<std::string> events;
  const auto build_archive = [&](const auto &archive_entries) {
    events.emplace_back("archive");
    return log_export::build_zip_from_entries(archive_entries);
  };
  const auto make_filename = [&] {
    events.emplace_back("filename");
    return std::string("vibeshine_logs-20261021-123456.zip");
  };

  log_export::write_archive_response<std::multimap<std::string, std::string>>(
    response,
    200,
    entries,
    build_archive,
    make_filename
  );

  EXPECT_EQ(events, (std::vector<std::string> {"archive", "filename"}));
  EXPECT_EQ(response->status, 200);
  ASSERT_EQ(response->body.size(), 22u);
  EXPECT_EQ(response->body.substr(0, 4), std::string("PK\x05\x06", 4));
  EXPECT_EQ(response->headers.count("Content-Type"), 1u);
  const auto content_type = response->headers.find("Content-Type");
  const auto content_disposition = response->headers.find("Content-Disposition");
  const auto frame_options = response->headers.find("X-Frame-Options");
  const auto content_security_policy = response->headers.find("Content-Security-Policy");
  ASSERT_NE(content_type, response->headers.end());
  ASSERT_NE(content_disposition, response->headers.end());
  ASSERT_NE(frame_options, response->headers.end());
  ASSERT_NE(content_security_policy, response->headers.end());
  EXPECT_EQ(content_type->second, "application/zip");
  EXPECT_EQ(content_disposition->second, "attachment; filename=\"vibeshine_logs-20261021-123456.zip\"");
  EXPECT_EQ(frame_options->second, "DENY");
  EXPECT_EQ(content_security_policy->second, "frame-ancestors 'none';");
}

TEST_F(LogExportReaderTest, NonEmptyExportPassesEntriesThroughArchiveHttpResponsePath) {
  const auto response = std::make_shared<CapturedArchiveResponse>();
  const std::vector<log_export::ZipDataEntry> entries {{"sample.log", "ok", std::nullopt}};
  std::vector<std::string> events;
  const auto build_archive = [&](const auto &archive_entries) {
    events.emplace_back("archive");
    return log_export::build_zip_from_entries(archive_entries);
  };
  const auto make_filename = [&] {
    events.emplace_back("filename");
    std::tm timestamp {};
    timestamp.tm_year = 126;
    timestamp.tm_mon = 9;
    timestamp.tm_mday = 21;
    timestamp.tm_hour = 12;
    timestamp.tm_min = 34;
    timestamp.tm_sec = 56;
    return log_export::make_support_log_archive_filename(timestamp);
  };

  log_export::write_archive_response<std::multimap<std::string, std::string>>(
    response,
    200,
    entries,
    build_archive,
    make_filename
  );

  EXPECT_EQ(events, (std::vector<std::string> {"archive", "filename"}));
  const auto content_disposition = response->headers.find("Content-Disposition");
  ASSERT_NE(content_disposition, response->headers.end());
  EXPECT_EQ(content_disposition->second, "attachment; filename=\"vibeshine_logs-20261021-123456.zip\"");
  ASSERT_GE(response->body.size(), 30u);
  EXPECT_EQ(response->body.substr(0, 4), std::string("PK\x03\x04", 4));
  const auto name_length = static_cast<unsigned char>(response->body[26]) | (static_cast<unsigned char>(response->body[27]) << 8);
  const auto compressed_size = static_cast<std::uint32_t>(static_cast<unsigned char>(response->body[18])) |
                               (static_cast<std::uint32_t>(static_cast<unsigned char>(response->body[19])) << 8) |
                               (static_cast<std::uint32_t>(static_cast<unsigned char>(response->body[20])) << 16) |
                               (static_cast<std::uint32_t>(static_cast<unsigned char>(response->body[21])) << 24);
  ASSERT_EQ(response->body.substr(30, name_length), "sample.log");
  EXPECT_EQ(response->body.substr(30 + name_length, compressed_size), "ok");
  EXPECT_EQ(response->body.substr(response->body.size() - 22, 4), std::string("PK\x05\x06", 4));
}

TEST_F(LogExportReaderTest, ArchiveFilenameFormatPreservesTimestampFields) {
  std::tm timestamp {};
  timestamp.tm_year = 126;
  timestamp.tm_mon = 9;
  timestamp.tm_mday = 21;
  timestamp.tm_hour = 12;
  timestamp.tm_min = 34;
  timestamp.tm_sec = 56;

  EXPECT_EQ(log_export::make_support_log_archive_filename(timestamp), "vibeshine_logs-20261021-123456.zip");
}

TEST_F(LogExportReaderTest, ArchiveFailureDoesNotSampleFilenameOrWriteResponse) {
  const auto response = std::make_shared<CapturedArchiveResponse>();
  const std::vector<log_export::ZipDataEntry> entries;
  std::vector<std::string> events;
  const auto build_archive = [&](const auto &) -> std::string {
    events.emplace_back("archive");
    throw std::runtime_error("injected archive failure");
  };
  const auto make_filename = [&] {
    events.emplace_back("filename");
    return std::string("should-not-be-used.zip");
  };
  using HeaderMap = std::multimap<std::string, std::string>;

  EXPECT_THROW(
    (log_export::write_archive_response<HeaderMap>(response, 200, entries, build_archive, make_filename)),
    std::runtime_error
  );
  EXPECT_EQ(events, (std::vector<std::string> {"archive"}));
  EXPECT_EQ(response->status, 0);
  EXPECT_TRUE(response->body.empty());
}

TEST_F(LogExportReaderTest, UnreadableDuplicateDoesNotReportPathAvailable) {
  log_export::support_log_collection_t collection {64};
  ASSERT_TRUE(collection.add_file(write("first.log", "first\n"), "same.log"));
  const auto unreadable = write("unreadable.log", "unreadable content\n");
  const auto permissions = fs::status(unreadable).permissions();
  std::error_code ec;
  fs::permissions(unreadable, fs::perms::none, ec);
  if (ec) {
    GTEST_SKIP() << "This filesystem cannot restrict the fixture's read permission";
  }
  ASSERT_TRUE(fs::is_regular_file(unreadable));
  {
    std::ifstream probe(unreadable, std::ios::binary);
    if (probe) {
      fs::permissions(unreadable, permissions, ec);
      GTEST_SKIP() << "This process/filesystem can still open the permission-restricted fixture";
    }
  }
  EXPECT_FALSE(collection.add_file(unreadable, "same.log"));
  EXPECT_EQ(collection.bytes_read(), 6u);
  ASSERT_EQ(collection.entries().size(), 1u);
  EXPECT_EQ(collection.entries().front().data, "first\n");
  fs::permissions(unreadable, permissions, ec);
  EXPECT_FALSE(ec);
}
