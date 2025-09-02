/**
 * @file src/update.cpp
 * @brief Definitions for update checking, notification, and command execution.
 */

#include "update.h"

// standard includes
#include <cstdio>
#include <future>
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

  // (removed) previous date-based comparison helper

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

      // --- Tag-based (semver-like) comparison ----------------------------
      auto parse_semver = [](const std::string &ver) -> std::tuple<int, int, int> {
        if (ver.empty()) {
          return {0, 0, 0};
        }
        std::string v = ver;
        // strip leading 'v' or 'V'
        if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) {
          v.erase(0, 1);
        }
        // cut off pre-release/build metadata
        auto dash = v.find('-');
        auto plus = v.find('+');
        size_t cut = std::string::npos;
        if (dash != std::string::npos) {
          cut = dash;
        }
        if (plus != std::string::npos) {
          cut = (cut == std::string::npos) ? plus : std::min(cut, plus);
        }
        if (cut != std::string::npos) {
          v = v.substr(0, cut);
        }

        int a = 0, b = 0, c = 0;
        try {
          std::stringstream ss(v);
          std::string part;
          if (std::getline(ss, part, '.')) {
            a = std::stoi(part);
          }
          if (std::getline(ss, part, '.')) {
            b = std::stoi(part);
          }
          if (std::getline(ss, part, '.')) {
            c = std::stoi(part);
          }
        } catch (...) {
          // fall back to zeros
          a = b = c = 0;
        }
        return {a, b, c};
      };

      auto cmp_semver = [&](const std::string &lhs, const std::string &rhs) -> int {
        auto [a0, a1, a2] = parse_semver(lhs);
        auto [b0, b1, b2] = parse_semver(rhs);
        if (a0 != b0) {
          return (a0 < b0) ? -1 : 1;
        }
        if (a1 != b1) {
          return (a1 < b1) ? -1 : 1;
        }
        if (a2 != b2) {
          return (a2 < b2) ? -1 : 1;
        }
        return 0;
      };

      const std::string installed_version_tag = PROJECT_VERSION;
      const std::string latest_stable_tag = state.latest_release.version;
      const std::string latest_pre_tag = state.latest_prerelease.version;

      bool stable_available = !latest_stable_tag.empty() && (cmp_semver(installed_version_tag, latest_stable_tag) < 0);
      bool prerelease_available = config::sunshine.notify_pre_releases &&
                                  !latest_pre_tag.empty() &&
                                  (cmp_semver(installed_version_tag, latest_pre_tag) < 0) &&
                                  (!latest_stable_tag.empty() ? (cmp_semver(latest_stable_tag, latest_pre_tag) < 0) : true);

      if (prerelease_available) {
        BOOST_LOG(info) << "Update check: prerelease available tag="sv << state.latest_prerelease.version
                        << ", installed="sv << installed_version_tag;
        notify_new_version(state.latest_prerelease.version, true);
      } else if (stable_available) {
        BOOST_LOG(info) << "Update check: stable available tag="sv << state.latest_release.version
                        << ", installed="sv << installed_version_tag;
        notify_new_version(state.latest_release.version, false);
      } else {
        BOOST_LOG(info) << "Update check (tag-based): up-to-date. installed="sv << installed_version_tag
                        << ", stable="sv << latest_stable_tag
                        << ", prerelease="sv << latest_pre_tag;
      }
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "Update check failed: "sv << e.what();
    }
  }

  void trigger_check(bool force) {
    const bool in_progress = state.check_in_progress.load();
    if (in_progress) {
      BOOST_LOG(info) << "Update check trigger skipped: another check is in progress (force="sv << (force ? "true"sv : "false"sv) << ')';
      return;
    }
    if (!force && config::sunshine.update_check_interval_seconds == 0) {
      BOOST_LOG(info) << "Update check trigger skipped: checks are disabled by config (interval=0)"sv;
      return;
    }
    if (!force) {
      auto now = std::chrono::steady_clock::now();
      auto since_last = std::chrono::duration_cast<std::chrono::seconds>(now - state.last_check_time);
      if (since_last < std::chrono::seconds(config::sunshine.update_check_interval_seconds)) {
        BOOST_LOG(info) << "Update check trigger throttled: last ran "sv << since_last.count() << "s ago (interval="sv << config::sunshine.update_check_interval_seconds << 's' << ')';
        return;
      }
    }
    BOOST_LOG(info) << "Update check trigger accepted (force="sv << (force ? "true"sv : "false"sv) << ')';
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
