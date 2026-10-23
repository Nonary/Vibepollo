/**
 * @file src/process_catalog.h
 * @brief Catalog publication, launch environment handoff and refresh admission.
 */
#pragma once

#include "boost_process_shim.h"
#include "process_context.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace proc::catalog {
  using environment_t = boost_process_shim::environment;

  // References bind these operations to proc_t's actual launch/preparation
  // storage. No process, command iterator, or environment is copied to refresh.
  struct state_view_t {
    std::atomic<int> &app_id;
    environment_t &environment;
    std::optional<environment_t> &pending_environment;
    std::unordered_set<std::string> &stream_environment_keys;
    std::vector<ctx_t> &apps;
    ctx_t &active_app;
    std::mutex &mutex;
  };

  struct snapshot_t {
    environment_t environment;
    std::vector<ctx_t> apps;
  };
  using replace_idle_process_t = std::function<void(snapshot_t &&)>;

  // Caller owns both state mutexes, as proc_t's move assignment does.
  void move_state(state_view_t target, state_view_t source);
  void update_apps(state_view_t target, std::vector<ctx_t> &&apps, environment_t &&environment);
  // Publication and launch handoff run under the caller's lifecycle gate.
  void publish(state_view_t target, snapshot_t &&parsed, const replace_idle_process_t &replace_idle_process);
  // Process teardown/undo is the external boundary. Publish the next catalog
  // environment only after it has finished using the old session environment.
  void prepare_launch(state_view_t target, const std::function<void()> &teardown);

  class config_apply_scope {
  public:
    config_apply_scope();
    ~config_apply_scope();
    config_apply_scope(const config_apply_scope &) = delete;
    config_apply_scope &operator=(const config_apply_scope &) = delete;
    static bool active();
  };

  struct refresh_gates_t {
    std::mutex &lifecycle;
    std::shared_mutex &configuration;
    std::recursive_mutex &apps_file;
  };
  using read_snapshot_t = std::function<std::optional<snapshot_t>(const std::string &)>;
  using schedule_t = std::function<void(std::function<void()>, std::chrono::milliseconds)>;

  // A termination caller can already own lifecycle/configuration admission.
  // Queue its refresh request with an owned path; never reenter those gates on
  // the same stack. The scheduled worker uses normal nonblocking admission.
  void defer_refresh(std::string file_name,
    std::function<void(const std::string &)> request_refresh, schedule_t schedule);

  // Only parsing, OS process replacement and task/time scheduling are external
  // boundaries. Admission, publication and retries live in this implementation.
  void refresh(std::string file_name, refresh_gates_t gates, state_view_t target,
    read_snapshot_t read_snapshot, replace_idle_process_t replace_idle_process, schedule_t schedule);
}  // namespace proc::catalog
