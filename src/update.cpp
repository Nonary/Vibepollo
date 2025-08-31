/**
 * @file src/update.cpp
 * @brief Definitions for update checking, notification, and command execution.
 */

#include "update.h"

// standard includes
#include <cstdio>
#include <ctime>
#include <future>
#include <regex>
#include <sstream>
#include <thread>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "config.h"
#include "httpcommon.h"
#include "logging.h"
#include "nvhttp.h"  // for save_state persistence hooks
#include "platform/common.h"
#include "process.h"
#include "rtsp.h"  // for session_count
#include "system_tray.h"
#include "utility.h"

#include <boost/process/v1/environment.hpp>

using namespace std::literals;

namespace update {
  state_t state;

  static size_t write_to_string(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total = size * nmemb;
    auto *out = static_cast<std::string *>(userp);
    out->append(static_cast<char *>(contents), total);
    return total;
  }

  bool download_github_release_data(const std::string &owner, const std::string &repo, std::string &out_json) {
    std::string url = "https://api.github.com/repos/" + owner + "/" + repo + "/releases";
    CURL *curl = curl_easy_init();
    if (!curl) {
      BOOST_LOG(error) << "CURL init failed for GitHub API"sv;
      return false;
    }
    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
    headers = curl_slist_append(headers, "X-GitHub-Api-Version: 2022-11-28");
    headers = curl_slist_append(headers, "User-Agent: Sunshine-Updater/1.0");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out_json);
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
      BOOST_LOG(error) << "GitHub API request failed: "sv << curl_easy_strerror(res);
      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);
      return false;
    }
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (code < 200 || code >= 300) {
      BOOST_LOG(error) << "GitHub API returned HTTP "sv << code;
      return false;
    }
    return true;
  }

  // Parse a limited ISO 8601 timestamp (YYYY-MM-DDThh:mm:ssZ) into a system_clock::time_point
  static std::chrono::system_clock::time_point parse_iso8601_utc(const std::string &s) {
    // Expect Zulu time from GitHub (e.g., 2024-08-20T21:30:00Z)
    if (s.size() < 20) {
      return std::chrono::system_clock::time_point {};
    }
    std::tm tm {};
    try {
      tm.tm_year = std::stoi(s.substr(0, 4)) - 1900;
      tm.tm_mon = std::stoi(s.substr(5, 2)) - 1;
      tm.tm_mday = std::stoi(s.substr(8, 2));
      tm.tm_hour = std::stoi(s.substr(11, 2));
      tm.tm_min = std::stoi(s.substr(14, 2));
      tm.tm_sec = std::stoi(s.substr(17, 2));
    } catch (...) {
      return std::chrono::system_clock::time_point {};
    }
#if defined(_WIN32)
    time_t t = _mkgmtime(&tm);
#else
    time_t t = timegm(&tm);
#endif
    if (t == (time_t) -1) {
      return std::chrono::system_clock::time_point {};
    }
    return std::chrono::system_clock::from_time_t(t);
  }

  static void notify_new_version(const std::string &version, bool prerelease) {
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
    if (version.empty()) {
      return;
    }
    std::string title = prerelease ? "New update available (Pre-release)" : "New update available (Stable)";
    std::string body = "Version " + version;
    state.last_notified_version = version;
    state.last_notified_is_prerelease = prerelease;
    state.last_notified_url = prerelease ? state.latest_prerelease.url : state.latest_release.url;
    // On click, open the release page directly
    system_tray::tray_notify(title.c_str(), body.c_str(), []() {
      open_last_notified_release_page();
    });
#endif
    // We intentionally allow repeated notifications; do not persist last_notified_version
  }

  static void perform_check() {
    state.check_in_progress = true;
    auto fg = util::fail_guard([]() {
      state.check_in_progress = false;
    });
    try {
      // Fetch releases list once and compute latest stable/prerelease
      BOOST_LOG(info) << "Update check: querying GitHub releases from repo "sv
                      << SUNSHINE_REPO_OWNER << '/' << SUNSHINE_REPO_NAME;
      std::string releases_json;
      if (download_github_release_data(SUNSHINE_REPO_OWNER, SUNSHINE_REPO_NAME, releases_json)) {
        auto j = nlohmann::json::parse(releases_json);
        // Reset release info
        state.latest_release = release_info_t {};
        state.latest_prerelease = release_info_t {};

        for (auto &rel : j) {
          bool is_prerelease = rel.value("prerelease", false);
          bool is_draft = rel.value("draft", false);

          // Parse assets for this release
          std::vector<asset_info_t> assets;
          if (rel.contains("assets") && rel["assets"].is_array()) {
            for (auto &asset : rel["assets"]) {
              asset_info_t asset_info;
              asset_info.name = asset.value("name", "");
              asset_info.download_url = asset.value("browser_download_url", "");
              asset_info.size = asset.value("size", 0);
              asset_info.content_type = asset.value("content_type", "");

              // Extract SHA256 from digest field (format: "sha256:hash")
              std::string digest = asset.value("digest", "");
              if (digest.starts_with("sha256:")) {
                asset_info.sha256 = digest.substr(7);
              }

              if (!asset_info.name.empty() && !asset_info.download_url.empty()) {
                assets.push_back(asset_info);
              }
            }
          }

          if (!is_draft && !is_prerelease && state.latest_release.version.empty()) {
            state.latest_release.version = rel.value("tag_name", "");
            state.latest_release.url = rel.value("html_url", "");
            state.latest_release.name = rel.value("name", "");
            state.latest_release.body = rel.value("body", "");
            state.latest_release.published_at = rel.value("published_at", "");
            state.latest_release.is_prerelease = false;
            state.latest_release.assets = assets;
            BOOST_LOG(info) << "Update check: latest stable tag="sv << state.latest_release.version;
          }
          if (config::sunshine.notify_pre_releases && is_prerelease && state.latest_prerelease.version.empty()) {
            state.latest_prerelease.version = rel.value("tag_name", "");
            state.latest_prerelease.url = rel.value("html_url", "");
            state.latest_prerelease.name = rel.value("name", "");
            state.latest_prerelease.body = rel.value("body", "");
            state.latest_prerelease.published_at = rel.value("published_at", "");
            state.latest_prerelease.is_prerelease = true;
            state.latest_prerelease.assets = assets;
            BOOST_LOG(info) << "Update check: latest prerelease tag="sv << state.latest_prerelease.version;
          }
          if (!state.latest_release.version.empty() && (!config::sunshine.notify_pre_releases || !state.latest_prerelease.version.empty())) {
            break;  // got what we need
          }
        }
      }
      state.last_check_time = std::chrono::steady_clock::now();
      // Compare with current build by date (ISO 8601), not version
      const std::string installed_date_str = PROJECT_RELEASE_DATE;
      auto installed_tp = parse_iso8601_utc(installed_date_str);
      auto stable_tp = parse_iso8601_utc(state.latest_release.published_at);
      auto pre_tp = parse_iso8601_utc(state.latest_prerelease.published_at);

      bool stable_available = (stable_tp > installed_tp);
      bool prerelease_available = config::sunshine.notify_pre_releases && (pre_tp > installed_tp) && (pre_tp > stable_tp);

      if (prerelease_available && !state.latest_prerelease.version.empty()) {
        notify_new_version(state.latest_prerelease.version, true);
      } else if (stable_available && !state.latest_release.version.empty()) {
        notify_new_version(state.latest_release.version, false);
      } else {
        BOOST_LOG(info) << "Update check (date-based): up-to-date. installed="sv << installed_date_str
                        << ", stable="sv << state.latest_release.published_at
                        << ", prerelease="sv << state.latest_prerelease.published_at;
      }
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "Update check failed: "sv << e.what();
    }
  }

  void trigger_check(bool force) {
    if (state.check_in_progress.load()) {
      return;
    }
    if (!force && config::sunshine.update_check_interval_seconds == 0) {
      return;
    }
    if (!force) {
      auto now = std::chrono::steady_clock::now();
      if (now - state.last_check_time < std::chrono::seconds(config::sunshine.update_check_interval_seconds)) {
        return;
      }
    }
    std::thread([]() {
      perform_check();
    }).detach();
  }

  // update command removed

  void on_stream_started() {
    // Kick a metadata refresh after a small delay but do not auto-execute updates while streaming.
    std::thread([]() {
      std::this_thread::sleep_for(3s);
      trigger_check(true);
    }).detach();
  }

  void periodic() {
    // Only trigger checks if no streaming sessions are active
    if (rtsp_stream::session_count() == 0) {
      trigger_check(false);
    }
  }

  void open_last_notified_release_page() {
    try {
      const std::string &url = state.last_notified_url;
      if (!url.empty()) {
        platf::open_url(url);
      }
    } catch (...) {
      // swallow errors; opening the URL is best-effort
    }
  }
}  // namespace update
