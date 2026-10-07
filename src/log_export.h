/**
 * @file src/log_export.h
 * @brief Shared support-log sanitization and ZIP encoding.
 */
#pragma once

#include <algorithm>
#include <array>
#include <boost/asio/ip/address_v6.hpp>
#include <boost/crc.hpp>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <limits>
#include <optional>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>
#include <zlib.h>

namespace log_export {
  static inline void write_le16(std::string &out, uint16_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
  }

  static inline void write_le32(std::string &out, uint32_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 24) & 0xFF));
  }

  static inline void current_dos_datetime(uint16_t &dos_time, uint16_t &dos_date) {
    std::time_t tt = std::time(nullptr);
    std::tm tm {};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    dos_time = static_cast<uint16_t>(((tm.tm_hour & 0x1F) << 11) | ((tm.tm_min & 0x3F) << 5) | ((tm.tm_sec / 2) & 0x1F));
    int year = tm.tm_year + 1900;
    if (year < 1980) {
      year = 1980;
    }
    if (year > 2107) {
      year = 2107;
    }
    dos_date = static_cast<uint16_t>(((year - 1980) << 9) | (((tm.tm_mon + 1) & 0x0F) << 5) | (tm.tm_mday & 0x1F));
  }

  static bool deflate_buffer(const char *data, std::size_t size, std::string &out) {
    z_stream zs {};
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
      deflateEnd(&zs);
      return false;
    }
    std::array<unsigned char, 16384> buf {};
    out.clear();
    std::size_t offset = 0;
    int ret = Z_OK;
    do {
      if (zs.avail_in == 0 && offset < size) {
        std::size_t chunk = std::min<std::size_t>(size - offset, static_cast<std::size_t>(std::numeric_limits<uInt>::max()));
        zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data + offset));
        zs.avail_in = static_cast<uInt>(chunk);
        offset += chunk;
      }
      zs.next_out = buf.data();
      zs.avail_out = static_cast<uInt>(buf.size());
      int flush = (offset >= size && zs.avail_in == 0) ? Z_FINISH : Z_NO_FLUSH;
      ret = deflate(&zs, flush);
      if (ret == Z_STREAM_ERROR) {
        deflateEnd(&zs);
        return false;
      }
      std::size_t produced = buf.size() - zs.avail_out;
      if (produced > 0) {
        out.append(reinterpret_cast<char *>(buf.data()), produced);
      }
    } while (ret != Z_STREAM_END);
    deflateEnd(&zs);
    return true;
  }

  static std::chrono::system_clock::time_point file_time_to_system_clock(std::filesystem::file_time_type ft);
  static void to_dos_datetime(std::chrono::system_clock::time_point tp, uint16_t &dos_time, uint16_t &dos_date);

  struct ZipDataEntry {
    std::string name;
    std::string data;
    std::optional<std::filesystem::file_time_type> write_time;
  };

  class export_log_sanitizer_t {
  public:
    std::string sanitize(const std::string &input) {
      struct replacement_t {
        std::size_t begin;
        std::size_t end;
        int priority;
        std::string value;
      };

      std::vector<replacement_t> replacements;
      const auto add_matches = [&](const std::regex &pattern, std::size_t capture, int priority, const std::string &kind, bool validate_ipv6 = false, bool preserve_service_identity = false) {
        std::match_results<std::string::const_iterator> match;
        auto search_start = input.cbegin();
        while (std::regex_search(search_start, input.cend(), match, pattern)) {
          if (capture >= match.size() || !match[capture].matched) {
            break;
          }
          const std::string value = match.str(capture);
          if (validate_ipv6 && !is_ipv6(value)) {
            search_start += match.position(capture) + std::max<std::ptrdiff_t>(match.length(capture), 1);
            continue;
          }
          if (!(preserve_service_identity && kind == "USER" && is_service_identity(value))) {
            const auto offset = static_cast<std::size_t>(std::distance(input.cbegin(), search_start));
            replacements.push_back(replacement_t {
              offset + static_cast<std::size_t>(match.position(capture)),
              offset + static_cast<std::size_t>(match.position(capture) + match.length(capture)),
              priority,
              placeholder(kind, value),
            });
          }
          // Resume after the captured value, not the whole match. Boundary-based
          // patterns consume punctuation, which must remain available to find the
          // following address in a comma- or space-separated list.
          search_start += match.position(capture) + std::max<std::ptrdiff_t>(match.length(capture), 1);
        }
      };

      // Compiled once per process: sanitize() runs over every exported file and
      // regex construction is expensive enough to dominate small files.
      // Match only user components of conventional home-directory paths.
      static const std::regex user_home_windows {R"((?:[A-Za-z]:[\\/]+Users[\\/]+)([^\\/,:;"'<>()[\]{}|]+))", std::regex_constants::icase};
      static const std::regex user_home_posix {R"((?:^|[^A-Za-z0-9_])/(?:home|Users)/([^/\s:,;"'<>()[\]{}|]+))", std::regex_constants::icase};
      // Explicit fields are safer to redact than arbitrary words that happen to look like names.
      static const std::regex user_field_quoted {R"(\b(?:username|user_name|user)\b\s*[:=]\s*["']([^"']+)["'])", std::regex_constants::icase};
      static const std::regex user_field_bare {R"(\b(?:username|user_name|user)\b\s*[:=]\s*([A-Za-z0-9._-]+))", std::regex_constants::icase};
      static const std::regex ipv4_pattern {R"((?:^|[^0-9A-Za-z])((?:25[0-5]|2[0-4][0-9]|1[0-9]{2}|[1-9]?[0-9])(?:\.(?:25[0-5]|2[0-4][0-9]|1[0-9]{2}|[1-9]?[0-9])){3})(?:[^0-9A-Za-z]|$))"};
      static const std::regex ipv6_candidate {R"((?:^|[^0-9A-Za-z:.])([0-9A-Fa-f:.]{2,})(?:[^0-9A-Za-z:.]|$))"};
      static const std::regex mac_pattern {R"(\b([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5}|[0-9A-Fa-f]{2}(?:-[0-9A-Fa-f]{2}){5})\b)"};
      static const std::regex email_pattern {R"(\b[A-Za-z0-9.!#$%&'*+/=?^_`{|}~-]+@[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?(?:\.[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)+\b)"};

      add_matches(user_home_windows, 1, 10, "USER");
      add_matches(user_home_posix, 1, 10, "USER");
      add_matches(user_field_quoted, 1, 0, "USER", false, true);
      add_matches(user_field_bare, 1, 0, "USER", false, true);
      add_matches(ipv4_pattern, 1, 20, "IP");
      add_matches(ipv6_candidate, 1, 20, "IP", true);
      add_matches(mac_pattern, 1, 30, "MAC");
      add_matches(email_pattern, 0, 40, "EMAIL");

      std::sort(replacements.begin(), replacements.end(), [](const auto &a, const auto &b) {
        if (a.begin != b.begin) {
          return a.begin < b.begin;
        }
        if (a.end != b.end) {
          return a.end > b.end;
        }
        return a.priority < b.priority;
      });

      std::string output;
      output.reserve(input.size());
      std::size_t cursor = 0;
      for (const auto &replacement : replacements) {
        if (replacement.begin < cursor || replacement.end > input.size()) {
          continue;
        }
        output.append(input, cursor, replacement.begin - cursor);
        output.append(replacement.value);
        cursor = replacement.end;
      }
      output.append(input, cursor, std::string::npos);
      return output;
    }

  private:
    static bool is_service_identity(const std::string &value) {
      std::string normalized;
      normalized.reserve(value.size());
      for (const unsigned char ch : value) {
        if (std::isspace(ch)) {
          continue;
        }
        normalized.push_back(static_cast<char>(std::tolower(ch)));
      }
      return normalized == "system" || normalized == "localsystem" || normalized == "root" || normalized == "daemon" || normalized == "unknown" ||
             normalized == "nobody" || normalized == "networkservice" || normalized == "localservice" || normalized == "www-data";
    }

    static bool is_ipv6(const std::string &value) {
      // The candidate pattern matches every timestamp and hex token in a log
      // file; reject anything that cannot be IPv6 text (needs "::" or the 6-7
      // colons of an uncompressed address) before paying for a Winsock parse.
      const auto colons = std::count(value.begin(), value.end(), ':');
      if (colons < 6 && value.find("::") == std::string::npos) {
        return false;
      }
      boost::system::error_code ec;
      boost::asio::ip::make_address_v6(value, ec);
      return !ec;
    }

    static std::string key_for(const std::string &kind, const std::string &value) {
      std::string key = kind + ":" + value;
      if (kind == "IP" || kind == "MAC" || kind == "EMAIL") {
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) {
          return static_cast<char>(std::tolower(ch));
        });
      }
      return key;
    }

    std::string placeholder(const std::string &kind, const std::string &value) {
      const std::string key = key_for(kind, value);
      const auto it = placeholders.find(key);
      if (it != placeholders.end()) {
        return it->second;
      }
      const std::string result = "<" + kind + "_" + std::to_string(next_id++) + ">";
      placeholders.emplace(key, result);
      return result;
    }

    std::unordered_map<std::string, std::string> placeholders;
    std::size_t next_id = 1;
  };

  [[maybe_unused]] static ZipDataEntry make_export_log_entry(export_log_sanitizer_t &sanitizer, std::string name, std::string data, std::optional<std::filesystem::file_time_type> write_time) {
    return ZipDataEntry {std::move(name), sanitizer.sanitize(data), write_time};
  }

  [[maybe_unused]] static std::string build_zip_from_entries(const std::vector<ZipDataEntry> &entries) {
    std::string out;

    struct CdEnt {
      std::string name;
      uint32_t crc;
      uint32_t comp_size;
      uint32_t uncomp_size;
      uint16_t method;
      uint32_t offset;
      uint16_t dostime;
      uint16_t dosdate;
    };

    std::vector<CdEnt> cd;
    for (const auto &e : entries) {
      const std::string &name = e.name;
      const std::string &data = e.data;
      boost::crc_32_type crc;
      crc.process_bytes(data.data(), data.size());
      uint32_t crc32 = crc.checksum();
      uint32_t uncomp_size = static_cast<uint32_t>(data.size());
      std::string compressed;
      bool use_deflate = deflate_buffer(data.data(), data.size(), compressed) && compressed.size() < data.size();
      const std::string &payload = use_deflate ? compressed : data;
      uint16_t method = use_deflate ? 8 : 0;
      uint32_t comp_size = static_cast<uint32_t>(payload.size());
      uint16_t dostime = 0, dosdate = 0;
      if (e.write_time) {
        to_dos_datetime(file_time_to_system_clock(*e.write_time), dostime, dosdate);
      } else {
        current_dos_datetime(dostime, dosdate);
      }
      uint32_t offset = static_cast<uint32_t>(out.size());
      write_le32(out, 0x04034b50u);
      write_le16(out, 20);
      write_le16(out, 0);
      write_le16(out, method);
      write_le16(out, dostime);
      write_le16(out, dosdate);
      write_le32(out, crc32);
      write_le32(out, comp_size);
      write_le32(out, uncomp_size);
      write_le16(out, static_cast<uint16_t>(name.size()));
      write_le16(out, 0);
      out.append(name.data(), name.size());
      out.append(payload.data(), payload.size());
      cd.push_back(CdEnt {name, crc32, comp_size, uncomp_size, method, offset, dostime, dosdate});
    }

    uint32_t cd_start = static_cast<uint32_t>(out.size());
    uint32_t cd_size = 0;
    for (const auto &e : cd) {
      std::string rec;
      write_le32(rec, 0x02014b50u);
      write_le16(rec, 20);
      write_le16(rec, 20);
      write_le16(rec, 0);
      write_le16(rec, e.method);
      write_le16(rec, e.dostime);
      write_le16(rec, e.dosdate);
      write_le32(rec, e.crc);
      write_le32(rec, e.comp_size);
      write_le32(rec, e.uncomp_size);
      write_le16(rec, static_cast<uint16_t>(e.name.size()));
      write_le16(rec, 0);
      write_le16(rec, 0);
      write_le16(rec, 0);
      write_le16(rec, 0);
      write_le32(rec, 0);
      write_le32(rec, e.offset);
      rec.append(e.name.data(), e.name.size());
      cd_size += static_cast<uint32_t>(rec.size());
      out.append(rec);
    }

    write_le32(out, 0x06054b50u);
    write_le16(out, 0);
    write_le16(out, 0);
    write_le16(out, static_cast<uint16_t>(cd.size()));
    write_le16(out, static_cast<uint16_t>(cd.size()));
    write_le32(out, cd_size);
    write_le32(out, cd_start);
    write_le16(out, 0);
    return out;
  }

  static std::chrono::system_clock::time_point file_time_to_system_clock(std::filesystem::file_time_type ft) {
    return std::chrono::time_point_cast<std::chrono::system_clock::duration>(ft - decltype(ft)::clock::now() + std::chrono::system_clock::now());
  }

  static void to_dos_datetime(std::chrono::system_clock::time_point tp, uint16_t &dos_time, uint16_t &dos_date) {
    std::time_t tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tm {};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    dos_time = static_cast<uint16_t>(((tm.tm_hour & 0x1F) << 11) | ((tm.tm_min & 0x3F) << 5) | ((tm.tm_sec / 2) & 0x1F));
    int year = tm.tm_year + 1900;
    if (year < 1980) {
      year = 1980;
    }
    if (year > 2107) {
      year = 2107;
    }
    dos_date = static_cast<uint16_t>(((year - 1980) << 9) | (((tm.tm_mon + 1) & 0x0F) << 5) | (tm.tm_mday & 0x1F));
  }

}  // namespace log_export
