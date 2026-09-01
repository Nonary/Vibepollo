/** @file src/steam_auto_sync.cpp */
#include "steam_auto_sync.h"

#include "confighttp.h"
#include "file_handler.h"
#include "logging.h"
#include "platform/common.h"
#include "steam_artwork.h"
#include "steam_auto_sync_policy.h"
#include "steam_integration.h"
#include "steam_sync_policy.h"

#include <chrono>
#include <nlohmann/json.hpp>

namespace platf::steam::autosync {
  namespace {
    constexpr auto poll_interval = std::chrono::seconds {30};

    class manager_t {
    public:
      void configure(settings_t settings) {
        {
          std::lock_guard lock {mutex_};
          settings_ = std::move(settings);
          ++epoch_;
        }
        condition_.notify_all();
      }

      void start() {
        std::lock_guard lock {mutex_};
        if (worker_.joinable()) {
          return;
        }
        stopping_ = false;
        worker_ = std::thread {[this] {
          run();
        }};
      }

      void stop() {
        {
          std::lock_guard lock {mutex_};
          stopping_ = true;
        }
        condition_.notify_all();
        if (worker_.joinable()) {
          worker_.join();
        }
      }

    private:
      void run() {
        std::uint64_t previous_fingerprint = 0;
        std::uint64_t observed_epoch = 0;
        bool have_fingerprint = false;
        while (true) {
          settings_t settings;
          std::uint64_t epoch;
          {
            std::lock_guard lock {mutex_};
            if (stopping_) {
              return;
            }
            settings = settings_;
            epoch = epoch_;
          }

          if (epoch != observed_epoch) {
            observed_epoch = epoch;
            // A provider-policy change (exclusions/include-tools/removal)
            // must reconcile even when Steam's files did not change.
            have_fingerprint = false;
          }

          if (settings.enabled && settings.auto_sync) {
            try {
              if (platf::steam::available()) {
                auto catalog = platf::steam::discover_catalog();
                const auto fingerprint = source_fingerprint(catalog);
                if (!have_fingerprint || fingerprint != previous_fingerprint) {
                  auto games = platf::steam::sync::policy::select_games(
                    catalog,
                    settings.sync_all_installed,
                    settings.recent_games,
                    settings.recent_max_age_days
                  );
                  platf::steam::artwork::prepare(games, platf::appdata());
                  std::lock_guard apps_lock {confighttp::apps_file_mutex()};
                  const auto text = file_handler::read_file(config::stream.file_apps.c_str());
                  if (!text.empty()) {
                    auto root = nlohmann::json::parse(text);
                    const auto changed = platf::steam::sync::policy::reconcile(
                      root,
                      games,
                      !settings.sync_all_installed || settings.remove_uninstalled,
                      settings.exclusions,
                      settings.include_tools,
                      settings.sync_all_installed ? "installed" : "recent"
                    );
                    if (changed && !confighttp::refresh_client_apps_cache(root)) {
                      BOOST_LOG(warning) << "Steam auto-sync could not save applications";
                    } else {
                      previous_fingerprint = fingerprint;
                      have_fingerprint = true;
                    }
                  }
                }
              }
            } catch (const std::exception &error) {
              BOOST_LOG(warning) << "Steam auto-sync failed: " << error.what();
            } catch (...) {
              BOOST_LOG(warning) << "Steam auto-sync failed with an unknown error";
            }
          }

          std::unique_lock lock {mutex_};
          const auto wake_epoch = epoch_;
          condition_.wait_for(lock, poll_interval, [this, wake_epoch] {
            return stopping_ || epoch_ != wake_epoch;
          });
          if (stopping_) {
            return;
          }
        }
      }

      std::mutex mutex_;
      std::condition_variable condition_;
      settings_t settings_;
      std::uint64_t epoch_ = 0;
      bool stopping_ = false;
      std::thread worker_;
    };

    manager_t &manager() {
      static manager_t instance;
      return instance;
    }
  }  // namespace

  void configure(settings_t settings) {
    manager().configure(std::move(settings));
  }

  void start() {
    manager().start();
  }

  void stop() {
    manager().stop();
  }
}  // namespace platf::steam::autosync
