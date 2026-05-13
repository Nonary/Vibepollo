/**
 * @file src/session_history_writer.cpp
 * @brief Internal writer queue, SQLite lifecycle, and readback helpers for session history.
 */

// standard includes
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <future>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

// local includes
#include "session_history_writer.h"
#include "session_history_storage.h"
#include "logging.h"

using namespace std::literals;

namespace session_history::writer {
  namespace {

    enum class cmd_type {
      begin_session,
      end_session,
      delete_session,
      insert_sample,
      insert_event,
      prune,
      set_end_time_for_tests,
      stop
    };

    struct write_cmd_t {
      cmd_type type;
      session_metadata_t metadata;
      session_sample_t sample;
      session_event_t event;
      std::string uuid;
      double timestamp_unix = 0;
      std::shared_ptr<std::promise<delete_result_e>> delete_completion;
      std::shared_ptr<std::promise<bool>> bool_completion;
    };

    storage::db_ptr g_write_db;
    std::mutex g_db_mutex;

    std::mutex g_queue_mutex;
    std::condition_variable g_queue_cv;
    std::deque<write_cmd_t> g_priority_queue;
    std::deque<write_cmd_t> g_regular_queue;
    std::deque<write_cmd_t> g_sample_queue;

    std::thread g_writer_thread;
    std::atomic<bool> g_running {false};
    std::filesystem::path g_history_db_path;

    std::mutex g_settings_mutex;
    settings_t g_settings;

    constexpr int MAX_HISTORY_SESSIONS = 50;
    constexpr std::size_t MAX_PENDING_WRITE_COMMANDS = 4096;
    constexpr int DEFAULT_DETAIL_SAMPLE_LIMIT = 1800;
    constexpr int DEFAULT_DETAIL_EVENT_LIMIT = 500;
    constexpr int MAX_SAMPLES_PER_SESSION = 7200;
    constexpr int MAX_EVENTS_PER_SESSION = 2000;
    constexpr int SESSION_HISTORY_SCHEMA_VERSION = 7;
    constexpr auto DELETE_WAIT_TIMEOUT = std::chrono::seconds(5);
    constexpr std::size_t DEFAULT_MAX_PENDING_PRIORITY_COMMANDS = 1024;
    constexpr std::size_t DEFAULT_MAX_PENDING_REGULAR_COMMANDS = 2048;
    constexpr std::size_t DEFAULT_MAX_PENDING_SAMPLE_COMMANDS = 4096;
    constexpr std::size_t DEFAULT_MAX_SAMPLES_PER_BATCH = 128;

    struct queue_limits_t {
      std::size_t priority_limit = DEFAULT_MAX_PENDING_PRIORITY_COMMANDS;
      std::size_t regular_limit = DEFAULT_MAX_PENDING_REGULAR_COMMANDS;
      std::size_t sample_limit = DEFAULT_MAX_PENDING_SAMPLE_COMMANDS;
      std::size_t sample_batch_size = DEFAULT_MAX_SAMPLES_PER_BATCH;
    };

    struct delete_completion_t {
      std::shared_ptr<std::promise<delete_result_e>> completion;
      delete_result_e result = delete_result_e::failed;
    };

    struct bool_completion_t {
      std::shared_ptr<std::promise<bool>> completion;
      bool result = false;
    };

    std::mutex g_limits_mutex;
    queue_limits_t g_queue_limits;
    std::atomic<std::uint64_t> g_dropped_sample_count {0};
    std::atomic<bool> g_writer_degraded {false};

    settings_t current_settings() {
      std::lock_guard lk {g_settings_mutex};
      return g_settings;
    }

    queue_limits_t current_queue_limits() {
      std::lock_guard lk {g_limits_mutex};
      return g_queue_limits;
    }

    storage::db_ptr open_request_read_db() {
      std::filesystem::path db_path;
      {
        std::lock_guard lk {g_db_mutex};
        db_path = g_history_db_path;
      }
      if (db_path.empty()) {
        return {};
      }

      storage::db_ptr read_db;
      if (!storage::open_read_db(db_path.string(), read_db)) {
        return {};
      }
      sqlite3_busy_timeout(read_db.get(), 3000);
      storage::exec(read_db.get(), "PRAGMA foreign_keys = ON");
      return read_db;
    }

    storage::prune_options_t current_prune_options() {
      const auto settings = current_settings();

      storage::prune_options_t options;
      options.max_history_sessions = MAX_HISTORY_SESSIONS;
      if (settings.ttl_days > 0) {
        options.prune_sessions_ended_before_unix =
          storage::now_unix() - (static_cast<double>(settings.ttl_days) * 24.0 * 60.0 * 60.0);
      }
      options.max_db_size_bytes = settings.max_db_size_bytes;
      return options;
    }

    bool has_pending_commands() {
      return !g_priority_queue.empty() || !g_regular_queue.empty() || !g_sample_queue.empty();
    }

    bool enqueue(write_cmd_t cmd) {
      {
        std::lock_guard lk {g_queue_mutex};
        if (!g_running.load(std::memory_order_acquire) || !g_write_db) {
          return false;
        }
        const auto limits = current_queue_limits();
        if (cmd.type == cmd_type::insert_sample) {
          if (g_sample_queue.size() >= limits.sample_limit) {
            g_dropped_sample_count.fetch_add(1, std::memory_order_relaxed);
            g_writer_degraded.store(true, std::memory_order_relaxed);
            BOOST_LOG(warning) << "session_history: dropping sample because writer sample queue is full";
            return false;
          }
          g_sample_queue.push_back(std::move(cmd));
        } else if (cmd.type == cmd_type::insert_event) {
          if (g_priority_queue.size() >= limits.priority_limit) {
            BOOST_LOG(error) << "session_history: priority writer queue is full; rejecting command";
            return false;
          }
          g_priority_queue.push_back(std::move(cmd));
        } else {
          if (g_priority_queue.size() >= limits.priority_limit) {
            BOOST_LOG(error) << "session_history: priority writer queue is full; rejecting command";
            return false;
          }
          g_priority_queue.push_back(std::move(cmd));
        }
      }
      g_queue_cv.notify_one();
      return true;
    }

    std::vector<write_cmd_t> drain_queue() {
      std::vector<write_cmd_t> batch;
      bool had_remaining_samples = false;
      {
        std::unique_lock lk {g_queue_mutex};
        g_queue_cv.wait_for(lk, 500ms, [] { return has_pending_commands(); });
        if (!has_pending_commands()) {
          return batch;
        }

        while (!g_priority_queue.empty()) {
          batch.push_back(std::move(g_priority_queue.front()));
          g_priority_queue.pop_front();
        }
        while (!g_regular_queue.empty()) {
          batch.push_back(std::move(g_regular_queue.front()));
          g_regular_queue.pop_front();
        }

        const auto sample_batch_size = std::max<std::size_t>(1, current_queue_limits().sample_batch_size);
        const auto take_samples = std::min(sample_batch_size, g_sample_queue.size());
        for (std::size_t i = 0; i < take_samples; ++i) {
          batch.push_back(std::move(g_sample_queue.front()));
          g_sample_queue.pop_front();
        }
        had_remaining_samples = !g_sample_queue.empty();
      }
      if (had_remaining_samples) {
        g_queue_cv.notify_one();
      }
      return batch;
    }

    bool apply_write_cmd(
      sqlite3 *db,
      write_cmd_t &cmd,
      std::vector<delete_completion_t> &delete_completions,
      std::vector<bool_completion_t> &bool_completions) {
      switch (cmd.type) {
        case cmd_type::begin_session:
          return storage::process_begin(db, cmd.metadata);
        case cmd_type::end_session:
          return storage::process_end(db, cmd.uuid);
        case cmd_type::delete_session: {
          auto result = storage::process_delete(db, cmd.uuid);
          if (cmd.delete_completion) {
            delete_result_e delete_result = delete_result_e::failed;
            switch (result) {
              case storage::delete_apply_e::deleted:
                delete_result = delete_result_e::deleted;
                break;
              case storage::delete_apply_e::not_found:
                delete_result = delete_result_e::not_found;
                break;
              case storage::delete_apply_e::failed:
                delete_result = delete_result_e::failed;
                break;
            }
            delete_completions.push_back(delete_completion_t {cmd.delete_completion, delete_result});
          }
          return result != storage::delete_apply_e::failed;
        }
        case cmd_type::insert_sample:
          return storage::process_sample(db, cmd.sample, MAX_SAMPLES_PER_SESSION);
        case cmd_type::insert_event:
          return storage::process_event(db, cmd.event, MAX_EVENTS_PER_SESSION);
        case cmd_type::prune: {
          const bool pruned = storage::process_prune(db, current_prune_options());
          if (cmd.bool_completion) {
            bool_completions.push_back(bool_completion_t {cmd.bool_completion, pruned});
          }
          return pruned;
        }
        case cmd_type::set_end_time_for_tests: {
#ifdef SUNSHINE_TESTS
          const bool updated = storage::force_set_end_time(db, cmd.uuid, cmd.timestamp_unix);
          if (cmd.bool_completion) {
            bool_completions.push_back(bool_completion_t {cmd.bool_completion, updated});
          }
          return updated;
#else
          return false;
#endif
        }
        case cmd_type::stop:
          return true;
      }
      return true;
    }

    void resolve_completions(
      std::vector<delete_completion_t> &delete_completions,
      std::vector<bool_completion_t> &bool_completions,
      bool committed) {
      for (auto &completion : delete_completions) {
        completion.completion->set_value(committed ? completion.result : delete_result_e::failed);
      }
      delete_completions.clear();

      for (auto &completion : bool_completions) {
        completion.completion->set_value(committed && completion.result);
      }
      bool_completions.clear();
    }

    bool process_batch(std::vector<write_cmd_t> &batch) {
      if (batch.empty()) {
        return true;
      }

      std::lock_guard db_lock {g_db_mutex};
      if (!g_write_db) {
        for (auto &cmd : batch) {
          if (cmd.delete_completion) {
            cmd.delete_completion->set_value(delete_result_e::failed);
          }
          if (cmd.bool_completion) {
            cmd.bool_completion->set_value(false);
          }
        }
        return false;
      }

      if (!storage::exec(g_write_db.get(), "BEGIN IMMEDIATE TRANSACTION")) {
        BOOST_LOG(error) << "session_history: failed to begin transaction";
        for (auto &cmd : batch) {
          if (cmd.delete_completion) {
            cmd.delete_completion->set_value(delete_result_e::failed);
          }
          if (cmd.bool_completion) {
            cmd.bool_completion->set_value(false);
          }
        }
        return false;
      }

      std::vector<delete_completion_t> delete_completions;
      std::vector<bool_completion_t> bool_completions;
      bool ok = true;
      for (auto &cmd : batch) {
        if (!apply_write_cmd(g_write_db.get(), cmd, delete_completions, bool_completions)) {
          ok = false;
          break;
        }
      }

      bool committed = false;
      if (ok) {
        committed = storage::exec(g_write_db.get(), "COMMIT");
        if (!committed) {
          BOOST_LOG(error) << "session_history: failed to commit transaction";
        }
      }
      if (!committed) {
        storage::exec(g_write_db.get(), "ROLLBACK");
      }

      if (committed) {
        const bool saw_prune = std::any_of(batch.begin(), batch.end(), [](const write_cmd_t &cmd) {
          return cmd.type == cmd_type::prune;
        });
        if (saw_prune) {
          storage::checkpoint(g_write_db.get());
          if (!g_history_db_path.empty()) {
            storage::tighten_history_db_permissions(g_history_db_path);
          }
        }
      }

      resolve_completions(delete_completions, bool_completions, committed);
      return committed;
    }

    void writer_loop() {
      while (g_running.load(std::memory_order_acquire)) {
        auto batch = drain_queue();
        if (batch.empty()) {
          continue;
        }
        if (!process_batch(batch)) {
          BOOST_LOG(error) << "session_history: failed to process writer batch";
        }
      }

      while (true) {
        std::vector<write_cmd_t> remaining;
        {
          std::lock_guard lk {g_queue_mutex};
          if (!has_pending_commands()) {
            break;
          }
          while (!g_priority_queue.empty()) {
            remaining.push_back(std::move(g_priority_queue.front()));
            g_priority_queue.pop_front();
          }
          while (!g_regular_queue.empty()) {
            remaining.push_back(std::move(g_regular_queue.front()));
            g_regular_queue.pop_front();
          }
          while (!g_sample_queue.empty()) {
            remaining.push_back(std::move(g_sample_queue.front()));
            g_sample_queue.pop_front();
          }
        }
        if (!process_batch(remaining)) {
          BOOST_LOG(error) << "session_history: failed to flush writer batch during shutdown";
        }
      }
    }

  }  // namespace

  void update_settings(const settings_t &settings) {
    std::lock_guard lk {g_settings_mutex};
    g_settings.enabled = settings.enabled;
    g_settings.ttl_days = std::max(settings.ttl_days, 0);
    g_settings.max_db_size_bytes = settings.max_db_size_bytes;
  }

  bool is_enabled() {
    return current_settings().enabled;
  }

  bool is_available() {
    std::lock_guard lk {g_db_mutex};
    return is_enabled() && g_running.load(std::memory_order_acquire) && static_cast<bool>(g_write_db);
  }

  void init(const std::string &db_path) {
    BOOST_LOG(info) << "session_history: initializing database at " << db_path;
    {
      std::lock_guard lk {g_db_mutex};
      g_history_db_path = std::filesystem::path {db_path};
    }

    if (!is_enabled()) {
      BOOST_LOG(info) << "session_history: disabled by configuration";
      return;
    }

    {
      std::lock_guard lk {g_db_mutex};
      if (!storage::open_write_db(db_path, g_write_db)) {
        return;
      }
      sqlite3_busy_timeout(g_write_db.get(), 3000);
      storage::exec(g_write_db.get(), "PRAGMA foreign_keys = ON");

      if (!storage::apply_schema_and_migrations(g_write_db.get(), SESSION_HISTORY_SCHEMA_VERSION)) {
        g_write_db.reset();
        return;
      }
    }
    storage::tighten_history_db_permissions(g_history_db_path);

    g_running.store(true, std::memory_order_release);
    g_writer_thread = std::thread {writer_loop};

    (void) enqueue_prune();

    BOOST_LOG(info) << "session_history: initialized";
  }

  void shutdown() {
    g_running.store(false, std::memory_order_release);
    g_queue_cv.notify_all();

    if (g_writer_thread.joinable()) {
      g_writer_thread.join();
    }

    {
      std::lock_guard lk {g_db_mutex};
      g_write_db.reset();
      g_history_db_path.clear();
    }
  }

  bool enqueue_begin(const session_metadata_t &metadata) {
    write_cmd_t cmd;
    cmd.type = cmd_type::begin_session;
    cmd.metadata = metadata;
    return enqueue(std::move(cmd));
  }

  bool enqueue_end(const std::string &uuid) {
    write_cmd_t cmd;
    cmd.type = cmd_type::end_session;
    cmd.uuid = uuid;
    return enqueue(std::move(cmd));
  }

  bool enqueue_event(const session_event_t &event) {
    write_cmd_t cmd;
    cmd.type = cmd_type::insert_event;
    cmd.event = event;
    return enqueue(std::move(cmd));
  }

  bool enqueue_sample(session_sample_t sample) {
    write_cmd_t cmd;
    cmd.type = cmd_type::insert_sample;
    cmd.sample = std::move(sample);
    if (cmd.sample.timestamp_unix <= 0) {
      cmd.sample.timestamp_unix = storage::now_unix();
    }
    return enqueue(std::move(cmd));
  }

  bool enqueue_prune() {
    write_cmd_t cmd;
    cmd.type = cmd_type::prune;
    return enqueue(std::move(cmd));
  }

  std::vector<session_summary_t> list_sessions(int limit, int offset) {
    if (auto read_db = open_request_read_db()) {
      return storage::read_session_summaries(read_db.get(), limit, offset);
    }

    std::lock_guard lk {g_db_mutex};
    auto *db = g_write_db.get();
    if (!db) return {};
    return storage::read_session_summaries(db, limit, offset);
  }

  std::optional<session_detail_t> get_session_detail(const std::string &uuid, bool include_all) {
    if (auto read_db = open_request_read_db()) {
      return storage::read_session_detail(
        read_db.get(),
        uuid,
        include_all,
        DEFAULT_DETAIL_SAMPLE_LIMIT,
        DEFAULT_DETAIL_EVENT_LIMIT);
    }

    std::lock_guard lk {g_db_mutex};
    auto *db = g_write_db.get();
    if (!db) return std::nullopt;
    return storage::read_session_detail(
      db,
      uuid,
      include_all,
      DEFAULT_DETAIL_SAMPLE_LIMIT,
      DEFAULT_DETAIL_EVENT_LIMIT);
  }

  delete_result_e delete_session(const std::string &uuid) {
    if (!is_available()) {
      return delete_result_e::unavailable;
    }

    auto completion = std::make_shared<std::promise<delete_result_e>>();
    auto result = completion->get_future();

    write_cmd_t cmd;
    cmd.type = cmd_type::delete_session;
    cmd.uuid = uuid;
    cmd.delete_completion = completion;
    if (!enqueue(std::move(cmd))) {
      return delete_result_e::unavailable;
    }

    if (result.wait_for(DELETE_WAIT_TIMEOUT) != std::future_status::ready) {
      BOOST_LOG(error) << "session_history: timed out waiting for delete completion for uuid=" << uuid;
      return delete_result_e::timeout;
    }

    return result.get();
  }

  history_status_t get_status() {
    history_status_t status;
    {
      std::lock_guard lk {g_db_mutex};
      status.available = g_running.load(std::memory_order_acquire) && static_cast<bool>(g_write_db);
    }
    {
      std::lock_guard lk {g_queue_mutex};
      status.pending_priority_commands = g_priority_queue.size();
      status.pending_regular_commands = g_regular_queue.size();
      status.pending_samples = g_sample_queue.size();
    }
    status.degraded = g_writer_degraded.load(std::memory_order_relaxed);
    status.dropped_samples = g_dropped_sample_count.load(std::memory_order_relaxed);
    return status;
  }

#ifdef SUNSHINE_TESTS
  bool set_session_end_time_for_tests(const std::string &uuid, double end_time_unix) {
    if (!is_available()) {
      return false;
    }

    auto completion = std::make_shared<std::promise<bool>>();
    auto result = completion->get_future();

    write_cmd_t cmd;
    cmd.type = cmd_type::set_end_time_for_tests;
    cmd.uuid = uuid;
    cmd.timestamp_unix = end_time_unix;
    cmd.bool_completion = completion;
    if (!enqueue(std::move(cmd))) {
      return false;
    }

    if (result.wait_for(DELETE_WAIT_TIMEOUT) != std::future_status::ready) {
      return false;
    }
    return result.get();
  }

  bool prune_now_for_tests() {
    if (!is_available()) {
      return false;
    }

    auto completion = std::make_shared<std::promise<bool>>();
    auto result = completion->get_future();

    write_cmd_t cmd;
    cmd.type = cmd_type::prune;
    cmd.bool_completion = completion;
    if (!enqueue(std::move(cmd))) {
      return false;
    }

    if (result.wait_for(DELETE_WAIT_TIMEOUT) != std::future_status::ready) {
      return false;
    }
    return result.get();
  }

  void configure_queue_limits_for_tests(
    std::size_t priority_limit,
    std::size_t regular_limit,
    std::size_t sample_limit,
    std::size_t sample_batch_size) {
    std::lock_guard lk {g_limits_mutex};
    g_queue_limits.priority_limit = std::max<std::size_t>(1, priority_limit);
    g_queue_limits.regular_limit = std::max<std::size_t>(1, regular_limit);
    g_queue_limits.sample_limit = std::max<std::size_t>(1, sample_limit);
    g_queue_limits.sample_batch_size = std::max<std::size_t>(1, sample_batch_size);
    g_dropped_sample_count.store(0, std::memory_order_relaxed);
    g_writer_degraded.store(false, std::memory_order_relaxed);
  }

  void reset_queue_limits_for_tests() {
    std::lock_guard lk {g_limits_mutex};
    g_queue_limits = queue_limits_t {};
    g_dropped_sample_count.store(0, std::memory_order_relaxed);
    g_writer_degraded.store(false, std::memory_order_relaxed);
  }
#endif

}  // namespace session_history::writer
