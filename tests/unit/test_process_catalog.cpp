// Direct behavioral coverage of the same compiled catalog component used by proc_t.
#include "src/process_catalog.h"

#include <atomic>
#include <future>
#include <iostream>
#include <map>
#include <stdexcept>
#include <thread>
#include <utility>

using namespace std::chrono_literals;
namespace catalog = proc::catalog;

namespace {
  void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
  }

  // Storage fixtures use the real production app, command and environment
  // types. All publication/move/environment behavior is in process_catalog.cpp.
  struct storage_t {
    std::atomic<int> app_id {0};
    catalog::environment_t environment;
    std::optional<catalog::environment_t> pending;
    std::unordered_set<std::string> stream_keys;
    std::vector<proc::ctx_t> apps;
    proc::ctx_t active_app {};
    std::mutex mutex;

    catalog::state_view_t view() {
      return {app_id, environment, pending, stream_keys, apps, active_app, mutex};
    }
  };

  std::string value(catalog::environment_t &environment, const char *name) {
    for (const auto &entry : environment) {
      if (entry.get_name() == name) return entry.to_string();
    }
    return {};
  }

  // Deterministic task/time boundary: invoke the exact callback provided by the
  // production component, without implementing any admission or retry policy.
  class scheduler_t {
    std::mutex mutex;
    std::vector<std::function<void()>> tasks;

  public:
    std::vector<std::chrono::milliseconds> delays;
    void schedule(std::function<void()> callback, std::chrono::milliseconds delay) {
      std::lock_guard lock {mutex};
      delays.push_back(delay);
      tasks.push_back(std::move(callback));
    }
    size_t size() {
      std::lock_guard lock {mutex};
      return tasks.size();
    }
    void run_one() {
      std::function<void()> task;
      {
        std::lock_guard lock {mutex};
        require(!tasks.empty(), "no scheduled retry");
        task = std::move(tasks.front());
        tasks.erase(tasks.begin());
      }
      task();
    }
  };

  struct fixture_t {
    storage_t state;
    scheduler_t scheduler;
    std::mutex lifecycle;
    std::shared_mutex configuration;
    std::recursive_mutex file;
    std::atomic<int> reads {0}, replacements {0}, revision {1};
    bool parse_fails = false, include_active_app = true;
    std::string last_path;

    void activate() {
      state.app_id = 7;
      state.active_app.uuid = "active";
      state.active_app.id = "7";
      state.active_app.prep_cmds.emplace_back(std::string(4096, 'D'), std::string(4096, 'U'), false);
      state.environment["client"] = "session-client";
      state.environment["catalog"] = "old-catalog";
      state.environment["retired"] = "old-only";
      state.stream_keys.insert("client");
    }

    // File/parser boundary returns real parsed storage; process replacement
    // omits OS handles but uses the exact move_state used by proc_t::operator=.
    std::optional<catalog::snapshot_t> read_snapshot(const std::string &path) {
      ++reads;
      last_path = path;
      if (parse_fails) return std::nullopt;
      catalog::snapshot_t parsed;
      if (include_active_app) {
        proc::ctx_t app {};
        app.uuid = "active";
        app.id = std::to_string(40 + revision.load());
        parsed.apps.push_back(std::move(app));
      }
      parsed.environment["client"] = "configured-client";
      parsed.environment["catalog"] = std::to_string(revision.load());
      return parsed;
    }

    void refresh(std::string path = "apps.json") {
      catalog::refresh(std::move(path), {lifecycle, configuration, file}, state.view(),
        [this](const std::string &name) { return read_snapshot(name); },
        [this](catalog::snapshot_t &&snapshot) {
          storage_t parsed;
          parsed.environment = std::move(snapshot.environment);
          parsed.apps = std::move(snapshot.apps);
          std::scoped_lock lock {state.mutex, parsed.mutex};
          catalog::move_state(state.view(), parsed.view());
          ++replacements;
        },
        [this](std::function<void()> task, std::chrono::milliseconds delay) {
          scheduler.schedule(std::move(task), delay);
        });
    }

    void expect_retry() {
      require(reads == 0 && replacements == 0, "contended refresh touched process/catalog state");
      require(scheduler.size() == 1 && scheduler.delays.back() == 50ms, "missing bounded 50ms retry");
    }
    void expect_active(const char *prep) {
      require(replacements == 0, "active process was replaced");
      require(prep[0] == 'D' && state.active_app.prep_cmds[0].do_cmd.data() == prep,
        "active preparation storage was invalidated");
      require(value(state.environment, "client") == "session-client" &&
        value(state.environment, "catalog") == "old-catalog" && state.stream_keys.contains("client"),
        "active undo environment was changed");
    }
  };

  template<class Future>
  void finishes(Future &future, const char *message) {
    // A whole-process timeout in the runner remains the final guard if an
    // unsafe baseline leaves a std::future destructor waiting on deadlock.
    require(future.wait_for(500ms) == std::future_status::ready, message);
    future.get();
  }

  void idle_refresh() {
    fixture_t f;
    f.refresh();
    require(f.reads == 1 && f.replacements == 1 && f.scheduler.size() == 0, "idle refresh did not publish once");
    require(f.state.apps[0].id == "41" && value(f.state.environment, "catalog") == "1", "idle catalog/environment missing");
  }

  void active_storage() {
    fixture_t f;
    f.activate();
    const char *prep = f.state.active_app.prep_cmds[0].do_cmd.data();
    f.refresh();
    f.expect_active(prep);
    require(f.state.app_id == 41 && f.state.pending && value(*f.state.pending, "catalog") == "1", "active catalog was not updated");
  }

  void missing_active_entry() {
    fixture_t f;
    f.activate();
    f.include_active_app = false;
    const char *prep = f.state.active_app.prep_cmds[0].do_cmd.data();
    f.refresh();
    f.expect_active(prep);
    require(f.state.app_id == 7 && f.state.apps.empty(), "removed catalog entry dropped live session identity");
  }

  void ipc_teardown() {
    fixture_t f;
    f.activate();
    const char *prep = f.state.active_app.prep_cmds[0].do_cmd.data();
    {
      std::unique_lock teardown {f.lifecycle};
      auto pipe_worker = std::async(std::launch::async, [&]() {
        std::lock_guard writer {f.file};
        std::string local_path = "owned-apps.json";
        f.refresh(local_path);
      });
      // Join while teardown retains lifecycle admission, just as IPC stop does.
      finishes(pipe_worker, "IPC worker waited for lifecycle while teardown joined it");
      f.expect_retry();
      f.expect_active(prep);
    }
    f.revision = 2;
    f.scheduler.run_one();
    f.expect_active(prep);
    require(f.last_path == "owned-apps.json" && f.state.apps[0].id == "42", "retry lost path ownership or read an old snapshot");
  }

  void configuration_contention() {
    fixture_t f;
    {
      std::unique_lock writer {f.configuration};
      auto callback = std::async(std::launch::async, [&]() {
        std::lock_guard file {f.file};
        f.refresh();
      });
      finishes(callback, "provider waited for a config writer while owning its file mutex");
      f.expect_retry();
    }
    f.scheduler.run_one();
    require(f.reads == 1 && f.scheduler.size() == 0, "config-contended retry did not complete");
  }

  void file_configuration_cycle(bool deferred) {
    fixture_t f;
    f.activate();
    const char *prep = f.state.active_app.prep_cmds[0].do_cmd.data();
    if (deferred) {
      std::unique_lock lifecycle {f.lifecycle};
      auto callback = std::async(std::launch::async, [&]() { f.refresh(); });
      finishes(callback, "initial callback waited for lifecycle");
      f.expect_retry();
    }

    std::promise<void> file_held;
    std::promise<bool> request_apply;
    auto file_ready = file_held.get_future();
    auto apply_ready = request_apply.get_future();
    auto delete_app = std::async(std::launch::async, [&]() {
      std::lock_guard file {f.file};
      file_held.set_value();
      if (!apply_ready.get()) return;
      std::unique_lock config_writer {f.configuration};
      catalog::config_apply_scope apply;
      f.revision = 3;
    });
    file_ready.wait();

    auto attempt = std::async(std::launch::async, [&]() {
      if (deferred) f.scheduler.run_one();
      else f.refresh();
    });
    // Refresh must return while the writer still owns the file mutex and has
    // not requested the config gate. On failure, let the writer release the
    // file without requesting config so a blocking-file regression can unwind.
    const bool returned_while_file_owned = attempt.wait_for(500ms) == std::future_status::ready;
    request_apply.set_value(returned_while_file_owned);
    finishes(delete_app, "deleteApp could not acquire config write gate while owning apps file");
    finishes(attempt, "refresh retained lifecycle/config gates while waiting for apps file");
    require(returned_while_file_owned, "refresh did not return while writer owned apps file before requesting config");
    f.expect_retry();
    f.expect_active(prep);
    f.scheduler.run_one();
    require(f.reads == 1 && f.state.apps[0].id == "43", "retry failed to publish the completed writer's latest file");
    f.expect_active(prep);
  }

  void config_recursion() {
    fixture_t f;
    {
      std::unique_lock lifecycle {f.lifecycle};
      std::unique_lock writer {f.configuration};
      catalog::config_apply_scope apply;
      std::lock_guard file {f.file};
      f.refresh();
      f.expect_retry();
      require(!std::async(std::launch::async, catalog::config_apply_scope::active).get(), "config scope leaked across threads");
    }
    require(!catalog::config_apply_scope::active(), "config scope did not reset");
    f.scheduler.run_one();
    require(f.reads == 1, "config callback retry did not finish");
  }

  void config_scope_exception() {
    require(!catalog::config_apply_scope::active(), "config scope starts active");
    {
      catalog::config_apply_scope outer;
      try {
        catalog::config_apply_scope inner;
        throw std::runtime_error("apply failed");
      } catch (const std::runtime_error &) {
        require(catalog::config_apply_scope::active(), "nested failure cleared the outer config scope");
      }
    }
    require(!catalog::config_apply_scope::active(), "exception left config scope active");
  }

  void failed_parse() {
    fixture_t f;
    f.activate();
    f.parse_fails = true;
    const char *prep = f.state.active_app.prep_cmds[0].do_cmd.data();
    f.refresh();
    f.expect_active(prep);
    require(f.reads == 1 && f.scheduler.size() == 0 && !f.state.pending, "failed parse modified or retried catalog");
  }

  void undo_environment() {
    fixture_t f;
    f.activate();
    const char *prep = f.state.active_app.prep_cmds[0].do_cmd.data();
    f.refresh();
    f.revision = 2;
    f.refresh();
    f.expect_active(prep);
    int undo_calls = 0;
    std::unique_lock launch {f.lifecycle};
    catalog::prepare_launch(f.state.view(), [&]() {
      ++undo_calls;
      f.expect_active(prep);
      require(f.state.active_app.prep_cmds[0].undo_cmd == std::string(4096, 'U'), "undo command was replaced");
      f.state.app_id = -1; // OS/process teardown boundary completes here.
    });
    require(undo_calls == 1 && value(f.state.environment, "catalog") == "2" &&
      value(f.state.environment, "client") == "configured-client" && value(f.state.environment, "retired").empty(),
      "next launch did not receive the latest clean catalog environment");
    require(!f.state.pending && f.state.stream_keys.empty(), "pending environment was not consumed exactly once");
  }

  void failed_teardown() {
    fixture_t f;
    f.activate();
    const char *prep = f.state.active_app.prep_cmds[0].do_cmd.data();
    f.refresh();
    std::unique_lock launch {f.lifecycle};
    try {
      catalog::prepare_launch(f.state.view(), []() { throw std::runtime_error("teardown failed"); });
      throw std::runtime_error("teardown failure was swallowed");
    } catch (const std::runtime_error &error) {
      require(std::string(error.what()) == "teardown failed", "unexpected teardown outcome");
    }
    f.expect_active(prep);
    require(f.state.pending.has_value(), "failed teardown consumed the next environment");
  }

  void teardown_then_idle_retry() {
    fixture_t f;
    f.activate();
    {
      std::unique_lock lifecycle {f.lifecycle};
      auto callback = std::async(std::launch::async, [&]() { f.refresh(); });
      finishes(callback, "callback waited for teardown");
      f.state.app_id = -1;
    }
    f.scheduler.run_one();
    require(f.replacements == 1 && f.state.active_app.prep_cmds.empty() &&
      value(f.state.environment, "client") == "configured-client", "completed teardown did not permit idle replacement");
  }

  void termination_refresh() {
    fixture_t f;
    f.activate();
    const auto owner = std::this_thread::get_id();
    bool on_termination_stack = true;
    unsigned requested = 0;
    std::string path = std::string(4096, 'T') + ".json";
    const auto expected_path = path;
    {
      std::unique_lock lifecycle {f.lifecycle};
      std::shared_lock configuration {f.configuration};
      catalog::defer_refresh(path,
        [&](const std::string &owned_path) {
          // Detect an inline refresh before it could violate the real mutex's
          // same-owner precondition. All later admission uses the actual code.
          require(!on_termination_stack || std::this_thread::get_id() != owner,
            "termination refresh reentered its owned gates on the same stack");
          ++requested;
          f.refresh(owned_path);
        },
        [&](std::function<void()> task, std::chrono::milliseconds delay) {
          f.scheduler.schedule(std::move(task), delay);
        });
      require(requested == 0 && f.scheduler.size() == 1 && f.reads == 0,
        "termination synchronously entered refresh");
      require(f.scheduler.delays.back() == 50ms, "termination refresh delay differs");
      path.assign("changed-after-scheduling");
      // The worker may run before termination ends: it must defer on a
      // different thread, retaining the path and reading no stale catalog.
      auto early_worker = std::async(std::launch::async, [&]() { f.scheduler.run_one(); });
      finishes(early_worker, "termination refresh worker waited on teardown");
      require(requested == 1 && f.reads == 0 && f.scheduler.size() == 1,
        "contended termination worker read or failed to retry");
      f.revision = 2;
      f.state.app_id = -1;
      on_termination_stack = false;
    }
    f.scheduler.run_one();
    require(f.reads == 1 && f.replacements == 1 && f.last_path == expected_path &&
      value(f.state.environment, "catalog") == "2", "termination retry lost owned path or latest data");
  }

  void target_metadata() {
    fixture_t f;
    f.activate();
    const auto *prep = f.state.active_app.prep_cmds[0].do_cmd.data();
    proc::ctx_t app {};
    app.uuid = "active";
    app.id = "81";
    app.state_cmds.emplace_back("pause-do", "resume-undo", true);
    app.idx = "target-index";
    app.gamepad = "dualsense";
    app.virtual_display = true;
    app.virtual_display_primary = true;
    app.use_app_identity = true;
    app.per_client_app_identity = true;
    app.allow_client_commands = false;
    app.terminate_on_pause = true;
    app.scale_factor = 125;
    app.lossless_scaling_target_fps = 119.88;
    app.steam_id = "123456";
    app.lutris_id = "42";
    app.playnite_id = "target-game";
    app.art_version = "target-art";
    app.id_aliases = {"old-target-id"};
    app.config_overrides = {{"output_name", "target-output"}};
    catalog::environment_t next;
    next["catalog"] = "latest-target";
    catalog::update_apps(f.state.view(), {app}, std::move(next));
    f.expect_active(prep);
    const auto &published = f.state.apps.at(0);
    require(published.state_cmds.size() == 1 && published.state_cmds[0].do_cmd == "pause-do" &&
      published.state_cmds[0].elevated && published.idx == "target-index" && published.gamepad == "dualsense" &&
      published.virtual_display && published.virtual_display_primary && published.use_app_identity &&
      published.per_client_app_identity && !published.allow_client_commands && published.terminate_on_pause &&
      published.scale_factor == 125 && published.lossless_scaling_target_fps == 119.88 &&
      published.steam_id == "123456" && published.lutris_id == "42" && published.playnite_id == "target-game" &&
      published.art_version == "target-art" && published.id_aliases.at(0) == "old-target-id" &&
      published.config_overrides.at("output_name") == "target-output", "target catalog fields were lost");
    bool teardown_ran = false;
    catalog::prepare_launch(f.state.view(), [&]() {
      teardown_ran = true;
      f.expect_active(prep);
    });
    require(teardown_ran && !f.state.pending && value(f.state.environment, "catalog") == "latest-target",
      "target environment promoted before teardown or not promoted after it");
  }

  void repeated_retry() {
    fixture_t f;
    {
      std::unique_lock writer {f.configuration};
      for (int i = 0; i < 3; ++i) {
        auto callback = std::async(std::launch::async, [&]() {
          if (f.scheduler.size()) f.scheduler.run_one();
          else f.refresh();
        });
        finishes(callback, "retry blocked instead of rescheduling");
        f.expect_retry();
      }
    }
    require(f.scheduler.delays.size() == 3, "retry count differs from attempted admissions");
    f.scheduler.run_one();
    require(f.reads == 1 && f.scheduler.size() == 0, "repeated retry published more than once");
  }
}

int main(int argc, char **argv) {
  const std::map<std::string, std::function<void()>> cases {
    {"idle", idle_refresh}, {"active-storage", active_storage}, {"missing-active-entry", missing_active_entry},
    {"ipc-teardown", ipc_teardown}, {"config-contention", configuration_contention},
    {"immediate-file-config-cycle", []() { file_configuration_cycle(false); }},
    {"deferred-file-config-cycle", []() { file_configuration_cycle(true); }},
    {"config-recursion", config_recursion}, {"config-scope-exception", config_scope_exception},
    {"failed-parse", failed_parse}, {"undo-environment", undo_environment},
    {"failed-teardown", failed_teardown}, {"teardown-idle-retry", teardown_then_idle_retry},
    {"repeated-retry", repeated_retry}, {"target-metadata", target_metadata}, {"termination-refresh", termination_refresh}
  };
  std::string current = argc > 1 ? argv[1] : "all";
  try {
    if (argc == 1) {
      for (const auto &[name, scenario] : cases) {
        current = name;
        scenario();
        std::cout << name << ": PASS\n";
      }
      return 0;
    }
    require(argc == 2, "provide one scenario name or --list");
    if (std::string(argv[1]) == "--list") {
      for (const auto &[name, scenario] : cases) std::cout << name << '\n';
      return 0;
    }
    cases.at(argv[1])();
    std::cout << argv[1] << ": PASS\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << current << ": FAIL: " << error.what() << '\n';
    return 1;
  }
}
