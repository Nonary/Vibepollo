#include "src/managed_app_focus.h"

#include "src/logging.h"

#include <cstdlib>
#include <gio/gio.h>

namespace managed_app_focus {
  namespace {
    class broker_session final: public session {
    public:
      explicit broker_session(GSubprocess *process):
          process_(process) {}

      ~broker_session() override {
        // Killing the client closes its generation-bound broker connection;
        // the broker cancels the desktop helper and its transient user scope.
        g_subprocess_force_exit(process_);
        g_subprocess_wait(process_, nullptr, nullptr);
        g_object_unref(process_);
      }

    private:
      GSubprocess *process_;
    };
  }  // namespace

  std::unique_ptr<session> start(const target &app, policy options) {
    if (!options.enabled()) {
      return {};
    }
    const auto attempts = std::to_string(options.attempts);
    const auto timeout = std::to_string(options.timeout_secs);
    const bool machine = std::getenv("VIBEPOLLO_MACHINE_HOST") != nullptr;
    GError *error = nullptr;
    auto *process = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, &error, machine ? "/usr/libexec/vibeshine/vibepollo-session-exec" : "/usr/libexec/vibeshine/vibepollo-app-focus", machine ? "managed-focus" : "--managed", app.provider.c_str(), app.id.c_str(), attempts.c_str(), timeout.c_str(), options.exit_on_first ? "1" : "0", nullptr);
    if (!process) {
      BOOST_LOG(warning) << "Managed application focus could not start: " << (error ? error->message : "unknown error");
      g_clear_error(&error);
      return {};
    }
    return std::make_unique<broker_session>(process);
  }
}  // namespace managed_app_focus
