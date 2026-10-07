#include "display_power.h"

#include "src/logging.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <mutex>
#include <poll.h>
#include <unistd.h>

namespace platf::display_power {
  namespace {
    struct lease_t {
      GSubprocess *process = nullptr;

      bool alive() const {
        auto *output = g_subprocess_get_stdout_pipe(process);
        struct pollfd watched {g_unix_input_stream_get_fd(G_UNIX_INPUT_STREAM(output)), POLLIN, 0};
        // After READY the worker writes no more stdout. EOF/error means its
        // generation or bus has gone away; do not reuse that stale lease.
        const int result = poll(&watched, 1, 0);
        return result == 0 || (result < 0 && errno == EINTR);
      }

      ~lease_t() {
        if (process) {
          // The session broker cancels its generation-bound worker when its
          // client disconnects; the worker's D-Bus connection owns the inhibit.
          g_subprocess_force_exit(process);
          g_subprocess_wait(process, nullptr, nullptr);
          g_object_unref(process);
        }
      }
    };

    std::mutex lease_mutex;
    std::weak_ptr<lease_t> shared_lease;

    std::shared_ptr<lease_t> start_ready(const char *operation) {
      auto lease = std::make_shared<lease_t>();
      GError *spawn_error = nullptr;
      lease->process = g_subprocess_new(
        G_SUBPROCESS_FLAGS_STDOUT_PIPE,
        &spawn_error,
        "/usr/libexec/vibeshine/vibepollo-session-exec",
        operation,
        nullptr
      );
      if (!lease->process) {
        BOOST_LOG(error) << "Linux display power: " << (spawn_error ? spawn_error->message : "could not start session helper");
        g_clear_error(&spawn_error);
        return {};
      }
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
      auto *output = g_subprocess_get_stdout_pipe(lease->process);
      struct pollfd watched {g_unix_input_stream_get_fd(G_UNIX_INPUT_STREAM(output)), POLLIN, 0};
      while (std::chrono::steady_clock::now() < deadline) {
        const int ready = poll(&watched, 1, 100);
        if (ready < 0 && errno == EINTR) {
          continue;
        }
        if (ready < 0) {
          break;
        }
        if (!ready) {
          continue;
        }
        char token = 0;
        if ((watched.revents & POLLIN) && read(watched.fd, &token, 1) == 1 && token == 'R') {
          return lease;
        }
        break;
      }
      BOOST_LOG(error) << "Linux display power: session wake/inhibition did not become ready.";
      return {};
    }
  }  // namespace

  std::shared_ptr<void> acquire() {
    // Standalone/SteamOS does not have a machine session broker.
    if (!std::getenv("VIBEPOLLO_MACHINE_HOST")) {
      static const auto noop = std::make_shared<int>(0);
      return noop;
    }
    std::lock_guard lock {lease_mutex};
    if (auto lease = shared_lease.lock()) {
      if (!lease->alive()) {
        auto replacement = start_ready("display-power");
        if (!replacement) {
          return {};
        }
        // Keep the owner object stable: already-active streams still hold it.
        // Replacing only the weak cache would release the new worker as soon
        // as this one reconnecting client ends, despite those older owners.
        std::swap(lease->process, replacement->process);
        return lease;
      }
      // Share one long-lived broker connection across pending/active RTSP and
      // WebRTC owners. A new launch still gets a fresh wake before topology.
      if (!start_ready("display-wake")) {
        return {};
      }
      return lease;
    }
    auto lease = start_ready("display-power");
    shared_lease = lease;
    return lease;
  }

}  // namespace platf::display_power
