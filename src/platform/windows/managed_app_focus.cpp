#include "src/managed_app_focus.h"

#include "tools/playnite_launcher/focus_utils.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace managed_app_focus {
  namespace {
    class windows_session final: public session {
    public:
      windows_session(target app, policy options):
          worker_([this, app = std::move(app), options] {
            budget remaining(options, budget::clock::now());
            while (!cancelled_ && remaining.active(budget::clock::now())) {
              const auto windows = playnite_launcher::focus::discover_game_windows(app.install_dir.wstring());
              for (auto window : windows) {
                remaining.observe(playnite_launcher::focus::window_identity(window), budget::clock::now());
              }
              // An expired activation allowance does not invalidate an already
              // focused game. Additional windows must not displace it.
              if (!cancelled_ && playnite_launcher::focus::find_foreground_game_window(windows)) {
                remaining.confirmed();
              } else {
                for (auto window : windows) {
                  const auto identity = playnite_launcher::focus::window_identity(window);
                  const auto cancel = [&] { return cancelled_ || !remaining.can_focus(identity, budget::clock::now()); };
                  if (cancelled_) {
                    break;
                  }
                  if (cancel()) {
                    continue;
                  }
                  if (playnite_launcher::focus::focus_game_window(window, cancel)) {
                    remaining.confirmed();
                    break;
                  }
                }
              }
              std::unique_lock lock(mutex_);
              wake_.wait_for(lock, std::chrono::seconds(1), [this] { return cancelled_.load(); });
            }
          }) {}

      ~windows_session() override {
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
    return std::make_unique<windows_session>(app, options);
  }
}  // namespace managed_app_focus
