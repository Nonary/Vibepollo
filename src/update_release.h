#pragma once

#include "version_compare.h"
#include <chrono>
#include <nlohmann/json.hpp>
#include <regex>

// Notification policy only: this does not download/install application packages.
namespace release_selection {
  using nlohmann::json;
  inline bool ordered_version(const std::string &version) {
    static const std::regex pattern(R"(^v?[0-9]+\.[0-9]+\.[0-9]+(-[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)*)?(\+[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)*)?$)");
    if (!std::regex_match(version, pattern)) return false;
    auto core = version.substr(version.starts_with('v') ? 1 : 0);
    core = core.substr(0, core.find_first_of("-+"));
    std::stringstream parts(core);
    std::string part;
    while (std::getline(parts, part, '.')) { int number; if (!version_compare::parse_nonnegative_integer(part, number)) return false; }
    return true;
  }
  inline bool daily_tag(const std::string &tag) {
    static const std::regex pattern(R"(^daily-[0-9]{4}-[0-9]{2}-[0-9]{2}(\.([1-9][0-9]{0,2}))?$)");
    static const std::regex semver(R"(^[0-9]+\.[0-9]+\.[0-9]+-daily\.d[0-9]{8}(\.([1-9][0-9]{0,2}))?$)");
    static const std::regex nightly(R"(^[0-9]+\.[0-9]+\.[0-9]+-nightly\.d[0-9]{8}\.(r\.[1-9][0-9]{0,2}\.)?[0-9a-f]{12}$)");
    return std::regex_match(tag, pattern) || std::regex_match(tag, semver) || std::regex_match(tag, nightly);
  }
  inline bool asset(const json &release, const std::string &name) {
    if (!release.contains("assets") || !release["assets"].is_array()) return false;
    for (const auto &item : release["assets"]) {
      if (item.is_object() && item.contains("name") && item["name"].is_string() && item["name"] == name &&
          item.contains("state") && item["state"].is_string() && item["state"] == "uploaded" &&
          item.contains("size") && item["size"].is_number() && item["size"].get<double>() > 0) return true;
    }
    return false;
  }
  inline bool macos_package(const json &release, bool arm64) {
    return asset(release, arm64 ? "Vibepollo-macOS-arm64.dmg" : "Vibepollo-macOS-x86_64.dmg");
  }
  inline bool complete(const json &release, bool nightly) {
    if (!release.is_object() || !release.contains("draft") || !release["draft"].is_boolean() || release["draft"].get<bool>() ||
        !release.contains("prerelease") || !release["prerelease"].is_boolean() ||
        !release.contains("tag_name") || !release["tag_name"].is_string()) return false;
#ifdef _WIN32
    auto version = release["tag_name"].get<std::string>();
    if (version.starts_with('v')) version.erase(0, 1);
    const bool package = asset(release, "VibepolloSetup.exe") || (!nightly &&
      (asset(release, "VibepolloSetup-v" + version + ".exe") || asset(release, "VibepolloSetup-" + version + ".exe")));
#elif defined(__APPLE__)
#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
    const bool package = macos_package(release, true);
#elif defined(__x86_64__) || defined(_M_X64)
    const bool package = macos_package(release, false);
#else
    const bool package = false;
#endif
#else
    bool package = false;
    if (release.contains("assets") && release["assets"].is_array()) {
      for (const auto &item : release["assets"]) {
        if (!item.is_object() || !item.contains("name") || !item["name"].is_string()) continue;
        const auto name = item["name"].get<std::string>();
        if ((name.ends_with(".AppImage") || name.ends_with(".deb") || name.ends_with(".rpm") || name.ends_with(".pkg.tar.zst")) &&
#if defined(__x86_64__)
            (name.find("x86_64") != std::string::npos || name.find("amd64") != std::string::npos) &&
#elif defined(__aarch64__)
            (name.find("aarch64") != std::string::npos || name.find("arm64") != std::string::npos) &&
#else
            false &&
#endif
            asset(release, name)) package = true;
      }
    }
#endif
    return package && (!nightly || (release["prerelease"].get<bool>() && asset(release, "SHA256SUMS")));
  }
  inline bool valid_metadata(const std::string &tag, const json &metadata) {
    if (!daily_tag(tag) || !metadata.is_object()) return false;
    static const std::regex new_nightly(R"(^([0-9]+\.[0-9]+\.[0-9]+)-nightly\.d([0-9]{4})([0-9]{2})([0-9]{2})\.(?:r\.([1-9][0-9]{0,2})\.)?([0-9a-f]{12})$)");
    std::smatch nightly_match;
    if (std::regex_match(tag, nightly_match, new_nightly)) {
      const auto source = metadata.value("source_commit", "");
      static const std::regex sha(R"(^[0-9a-f]{40}$)");
      if (!std::regex_match(source, sha) || !ordered_version(tag) || metadata.value("schema_version", 0) != 1 ||
          metadata.value("channel", "") != "nightly" || metadata.value("tag", "") != tag ||
          metadata.value("version", "") != tag || nightly_match[6].str() != source.substr(0, 12) ||
          !metadata.contains("nightly_sequence") || !metadata["nightly_sequence"].is_number_integer()) return false;
      const auto day = nightly_match[2].str() + nightly_match[3].str() + nightly_match[4].str();
      const auto calendar = std::chrono::year_month_day {
        std::chrono::year {std::stoi(nightly_match[2])},
        std::chrono::month {static_cast<unsigned>(std::stoi(nightly_match[3]))},
        std::chrono::day {static_cast<unsigned>(std::stoi(nightly_match[4]))}};
      if (!calendar.ok() || int(calendar.year()) < 2000 || int(calendar.year()) > 2099) return false;
      const auto sequence = nightly_match[5].matched ? std::stoll(day) * 1000 + std::stoi(nightly_match[5]) : std::stoll(day);
      return metadata["nightly_sequence"].get<long long>() == sequence;
    }
    if (!tag.starts_with("daily-")) {
      static const std::regex semver(R"(^([0-9]+\.[0-9]+\.[0-9]+)-daily\.d([0-9]{4})([0-9]{2})([0-9]{2})(\.([1-9][0-9]{0,2}))?$)");
      std::smatch match;
      if (metadata.value("tag", "") != tag || !std::regex_match(tag, match, semver) ||
          metadata.value("version", "") != tag + "+" + metadata.value("source_commit", "").substr(0,12)) return false;
      const auto legacy = "daily-" + match[2].str() + "-" + match[3].str() + "-" + match[4].str() + match[5].str();
      auto normalized = metadata;
      normalized["tag"] = legacy;
      return valid_metadata(legacy, normalized);
    }
    const std::string date = tag.substr(6,4) + tag.substr(11,2) + tag.substr(14,2);
    const int revision = tag.size() > 16 ? std::stoi(tag.substr(17)) : 1;
    if (revision > 999) return false;
    const long long sequence = tag.size() == 16 ? std::stoll(date) : std::stoll(date) * 1000 + revision;
    static const std::regex sha(R"(^[0-9a-f]{40}$)");
    if (metadata.value("schema_version", 0) != 1 || metadata.value("channel", "") != "nightly" ||
        metadata.value("tag", "") != tag || !metadata.contains("nightly_sequence") || !metadata["nightly_sequence"].is_number_integer() ||
        metadata["nightly_sequence"].get<long long>() != sequence || !metadata.contains("source_commit") || !metadata["source_commit"].is_string()) return false;
    const auto source = metadata["source_commit"].get<std::string>();
    const auto version = metadata.value("version", "");
    if (!std::regex_match(source, sha) || !ordered_version(version)) return false;
    const auto suffix = "-daily.d" + date + (tag.size() == 16 ? "" : "." + std::to_string(revision)) + "+" + source.substr(0,12);
    return version.ends_with(suffix) && version.substr(0, version.size() - suffix.size()).find_first_of("-+") == std::string::npos;
  }
  inline json api_metadata(const json &release) {
    try {
      if (!release.is_object() || !release.contains("body") || !release["body"].is_string()) return nullptr;
      const auto body = release["body"].get<std::string>();
      const std::string marker = "<!-- nightly-identity: ";
      const auto begin = body.find(marker);
      if (begin == std::string::npos || (begin != 0 && body[begin - 1] != '\n') ||
          body.find(marker, begin + marker.size()) != std::string::npos) return nullptr;
      const auto end = body.find(" -->", begin + marker.size());
      if (end == std::string::npos) return nullptr;
      const auto metadata = json::parse(body.substr(begin + marker.size(), end - begin - marker.size()), nullptr, false);
      if (!valid_metadata(release.value("tag_name", ""), metadata)) return nullptr;
      return metadata;
    } catch (const json::exception &) { return nullptr; }
  }
  inline std::string identity(const json &release) {
    const auto tag = release.value("tag_name", "");
    if (!daily_tag(tag)) {
      // Malformed nightly identities must not fall back to an unqualified
      // ordinary prerelease and bypass metadata verification.
      if (tag.find("-nightly.d") != std::string::npos || tag.find("-daily.d") != std::string::npos) return "";
      return ordered_version(tag) ? tag : "";
    }
    const auto metadata = api_metadata(release);
    if (metadata.is_null()) return "";
    return metadata["version"].get<std::string>();
  }
  inline bool nightly_version(const std::string &version) {
    return version.find("-daily.d") != std::string::npos || version.find("-nightly.d") != std::string::npos;
  }
  inline bool same_base(const std::string &left, const std::string &right) {
    const auto a = version_compare::parse_semver(left);
    const auto b = version_compare::parse_semver(right);
    return a.major == b.major && a.minor == b.minor && a.patch == b.patch;
  }
  inline int compare_notification_versions(const std::string &left, const std::string &right) {
    const bool a = nightly_version(left), b = nightly_version(right);
    if (same_base(left, right) && a != b) {
      // A nightly is development toward this numeric base, not a lexically
      // sorted channel between beta and rc. Stable respins remain above it.
      return a ? -1 : 1;
    }
    // Compare calendar day and revision, never the random source SHA. Legacy
    // hash-only nightlies are revision zero; old daily.N forms remain accepted.
    static const std::regex dated_nightly(R"(^[0-9]+\.[0-9]+\.[0-9]+-(?:nightly\.d([0-9]{8})\.(?:r\.([1-9][0-9]{0,2})\.)?[0-9a-f]{12}|daily\.d([0-9]{8})(?:\.([1-9][0-9]{0,2}))?)(?:\+[0-9A-Za-z.-]+)?$)");
    std::smatch lm, rm;
    if (same_base(left, right) && std::regex_match(left, lm, dated_nightly) &&
        std::regex_match(right, rm, dated_nightly)) {
      const auto key = [](const std::smatch &match) {
        const bool nightly = match[1].matched;
        return std::pair {std::stoll(match[nightly ? 1 : 3]),
                          match[nightly ? 2 : 4].matched ? std::stoi(match[nightly ? 2 : 4]) : 0};
      };
      const auto l = key(lm), r = key(rm);
      // On the same day the SHA-tagged nightly family supersedes historical
      // daily revisions, independent of their old channel spelling.
      if (l.first == r.first && lm[1].matched != rm[1].matched) return lm[1].matched ? 1 : -1;
      return l == r ? 0 : l < r ? -1 : 1;
    }
    return version_compare::compare_semver(left, right);
  }
  inline json select(const json &releases, const std::string &installed, bool allow_prereleases) {
    if (!releases.is_array() || !ordered_version(installed)) return nullptr;
    json best = nullptr;
    std::string best_version = installed;
    for (const auto &release : releases) {
      try {
        if (!release.is_object() || !release.contains("tag_name") || !release["tag_name"].is_string()) continue;
        const auto tag = release["tag_name"].get<std::string>();
        if (!complete(release, daily_tag(tag)) || (release["prerelease"].get<bool>() && !allow_prereleases)) continue;
        const auto version = identity(release);
        // A manual prerelease on the same numeric base cannot prove it is newer
        // than an installed nightly without source lineage metadata. Fail closed.
        if (nightly_version(installed) && !nightly_version(version) && release["prerelease"].get<bool>() && same_base(installed, version)) continue;
        if (!version.empty() && compare_notification_versions(best_version, version) < 0) { best = release; best_version = version; }
      } catch (const json::exception &) { /* Ignore malformed entries independently. */ }
    }
    return best;
  }
}
