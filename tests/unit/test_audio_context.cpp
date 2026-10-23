/** Direct production-component tests; only endpoints/processes/joins are faked. */
#include "src/app_audio_termination.h"
#include "src/audio_context.h"
#include "src/audio_display_transition.h"
#include "src/audio_role_control.h"
#include "src/config_display_transition.h"
#include "src/webrtc_capture_owner.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <iostream>
#include <latch>
#include <map>
#include <set>
#include <stdexcept>
#include <system_error>

namespace {
  enum class role_e {
    console,
    multimedia,
    communications
  };
  using roles_t = audio::role_control_t<role_e, 3>;
  using ids_t = roles_t::ids_t;
  const ids_t original {L"dp", L"dp", L"headset"};

  void require(bool condition, const char *message) {
    if (!condition) {
      throw std::runtime_error(message);
    }
  }

  struct endpoints_t {
    std::mutex mutex;
    ids_t defaults {original};
    std::set<std::wstring> available {L"dp", L"headset", L"fallback", L"stream", L"next", L"user"};
    std::map<role_e, int> failures;
    std::map<std::wstring, std::wstring> aliases;
    std::vector<std::string> events;
    std::function<void(role_e, const std::wstring &)> before_write;
    std::function<void()> during_resolve;
    int writes {}, captures {}, created {}, destroyed {}, pinned {};
    bool format_failure {}, factory_failure {}, info_failure {};

    ids_t read() {
      std::scoped_lock lock(mutex);
      return defaults;
    }

    long write(role_e role, const std::wstring &id) {
      // MMDevice callbacks can run while an RPC is in flight.
      if (before_write) {
        before_write(role, id);
      }
      std::scoped_lock lock(mutex);
      if (!available.contains(id) || failures[role]-- > 0) {
        return -1;
      }
      defaults[static_cast<std::size_t>(role)] = id;
      events.push_back("write:" + std::string(id.begin(), id.end()));
      ++writes;
      return 0;
    }

    std::optional<std::wstring> resolve(const std::wstring &id) {
      if (during_resolve) {
        during_resolve();
      }
      std::scoped_lock lock(mutex);
      auto match = aliases.contains(id) ? aliases[id] : id;
      return available.contains(match) ? std::make_optional(match) : std::nullopt;
    }

    void exclusive_display() {
      std::scoped_lock lock(mutex);
      events.push_back("display");
      available.erase(L"dp");
      defaults = {L"fallback", L"stream", L"headset"};
    }

    void user(role_e role, const std::wstring &id = L"user") {
      std::scoped_lock lock(mutex);
      defaults[static_cast<std::size_t>(role)] = id;
    }

    void display_returns() {
      std::scoped_lock lock(mutex);
      available.insert(L"dp");
    }
  };

  struct fixture_t;

  // This is an MMDevice adapter, not an alternative role-control model: every
  // capture, assignment, direct restore, queue and retry decision is delegated
  // to the exact role_control_t used by platform/windows/audio.cpp.
  class device_control_t final: public platf::audio_control_t {
  public:
    explicit device_control_t(fixture_t &fixture);
    ~device_control_t() override;
    std::optional<platf::sink_t> sink_info() override;
    int set_sink(const std::string &) override;

    int restore_sink(const std::string &) override {
      return roles_.restore();
    }

    void reset_default_device(const std::string &) override {
      roles_.reset_failed(L"stream");
    }

    int set_capture_sink(const std::string &) override;

    bool is_sink_available(const std::string &) override {
      return true;
    }

    std::unique_ptr<platf::mic_t> microphone(const std::uint8_t *, int, std::uint32_t, std::uint32_t, bool, bool) override {
      return {};
    }

    roles_t &roles() {
      return roles_;
    }

  private:
    fixture_t &fixture_;
    roles_t roles_;
  };

  struct fixture_t {
    struct job_t {
      std::wstring steam;
      roles_t::pending_list_t roles;
      std::uint64_t epoch;
    };

    endpoints_t os;
    roles_t::registry_t registry;
    std::vector<job_t> jobs;
    std::vector<std::jthread> retired;
    bool app_active {}, scheduler_failure {};
    std::string virtual_sink {"stream"};
    std::function<void()> observe_factory;
    audio::context_manager_t manager;

    explicit fixture_t(bool retain = true):
        manager {{
          [this]() -> std::unique_ptr<platf::audio_control_t> {
            if (observe_factory) {
              observe_factory();
            }
            if (os.factory_failure) {
              return {};
            }
            return std::make_unique<device_control_t>(*this);
          },
          [this] {
            return app_active;
          },
          [] {
            return std::string {};
          },
          {},
          retain,
        }} {}

    roles_t::device_api_t devices() {
      return {
        [this] {
          return os.read();
        },
        [this](role_e role, const std::wstring &id) {
          return os.write(role, id);
        },
        [this](const std::wstring &id) {
          return os.resolve(id);
        },
        [this](std::jthread thread) {
          if (thread.joinable()) {
            thread.request_stop();
            retired.push_back(std::move(thread));
          }
        },
        [this](const std::wstring &steam, roles_t::pending_list_t roles, std::uint64_t epoch) {
          if (scheduler_failure) {
            throw std::runtime_error("OS scheduler failure");
          }
          jobs.push_back({steam, std::move(roles), epoch});
        },
        [](std::uint64_t) {
        },
        {},
      };
    }

    // Deterministic OS scheduler: run the real retry component on a real
    // registered worker thread. No fake epoch/ownership predicates are used.
    std::vector<roles_t::retry_result_e> retry(const job_t &job) {
      std::latch published(1);
      auto token = std::make_shared<std::atomic_bool>(true);
      std::vector<roles_t::retry_result_e> result;
      std::promise<void> finished;
      auto completion = finished.get_future();
      roles_t retry_control(registry, devices());
      std::jthread thread([&](std::stop_token stop) {
        published.wait();
        for (const auto &role : job.roles) {
          result.push_back(retry_control.retry(role, stop, token, job.epoch));
        }
        finished.set_value();
      });
      const auto worker_id = thread.get_id();
      {
        std::scoped_lock lock(registry.mutex);
        registry.token = token;
        registry.worker = std::move(thread);
      }
      published.count_down();
      completion.wait();
      std::jthread joined;
      {
        std::scoped_lock lock(registry.mutex);
        if (registry.worker.get_id() == worker_id) {
          joined = std::move(registry.worker);
        }
      }
      if (joined.joinable()) {
        joined.join();
      }
      return result;
    }

    void terminal() {
      app_active = false;
      manager.app_termination_requested();
    }
  };

  device_control_t::device_control_t(fixture_t &f):
      fixture_ {f},
      roles_ {f.registry, f.devices()} {
    ++f.os.created;
  }

  device_control_t::~device_control_t() {
    ++fixture_.os.destroyed;
  }

  std::optional<platf::sink_t> device_control_t::sink_info() {
    if (fixture_.os.info_failure) {
      return std::nullopt;
    }
    const auto ids = fixture_.os.read();
    roles_.capture(ids);
    fixture_.os.events.push_back("capture");
    ++fixture_.os.captures;
    return platf::sink_t {std::string(ids[0].begin(), ids[0].end()), platf::sink_t::null_t {fixture_.virtual_sink, fixture_.virtual_sink, fixture_.virtual_sink}};
  }

  int device_control_t::set_sink(const std::string &id) {
    if (fixture_.os.format_failure) {
      return -1;
    }
    return roles_.assign(std::wstring(id.begin(), id.end()));
  }

  int device_control_t::set_capture_sink(const std::string &) {
    ++fixture_.os.pinned;
    return 0;
  }

  struct capture_t {
    fixture_t &fixture;
    audio::audio_ctx_ref_t ref;

    explicit capture_t(fixture_t &f):
        fixture {f},
        ref {f.manager.acquire()} {
      f.manager.owner_acquired();
    }

    ~capture_t() {
      release();
    }

    void release() {
      if (ref) {
        fixture.manager.owner_released();
        ref.release();
      }
    }

    int assign(const std::string &sink = "stream", bool capture_only = false) {
      return fixture.manager.assign(*ref.get(), sink, capture_only);
    }
  };

  void pre_display_first_sink() {
    fixture_t f;
    auto hold = f.manager.before_display_change(true);
    f.os.exclusive_display();
    {
      capture_t capture(f);
      require(capture.assign() == 0, "stream assignment failed");
      f.os.display_returns();
      f.terminal();
    }
    audio::join_capture_and_release(hold, [] {
    });
    const auto observed = f.os.read();
    std::cout << "observed defaults: " << std::string(observed[0].begin(), observed[0].end()) << '\n';
    require(observed == original, "pre-display DP/mixed defaults were replaced by fallback");
    require(f.os.events.front() == "capture", "endpoint capture occurred after display mutation");
  }

  void first_sink_preservation() {
    fixture_t f;
    device_control_t control(f);
    control.sink_info();
    f.os.exclusive_display();
    require(control.set_sink("stream") == 0, "assignment failed");
    require(control.set_sink("stream") == 0, "repeat assignment failed");
    f.os.display_returns();
    control.restore_sink("");
    require(f.os.read() == original, "first/repeated sink assignment recaptured fallback");
  }

  void newer_choices() {
    fixture_t f;
    device_control_t control(f);
    control.sink_info();
    control.set_sink("stream");
    f.os.user(role_e::console);
    f.os.before_write = [&](role_e role, const std::wstring &id) {
      if (role == role_e::multimedia && id == L"dp") {
        f.os.user(role_e::communications);
      }
    };
    control.restore_sink("");
    const auto observed = f.os.read();
    std::cout << "observed communications choice: " << std::string(observed[2].begin(), observed[2].end()) << '\n';
    require(f.os.read() == ids_t {L"user", L"dp", L"user"}, "restore overwrote a later user choice");
  }

  void role_read_write_gap() {
    fixture_t f;
    device_control_t control(f);
    control.sink_info();
    control.set_sink("stream");
    f.os.before_write = [&](role_e role, const std::wstring &id) {
      if (role == role_e::console && id == L"dp") {
        // The user changes the same role after the final default read, while
        // SetDefaultEndpoint is in flight. Windows offers no compare-and-set.
        f.os.user(role);
      }
    };
    control.restore_sink("");
    require(f.os.read()[0] == L"dp", "device fake no longer models the separate read/write boundary");
    std::cout << "observed read/write gap choice: dp (late user choice overwritten)\n";
  }

  void delayed_arrival() {
    fixture_t f;
    auto hold = f.manager.before_display_change(true);
    f.os.exclusive_display();
    {
      capture_t capture(f);
      capture.assign();
      f.terminal();
    }
    hold.reset();
    require(f.jobs.size() == 1 && f.jobs.back().roles.size() == 2, "missing display roles were not queued separately");
    auto job = f.jobs.back();
    auto missing = f.retry(job);
    require(std::all_of(missing.begin(), missing.end(), [](auto r) {
              return r == roles_t::retry_result_e::no_device;
            }),
            "retry did not retain missing endpoints");
    f.os.display_returns();
    auto returned = f.retry(job);
    require(std::all_of(returned.begin(), returned.end(), [](auto r) {
              return r == roles_t::retry_result_e::restored;
            }),
            "arrival did not restore queued roles");
    require(f.os.read() == original, "delayed DP restoration lost mixed defaults");
  }

  void retry_user_choice() {
    fixture_t f;
    device_control_t control(f);
    control.sink_info();
    control.set_sink("stream");
    f.os.available.erase(L"dp");
    control.restore_sink("");
    control.reset_default_device("");
    auto job = f.jobs.back();
    f.os.display_returns();
    f.os.during_resolve = [&] {
      f.os.user(role_e::console);
    };
    const auto result = f.retry(job);
    require(result[0] == roles_t::retry_result_e::no_longer_owned && f.os.read()[0] == L"user", "retry overwrote a choice made during endpoint lookup");
  }

  void inherited_recovery() {
    fixture_t f;
    device_control_t old(f);
    old.sink_info();
    old.set_sink("stream");
    f.os.available.erase(L"dp");
    old.restore_sink("");
    old.reset_default_device("");
    device_control_t next(f);
    next.sink_info();
    next.set_sink("stream");
    f.os.display_returns();
    next.restore_sink("");
    require(f.os.read() == original, "successor assignment discarded inherited role recovery");
  }

  void late_worker_cannot_overwrite_successor() {
    fixture_t f;
    device_control_t old(f);
    old.sink_info();
    old.set_sink("stream");
    f.os.available.erase(L"dp");
    old.restore_sink("");
    old.reset_default_device("");
    const auto job = f.jobs.back();
    f.os.display_returns();
    std::promise<void> entered, unblock;
    auto ready = entered.get_future();
    auto proceed = unblock.get_future().share();
    f.os.before_write = [&](role_e role, const std::wstring &id) {
      if (role == role_e::console && id == L"dp") {
        entered.set_value();
        proceed.wait();
      }
    };
    auto retry = std::async(std::launch::async, [&] {
      return f.retry(job);
    });
    ready.wait();
    device_control_t next(f);
    next.sink_info();
    next.set_sink("next");
    unblock.set_value();
    const auto result = retry.get();
    require(result[0] == roles_t::retry_result_e::inactive, "obsolete in-flight worker retained authority");
    require(f.os.read() == ids_t {L"next", L"next", L"next"}, "late endpoint RPC overwrote the successor assignment");
  }

  void scheduler_failure() {
    fixture_t f;
    device_control_t control(f);
    control.sink_info();
    control.set_sink("stream");
    f.os.available.erase(L"dp");
    control.restore_sink("");
    f.scheduler_failure = true;
    bool failed = false;
    try {
      control.reset_default_device("");
    } catch (const std::runtime_error &) {
      failed = true;
    }
    require(failed && f.registry.pending.empty() && f.registry.steam.empty(), "scheduler failure left a phantom retry handoff");
  }

  void aborted_launch() {
    fixture_t f;
    auto hold = f.manager.before_display_change(true);
    f.os.user(role_e::console);
    hold.reset();
    require(f.os.created == 1 && f.os.destroyed == 1 && f.os.writes == 0, "aborted launch routed audio or leaked its control");
    require(f.os.read()[0] == L"user", "aborted launch restored a stale snapshot");
  }

  void failures_and_capture_only() {
    fixture_t f;
    f.os.factory_failure = true;
    require(!f.manager.before_display_change(true), "failed device construction produced a hold");
    f.os.factory_failure = false;
    f.os.info_failure = true;
    require(!f.manager.before_display_change(true), "failed endpoint enumeration produced a hold");
    f.os.info_failure = false;
    {
      auto hold = f.manager.before_display_change(false);
      require(!hold, "disabled audio captured endpoints");
    }
    {
      auto hold = f.manager.before_display_change(true);
      capture_t capture(f);
      f.os.format_failure = true;
      require(capture.assign() != 0, "failed format was accepted");
      f.terminal();
      capture.release();
      hold.reset();
      require(f.os.writes == 0, "failed format changed defaults on cleanup");
    }
    f.os.format_failure = false;
    {
      auto hold = f.manager.before_display_change(true);
      capture_t capture(f);
      require(capture.assign("dp", true) == 0, "capture-only pin failed");
      f.os.user(role_e::console);
      f.terminal();
      capture.release();
      hold.reset();
      require(f.os.writes == 0 && f.os.pinned == 1 && f.os.read()[0] == L"user", "capture-only operation routed/restored defaults");
    }
  }

  void partial_assignment() {
    fixture_t f;
    auto hold = f.manager.before_display_change(true);
    capture_t capture(f);
    f.os.failures[role_e::console] = 1;
    require(capture.assign() == 1, "partial assignment failure was not reported");
    f.terminal();
    capture.release();
    hold.reset();
    require(f.os.read() == original, "partial assignment failed to restore successful roles");
  }

  void pause_resume(bool abort_resume, bool resume) {
    fixture_t f;
    f.app_active = true;
    f.manager.app_started();
    auto hold = f.manager.before_display_change(true);
    {
      capture_t capture(f);
      capture.assign();
    }
    hold.reset();
    require(f.os.destroyed == 0 && f.os.read()[0] == L"stream", "pause released resumable ownership");
    if (resume) {
      auto pending = f.manager.before_display_change(true);
      if (!abort_resume) {
        capture_t capture(f);
        capture.assign();
      }
      pending.reset();
      require(f.os.created == 1 && f.os.writes == 3, "resume reassigned or recreated the retained control");
    }
    f.terminal();
    require(f.os.read() == original && f.os.destroyed == 1, "terminal event did not release retained ownership");
  }

  void concurrent_clients() {
    fixture_t f;
    f.app_active = true;
    f.manager.app_started();
    auto a = f.manager.before_display_change(true);
    capture_t first(f);
    first.assign();
    auto b = f.manager.before_display_change(true);
    capture_t second(f);
    second.assign();
    first.release();
    a.reset();
    require(f.os.created == 1 && f.os.destroyed == 0 && f.os.writes == 3, "departing peer restored/reassigned live capture");
    f.terminal();
    second.release();
    b.reset();
    require(f.os.read() == original && f.os.destroyed == 1, "last client did not restore exactly once");
  }

  void successor(bool abort) {
    fixture_t f;
    f.app_active = true;
    f.manager.app_started();
    auto a = f.manager.before_display_change(true);
    capture_t first(f);
    first.assign();
    f.manager.app_termination_requested();
    auto b = f.manager.before_display_change(true);
    f.manager.app_started();
    first.release();
    a.reset();
    require(f.os.writes == 3, "pending successor introduced an intermediate restore");
    if (!abort) {
      capture_t second(f);
      second.assign();
      f.terminal();
    }
    b.reset();
    require(f.os.read() == original && f.os.created == 1 && f.os.destroyed == 1, "successor/abort lost terminal restoration");
  }

  void join_hold(const char *transport) {
    fixture_t f;
    auto hold = f.manager.before_display_change(true);
    std::promise<void> captured, release;
    auto go = release.get_future();
    std::thread worker([&] {
      capture_t capture(f);
      capture.assign();
      captured.set_value();
      go.wait();
    });
    captured.get_future().wait();
    f.terminal();
    release.set_value();
    const auto after_join = [&] {
      std::cout << transport << " observed control destruction at join: " << f.os.destroyed << '\n';
      require(f.os.destroyed == 0 && f.os.read()[0] == L"stream", "snapshot released before capture join completed");
      f.os.events.push_back(std::string(transport) + ":joined");
    };
    if (std::string(transport) == "rtsp") {
      std::thread video([] {
      });
      std::vector<audio::join_stage_e> stages;
      bool watchdog_ended_before_release = false;
      audio::join_rtsp_capture(hold, video, worker, after_join, [&]() -> std::shared_ptr<void> {
        return std::shared_ptr<int>(new int, [&](int *value) {
          watchdog_ended_before_release = f.os.destroyed == 0;
          f.os.events.push_back("watchdog-ended");
          delete value;
        });
      },
                               [&](audio::join_stage_e stage) {
                                 stages.push_back(stage);
                               });
      require(stages == std::vector<audio::join_stage_e> {audio::join_stage_e::video, audio::join_stage_e::audio, audio::join_stage_e::control}, "RTSP production join order changed");
      require(std::find(f.os.events.begin(), f.os.events.end(), "watchdog-ended") != f.os.events.end(), "join watchdog outlived final release");
      require(watchdog_ended_before_release, "snapshot release was covered by the join watchdog");
    } else {
      webrtc_stream::WebRtcCaptureTeardown owner;
      owner.audio_context_guard = std::move(hold);
      owner.video_thread = std::thread([] {
      });
      owner.audio_thread = std::move(worker);
      owner.join_capture(after_join);
    }
    require(f.os.read() == original && f.os.destroyed == 1, "join boundary did not release terminal snapshot");
    const auto joined = std::find(f.os.events.begin(), f.os.events.end(), std::string(transport) + ":joined");
    require(joined != f.os.events.end() && std::find(joined, f.os.events.end(), "write:dp") != f.os.events.end(), "restoration preceded the joined event");
  }

  void failed_join() {
    fixture_t f;
    auto hold = f.manager.before_display_change(true);
    {
      capture_t capture(f);
      capture.assign();
      f.terminal();
    }
    try {
      audio::join_capture_and_release(hold, [] {
        throw std::runtime_error("join failed");
      });
    } catch (const std::runtime_error &) {}
    require(hold && f.os.destroyed == 0, "failed join released live ownership");
    audio::join_capture_and_release(hold, [] {
    });
    require(f.os.read() == original && f.os.destroyed == 1, "successful retry did not release the hold");
  }

  void non_retaining_platform() {
    fixture_t f(false);
    f.app_active = true;
    {
      capture_t capture(f);
      capture.assign();
    }
    require(f.os.read() == original && f.os.destroyed == 1, "non-retaining platform kept audio after capture");
  }

  void start_hold_failure() {
    for (bool video_started : {true, false}) {
      fixture_t f;
      auto pending = f.manager.before_display_change(true);
      webrtc_stream::CaptureThreadOwner owner;
      bool failed = false;
      try {
        webrtc_stream::start_capture_with_audio_hold(owner, pending, [&] {
          if (video_started) {
            owner.video_thread = std::thread([] {
            });
          }
          throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
        });
      } catch (const std::system_error &) {
        failed = true;
      }
      if (owner.video_thread.joinable()) {
        owner.video_thread.join();
      }
      std::cout << "failed audio start after video=" << (video_started ? "yes" : "no") << " observed destruction: " << f.os.destroyed << '\n';
      require(failed && !pending && !owner.audio_context_guard && !owner.audio_thread.joinable() && f.os.destroyed == 1 && f.os.writes == 0, "thread construction failure stranded the pre-display hold");
    }
  }

  void start_hold_success() {
    fixture_t f;
    auto pending = f.manager.before_display_change(true);
    webrtc_stream::WebRtcCaptureTeardown owner;
    std::promise<void> captured, release;
    auto go = release.get_future();
    webrtc_stream::start_capture_with_audio_hold(owner, pending, [&] {
      owner.video_thread = std::thread([] {
      });
      owner.audio_thread = std::thread([&] {
        capture_t capture(f);
        capture.assign();
        captured.set_value();
        go.wait();
      });
    });
    captured.get_future().wait();
    require(!pending && owner.audio_context_guard && f.os.destroyed == 0, "successful thread creation lost its hold transfer");
    f.terminal();
    release.set_value();
    owner.join_capture([&] {
      require(f.os.destroyed == 0, "successful start lost hold before join");
    });
    require(f.os.destroyed == 1 && f.os.read() == original, "successful start/join did not restore terminal audio");
  }

  void config_effective_before_revert() {
    fixture_t f;
    bool enabled = false, install_drivers = false;
    f.virtual_sink = "old";
    std::string factory_sink;
    bool factory_install = false;
    int captured_at_revert = -1;
    f.observe_factory = [&] {
      factory_sink = f.virtual_sink;
      factory_install = install_drivers;
    };
    {
      audio::display_change_hold_t request([&] {
        return f.manager.before_display_change(enabled);
      });
      config::display_transition::apply([&] {
        // Config-file parsing/install is the external edge. The production
        // sequencer owns installation -> predicate -> capture -> dispatch.
        enabled = true;
        install_drivers = true;
        f.virtual_sink = "next";
        f.os.events.push_back("installed");
        return config::display_transition::state_t {true, true, true, false};
      },
                                        [] {
                                          return false;
                                        },
                                        std::ref(request),
                                        [&](const auto &state) {
                                          require(!state.allow_disabled_recovery(), "REVERT arguments changed");
                                          captured_at_revert = f.os.captures;
                                          f.os.events.push_back("revert");
                                          f.os.exclusive_display();
                                        });
      request.before([&] {
        f.os.events.push_back("disarm");
      });
      std::shared_ptr<void> metadata;
      request.attach(metadata);
      {
        capture_t capture(f);
        const auto sink = capture.ref->sink.null->stereo;
        require(sink == "next" && capture.assign(sink) == 0, "capture used previous virtual_sink");
        f.os.display_returns();
        f.terminal();
      }
      metadata.reset();
    }
    const auto observed = f.os.read();
    std::cout << "config transition observed defaults: " << std::string(observed[0].begin(), observed[0].end()) << '\n';
    require(observed == original && captured_at_revert == 1, "config-triggered REVERT preceded the audio snapshot");
    require(factory_sink == "next" && factory_install && f.os.created == 1, "snapshot used stale effective audio settings");
    const auto installed = std::find(f.os.events.begin(), f.os.events.end(), "installed");
    const auto captured = std::find(f.os.events.begin(), f.os.events.end(), "capture");
    const auto reverted = std::find(f.os.events.begin(), f.os.events.end(), "revert");
    require(installed < captured && captured < reverted, "effective config/capture/REVERT order changed");
  }

  void config_revert_predicate() {
    for (bool changed : {false, true}) {
      for (bool active : {false, true}) {
        for (bool empty : {false, true}) {
          for (bool was_enabled : {false, true}) {
            for (bool disabled_now : {false, true}) {
              int installed = 0, captured = 0, reverted = 0, ownership_reads = 0;
              config::display_transition::apply([&] {
                ++installed;
                return config::display_transition::state_t {changed, empty, was_enabled, disabled_now};
              },
                                                [&] {
                                                  ++ownership_reads;
                                                  return active;
                                                },
                                                [&] {
                                                  ++captured;
                                                },
                                                [&](const auto &state) {
                                                  ++reverted;
                                                  require(state.allow_disabled_recovery() == (was_enabled && disabled_now), "enabled-to-disabled REVERT flag changed");
                                                });
              const int expected = changed && !active && empty;
              require(installed == 1 && captured == expected && reverted == expected && ownership_reads == (changed ? 1 : 0), "original REVERT predicate/short circuit behavior changed");
            }
          }
        }
      }
    }
    fixture_t f;
    bool enabled = true;
    audio::display_change_hold_t request([&] {
      return f.manager.before_display_change(enabled);
    });
    int reverted = 0;
    config::display_transition::apply([&] {
      enabled = false;
      return config::display_transition::state_t {true, true, true, false};
    },
                                      [] {
                                        return false;
                                      },
                                      std::ref(request),
                                      [&](const auto &) {
                                        ++reverted;
                                      });
    require(reverted == 1 && f.os.created == 0, "effective stream_audio=false changed REVERT or acquired audio");
  }

  void capture_failure_non_vetoing() {
    int reverted = 0;
    bool escaped = false;
    try {
      config::display_transition::apply([] {
        return config::display_transition::state_t {true, true, true, true};
      },
                                        [] {
                                          return false;
                                        },
                                        [] {
                                          throw std::runtime_error("MMDevice capture failure");
                                        },
                                        [&](const auto &state) {
                                          require(state.allow_disabled_recovery(), "disabled recovery flag was lost after capture failure");
                                          ++reverted;
                                        });
    } catch (...) {
      escaped = true;
    }
    std::cout << "capture exception observed REVERT count: " << reverted << '\n';
    require(!escaped && reverted == 1, "optional capture exception vetoed REVERT");
    audio::display_change_hold_t request([]() -> std::shared_ptr<void> {
      throw std::runtime_error("MMDevice construction failure");
    });
    int disarmed = 0;
    request.before([&] {
      ++disarmed;
    });
    std::shared_ptr<void> metadata;
    request.attach(metadata);
    require(disarmed == 1 && !metadata, "optional capture exception vetoed DISARM or published a hold");
  }

  void first_disarm_before_capture_loss() {
    fixture_t f;
    int captured_at_disarm = -1;
    {
      audio::display_change_hold_t request([&] {
        return f.manager.before_display_change(true);
      });
      request.before([&] {
        captured_at_disarm = f.os.captures;
        // Model existing in-flight display work finishing at this boundary;
        // DISARM itself is not claimed to start a new topology operation.
        f.os.events.push_back("disarm");
        f.os.exclusive_display();
      });
      std::shared_ptr<void> metadata;
      request.attach(metadata);
      {
        capture_t capture(f);
        capture.assign();
        f.os.display_returns();
        f.terminal();
      }
    }
    std::cout << "first DISARM observed capture count: " << captured_at_disarm << '\n';
    require(captured_at_disarm == 1 && f.os.read() == original, "first DISARM preceded audio capture");
  }

  void request_hold_rollback(bool fallback_capture) {
    fixture_t f;
    bool rollback_kept_control = false, response_kept_control = false;
    {
      audio::display_change_hold_t request([&] {
        return f.manager.before_display_change(true);
      });
      auto response_rollback = util::fail_guard([&] {
        response_kept_control = f.os.created == 1 && f.os.destroyed == 0;
        f.os.events.push_back("response-revert");
      });
      auto rollback = util::fail_guard([&] {
        config::display_transition::apply([] {
          return config::display_transition::state_t {true, true, true, false};
        },
                                          [] {
                                            return false;
                                          },
                                          std::ref(request),
                                          [&](const auto &) {
                                            std::cout << "rollback observed control destruction: " << f.os.destroyed << '\n';
                                            rollback_kept_control = f.os.created == 1 && f.os.destroyed == 0 && f.os.captures == 1;
                                            f.os.events.push_back("rollback-revert");
                                          });
      });
      {
        std::shared_ptr<void> metadata;
        if (fallback_capture) {
          request.capture_if_empty_and_attach(metadata);
        } else {
          request.before([&] {
            f.os.events.push_back("disarm");
          });
        }
        request.attach(metadata);
        f.os.user(role_e::console);
        // Denied/aborted after preparation: metadata unwinds before config and
        // response rollback, while the actual request owner remains live.
      }
    }
    require(rollback_kept_control && response_kept_control && f.os.created == 1 && f.os.destroyed == 1 && f.os.writes == 0 && f.os.read()[0] == L"user", "aborted metadata released the snapshot before config/response rollback or overwrote a choice");
  }

  void fallback_first_capture() {
    fixture_t f;
    int attempts = 0, captured_at_display = 0;
    {
      audio::display_change_hold_t request([&] {
        ++attempts;
        return f.manager.before_display_change(true);
      });
      // Resume can skip the earlier DISARM; preparation is the first boundary.
      std::shared_ptr<void> metadata;
      request.capture_if_empty_and_attach(metadata);
      require(metadata != nullptr, "first fallback did not attach the production context");
      captured_at_display = f.os.captures;
      f.os.exclusive_display();
      {
        capture_t capture(f);
        capture.assign();
        f.os.display_returns();
        f.terminal();
      }
      metadata.reset();
      require(f.os.destroyed == 0, "fallback metadata destruction released the request hold");
    }
    std::cout << "first fallback observed capture count before display: " << captured_at_display << '\n';
    require(attempts == 1 && captured_at_display == 1 && f.os.destroyed == 1 && f.os.read() == original, "first fallback failed to preserve pre-display roles or release once");
  }

  void fallback_capture_failure(bool earlier_failure) {
    fixture_t f;
    int attempts = 0, reverted = 0, disarmed = 0, prepared = 0;
    bool escaped = false;
    audio::display_change_hold_t request([&]() -> std::shared_ptr<void> {
      ++attempts;
      throw std::runtime_error("OS audio acquisition failure");
    });
    if (earlier_failure) {
      config::display_transition::apply([] {
        return config::display_transition::state_t {true, true, true, false};
      },
                                        [] {
                                          return false;
                                        },
                                        std::ref(request),
                                        [&](const auto &) {
                                          ++reverted;
                                        });
      request.before([&] {
        ++disarmed;
      });
    }
    std::shared_ptr<void> metadata;
    try {
      // The same complete method is called by NVHTTP preparation and WebRTC.
      request.capture_if_empty_and_attach(metadata);
      ++prepared;
    } catch (const std::exception &) {
      escaped = true;
    }
    std::cout << "fallback " << (earlier_failure ? "repeated" : "first")
              << " failure observed attempts: " << attempts << "; preparation=" << prepared
              << "; escaped=" << escaped << '\n';
    require(!escaped && prepared == 1 && !metadata && attempts == (earlier_failure ? 3 : 1) && reverted == (earlier_failure ? 1 : 0) && disarmed == (earlier_failure ? 1 : 0) && f.os.created == 0 && f.os.writes == 0, "optional fallback acquisition aborted preparation or changed earlier display operations");
  }

  void fallback_recovery_rollback() {
    fixture_t f;
    int attempts = 0;
    bool config_kept_control = false, response_kept_control = false;
    {
      audio::display_change_hold_t request([&] {
        if (++attempts == 1) {
          throw std::runtime_error("transient OS audio acquisition failure");
        }
        return f.manager.before_display_change(true);
      });
      auto response_rollback = util::fail_guard([&] {
        response_kept_control = f.os.created == 1 && f.os.destroyed == 0;
      });
      auto runtime_rollback = util::fail_guard([&] {
        config::display_transition::apply([] {
          return config::display_transition::state_t {true, true, true, false};
        },
                                          [] {
                                            return false;
                                          },
                                          std::ref(request),
                                          [&](const auto &) {
                                            config_kept_control = f.os.created == 1 && f.os.destroyed == 0;
                                          });
      });
      request();
      {
        std::shared_ptr<void> metadata;
        request.capture_if_empty_and_attach(metadata);
        require(metadata != nullptr && attempts == 2, "fallback failed to recover from an earlier acquisition exception");
        request.capture_if_empty_and_attach(metadata);
        f.os.user(role_e::console);
        // Failed admission/aborted preparation drops metadata before rollback.
      }
    }
    std::cout << "fallback recovery observed acquisitions: " << attempts
              << "; rollback-held=" << config_kept_control << "; response-held=" << response_kept_control << '\n';
    require(attempts == 2 && config_kept_control && response_kept_control && f.os.created == 1 && f.os.destroyed == 1 && f.os.writes == 0 && f.os.read()[0] == L"user", "successful retry lost rollback ownership, reacquired, or overwrote a user choice");
  }

  void fallback_existing_hold() {
    for (const bool metadata_owned : {false, true}) {
      fixture_t f;
      int attempts = 0;
      {
        std::shared_ptr<void> metadata;
        if (metadata_owned) {
          metadata = f.manager.before_display_change(true);
        }
        audio::display_change_hold_t request([&] {
          ++attempts;
          return f.manager.before_display_change(true);
        });
        if (!metadata_owned) {
          request();
        }
        const auto original_metadata = metadata.get();
        request.capture_if_empty_and_attach(metadata);
        request.capture_if_empty_and_attach(metadata);
        require(metadata && (!metadata_owned || metadata.get() == original_metadata), "fallback replaced existing pending ownership");
        metadata.reset();
        require(f.os.created == 1 && f.os.destroyed == 0, "fallback failed to retain/adopt existing ownership");
      }
      require(attempts == (metadata_owned ? 0 : 1) && f.os.created == 1 && f.os.destroyed == 1 && f.os.writes == 0, "fallback unnecessarily acquired or released existing ownership");
    }
  }

  void native_input_only_snapshot_gate() {
    fixture_t f;
    bool input_only = true;
    {
      audio::display_change_hold_t request([&] {
        return f.manager.before_display_change(true, input_only);
      });
      std::shared_ptr<void> metadata;
      request.before([] {});
      request.capture_if_empty_and_attach(metadata);
      std::cout << "native input-only observed audio constructions: " << f.os.created << '\n';
      require(!metadata && f.os.created == 0 && f.os.captures == 0, "native input-only request initialized audio before display work");
      input_only = false;
      request.capture_if_empty_and_attach(metadata);
      require(metadata && f.os.created == 1 && f.os.captures == 1, "native normal request lost its pre-display capture");
      metadata.reset();
      require(f.os.destroyed == 0, "native gate lost request ownership");
    }
    require(f.os.destroyed == 1 && f.os.writes == 0, "native gate release changed routing or final ownership");
  }

  void native_app_metadata_hold() {
    fixture_t f;
    f.app_active = true;
    f.manager.app_started();
    std::shared_ptr<void> app_metadata;
    {
      audio::display_change_hold_t request([&] {
        return f.manager.before_display_change(true);
      });
      std::shared_ptr<void> pending;
      request.capture_if_empty_and_attach(pending);
      // Process boundary: native proc_t retains the original launch-session
      // shared_ptr; RTSP startup copies its guard into the capture snapshot.
      app_metadata = pending;
      auto active = pending;
      {
        capture_t capture(f);
        capture.assign();
      }
      pending.reset();
      std::thread video_thread([] {}), audio_thread([] {});
      audio::join_rtsp_capture(active, video_thread, audio_thread,
                               [] {}, [] { return std::shared_ptr<void> {}; },
                               [](audio::join_stage_e) {});
      require(f.os.destroyed == 0 && f.os.created == 1, "native app metadata lost audio while app remains resumable");
    }
    {
      capture_t reconnect(f);
      reconnect.assign();
      require(f.os.created == 1, "native app metadata did not preserve the same context on reconnect");
      f.terminal();
      app_metadata.reset();
      require(f.os.destroyed == 0, "native terminal metadata release preceded active capture release");
    }
    std::cout << "native app metadata observed final destruction: " << f.os.destroyed << '\n';
    require(f.os.destroyed == 1 && f.os.read() == original, "native app/capture references failed terminal restoration");
  }

  // Process/metadata boundary only. The synchronization/ownership decisions are
  // the complete production helpers called by proc_t and clone_for_startup.
  // Move-only payload and a separate display owner expose accidental metadata
  // replacement, truncation or early destruction.
  struct app_metadata_t {
    std::shared_ptr<void> audio_context_guard;
    std::shared_ptr<void> display_power_guard;
    std::unique_ptr<int> cipher {std::make_unique<int>(621)};
    std::string client_metadata {"native-client-preferences-and-commands"};
  };

  void native_paused_terminal_order() {
    fixture_t f;
    f.app_active = true;
    f.manager.app_started();
    int display_releases = 0;
    auto metadata = std::make_shared<app_metadata_t>();
    const auto identity = metadata.get();
    metadata->display_power_guard = std::shared_ptr<void>(new int(1), [&](void *p) {
      ++display_releases;
      delete static_cast<int *>(p);
    });
    metadata->audio_context_guard = f.manager.before_display_change(true);
    {
      capture_t capture(f);
      require(capture.assign() == 0, "paused fixture failed to route audio");
    }
    require(f.os.destroyed == 0, "paused metadata failed to preserve reconnect audio");
    audio::begin_app_audio_termination(metadata, [&] {
      // Actual proc_t still reports its positive app id at this marker.
      require(f.app_active, "test moved native app-id teardown before the marker");
      f.manager.app_termination_requested();
    });
    const bool restored_before_display = f.os.read() == original && f.os.destroyed == 1;
    std::cout << "native paused terminal restored before display cleanup: " << restored_before_display << '\n';
    require(restored_before_display, "paused terminal restoration occurred after display cleanup");
    require(metadata.get() == identity && metadata->cipher && *metadata->cipher == 621 &&
              metadata->client_metadata == "native-client-preferences-and-commands" &&
              metadata->display_power_guard && display_releases == 0,
            "audio retirement dropped native metadata or display-power ownership");
    // This is the old _launch_session.reset position, after display cleanup.
    f.app_active = false;
    metadata.reset();
    require(display_releases == 1 && f.os.destroyed == 1, "native tail owners did not release exactly once");
  }

  void native_terminal_active_join() {
    fixture_t f;
    f.app_active = true;
    f.manager.app_started();
    auto metadata = std::make_shared<app_metadata_t>();
    metadata->audio_context_guard = f.manager.before_display_change(true);
    app_metadata_t startup;
    audio::copy_launch_audio_guard(*metadata, startup);
    auto active = std::move(startup.audio_context_guard);
    {
      capture_t capture(f);
      require(capture.assign() == 0, "active fixture failed to route audio");
    }
    audio::begin_app_audio_termination(metadata, [&] { f.manager.app_termination_requested(); });
    const bool process_audio_retired = !metadata->audio_context_guard;
    require(f.os.destroyed == 0 && f.os.read()[0] == L"stream", "terminal marker bypassed active capture ownership");
    std::thread video_thread([] {}), audio_thread([] {});
    int join_observations = 0;
    audio::join_rtsp_capture(active, video_thread, audio_thread,
      [&] { require(f.os.destroyed == 0, "audio restored before control join"); },
      [] { return std::shared_ptr<void> {}; },
      [&](audio::join_stage_e) {
        ++join_observations;
        require(f.os.destroyed == 0, "audio restored before real video/audio/control joins completed");
      });
    const bool restored_after_join = f.os.destroyed == 1 && f.os.read() == original;
    std::cout << "native active terminal process audio retired: " << process_audio_retired
              << "; restored after joins: " << restored_after_join << '\n';
    require(process_audio_retired && restored_after_join && join_observations > 0,
            "native process metadata stranded audio beyond the last capture join");
    require(metadata && metadata->cipher && metadata->client_metadata.size() > 20,
            "active termination discarded native metadata");
    f.app_active = false;
    metadata.reset();
  }

  void native_terminal_shared_original() {
    fixture_t f;
    f.app_active = true;
    f.manager.app_started();
    auto metadata = std::make_shared<app_metadata_t>();
    metadata->audio_context_guard = f.manager.before_display_change(true);
    {
      capture_t capture(f);
      require(capture.assign() == 0, "pending fixture failed to route audio");
    }
    // RTSP can read the original on I/O before it takes lifecycle admission.
    // Keep that strong owner while repeatedly exercising its real copy helper.
    auto pending_io = metadata;
    std::latch ready(1), terminate_finished(1);
    std::thread io([pending_io, &ready, &terminate_finished] {
      app_metadata_t startup;
      audio::copy_launch_audio_guard(*pending_io, startup);
      ready.count_down();
      terminate_finished.wait();
      for (int i = 0; i < 100; ++i) {
        audio::copy_launch_audio_guard(*pending_io, startup);
      }
    });
    ready.wait();
    audio::begin_app_audio_termination(metadata, [&] { f.manager.app_termination_requested(); });
    require(metadata->audio_context_guard && pending_io->audio_context_guard && f.os.destroyed == 0,
            "process termination mutated an aliased pending/I/O guard");
    terminate_finished.count_down();
    io.join();
    // The process still releases the complete original at its existing tail.
    // A pending/I/O alias must keep the snapshot alive even beyond that tail.
    metadata.reset();
    require(pending_io->audio_context_guard && f.os.destroyed == 0,
            "process tail bypassed pending/I/O ownership");
    pending_io.reset();
    require(f.os.destroyed == 1 && f.os.read() == original,
            "final pending/I/O owner failed terminal restoration");
  }

  void native_terminal_no_metadata() {
    std::shared_ptr<app_metadata_t> metadata;
    int markers = 0;
    audio::begin_app_audio_termination(metadata, [&] { ++markers; });
    require(markers == 1, "empty metadata suppressed the native terminal marker");
  }

  void denied_request_is_lazy() {
    fixture_t f;
    {
      audio::display_change_hold_t request([&] {
        return f.manager.before_display_change(true);
      });
      // Existing authorization/admission returns before any display boundary.
    }
    require(f.os.created == 0 && f.os.captures == 0 && f.os.writes == 0, "denied request construction acquired audio");
  }

  void prior_restore_boundary() {
    fixture_t f;
    f.os.exclusive_display();
    {
      audio::display_change_hold_t request([&] {
        return f.manager.before_display_change(true);
      });
      request.before([] {
      });
      std::shared_ptr<void> metadata;
      request.attach(metadata);
      {
        capture_t capture(f);
        capture.assign();
        f.os.display_returns();
        f.terminal();
      }
    }
    require(f.os.read()[0] == L"fallback", "test invented the default lost before acquisition");
  }

  fixture_t *unwind_fixture {};

  // A separate process runs this path. Self-join injects a real OS join error;
  // the catch requires unwinding the actual production teardown owner before
  // std::thread's still-joinable destructor calls the terminate handler.
  [[noreturn]] void owner_unwind(bool audio_join) {
    fixture_t f;
    unwind_fixture = &f;
    auto pending = f.manager.before_display_change(true);
    {
      capture_t capture(f);
      capture.assign();
      f.terminal();
    }
    std::set_terminate([] {
      const auto destroyed = unwind_fixture->os.destroyed;
      const bool held = destroyed == 0 && unwind_fixture->os.read()[0] == L"stream";
      std::fprintf(stderr, "owner unwind observed destruction: %d; hold=%s\n", destroyed, held ? "held" : "released");
      std::_Exit(held ? 0 : 1);
    });
    std::latch published(1);
    std::promise<void> never_completed;
    auto wait_for_termination = never_completed.get_future();
    std::thread self([&] {
      published.wait();
      try {
        webrtc_stream::WebRtcCaptureTeardown owner;
        owner.audio_context_guard = std::move(pending);
        if (audio_join) {
          owner.audio_thread = std::move(self);
        } else {
          owner.video_thread = std::move(self);
        }
        owner.join_capture();
      } catch (...) {
        std::fprintf(stderr, "join error escaped without a joinable thread destructor\n");
        std::_Exit(3);
      }
      std::_Exit(4);
    });
    published.count_down();
    wait_for_termination.wait();
    std::_Exit(5);
  }
}  // namespace

int main(int argc, char **argv) {
  if (argc > 1 && std::string_view(argv[1]) == "--owner-unwind-video") {
    owner_unwind(false);
  }
  if (argc > 1 && std::string_view(argv[1]) == "--owner-unwind-audio") {
    owner_unwind(true);
  }
  const std::vector<std::pair<std::string, std::function<void()>>> tests {
    {"roles.pre_display_first_sink", pre_display_first_sink},
    {"roles.first_sink_preservation", first_sink_preservation},
    {"roles.newer_choices", newer_choices},
    {"roles.read_write_gap", role_read_write_gap},
    {"roles.delayed_arrival", delayed_arrival},
    {"roles.retry_user_choice", retry_user_choice},
    {"roles.inherited_recovery", inherited_recovery},
    {"roles.late_worker_successor", late_worker_cannot_overwrite_successor},
    {"roles.scheduler_failure", scheduler_failure},
    {"context.aborted_launch", aborted_launch},
    {"context.failures_capture_only", failures_and_capture_only},
    {"context.partial_assignment", partial_assignment},
    {"context.pause_resume", [] {
       pause_resume(false, true);
     }},
    {"context.aborted_resume", [] {
       pause_resume(true, true);
     }},
    {"context.terminal_while_paused", [] {
       pause_resume(false, false);
     }},
    {"context.concurrent_clients", concurrent_clients},
    {"context.successor", [] {
       successor(false);
     }},
    {"context.aborted_successor", [] {
       successor(true);
     }},
    {"context.rtsp_join", [] {
       join_hold("rtsp");
     }},
    {"context.webrtc_join", [] {
       join_hold("webrtc");
     }},
    {"context.failed_join", failed_join},
    {"context.start_hold_failure", start_hold_failure},
    {"context.start_hold_success", start_hold_success},
    {"context.non_retaining_platform", non_retaining_platform},
    {"transition.config_prehold", config_effective_before_revert},
    {"transition.revert_predicate", config_revert_predicate},
    {"transition.capture_failure", capture_failure_non_vetoing},
    {"transition.first_disarm", first_disarm_before_capture_loss},
    {"transition.rollback_hold", [] {
       request_hold_rollback(false);
       request_hold_rollback(true);
     }},
    {"transition.fallback_first_capture", fallback_first_capture},
    {"transition.fallback_first_failure", [] {
       fallback_capture_failure(false);
     }},
    {"transition.fallback_repeated_failure", [] {
       fallback_capture_failure(true);
     }},
    {"transition.fallback_recovery_rollback", fallback_recovery_rollback},
    {"transition.fallback_existing_hold", fallback_existing_hold},
    {"transition.native_input_only", native_input_only_snapshot_gate},
    {"transition.native_app_metadata", native_app_metadata_hold},
    {"transition.native_paused_terminal", native_paused_terminal_order},
    {"transition.native_active_terminal", native_terminal_active_join},
    {"transition.native_shared_terminal", native_terminal_shared_original},
    {"transition.native_empty_terminal", native_terminal_no_metadata},
    {"transition.denied_lazy", denied_request_is_lazy},
    {"transition.prior_restore_boundary", prior_restore_boundary},
  };
  int failed = 0;
  for (const auto &[name, test] : tests) {
    if (argc > 1 && name.find(argv[1]) == std::string::npos) {
      continue;
    }
    try {
      test();
      std::cout << name << ": PASS\n";
    } catch (const std::exception &error) {
      ++failed;
      std::cout << name << ": FAIL: " << error.what() << '\n';
    }
  }
  return failed != 0;
}
