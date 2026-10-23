#include "rtx_hdr_runtime.h"

#include "display_helper_process.h"
#include "src/config.h"
#include "src/process.h"

#include <algorithm>
#include <atomic>
#include <boost/program_options/parsers.hpp>
#include <cstdlib>
#include <gio/gio.h>

namespace platf::rtx_hdr {
  namespace {
    std::atomic<std::shared_ptr<const frame_settings_t>> current {std::make_shared<const frame_settings_t>()};
    std::atomic<std::uint32_t> generation {0};
  }  // namespace

  void notify_live_settings_changed() {
    auto next = std::make_shared<frame_settings_t>();
    const auto &values = config::video.rtx_hdr;
    next->enabled = values.enabled && config::runtime_config_override_enabled("rtx_hdr");
    next->sdr_brightness = std::clamp(values.sdr_brightness, 0, 100);
    next->values = {std::clamp(values.contrast + 100, 0, 200), std::clamp(values.saturation + 100, 0, 200), std::clamp(values.middle_gray, 10, 100), std::clamp(values.peak_brightness, 400, 2000)};
    const auto previous = current.load();
    if (previous->generation && previous->enabled == next->enabled && previous->values == next->values && previous->sdr_brightness == next->sdr_brightness) {
      return;
    }
    next->generation = generation.fetch_add(1) + 1;
    current.store(std::move(next));
  }

  std::shared_ptr<const frame_settings_t> frame_settings() {
    return current.load();
  }

  live_output_metadata_state_t live_output_metadata_state() {
    const auto settings = current.load();
    return {settings->generation, settings->values.peak};
  }

  foreground_t::foreground_t():
      worker([this](std::stop_token stop) {
        poll(stop);
      }) {}

  foreground_t::~foreground_t() {
    worker.request_stop();
    wake.notify_all();
  }

  bool foreground_t::matches() const {
    const auto age = std::chrono::steady_clock::now().time_since_epoch() - std::chrono::nanoseconds(observed_at.load());
    return matched.load() && app_id.load() == proc::proc.current_app_id() && age < std::chrono::seconds(2);
  }

  void foreground_t::poll(std::stop_token stop) {
    while (!stop.stop_requested()) {
      const int id = proc::proc.current_app_id();
      bool match = false;
      try {
        const auto app = proc::proc.active_app_snapshot();
        if (!app) {
          // A brief lifecycle/catalog lock is not a foreground change. Keep
          // the last observation until its normal two-second TTL, without
          // refreshing it or letting it survive an application transition.
          if (id <= 0 || id != app_id.load()) {
            matched.store(false);
          }
          std::unique_lock lock(mutex);
          wake.wait_for(lock, stop, std::chrono::milliseconds(500), [] {
            return false;
          });
          continue;
        }
        std::string provider, identity;
        if (app && !app->steam_id.empty()) {
          provider = "steam";
          identity = app->steam_id;
        } else if (app && !app->lutris_id.empty()) {
          provider = "lutris";
          identity = app->lutris_id;
        } else if (app && !app->cmd.empty()) {
          const auto parts = boost::program_options::split_unix(app->cmd);
          if (!parts.empty()) {
            provider = "executable";
            identity = parts.front();
          }
        }
        if (id > 0 && !identity.empty()) {
          const bool machine = std::getenv("VIBEPOLLO_MACHINE_HOST") != nullptr;
          GError *error = nullptr;
          auto *process = g_subprocess_new(GSubprocessFlags(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE), &error, machine ? "/usr/libexec/vibeshine/vibepollo-session-exec" : "/usr/libexec/vibeshine/vibepollo-app-focus", machine ? "rtx-hdr-foreground" : "--foreground", provider.c_str(), identity.c_str(), nullptr);
          if (process) {
            const auto reply = linux_private_display::helper_process::communicate_until(process, std::chrono::steady_clock::now() + std::chrono::milliseconds(1500), machine);
            match = reply.success && reply.stdout_text == "1\n";
            g_object_unref(process);
          }
          g_clear_error(&error);
        }
      } catch (...) {
        match = false;
      }
      // Never publish a result for the app which was replaced during IPC.
      matched.store(false);
      app_id.store(id);
      observed_at.store(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
      matched.store(match && id == proc::proc.current_app_id());
      std::unique_lock lock(mutex);
      wake.wait_for(lock, stop, std::chrono::milliseconds(500), [] {
        return false;
      });
    }
    matched.store(false);
  }
}  // namespace platf::rtx_hdr
