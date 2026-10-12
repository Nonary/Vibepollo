/**
 * @file src/update.cpp
 * @brief Definitions for update checking, notification, and command execution.
 */

#include "update.h"

// standard includes
#include <algorithm>
#include <cstdio>
#include <future>
#include <sstream>
#include <thread>
#include <variant>

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
#include "version_compare.h"
#include "update_release.h"
#include <map>
#include <mutex>

using namespace std::literals;

namespace update {
  state_t state;

  static size_t write_to_string(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total = size * nmemb;
    auto *out = static_cast<std::string *>(userp);
    out->append(static_cast<char *>(contents), total);
    return total;
  }

  static bool download_json(const std::string &url, std::string &out_json) {
    out_json.clear();
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
    bool tls_configured = http::configure_curl_tls(curl);
    if (!tls_configured) {
      BOOST_LOG(warning) << "GitHub release check is using libcurl defaults for TLS";
    }
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#endif
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out_json);
    char errbuf[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
      BOOST_LOG(error) << "GitHub API request failed: "sv << curl_easy_strerror(res);
      if (errbuf[0] != '\0') {
        BOOST_LOG(error) << "GitHub API error detail: "sv << errbuf;
      }
      long verify_result = 0;
      if (curl_easy_getinfo(curl, CURLINFO_SSL_VERIFYRESULT, &verify_result) == CURLE_OK && verify_result != 0) {
        BOOST_LOG(error) << "TLS verification result code: "sv << verify_result;
      }
#ifdef CURLINFO_OS_ERRNO
      long os_err = 0;
      if (curl_easy_getinfo(curl, CURLINFO_OS_ERRNO, &os_err) == CURLE_OK && os_err != 0) {
        BOOST_LOG(error) << "OS-level error code: "sv << os_err;
      }
#endif
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

  bool download_github_release_data(const std::string &owner, const std::string &repo, std::string &out_json) {
    // Commit the list only after every page succeeds. A rate-limited partial list
    // must never be used to announce an update or reuse an earlier candidate.
    nlohmann::json releases = nlohmann::json::array();
    for (int page = 1; page <= 10; ++page) {
      std::string data;
      if (!download_json("https://api.github.com/repos/" + owner + "/" + repo + "/releases?per_page=100&page=" + std::to_string(page), data)) return false;
      auto batch = nlohmann::json::parse(data, nullptr, false);
      if (!batch.is_array()) return false;
      for (const auto &release : batch) releases.push_back(release);
      if (batch.size() < 100) {
        out_json = releases.dump();
        return true;
      }
    }
    return false; // Bounded pagination exhausted: retry rather than infer a winner.
  }

  bool download_nightly_metadata(const std::string &tag, std::string &out_json) {
    if (!release_selection::daily_tag(tag)) return false;
    static std::mutex cache_mutex;
    static std::map<std::string, nlohmann::json> metadata_cache;
    std::lock_guard<std::mutex> lock(cache_mutex);
    auto cached = metadata_cache.find(tag);
    if (cached == metadata_cache.end()) {
      std::string data;
      if (!download_json("https://github.com/" SUNSHINE_REPO_OWNER "/" SUNSHINE_REPO_NAME "/releases/download/" + tag + "/nightly-metadata.json", data)) return false;
      auto metadata = nlohmann::json::parse(data, nullptr, false);
      try {
        if (!release_selection::valid_metadata(tag, metadata)) return false;
      } catch (const nlohmann::json::exception &) { return false; }
      cached = metadata_cache.emplace(tag, std::move(metadata)).first;
    }
    out_json = cached->second.dump();
    return true;
  }

  static bool enrich_nightly_releases(nlohmann::json &releases) {
    for (auto &release : releases) {
      try {
        if (!release.is_object() || !release.contains("tag_name") || !release["tag_name"].is_string()) continue;
        const auto tag = release["tag_name"].get<std::string>();
        if (!release_selection::daily_tag(tag) || !release_selection::complete(release, true)) continue;
        std::string metadata;
        if (!download_nightly_metadata(tag, metadata)) continue;
        release["nightly_metadata"] = nlohmann::json::parse(metadata);
      } catch (const nlohmann::json::exception &) { /* malformed release */ }
    }
    return true;
  }

  static void notify_new_version(const std::string &version, bool prerelease) {
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
    if (version.empty() || (prerelease && !config::sunshine.notify_pre_releases)) {
      return;
    }
    // Persist delivered identities: dismissing an OS toast consumes it, including
    // across process restarts and stable/prerelease channel changes.
    std::string url;
    {
      std::lock_guard<std::mutex> lock(state.notification_mutex);
      if (version == state.last_notified_version || std::find(state.notified_versions.begin(), state.notified_versions.end(), version) != state.notified_versions.end()) {
        BOOST_LOG(debug) << "Update notification suppressed: already notified for "sv << version;
        return;
      }
      state.last_notified_version = version;
      state.last_notified_is_prerelease = prerelease;
      state.last_notified_url = prerelease ? state.latest_prerelease.url : state.latest_release.url;
      state.notified_versions.push_back(version);
      url = state.last_notified_url;
    }
    const std::string title = prerelease ? "New update available (Pre-release)" : "New update available (Stable)";
    // The C tray backend has no per-notification context, and Windows routes
    // older toast clicks through its latest global callback. Keep the exact
    // release link in the text; the Web UI provides its clickable counterpart.
    const std::string body = "Version " + version + (url.empty() ? "" : "\n" + url);
    system_tray::tray_notify(title.c_str(), body.c_str());
    // Do not hold the notification lock while acquiring nvhttp's client lock.
    nvhttp::save_state();
#endif
  }

  static void perform_check() {
    auto fg = util::fail_guard([]() {
      state.check_in_progress = false;
    });
    try {
      state.last_check_time = std::chrono::steady_clock::now();
      const bool allow_prerelease_updates =
        config::sunshine.notify_pre_releases;
      // Fetch releases list once and compute latest stable/prerelease
      BOOST_LOG(info) << "Update check: querying GitHub releases from repo "sv
                      << SUNSHINE_REPO_OWNER << '/' << SUNSHINE_REPO_NAME;
      std::string releases_json;
      if (!download_github_release_data(SUNSHINE_REPO_OWNER, SUNSHINE_REPO_NAME, releases_json)) return;
      auto j = nlohmann::json::parse(releases_json);
      if (!j.is_array() || (allow_prerelease_updates && !enrich_nightly_releases(j))) return;
      const auto stable = release_selection::select(j, PROJECT_VERSION, false);
      const auto any = release_selection::select(j, PROJECT_VERSION, config::sunshine.notify_pre_releases);
      auto info = [](const nlohmann::json &rel) {
        release_info_t result {};
        if (rel.is_null()) return result;
        result.version = rel.value("tag_name", "");
        result.url = "https://github.com/" SUNSHINE_REPO_OWNER "/" SUNSHINE_REPO_NAME "/releases/tag/" + result.version;
        auto text = [](const nlohmann::json &object, const char *key) {
          return object.contains(key) && object[key].is_string() ? object[key].get<std::string>() : std::string {};
        };
        result.name = text(rel, "name");
        result.body = text(rel, "body");
        result.published_at = text(rel, "published_at");
        result.is_prerelease = rel.value("prerelease", false);
        for (const auto &asset : rel["assets"]) {
          if (!asset.is_object()) continue;
          asset_info_t item {};
          item.name = text(asset, "name");
          item.download_url = text(asset, "browser_download_url");
          item.size = asset.value("size", 0LL);
          item.content_type = text(asset, "content_type");
          result.assets.push_back(std::move(item));
        }
        return result;
      };
      state.latest_release = info(stable);
      state.latest_prerelease = (!any.is_null() && any.value("prerelease", false)) ? info(any) : release_info_t {};
      state.last_check_time = std::chrono::steady_clock::now();

      if (!any.is_null()) {
        notify_new_version(any.value("tag_name", ""), any.value("prerelease", false));
      }
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "Update check failed: "sv << e.what();
    }
  }

  void trigger_check(bool force) {
    bool expected = false;
    if (!state.check_in_progress.compare_exchange_strong(expected, true)) {
      BOOST_LOG(info) << "Update check trigger skipped: another check is in progress (force="sv << (force ? "true"sv : "false"sv) << ')';
      return;
    }
    auto trigger_guard = util::fail_guard([]() { state.check_in_progress = false; });
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
    trigger_guard.disable();
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
      std::string url;
      { std::lock_guard<std::mutex> lock(state.notification_mutex); url = state.last_notified_url; }
      if (!url.empty()) {
        platf::open_url(url);
      }
    } catch (...) {
      // swallow errors; opening the URL is best-effort
    }
  }
}  // namespace update
