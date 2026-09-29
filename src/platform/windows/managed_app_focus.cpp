#include "src/managed_app_focus.h"

#include "tools/playnite_launcher/focus_utils.h"

#include <atomic>
#include <thread>

namespace managed_app_focus {
  namespace {
    class windows_session final: public session {
    public:
      windows_session(target app, policy options):
          worker_([this, app = std::move(app), options] {
            playnite_launcher::focus::focus_by_install_dir_extended(app.install_dir.wstring(), options.attempts, options.timeout_secs, options.exit_on_first, [this] {
              return cancelled_.load();
            });
          }) {}

      ~windows_session() override {
        cancelled_ = true;
        worker_.join();
      }

    private:
      std::atomic_bool cancelled_ {false};
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
