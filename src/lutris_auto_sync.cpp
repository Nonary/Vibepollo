#include "lutris_auto_sync.h"

#include "config.h"
#include "confighttp.h"
#include "file_handler.h"
#include "logging.h"
#include "lutris_integration.h"
#include "lutris_sync_policy.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>

namespace platf::lutris::autosync {
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
        if (worker_.joinable()) return;
        stopping_ = false;
        worker_ = std::thread {[this] { run(); }};
      }

      void stop() {
        {
          std::lock_guard lock {mutex_};
          stopping_ = true;
        }
        condition_.notify_all();
        if (worker_.joinable()) worker_.join();
      }

    private:
      void run() {
        std::uint64_t previous_fingerprint = 0;
        std::uint64_t observed_epoch = 0;
        bool have_fingerprint = false;
        while (true) {
          settings_t settings;
          std::uint64_t epoch = 0;
          {
            std::lock_guard lock {mutex_};
            if (stopping_) return;
            settings = settings_;
            epoch = epoch_;
          }
          if (epoch != observed_epoch) {
            observed_epoch = epoch;
            have_fingerprint = false;
          }
          if (settings.enabled && settings.auto_sync) {
            try {
              const auto games = discover();
              const auto fingerprint = source_fingerprint(games);
              if (!have_fingerprint || fingerprint != previous_fingerprint) {
                std::lock_guard apps_lock {confighttp::apps_file_mutex()};
                const auto contents = file_handler::read_file(config::stream.file_apps.c_str());
                if (!contents.empty()) {
                  auto root = nlohmann::json::parse(contents);
                  const auto changed = sync::policy::reconcile(
                    root, games, settings.remove_uninstalled, settings.exclusions, settings.include_steam);
                  if (changed && !confighttp::refresh_client_apps_cache(root)) {
                    BOOST_LOG(warning) << "Lutris auto-sync could not save applications";
                  } else {
                    previous_fingerprint = fingerprint;
                    have_fingerprint = true;
                  }
                }
              }
            } catch (const std::exception &error) {
              BOOST_LOG(warning) << "Lutris auto-sync failed: " << error.what();
            } catch (...) {
              BOOST_LOG(warning) << "Lutris auto-sync failed with an unknown error";
            }
          }

          std::unique_lock lock {mutex_};
          const auto wake_epoch = epoch_;
          condition_.wait_for(lock, poll_interval, [this, wake_epoch] {
            return stopping_ || epoch_ != wake_epoch;
          });
          if (stopping_) return;
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
      static manager_t value;
      return value;
    }
  }  // namespace

  void configure(settings_t settings) { manager().configure(std::move(settings)); }
  void start() { manager().start(); }
  void stop() { manager().stop(); }
}  // namespace platf::lutris::autosync
