/**
 * @file src/main.cpp
 * @brief Definitions for the main entry point for Sunshine.
 */
// standard includes
#include <codecvt>
#include <csignal>
#include <fstream>
#include <iostream>

// local includes
#include "confighttp.h"
#include "entry_handler.h"
#include "globals.h"
#include "httpcommon.h"
#include "logging.h"
#include "main.h"
#include "nvhttp.h"
#include "process.h"
#include "system_tray.h"
#include "update.h"
#include "upnp.h"
#include "uuid.h"
#include "video.h"
#ifdef _WIN32
  #include "src/platform/windows/playnite_integration.h"
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

void on_signal_forwarder(int sig) {
  signal_handlers.at(sig)();
}

template<class FN>
void on_signal(int sig, FN &&fn) {
  signal_handlers.emplace(sig, std::forward<FN>(fn));

  std::signal(sig, on_signal_forwarder);
}

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
        // Terminate ourselves with a blocking exit call
        std::cout << "Received WM_ENDSESSION"sv << std::endl;
        lifetime::exit_sunshine(0, false);
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

  task_pool_util::TaskPool::task_id_t force_shutdown = nullptr;

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

  // Log publisher metadata
  log_publisher_data();

  // Log modified_config_settings
  for (auto &[name, val] : config::modified_config_settings) {
    BOOST_LOG(info) << "config: '"sv << name << "' = "sv << val;
  }
  config::modified_config_settings.clear();

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
  auto session_monitor_hwnd_future = session_monitor_hwnd_promise.get_future();
  std::promise<void> session_monitor_join_thread_promise;
  auto session_monitor_join_thread_future = session_monitor_join_thread_promise.get_future();

  std::thread session_monitor_thread([&]() {
    session_monitor_join_thread_promise.set_value_at_thread_exit();

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
    MSG msg {};
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
  });

  auto session_monitor_join_thread_guard = util::fail_guard([&]() {
    if (session_monitor_hwnd_future.wait_for(1s) == std::future_status::ready) {
      if (HWND session_monitor_hwnd = session_monitor_hwnd_future.get()) {
        PostMessage(session_monitor_hwnd, WM_CLOSE, 0, 0);
      }

      if (session_monitor_join_thread_future.wait_for(1s) == std::future_status::ready) {
        session_monitor_thread.join();
        return;
      } else {
        BOOST_LOG(warning) << "session_monitor_join_thread_future reached timeout";
      }
    } else {
      BOOST_LOG(warning) << "session_monitor_hwnd_future reached timeout";
    }

    session_monitor_thread.detach();
  });

#endif

  task_pool.start(1);

#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
  // create tray thread and detach it
  system_tray::run_tray();
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

  // Create signal handler after logging has been initialized
  auto shutdown_event = mail::man->event<bool>(mail::shutdown);
  on_signal(SIGINT, [&force_shutdown, shutdown_event]() {
    BOOST_LOG(info) << "Interrupt handler called"sv;

    auto task = []() {
      BOOST_LOG(fatal) << "10 seconds passed, yet Sunshine's still running: Forcing shutdown"sv;
      logging::log_flush();
      lifetime::debug_trap();
    };

    proc::proc.terminate();

    force_shutdown = task_pool.pushDelayed(task, 10s).task_id;

    shutdown_event->raise(true);
  });

  on_signal(SIGTERM, [&force_shutdown, shutdown_event]() {
    BOOST_LOG(info) << "Terminate handler called"sv;

    auto task = []() {
      BOOST_LOG(fatal) << "10 seconds passed, yet Sunshine's still running: Forcing shutdown"sv;
      logging::log_flush();
      lifetime::debug_trap();
    };
    force_shutdown = task_pool.pushDelayed(task, 10s).task_id;

    shutdown_event->raise(true);
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

  auto proc_deinit_guard = proc::init();
  if (!proc_deinit_guard) {
    BOOST_LOG(error) << "Proc failed to initialize"sv;
  }

  reed_solomon_init();
  auto input_deinit_guard = input::init();

  if (input::probe_gamepads()) {
    BOOST_LOG(warning) << "No gamepad input is available"sv;
  }

  if (video::probe_encoders()) {
#ifdef _WIN32
    bool allow_probing = video::allow_encoder_probing();
    using dd_config_option_e = config::video_t::dd_t::config_option_e;
    const auto dd_option = config::video.dd.configuration_option;
    const bool dd_available = dd_option != dd_config_option_e::disabled && !config::video.headless_mode;

    bool encoder_recovered = false;
    bool dd_bootstrap_applied = false;

    if (dd_available) {
      rtsp_stream::launch_session_t dd_probe_session {};
      dd_probe_session.width = 1920;
      dd_probe_session.height = 1080;
      dd_probe_session.fps = 60000;
      dd_probe_session.enable_sops = true;
      dd_probe_session.enable_hdr = false;
      dd_probe_session.scale_factor = 100;
      dd_probe_session.virtual_display = false;
      dd_probe_session.device_name = "Display Helper Probe";
      dd_probe_session.unique_id = PROBE_DISPLAY_UUID;

      BOOST_LOG(info) << "Display helper bootstrap requested for encoder probing."sv;
      dd_bootstrap_applied = display_helper_integration::apply_from_session(config::video, dd_probe_session);
      if (dd_bootstrap_applied) {
        if (!video::probe_encoders()) {
          encoder_recovered = true;
        } else {
          BOOST_LOG(warning) << "Encoder probe still failing after display helper bootstrap."sv;
        }
      } else {
        BOOST_LOG(info) << "Display helper bootstrap unavailable; continuing with virtual display fallback if needed."sv;
      }
    }

    if (dd_bootstrap_applied) {
      display_helper_integration::revert();
    }

    // Create a temporary virtual display for encoder capability probing if display helper could not recover
    if (!encoder_recovered && proc::vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::OK) {
      std::string probe_uuid_str = PROBE_DISPLAY_UUID;
      auto probe_uuid = uuid_util::uuid_t::parse(probe_uuid_str);
      auto *probe_guid = (GUID *) (void *) &probe_uuid;

      BOOST_LOG(info) << "Creating a temporary virtual display to probe for encoders..."sv;

      if (!config::video.adapter_name.empty()) {
        VDISPLAY::setRenderAdapterByName(platf::from_utf8(config::video.adapter_name));
      }

      VDISPLAY::createVirtualDisplay(
        probe_uuid_str.c_str(),
        "Probe",
        800,
        600,
        60,
        *probe_guid
      );

      std::this_thread::sleep_for(500ms);

      // Probe again anyways
      if (video::probe_encoders()) {
        if (allow_probing) {
          BOOST_LOG(error) << "Video failed to find working encoder: allow probing but failed"sv;
        } else {
          BOOST_LOG(error) << "Video failed to find working encoder even after attempted with a virtual display"sv;
        }
      } else {
        encoder_recovered = true;
      }

      VDISPLAY::removeVirtualDisplay(*probe_guid);
    } else if (!encoder_recovered && !allow_probing) {
      BOOST_LOG(error) << "Video failed to find working encoder: probe failed and virtual display driver isn't initialized"sv;
    }
#else
    BOOST_LOG(error) << "Video failed to find working encoder: probing failed."sv;
#endif
  }

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

#ifdef _WIN32
  // If we're using the default port and GameStream is enabled, warn the user
  if (config::sunshine.port == 47989 && is_gamestream_enabled()) {
    BOOST_LOG(fatal) << "GameStream is still enabled in GeForce Experience! This *will* cause streaming problems with Apollo!"sv;
    BOOST_LOG(fatal) << "Disable GameStream on the SHIELD tab in GeForce Experience or change the Port setting on the Advanced tab in the Apollo Web UI."sv;
  }
#endif

  // Wait for shutdown
  shutdown_event->view();

  httpThread.join();
  configThread.join();
  rtspThread.join();

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

  return lifetime::desired_exit_code;
}
