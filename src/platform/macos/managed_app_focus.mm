#include "src/managed_app_focus.h"

#include "src/steam_process_tracker.h"

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
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
                // Read window IDs/PIDs and geometry only; no window titles,
                // images, Accessibility APIs, or permission requests are used.
                CFArrayRef copied = CGWindowListCopyWindowInfo(kCGWindowListOptionAll | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
                NSArray *windows = (__bridge NSArray *) copied;
                for (NSRunningApplication *candidate in NSWorkspace.sharedWorkspace.runningApplications) {
                  const char *path = candidate.executableURL.fileSystemRepresentation;
                  if (!path || !platf::steam::lifecycle::path_is_within(path, app.install_dir)) {
                    continue;
                  }
                  bool has_window = false;
                  for (NSDictionary *window in windows) {
                    NSNumber *pid = window[(__bridge NSString *) kCGWindowOwnerPID];
                    NSNumber *number = window[(__bridge NSString *) kCGWindowNumber];
                    NSNumber *layer = window[(__bridge NSString *) kCGWindowLayer];
                    CGRect bounds {};
                    if (pid.intValue != candidate.processIdentifier || !number || layer.intValue != 0 ||
                        !CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef) window[(__bridge NSString *) kCGWindowBounds], &bounds) ||
                        bounds.size.width <= 0 || bounds.size.height <= 0) {
                      continue;
                    }
                    const auto identity = std::to_string(candidate.processIdentifier) + ':' + std::to_string(number.unsignedIntValue);
                    remaining.observe(identity, budget::clock::now());
                    has_window = has_window || remaining.can_focus(identity, budget::clock::now());
                  }
                  // Preserve AppKit-only activation if metadata enumeration
                  // fails; do not introduce an Accessibility permission flow.
                  if (!copied) {
                    const auto identity = "app:" + std::to_string(candidate.processIdentifier);
                    remaining.observe(identity, budget::clock::now());
                    has_window = remaining.can_focus(identity, budget::clock::now());
                  }
                  if (!cancelled_ && has_window && remaining.can_focus(budget::clock::now()) &&
                      [candidate activateWithOptions:NSApplicationActivateIgnoringOtherApps] && candidate.active) {
                    remaining.confirmed();
                    break;
                  }
                }
                if (copied) {
                  CFRelease(copied);
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
