/**
 * @file src/main.cpp
 * @brief Definitions for the main entry point for Sunshine.
 */
// standard includes
#include <algorithm>
#include <atomic>
#include <codecvt>
#include <condition_variable>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <system_error>
#include <thread>

// local includes
#include "confighttp.h"
#include "entry_handler.h"
#include "globals.h"
#include "host_stats.h"
#include "httpcommon.h"
#include "logging.h"
#include "main.h"
#include "nvhttp.h"
#include "process.h"
#include "rtsp.h"
#include "system_tray.h"
#include "update.h"
#include "upnp.h"
#include "version_compare.h"
#include "uuid.h"
#include "video.h"
#include "session_history.h"
#include "state_storage.h"
#include "webrtc_stream.h"
#ifdef _WIN32
  #include <shobjidl.h>

  #include "src/display_helper_integration.h"
  #include "src/platform/windows/frame_limiter_nvcp.h"
  #include "src/platform/windows/misc.h"
  #include "src/platform/windows/playnite_integration.h"
  #include "src/platform/windows/rtss_integration.h"
  #include "src/platform/windows/startup_display_policy.h"
  #include "src/platform/windows/virtual_display.h"
  #include "src/platform/windows/virtual_display_cleanup.h"
#endif

#ifdef _WIN32
  #include "platform/windows/misc.h"
  #include "platform/windows/display_helper_integration.h"
  #include "platform/windows/virtual_display.h"
#endif

#define PROBE_DISPLAY_UUID "38F72B96-B00C-4F21-8B6C-E1BFF1602B0E"

extern "C" {
#include "rswrapper.h"
}

using namespace std::literals;

std::map<int, std::function<void()>> signal_handlers;

#ifdef _WIN32
  #define WIDEN_STRING_LITERAL_IMPL(value) L##value
  #define WIDEN_STRING_LITERAL(value) WIDEN_STRING_LITERAL_IMPL(value)
#endif

void on_signal_forwarder(int sig) {
  signal_handlers.at(sig)();
}

template<class FN>
void on_signal(int sig, FN &&fn) {
  signal_handlers.emplace(sig, std::forward<FN>(fn));

  std::signal(sig, on_signal_forwarder);
}

namespace {
  static_assert(std::atomic_bool::is_always_lock_free, "shutdown signal flag must be lock-free in a signal handler");

  class shutdown_deadline_t {
  public:
    explicit shutdown_deadline_t(std::atomic_bool *signal_requested):
        signal_requested_ {signal_requested} {
      try {
        worker_ = std::jthread([this](std::stop_token) {
          run();
        });
      } catch (const std::system_error &e) {
        BOOST_LOG(error) << "Unable to create the shutdown deadline watchdog: " << e.what();
      }
    }

    shutdown_deadline_t(const shutdown_deadline_t &) = delete;
    shutdown_deadline_t &operator=(const shutdown_deadline_t &) = delete;

    ~shutdown_deadline_t() {
      complete();
    }

    void arm() {
      if (!worker_.joinable()) {
        return;
      }
      std::lock_guard lock {mutex_};
      if (state_ == state_e::idle) {
        state_ = state_e::armed;
        cv_.notify_one();
      }
    }

    void complete() {
      {
        std::lock_guard lock {mutex_};
        if (state_ != state_e::firing) {
          state_ = state_e::completed;
        }
        cv_.notify_one();
      }

      // This object is owned by main(), never by its worker. Joining here
      // prevents a deadline thread from escaping into CRT/static teardown.
      if (worker_.joinable()) {
        worker_.join();
      }
    }

  private:
    enum class state_e {
      idle,
      armed,
      completed,
      firing,
    };

    void run() {
      std::unique_lock lock {mutex_};
      while (state_ == state_e::idle && (!signal_requested_ || !signal_requested_->load(std::memory_order_relaxed))) {
        // std::signal handlers cannot notify a condition variable safely. Poll
        // the signal-safe flag so startup work is covered before main reaches
        // shutdown_event->view().
        cv_.wait_for(lock, std::chrono::milliseconds(50));
      }
      if (state_ == state_e::idle) {
        state_ = state_e::armed;
      }
      if (state_ != state_e::armed) {
        return;
      }

      constexpr auto kShutdownDeadline = std::chrono::seconds(10);
      if (cv_.wait_until(lock, std::chrono::steady_clock::now() + kShutdownDeadline, [this] {
            return state_ != state_e::armed;
          })) {
        return;
      }

      // Completion and expiry serialize through mutex_. A recovered shutdown
      // therefore cannot leave a stale timer behind to trap later.
      state_ = state_e::firing;
      lock.unlock();
      BOOST_LOG(fatal) << "10 seconds passed, yet Sunshine's still running: Forcing shutdown"sv;
      lifetime::debug_trap();
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    state_e state_ {state_e::idle};
    std::atomic_bool *signal_requested_ = nullptr;
    std::jthread worker_;
  };
}  // namespace

std::map<std::string_view, std::function<int(const char *name, int argc, char **argv)>> cmd_to_func {
  {"creds"sv, [](const char *name, int argc, char **argv) {
     return args::creds(name, argc, argv);
   }},
  {"help"sv, [](const char *name, int argc, char **argv) {
     return args::help(name);
   }},
  {"version"sv, [](const char *name, int argc, char **argv) {
     return args::version();
   }},
#ifdef _WIN32
  {"restore-nvprefs-undo"sv, [](const char *name, int argc, char **argv) {
     return args::restore_nvprefs_undo();
   }},
#endif
};

#ifdef _WIN32
LRESULT CALLBACK SessionMonitorWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  switch (uMsg) {
    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    case WM_ENDSESSION:
      {
        if (!wParam) {
          return 0;
        }

        // Trigger async shutdown so the message loop keeps pumping.
        // When the main thread's cleanup guard runs, it will
        // PostMessage(WM_CLOSE) to this window, which dispatches
        // through the outer GetMessage loop → WM_CLOSE → DestroyWindow
        // → WM_DESTROY → PostQuitMessage → GetMessage returns 0 →
        // thread exits.
        //
        // Previously this called exit_sunshine(0, false) which blocked
        // the thread in a sleep loop, starving the message pump.  That
        // prevented WM_CLOSE from ever being processed, the join
        // timed out, the thread was detached, and the still-running
        // thread crashed during CRT atexit cleanup (abort/FAST_FAIL).
        BOOST_LOG(info) << "Received WM_ENDSESSION"sv;
        lifetime::exit_sunshine(0, true);
        return 0;
      }
    default:
      return DefWindowProc(hwnd, uMsg, wParam, lParam);
  }
}

WINAPI BOOL ConsoleCtrlHandler(DWORD type) {
  if (type == CTRL_CLOSE_EVENT) {
    BOOST_LOG(info) << "Console closed handler called";
    lifetime::exit_sunshine(0, false);
  }
  return FALSE;
}
#endif

int main(int argc, char *argv[]) {
  lifetime::argv = argv;

#ifdef _WIN32
  // Avoid searching the PATH in case a user has configured their system insecurely
  // by placing a user-writable directory in the system-wide PATH variable.
  SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  setlocale(LC_ALL, "C");
#endif

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  // Use UTF-8 conversion for the default C++ locale (used by boost::log)
  std::locale utf8_locale(std::locale(), new std::codecvt_utf8<wchar_t>);
  std::locale::global(utf8_locale);
  boost::filesystem::path::imbue(utf8_locale);
#pragma GCC diagnostic pop

  mail::man = std::make_shared<safe::mail_raw_t>();

  // parse config file
  if (config::parse(argc, argv)) {
    return 0;
  }

  auto log_deinit_guard = logging::init(config::sunshine.min_log_level, config::sunshine.log_file);
  if (!log_deinit_guard) {
    BOOST_LOG(error) << "Logging failed to initialize"sv;
  }

#ifdef _WIN32
  const auto app_user_model_id_status =
    SetCurrentProcessExplicitAppUserModelID(WIDEN_STRING_LITERAL(PROJECT_APP_USER_MODEL_ID));
  if (FAILED(app_user_model_id_status)) {
    BOOST_LOG(warning) << "Failed to set explicit AppUserModelID; Windows may reuse legacy notification branding"sv;
  }
#endif

#ifndef SUNSHINE_EXTERNAL_PROCESS
  // Setup third-party library logging
  logging::setup_av_logging(config::sunshine.min_log_level);
  logging::setup_libdisplaydevice_logging(config::sunshine.min_log_level);
#endif

#ifdef __ANDROID__
  // Setup Android-specific logging
  logging::setup_android_logging();
#endif

  // logging can begin at this point
  // if anything is logged prior to this point, it will appear in stdout, but not in the log viewer in the UI
  // the version should be printed to the log before anything else
  BOOST_LOG(info) << PROJECT_NAME << " version: " << PROJECT_VERSION << " commit: " << PROJECT_VERSION_COMMIT;
#ifdef _WIN32
  const auto windows_version = platf::query_windows_version();
  BOOST_LOG(info) << "Windows version: product=" << windows_version.product_name
                  << ", display_version=" << windows_version.display_version
                  << ", build=" << windows_version.current_build;
  if (windows_version.build_number.has_value() && *windows_version.build_number < 22000) {
    BOOST_LOG(warning) << "Windows 10 detected; HDR will not work on the Vibepollo Virtual Display.";
  }
#endif
  if (version_compare::is_prerelease_channel(PROJECT_VERSION)) {
    BOOST_LOG(info) << "Prerelease build detected; default min_log_level is debug unless overridden.";
  }
  BOOST_LOG(info) << "Effective min_log_level=" << config::sunshine.min_log_level;

  // Log publisher metadata
  log_publisher_data();

  // Log modified_config_settings
  config::log_config_settings(config::modified_config_settings, false);
  config::modified_config_settings.clear();

#ifdef _WIN32
  statefile::repair_config_permissions();
#endif

#ifdef _WIN32
  platf::frame_limiter_nvcp::restore_pending_overrides();
  platf::rtss_restore_pending_overrides();
#endif

  if (!config::sunshine.cmd.name.empty()) {
    auto fn = cmd_to_func.find(config::sunshine.cmd.name);
    if (fn == std::end(cmd_to_func)) {
      BOOST_LOG(fatal) << "Unknown command: "sv << config::sunshine.cmd.name;

      BOOST_LOG(info) << "Possible commands:"sv;
      for (auto &[key, _] : cmd_to_func) {
        BOOST_LOG(info) << '\t' << key;
      }

      return 7;
    }

    return fn->second(argv[0], config::sunshine.cmd.argc, config::sunshine.cmd.argv);
  }

  // Display configuration is managed by the external Windows helper; no in-process init.

  // Construct the process-owned shutdown deadline before the session monitor.
  // Its cleanup guard also runs on early startup returns, so it must be able
  // to bound that join as well as the ordinary shutdown path below.
  auto shutdown_event = mail::man->event<bool>(mail::shutdown);
  std::atomic_bool shutdown_signal_requested {false};
  shutdown_deadline_t shutdown_deadline {&shutdown_signal_requested};

#ifdef WIN32
  // Modify relevant NVIDIA control panel settings if the system has corresponding gpu
  if (nvprefs_instance.load()) {
    // Restore global settings to the undo file left by improper termination of sunshine.exe
    nvprefs_instance.restore_from_and_delete_undo_file_if_exists();
    // Modify application settings for sunshine.exe
    nvprefs_instance.modify_application_profile();
    // Modify global settings, undo file is produced in the process to restore after improper termination
    nvprefs_instance.modify_global_profile();
    // Unload dynamic library to survive driver re-installation
    nvprefs_instance.unload();
  }

  // Wait as long as possible to terminate Sunshine.exe during logoff/shutdown
  SetProcessShutdownParameters(0x100, SHUTDOWN_NORETRY);

  // We must create a hidden window to receive shutdown notifications since we load gdi32.dll
  std::promise<HWND> session_monitor_hwnd_promise;
  auto session_monitor_hwnd_future = session_monitor_hwnd_promise.get_future().share();
  std::promise<DWORD> session_monitor_thread_id_promise;
  auto session_monitor_thread_id_future = session_monitor_thread_id_promise.get_future().share();
  std::promise<void> session_monitor_join_thread_promise;
  auto session_monitor_join_thread_future = session_monitor_join_thread_promise.get_future();

  std::thread session_monitor_thread([&]() {
    platf::set_thread_name("session_monitor");
    session_monitor_join_thread_promise.set_value_at_thread_exit();

    // Create a message queue immediately so shutdown can always fall back
    // to PostThreadMessage(WM_QUIT), even if window creation fails.
    MSG msg {};
    PeekMessage(&msg, nullptr, 0, 0, PM_NOREMOVE);
    session_monitor_thread_id_promise.set_value(GetCurrentThreadId());

    WNDCLASSA wnd_class {};
    wnd_class.lpszClassName = "SunshineSessionMonitorClass";
    wnd_class.lpfnWndProc = SessionMonitorWindowProc;
    if (!RegisterClassA(&wnd_class)) {
      session_monitor_hwnd_promise.set_value(nullptr);
      BOOST_LOG(error) << "Failed to register session monitor window class"sv << std::endl;
      return;
    }

    auto wnd = CreateWindowExA(
      0,
      wnd_class.lpszClassName,
      "Sunshine Session Monitor Window",
      0,
      CW_USEDEFAULT,
      CW_USEDEFAULT,
      CW_USEDEFAULT,
      CW_USEDEFAULT,
      nullptr,
      nullptr,
      nullptr,
      nullptr
    );

    session_monitor_hwnd_promise.set_value(wnd);

    if (!wnd) {
      BOOST_LOG(error) << "Failed to create session monitor window"sv << std::endl;
      return;
    }

    ShowWindow(wnd, SW_HIDE);

    // Run the message loop for our window
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
  });

  auto shutdown_session_monitor = [&]() {
    if (!session_monitor_thread.joinable()) {
      return;
    }

    // This cleanup guard covers early returns before shutdown_event->view().
    // Arm the owned deadline before any potentially unbounded join so that a
    // pathological message loop cannot hang process teardown indefinitely.
    shutdown_deadline.arm();

    auto request_session_monitor_shutdown = [&](bool force_quit_only) {
      if (!force_quit_only) {
        if (session_monitor_hwnd_future.wait_for(1s) == std::future_status::ready) {
          if (HWND session_monitor_hwnd = session_monitor_hwnd_future.get()) {
            if (PostMessage(session_monitor_hwnd, WM_CLOSE, 0, 0)) {
              return true;
            }

            BOOST_LOG(warning) << "Failed to post WM_CLOSE to session monitor window: "sv << GetLastError();
          } else {
            BOOST_LOG(warning) << "Session monitor window was not created"sv;
          }
        } else {
          BOOST_LOG(warning) << "session_monitor_hwnd_future reached timeout";
        }
      }

      if (session_monitor_thread_id_future.wait_for(1s) != std::future_status::ready) {
        BOOST_LOG(warning) << "session_monitor_thread_id_future reached timeout";
        return false;
      }

      const DWORD session_monitor_thread_id = session_monitor_thread_id_future.get();
      if (!session_monitor_thread_id) {
        BOOST_LOG(warning) << "Session monitor thread id was not set"sv;
        return false;
      }

      if (PostThreadMessage(session_monitor_thread_id, WM_QUIT, 0, 0)) {
        return true;
      }

      BOOST_LOG(warning) << "Failed to post WM_QUIT to session monitor thread: "sv << GetLastError();
      return false;
    };

    request_session_monitor_shutdown(false);

    if (session_monitor_join_thread_future.wait_for(3s) == std::future_status::ready) {
      session_monitor_thread.join();
      return;
    }

    BOOST_LOG(warning) << "session_monitor_join_thread_future reached timeout";
    request_session_monitor_shutdown(true);

    // This thread owns a window/message queue and may access logging and
    // process globals. It must never escape into CRT teardown. The shutdown
    // deadline is armed before the normal call site below, so an unexpected
    // stuck message loop is diagnosed instead of being detached unsafely.
    if (session_monitor_join_thread_future.wait_for(5s) == std::future_status::ready) {
      session_monitor_thread.join();
    } else {
      BOOST_LOG(error) << "session_monitor_thread still running after forced WM_QUIT; waiting for owned thread exit";
      session_monitor_thread.join();
    }
  };

  auto session_monitor_join_thread_guard = util::fail_guard([&]() {
    shutdown_session_monitor();
  });

#endif

  task_pool.start(1);

#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
  // create tray thread and detach it if enabled in config
  if (config::sunshine.system_tray) {
    system_tray::run_tray();
  }
  // Schedule periodic update checks if configured
  if (config::sunshine.update_check_interval_seconds > 0) {
    // Trigger an immediate update check on startup so users don't wait
    // a full interval before the first detection occurs.
    update::trigger_check(true);

    auto schedule_periodic = std::make_shared<std::function<void()>>();
    *schedule_periodic = [schedule_periodic]() {
      update::periodic();
      if (config::sunshine.update_check_interval_seconds > 0) {
        task_pool.pushDelayed(*schedule_periodic, std::chrono::seconds(config::sunshine.update_check_interval_seconds));
      }
    };
    task_pool.pushDelayed(*schedule_periodic, std::chrono::seconds(config::sunshine.update_check_interval_seconds));
  }
#endif

  // Create signal handlers after logging has been initialized.
  on_signal(SIGINT, [&shutdown_signal_requested, shutdown_event]() {
    shutdown_signal_requested.store(true, std::memory_order_relaxed);
    BOOST_LOG(info) << "Interrupt handler called"sv;

    // Preserve Vibepollo's eager application cleanup; the owned deadline above
    // replaces the detached task-pool watchdog that used to follow it.
    proc::proc.terminate();
    // Break out of the main loop
    shutdown_event->raise(true);
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
    if (config::sunshine.system_tray) {
      system_tray::end_tray();
    }
#endif
  });

  on_signal(SIGTERM, [&shutdown_signal_requested, shutdown_event]() {
    shutdown_signal_requested.store(true, std::memory_order_relaxed);
    BOOST_LOG(info) << "Terminate handler called"sv;

    // Break out of the main loop
    shutdown_event->raise(true);
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
    if (config::sunshine.system_tray) {
      system_tray::end_tray();
    }
#endif
  });

#ifdef _WIN32
  // Terminate gracefully on Windows when console window is closed
  SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
#endif

  proc::refresh(config::stream.file_apps);

  // If any of the following fail, we log an error and continue event though sunshine will not function correctly.
  // This allows access to the UI to fix configuration problems or view the logs.

  auto platf_deinit_guard = platf::init();
  if (!platf_deinit_guard) {
    BOOST_LOG(error) << "Platform failed to initialize"sv;
  }

#ifdef _WIN32
  // Reconcile the Vulkan HDR implicit-layer registration with the configured preference. This makes
  // the Web UI toggle authoritative over the installer's unconditional registration and self-heals
  // when the installer's (now best-effort) registration was skipped or failed. Only attempt when
  // running as SYSTEM, since it writes HKLM; the call short-circuits when already in the desired state.
  if (platf::is_running_as_system()) {
    platf::set_vulkan_hdr_layer_enabled(config::video.dd.vulkan_hdr_layer);
  }
#endif

  auto host_stats_deinit_guard = host_stats::start();

  if (shutdown_event->peek()) {
    return lifetime::desired_exit_code;
  }

  auto proc_deinit_guard = proc::init();
  if (!proc_deinit_guard) {
    BOOST_LOG(error) << "Proc failed to initialize"sv;
  }

  if (shutdown_event->peek()) {
    return lifetime::desired_exit_code;
  }

  reed_solomon_init();
  auto input_deinit_guard = input::init();

  if (input::probe_gamepads()) {
    BOOST_LOG(warning) << "No gamepad input is available"sv;
  }

  auto startup_probe = [&shutdown_event]() {
#ifdef _WIN32
    bool desktop_defer_logged = false;
    bool driver_init_attempted = false;
    bool startup_recovery_checked = false;
    while (!shutdown_event->peek()) {
#endif
      if (video::has_attempted_encoder_probe()) {
        BOOST_LOG(debug) << "Startup encoder probe skipped; probe already attempted.";
        return;
      }

      if (shutdown_event->peek()) {
        return;
      }

#ifdef _WIN32
      const auto has_stream_activity = [] {
        return rtsp_stream::has_pending_launch_or_startup() ||
               rtsp_stream::session_count() != 0 ||
               webrtc_stream::has_active_or_pending_sessions();
      };
      const platf::startup_display_policy::state startup_state {
        .interactive_desktop = platf::is_default_input_desktop_active(),
        .stream_active = has_stream_activity(),
        .shutting_down = shutdown_event->peek(),
      };
      if (platf::startup_display_policy::should_retry(startup_state)) {
        if (!desktop_defer_logged) {
          BOOST_LOG(info) << "Startup display initialization deferred until the interactive desktop is ready; RTSP listener remains available.";
          desktop_defer_logged = true;
        }
        std::this_thread::sleep_for(250ms);
        continue;
      }
      if (!platf::startup_display_policy::should_run(startup_state)) {
        if (startup_state.stream_active) {
          BOOST_LOG(debug) << "Startup display initialization skipped; a streaming session owns the display lifecycle.";
        }
        return;
      }
      if (desktop_defer_logged) {
        BOOST_LOG(info) << "Interactive desktop is ready; resuming deferred startup display initialization.";
      }

      // Keep driver recovery and the startup janitor out of the pre-listener
      // path. Both can block while Windows restarts a virtual-display device,
      // and the janitor must not mutate an intentional display before the
      // user's interactive desktop exists.
      if (!driver_init_attempted && VDISPLAY::should_auto_enable_virtual_display()) {
        BOOST_LOG(info) << "No physical monitors detected after the interactive desktop became ready. Initializing virtual display driver.";
        proc::initVDisplayDriver();
        driver_init_attempted = true;
      }

      if (shutdown_event->peek() || has_stream_activity()) {
        return;
      }

      // Crash-recovery janitor: only run after the interactive desktop is
      // ready and while no RTSP/WebRTC session can claim the display.
      if (!startup_recovery_checked) {
        startup_recovery_checked = true;
        const auto virtual_displays = VDISPLAY::enumerateVirtualDisplays();
        const bool has_active_virtual_display = std::any_of(
          virtual_displays.begin(),
          virtual_displays.end(),
          [](const VDISPLAY::VirtualDisplayInfo &info) {
            return info.is_active;
          }
        );
        if (has_active_virtual_display) {
          BOOST_LOG(warning) << "Startup detected active virtual display(s) with no active stream session; running cleanup.";
          (void) platf::virtual_display_cleanup::run("startup_recovery", config::video.dd.config_revert_on_disconnect);
        }
      }

      if (shutdown_event->peek() || has_stream_activity()) {
        return;
      }

      if (!VDISPLAY::should_auto_enable_virtual_display() && !VDISPLAY::has_active_physical_display()) {
        BOOST_LOG(debug) << "Startup encoder probe skipped; no active display exists and virtual display auto-enable is disabled.";
        return;
      }

      // Ensure the selected adapter has a usable output before a cold probe.
      // The temporary probe target is scoped to this attempt; a later launch
      // will create the client display it actually needs.
      auto encoder_probe_display_result = VDISPLAY::ensure_display();
      if (!encoder_probe_display_result.ready_for_probe()) {
        VDISPLAY::cleanup_ensure_display(encoder_probe_display_result);
        BOOST_LOG(info)
          << "Startup encoder probe skipped because the exact display target did not become usable.";
        return;
      }
      auto cleanup_encoder_probe_display = util::fail_guard([&encoder_probe_display_result]() {
        VDISPLAY::cleanup_ensure_display(encoder_probe_display_result);
      });

      if (shutdown_event->peek()) {
        return;
      }
#endif

      bool encoder_probe_failed = video::probe_encoders();

#ifdef _WIN32
      // Re-resolve the exact retained target before retrying. Never let another
      // active output satisfy readiness for the requested probe display.
      if (encoder_probe_failed && !shutdown_event->peek()) {
        BOOST_LOG(info) << "Startup encoder probe failed; rechecking exact display readiness before retry.";
        auto retry_display_result = VDISPLAY::ensure_display();
        auto cleanup_retry_display = util::fail_guard([&retry_display_result]() {
          VDISPLAY::cleanup_ensure_display(retry_display_result);
        });
        if (retry_display_result.ready_for_probe()) {
          BOOST_LOG(info) << "Exact display target became ready; retrying startup encoder probe.";
          encoder_probe_failed = video::probe_encoders();
        }
      }

#endif

      if (encoder_probe_failed) {
        BOOST_LOG(error) << "Failed to probe encoders during startup.";
      }
      return;
#ifdef _WIN32
    }
#endif
  };

  // Initialize session history in its own directory so database hardening never
  // touches the shared config root that also contains credentials/pairing state.
  {
    std::filesystem::path state_path {config::nvhttp.file_state};
    const auto config_dir = state_path.parent_path();
    const auto history_dir = config_dir / "session_history";
    const auto history_db = history_dir / "session_history.db";

    // Best-effort alpha.1/alpha.2 migration: preserve any database that was
    // created in the shared config root before the storage was isolated.
    const auto legacy_history_db = config_dir / "session_history.db";
    auto move_if_needed = [](const std::filesystem::path &from, const std::filesystem::path &to) -> bool {
      std::error_code ec;
      const bool from_exists = std::filesystem::exists(from, ec);
      if (!from_exists || ec) {
        return false;
      }
      ec.clear();
      const bool to_exists = std::filesystem::exists(to, ec);
      if (to_exists || ec) {
        return false;
      }

      ec.clear();
      std::filesystem::create_directories(to.parent_path(), ec);
      if (ec) {
        BOOST_LOG(warning) << "session_history: failed to create isolated history directory "
                           << to.parent_path().string() << ": " << ec.message();
        return false;
      }

      std::filesystem::rename(from, to, ec);
      if (ec) {
        BOOST_LOG(warning) << "session_history: failed to move legacy database file "
                           << from.string() << " to " << to.string() << ": " << ec.message();
        return false;
      }

      return true;
    };
    const bool migrated_main_db = move_if_needed(legacy_history_db, history_db);
    if (migrated_main_db) {
      move_if_needed(legacy_history_db.string() + "-wal", history_db.string() + "-wal");
      move_if_needed(legacy_history_db.string() + "-shm", history_db.string() + "-shm");
    }

    session_history::init(history_db.string());
  }
  auto session_history_shutdown_guard = util::fail_guard([]() {
    session_history::shutdown();
  });

  if (http::init()) {
    BOOST_LOG(fatal) << "HTTP interface failed to initialize"sv;

#ifdef _WIN32
    BOOST_LOG(fatal) << "To relaunch Apollo successfully, use the shortcut in the Start Menu. Do not run sunshine.exe manually."sv;
    std::this_thread::sleep_for(10s);
#endif

    return -1;
  }

#ifdef _WIN32
  // Start Playnite integration (IPC + handlers)
  auto playnite_integration_guard = platf::playnite::start();
#endif

  std::unique_ptr<platf::deinit_t> mDNS;
  auto sync_mDNS = std::async(std::launch::async, [&mDNS]() {
    if (config::sunshine.enable_discovery) {
      mDNS = platf::publish::start();
    }
  });

  std::unique_ptr<platf::deinit_t> upnp_unmap;
  auto sync_upnp = std::async(std::launch::async, [&upnp_unmap]() {
    upnp_unmap = upnp::start();
  });

  // FIXME: Temporary workaround: Simple-Web_server needs to be updated or replaced
  if (shutdown_event->peek()) {
    return lifetime::desired_exit_code;
  }

  std::thread httpThread {nvhttp::start};
  std::thread configThread {confighttp::start};
  std::thread rtspThread {rtsp_stream::start};

  // Start listeners before any display-driver recovery or cold encoder probe.
  // A boot-time driver restart can take several bounded attempts; it must not
  // make the service unreachable while the interactive desktop converges.
  startup_probe();

#ifdef _WIN32
  // If we're using the default port and GameStream is enabled, warn the user
  if (config::sunshine.port == 47989 && is_gamestream_enabled()) {
    BOOST_LOG(fatal) << "GameStream is still enabled in GeForce Experience! This *will* cause streaming problems with Apollo!"sv;
    BOOST_LOG(fatal) << "Disable GameStream on the SHIELD tab in GeForce Experience or change the Port setting on the Advanced tab in the Apollo Web UI."sv;
  }
#endif

  // Wait for shutdown
  shutdown_event->view();
  // Arm the owned watchdog from main so signal handlers never construct
  // watchdog threads or queue watchdog work from signal context.
  shutdown_deadline.arm();

#ifdef WIN32
  // Join the hidden shutdown-notification window while the deadline watchdog
  // is still armed. The guard remains for early-return paths only.
  shutdown_session_monitor();
  session_monitor_join_thread_guard.disable();
#endif

#ifdef _WIN32
  // Stop the owned lock-screen virtual-output worker before recovery workers
  // can publish more overrides. Both use configuration, the display helper,
  // and mail, all of which remain live until these joins complete.
  config::request_deferred_virtual_output_reapply_shutdown();
  VDISPLAY::request_virtual_display_recovery_shutdown();
  config::join_deferred_virtual_output_reapply_worker();
  VDISPLAY::join_virtual_display_recovery_monitors();
#endif

  httpThread.join();
  configThread.join();
  rtspThread.join();

#ifdef _WIN32
  // Full process shutdown cannot leave the paused-session watchdog running.
  // If it survives past main(), CRT teardown can fast-fail while the helper
  // watchdog thread is still unwinding.
  display_helper_integration::stop_watchdog(true);

  // The virtual display watchdog thread also lives in static storage.
  // Ensure it is joined before CRT on-exit handlers destroy the thread object.
  VDISPLAY::cleanup_retained_ensure_display();
  VDISPLAY::closeVDisplayDevice();
#endif

  task_pool.stop();
  task_pool.join();

  // stop system tray
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
  system_tray::end_tray();
#endif

#ifdef WIN32
  // Restore global NVIDIA control panel settings
  if (nvprefs_instance.owning_undo_file() && nvprefs_instance.load()) {
    nvprefs_instance.restore_global_profile();
    nvprefs_instance.unload();
  }
#endif

  session_history::shutdown();
  session_history_shutdown_guard.disable();

  return lifetime::desired_exit_code;
}
