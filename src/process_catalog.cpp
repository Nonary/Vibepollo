/**
 * @file src/process_catalog.cpp
 * @brief Catalog publication and nonblocking refresh coordination.
 */
#include "process_catalog.h"

#include "utility.h"

#include <algorithm>
#include <utility>

namespace proc::catalog {
  namespace {
    thread_local unsigned int config_apply_depth = 0;
  }

  void move_state(state_view_t target, state_view_t source) {
    target.app_id.store(source.app_id.load(std::memory_order_acquire), std::memory_order_release);
    target.environment = std::move(source.environment);
    target.pending_environment = std::move(source.pending_environment);
    target.stream_environment_keys = std::move(source.stream_environment_keys);
    target.apps = std::move(source.apps);
    target.active_app = std::move(source.active_app);
  }

  void update_apps(state_view_t target, std::vector<ctx_t> &&apps, environment_t &&environment) {
    std::scoped_lock lock {target.mutex};
    const bool app_is_active = target.app_id.load(std::memory_order_acquire) > 0;
    if (app_is_active && !target.active_app.uuid.empty()) {
      const auto refreshed_app = std::find_if(apps.begin(), apps.end(), [&](const ctx_t &candidate) {
        return candidate.uuid == target.active_app.uuid;
      });
      if (refreshed_app != apps.end()) {
        target.app_id = util::from_view(refreshed_app->id);
      }
    }
    target.apps = std::move(apps);
    if (app_is_active) {
      target.pending_environment = std::move(environment);
    } else {
      target.environment = std::move(environment);
      target.pending_environment.reset();
      target.stream_environment_keys.clear();
    }
  }

  void publish(state_view_t target, snapshot_t &&parsed, const replace_idle_process_t &replace_idle_process) {
    // Child existence is not session identity: launch and teardown can both
    // retain prep-command references while no child is running.
    if (target.app_id.load(std::memory_order_acquire) > 0) {
      update_apps(target, std::move(parsed.apps), std::move(parsed.environment));
    } else {
      replace_idle_process(std::move(parsed));
    }
  }

  void prepare_launch(state_view_t target, const std::function<void()> &teardown) {
    teardown();
    if (target.pending_environment) {
      target.environment = std::move(*target.pending_environment);
      target.pending_environment.reset();
      target.stream_environment_keys.clear();
    }
  }

  config_apply_scope::config_apply_scope() {
    ++config_apply_depth;
  }

  config_apply_scope::~config_apply_scope() {
    --config_apply_depth;
  }

  bool config_apply_scope::active() {
    return config_apply_depth != 0;
  }

  void defer_refresh(std::string file_name,
    std::function<void(const std::string &)> request_refresh, schedule_t schedule) {
    schedule([file_name = std::move(file_name), request_refresh = std::move(request_refresh)]() {
      request_refresh(file_name);
    }, std::chrono::milliseconds {50});
  }

  void refresh(std::string file_name, refresh_gates_t gates, state_view_t target,
    read_snapshot_t read_snapshot, replace_idle_process_t replace_idle_process, schedule_t schedule) {
    {
      // A configuration callback can already own the non-recursive lifecycle
      // mutex or configuration write gate on this thread. Do not reacquire it.
      std::unique_lock lifecycle_lock {gates.lifecycle, std::defer_lock};
      std::shared_lock config_lock {gates.configuration, std::defer_lock};
      if (!config_apply_scope::active() && lifecycle_lock.try_lock() && config_lock.try_lock()) {
        // deleteApp can own this file mutex while waiting for config write
        // admission. Immediate and deferred refreshes must never wait here.
        std::unique_lock apps_lock {gates.apps_file, std::try_to_lock};
        if (apps_lock.owns_lock()) {
          auto parsed = read_snapshot(file_name);
          if (parsed) {
            publish(target, std::move(*parsed), replace_idle_process);
          }
          return;
        }
      }
    }

    // Release every gate before scheduling the retry. The callback owns its
    // path and reads the latest file only after all three gates are admitted.
    schedule([file_name = std::move(file_name), gates, target, read_snapshot, replace_idle_process, schedule]() {
      refresh(file_name, gates, target, read_snapshot, replace_idle_process, schedule);
    }, std::chrono::milliseconds {50});
  }
}  // namespace proc::catalog
