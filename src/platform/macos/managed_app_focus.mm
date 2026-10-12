#include "src/managed_app_focus.h"

#include "src/steam_process_tracker.h"

#import <AppKit/AppKit.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace managed_app_focus {
  namespace {
    class macos_session final: public session {
    public:
      macos_session(target app, policy options):
          worker_([this, app = std::move(app), options] {
            budget remaining(options, budget::clock::now());
            while (!cancelled_ && remaining.active(budget::clock::now())) {
              @autoreleasepool {
                for (NSRunningApplication *candidate in NSWorkspace.sharedWorkspace.runningApplications) {
                  const char *path = candidate.executableURL.fileSystemRepresentation;
                  if (!path || !platf::steam::lifecycle::path_is_within(path, app.install_dir)) {
                    continue;
                  }
                  if ([candidate activateWithOptions:NSApplicationActivateIgnoringOtherApps] && candidate.active) {
                    remaining.confirmed();
                    break;
                  }
                }
              }
              std::unique_lock lock(mutex_);
              wake_.wait_for(lock, std::chrono::seconds(1), [this] {
                return cancelled_.load();
              });
            }
          }) {}

      ~macos_session() override {
        cancelled_ = true;
        wake_.notify_all();
        worker_.join();
      }

    private:
      std::atomic_bool cancelled_ {false};
      std::mutex mutex_;
      std::condition_variable wake_;
      std::thread worker_;
    };
  }  // namespace

  std::unique_ptr<session> start(const target &app, policy options) {
    if (!options.enabled() || app.install_dir.empty()) {
      return {};
    }
    return std::make_unique<macos_session>(app, options);
  }
}  // namespace managed_app_focus
