#include "frame_limiter.h"

#include "mangohud_policy.h"
#include "src/config.h"
#include "src/logging.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <memory>
#include <mutex>
#include <optional>
#include <poll.h>
#include <string>
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

    struct owner_policy_t {
      framegen::stream_start_policy_t stream;
      proton_color_mode color_mode = proton_color_mode::sdr;
    };

    std::mutex limiter_mutex;
    std::array<std::optional<owner_policy_t>, 2> owners {};
    std::unique_ptr<limiter_lease> lease;
    std::string active_signature;

    const char *color_mode_name(proton_color_mode color_mode) {
      switch (color_mode) {
        case proton_color_mode::hdr:
          return "hdr";
        case proton_color_mode::sdr10:
          return "sdr10";
        default:
          return "sdr";
      }
    }

    std::unique_ptr<limiter_lease> start(
      const mangohud::launch_policy_t &policy,
      proton_color_mode color_mode
    ) {
      const bool proton = mangohud::proton_provider_selected(config::frame_limiter.provider);
      gchar *mangohud_path = g_find_program_in_path("mangohud");
      const bool available = mangohud_path != nullptr;
      g_free(mangohud_path);
      const bool limiter_available = policy.enabled && (proton || available);
      const bool overlay = limiter_available && available &&
                           (!proton || mangohud::proton_overlay_provider_selected(config::frame_limiter.provider));
      const char *provider = !limiter_available ? "disabled" :
                             proton ? (overlay ? "mangohud-proton" : "proton") : "mangohud";
      const std::string limit = std::to_string(limiter_available ? policy.limit_millihz : 0);
      const auto &configured_preset = config::frame_limiter.mangohud_preset;
      const char *preset = overlay && (configured_preset == "1" || configured_preset == "2" ||
                                       configured_preset == "3" || configured_preset == "4") ?
                             configured_preset.c_str() :
                             "custom";
      const char *graph = overlay && config::frame_limiter.mangohud_always_show_graph ? "1" : "0";
      const char *method = limiter_available && !proton &&
                             config::frame_limiter.mangohud_limiter_method == "early" ?
                             "early" : "late";
      auto result = std::make_unique<limiter_lease>();
      GError *error = nullptr;
      if (std::getenv("VIBEPOLLO_MACHINE_HOST")) {
        result->process = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE, &error, "/usr/libexec/vibeshine/vibepollo-session-exec", "global-limiter", provider, limit.c_str(), preset, graph, method, color_mode_name(color_mode), nullptr);
      } else {
        result->process = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE, &error, "/usr/libexec/vibeshine/vibepollo-steam-launch", "--global", provider, limit.c_str(), preset, graph, method, "0", "0", color_mode_name(color_mode), nullptr);
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
            BOOST_LOG(info) << "Global Linux Proton stream policy ready in " << color_mode_name(color_mode)
                            << " mode" << (limiter_available ? " with " + std::string(provider) +
                                                               " limiting at " + policy.limit + " FPS" : "")
                            << " for external launches (" << ready_line.substr(6) << " installations).";
            return result;
          }
          break;
        }
        if (ready_line.size() >= 64) {
          break;
        }
        ready_line += token;
      }
      BOOST_LOG(warning) << "Global Linux Proton stream policy could not prepare external launches; managed launches remain available.";
      return {};
    }

    proton_color_mode aggregate_color_mode() {
      bool sdr10 = false;
      for (const auto &owner : owners) {
        if (!owner) {
          continue;
        }
        if (owner->color_mode == proton_color_mode::hdr) {
          return proton_color_mode::hdr;
        }
        sdr10 |= owner->color_mode == proton_color_mode::sdr10;
      }
      return sdr10 ? proton_color_mode::sdr10 : proton_color_mode::sdr;
    }

    void reconcile() {
      const auto selected = std::find_if(owners.begin(), owners.end(), [](const auto &owner) {
        return owner.has_value();
      });
      if (selected == owners.end()) {
        lease.reset();
        active_signature.clear();
        return;
      }

      const auto policy = mangohud::make_launch_policy(
        config::frame_limiter.provider,
        config::frame_limiter.enable,
        config::frame_limiter.virtual_display_limiter_enabled(),
        (*selected)->stream,
        config::frame_limiter.fps_limit_millihz
      );
      const auto color_mode = aggregate_color_mode();
      const std::string signature = std::string(color_mode_name(color_mode)) + "|" +
                                    (policy.enabled ? "1|" + std::to_string(policy.limit_millihz) : "0") + "|" +
                                    config::frame_limiter.provider + "|" +
                                    config::frame_limiter.mangohud_preset + "|" +
                                    (config::frame_limiter.mangohud_always_show_graph ? "1" : "0") + "|" +
                                    config::frame_limiter.mangohud_limiter_method;
      if (lease && lease->alive() && signature == active_signature) {
        return;
      }
      lease.reset();
      active_signature.clear();
      lease = start(policy, color_mode);
      if (lease) {
        active_signature = signature;
      }
    }
  }  // namespace

  void frame_limiter_streaming_start(
    frame_limiter_owner owner,
    const framegen::stream_start_policy_t &stream_policy,
    proton_color_mode color_mode
  ) {
    std::lock_guard lock(limiter_mutex);
    owners[static_cast<std::size_t>(owner)] = owner_policy_t {stream_policy, color_mode};
    reconcile();
  }

  void frame_limiter_streaming_stop(frame_limiter_owner owner, bool) {
    std::lock_guard lock(limiter_mutex);
    owners[static_cast<std::size_t>(owner)].reset();
    reconcile();
  }
}  // namespace platf
