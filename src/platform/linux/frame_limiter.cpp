#include "frame_limiter.h"

#include "mangohud_policy.h"
#include "src/config.h"
#include "src/logging.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <memory>
#include <mutex>
#include <poll.h>
#include <unistd.h>

namespace platf {
  namespace {
    struct limiter_lease {
      GSubprocess *process = nullptr;

      ~limiter_lease() {
        if (process) {
          // Disconnecting the client makes the generation-bound broker cancel
          // its exact transient unit. The socket policy dies with that unit.
          g_subprocess_force_exit(process);
          g_subprocess_wait(process, nullptr, nullptr);
          g_object_unref(process);
        }
      }

      bool alive() const {
        auto *output = g_subprocess_get_stdout_pipe(process);
        pollfd watched {g_unix_input_stream_get_fd(G_UNIX_INPUT_STREAM(output)), POLLIN, 0};
        const int result = poll(&watched, 1, 0);
        return result == 0 || (result < 0 && errno == EINTR);
      }
    };

    std::mutex limiter_mutex;
    std::array<bool, 2> owners {};
    std::unique_ptr<limiter_lease> lease;

    std::unique_ptr<limiter_lease> start(const mangohud::launch_policy_t &policy) {
      const bool proton = mangohud::proton_provider_selected(config::frame_limiter.provider);
      gchar *mangohud_path = g_find_program_in_path("mangohud");
      const bool available = mangohud_path != nullptr;
      g_free(mangohud_path);
      if (!proton && !available) {
        return {};
      }
      const bool overlay = available && (!proton || mangohud::proton_overlay_provider_selected(config::frame_limiter.provider));
      const char *provider = proton ? (overlay ? "mangohud-proton" : "proton") : "mangohud";
      const std::string limit = std::to_string(policy.limit_millihz);
      const auto &configured_preset = config::frame_limiter.mangohud_preset;
      const char *preset = overlay && (configured_preset == "1" || configured_preset == "2" ||
                                       configured_preset == "3" || configured_preset == "4") ?
                             configured_preset.c_str() :
                             "custom";
      const char *graph = overlay && config::frame_limiter.mangohud_always_show_graph ? "1" : "0";
      const char *method = !proton && config::frame_limiter.mangohud_limiter_method == "early" ? "early" : "late";
      auto result = std::make_unique<limiter_lease>();
      GError *error = nullptr;
      if (std::getenv("VIBESHINE_MACHINE_HOST")) {
        result->process = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE, &error, "/usr/libexec/vibeshine/vibepollo-session-exec", "global-limiter", provider, limit.c_str(), preset, graph, method, nullptr);
      } else {
        result->process = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE, &error, "/usr/libexec/vibeshine/vibepollo-steam-launch", "--global", provider, limit.c_str(), preset, graph, method, "0", "0", nullptr);
      }
      if (!result->process) {
        BOOST_LOG(warning) << "Global Linux limiter: " << (error ? error->message : "helper unavailable");
        g_clear_error(&error);
        return {};
      }
      auto *output = g_subprocess_get_stdout_pipe(result->process);
      pollfd watched {g_unix_input_stream_get_fd(G_UNIX_INPUT_STREAM(output)), POLLIN, 0};
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
      std::string ready_line;
      while (std::chrono::steady_clock::now() < deadline) {
        const int ready = poll(&watched, 1, 100);
        if (ready < 0 && errno == EINTR) {
          continue;
        }
        if (ready < 0 || (ready > 0 && !(watched.revents & POLLIN))) {
          break;
        }
        if (!ready) {
          continue;
        }
        char token;
        if (read(watched.fd, &token, 1) != 1) {
          break;
        }
        if (token == '\n') {
          if (ready_line.starts_with("READY ")) {
            BOOST_LOG(info) << "Global Linux " << provider << " limiter ready at " << policy.limit
                            << " FPS for external Proton launches (" << ready_line.substr(6) << " installations).";
            return result;
          }
          break;
        }
        if (ready_line.size() >= 64) {
          break;
        }
        ready_line += token;
      }
      BOOST_LOG(warning) << "Global Linux limiter could not prepare external Proton launches; managed launch limiting remains available.";
      return {};
    }
  }  // namespace

  void frame_limiter_streaming_start(frame_limiter_owner owner, const framegen::stream_start_policy_t &stream_policy) {
    std::lock_guard lock(limiter_mutex);
    owners[static_cast<std::size_t>(owner)] = true;
    if (lease && lease->alive()) {
      return;
    }
    lease.reset();
    const auto policy = mangohud::make_launch_policy(config::frame_limiter.provider, config::frame_limiter.enable, config::frame_limiter.virtual_display_limiter_enabled(), stream_policy, config::frame_limiter.fps_limit_millihz);
    if (policy.enabled) {
      lease = start(policy);
    }
  }

  void frame_limiter_streaming_stop(frame_limiter_owner owner, bool) {
    std::lock_guard lock(limiter_mutex);
    owners[static_cast<std::size_t>(owner)] = false;
    if (!owners[0] && !owners[1]) {
      lease.reset();
    }
  }
}  // namespace platf
