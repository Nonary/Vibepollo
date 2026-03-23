/**
 * @file src/main.cpp
 * @brief Definitions for the main entry point for Sunshine.
 */
// standard includes
#include <algorithm>
#include <codecvt>
#include <csignal>
#include <fstream>
#include <iostream>

// local includes
#include "config.h"
#include "confighttp.h"
#include "entry_handler.h"
#include "globals.h"
#include "httpcommon.h"
#include "logging.h"
#include "main.h"
#include "nvhttp.h"
#include "process.h"
#include "rtsp.h"
#include "system_tray.h"
#include "update.h"
#include "upnp.h"
#include "uuid.h"
#include "video.h"
#include "webrtc_stream.h"
#ifdef _WIN32
  #include <shobjidl.h>

  #include "src/platform/windows/display_helper_integration.h"
  #include "src/platform/windows/frame_limiter_nvcp.h"
  #include "src/platform/windows/playnite_integration.h"
  #include "src/platform/windows/rtss_integration.h"
  #include "src/platform/windows/virtual_display.h"
  #include "src/platform/windows/virtual_display_cleanup.h"
#endif

#ifdef _WIN32
  #include "platform/windows/misc.h"
  #include "platform/windows/display_helper_integration.h"
  #include "platform/windows/virtual_display.h"
  #include <shellapi.h>
  #include <mmdeviceapi.h>
  #include <functiondiscoverykeys_devpkey.h>
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
#ifdef PROJECT_VERSION_PRERELEASE
  if (std::string_view(PROJECT_VERSION_PRERELEASE).size() > 0) {
    BOOST_LOG(info) << "Prerelease build detected; default min_log_level is debug unless overridden.";
  }
#endif
  BOOST_LOG(info) << "Effective min_log_level=" << config::sunshine.min_log_level;

  // Log mic passthrough configuration for diagnostics
  if (!config::audio.mic_sink.empty()) {
    BOOST_LOG(info) << "[mic] mic_sink = \"" << config::audio.mic_sink << "\"";
    BOOST_LOG(info) << "[mic] mic_capture_device = \"" << config::audio.mic_capture_device << "\"";
  } else {
    BOOST_LOG(info) << "[mic] mic_sink not configured — mic passthrough will be disabled";
  }

#ifdef _WIN32
  // Validate mic_capture_device against available WASAPI capture devices at startup.
  if (!config::audio.mic_capture_device.empty()) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *pEnum = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void **) &pEnum);
    if (SUCCEEDED(hr) && pEnum) {
      IMMDeviceCollection *pCollection = nullptr;
      hr = pEnum->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &pCollection);
      if (SUCCEEDED(hr) && pCollection) {
        UINT count = 0;
        pCollection->GetCount(&count);
        bool found = false;
        std::string device_list;
        for (UINT i = 0; i < count; ++i) {
          IMMDevice *pDev = nullptr;
          if (SUCCEEDED(pCollection->Item(i, &pDev)) && pDev) {
            IPropertyStore *pProps = nullptr;
            if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READ, &pProps)) && pProps) {
              PROPVARIANT varName;
              PropVariantInit(&varName);
              if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &varName)) && varName.pwszVal) {
                int len = WideCharToMultiByte(CP_UTF8, 0, varName.pwszVal, -1, nullptr, 0, nullptr, nullptr);
                std::string name(len - 1, '\0');
                WideCharToMultiByte(CP_UTF8, 0, varName.pwszVal, -1, name.data(), len, nullptr, nullptr);
                if (!device_list.empty()) device_list += ", ";
                device_list += "\"" + name + "\"";
                if (name.find(config::audio.mic_capture_device) != std::string::npos) found = true;
              }
              PropVariantClear(&varName);
              pProps->Release();
            }
            pDev->Release();
          }
        }
        if (!found) {
          BOOST_LOG(warning) << "[mic] WARNING: mic_capture_device '" << config::audio.mic_capture_device
                             << "' not found. Available devices: [" << device_list << "]";
        } else {
          BOOST_LOG(info) << "[mic] mic_capture_device '" << config::audio.mic_capture_device << "' found among capture devices";
        }
        pCollection->Release();
      }
      pEnum->Release();
    }
    CoUninitialize();
  }
#endif

  // Log rotation: keep the 10 most recent log files, delete the rest.
  {
    namespace fs = std::filesystem;
    auto log_dir = platf::appdata() / "logs";
    if (fs::exists(log_dir)) {
      std::vector<fs::directory_entry> log_files;
      for (auto &entry : fs::directory_iterator(log_dir)) {
        if (entry.is_regular_file()) log_files.push_back(entry);
      }
      std::sort(log_files.begin(), log_files.end(), [](const fs::directory_entry &a, const fs::directory_entry &b) {
        return a.last_write_time() > b.last_write_time();
      });
      constexpr std::size_t k_max_logs = 10;
      for (std::size_t i = k_max_logs; i < log_files.size(); ++i) {
        std::error_code ec;
        fs::remove(log_files[i].path(), ec);
        if (!ec) BOOST_LOG(debug) << "Log rotation: removed " << log_files[i].path().filename().string();
      }
    }
  }

  // Log publisher metadata
  log_publisher_data();

  // Log modified_config_settings
  for (auto &[name, val] : config::modified_config_settings) {
    BOOST_LOG(info) << "config: '"sv << name << "' = "sv << val;
  }
  config::modified_config_settings.clear();

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

  // Apply any pending auto-downloaded update before normal startup.
  // If a downloaded installer is ready, launch it silently and exit so the installer
  // can replace running binaries. Never auto-apply during a stream (session_count > 0
  // at this point would be unusual, but guard anyway).
  if (update::apply_pending_update_if_ready()) {
    return 0;
  }

#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
  // create tray thread and detach it if enabled in config
  if (config::sunshine.system_tray) {
    system_tray::run_tray();
  }

#ifdef _WIN32
  // First-run: open web UI in browser so users can complete initial setup
  // (set credentials, configure apps, etc.) without manually navigating there.
  if (!std::filesystem::exists(config::sunshine.credentials_file)) {
    BOOST_LOG(info) << "First run detected — opening web UI in browser";
    ShellExecuteW(nullptr, L"open", L"https://localhost:47990", nullptr, nullptr, SW_SHOWNORMAL);
  }
#endif

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

#ifdef _WIN32
  // Check if virtual display should be auto-enabled due to no physical monitors
  if (VDISPLAY::should_auto_enable_virtual_display()) {
    BOOST_LOG(info) << "No physical monitors detected at initialization. Initializing virtual display driver.";
    proc::initVDisplayDriver();
  }

  // A helper restore loop can outlive Sunshine itself because the helper runs in a
  // separate process. Disarm any orphaned restore activity before we decide whether
  // additional startup cleanup is needed.
  (void) display_helper_integration::disarm_pending_restore();

  // Crash-recovery janitor: if Sunshine starts and finds active virtual displays before
  // any RTSP/WebRTC sessions exist, force cleanup to prevent stuck fallback issues.
  if (rtsp_stream::session_count() == 0 && !webrtc_stream::has_active_sessions()) {
    const auto virtual_displays = VDISPLAY::enumerateSudaVDADisplays();
    const bool has_active_virtual_display = std::any_of(
      virtual_displays.begin(),
      virtual_displays.end(),
      [](const VDISPLAY::SudaVDADisplayInfo &info) {
        return info.is_active;
      }
    );
    if (has_active_virtual_display) {
      BOOST_LOG(warning) << "Startup detected active virtual display(s) with no active stream session; running cleanup.";
      (void) platf::virtual_display_cleanup::run("startup_recovery", config::video.dd.config_revert_on_disconnect);
    }
  }

  // Startup safety: if CABLE Output is the Windows default input (from a
  // crashed session), reset it to the first real microphone now.
  {
    auto startup_audio = platf::audio_control();
    if (startup_audio) {
      auto def_name = startup_audio->get_current_default_capture_name();
      if (!def_name.empty() && def_name.find("CABLE") != std::string::npos) {
        BOOST_LOG(warning) << "Startup: CABLE Output is default input from crashed session — restoring first real capture device";
        startup_audio->reset_default_capture_to_first_real();
      }
    }
  }
#endif

  reed_solomon_init();
  auto input_deinit_guard = input::init();

  if (input::probe_gamepads()) {
    BOOST_LOG(warning) << "No gamepad input is available"sv;
  }

  auto startup_probe = []() {
    if (video::has_attempted_encoder_probe()) {
      BOOST_LOG(debug) << "Startup encoder probe skipped; probe already attempted.";
      return;
    }

#ifdef _WIN32
    // Ensure a display is available first; probing encoders generally requires a display.
    auto encoder_probe_display_result = VDISPLAY::ensure_display();
    if (!encoder_probe_display_result.success) {
      BOOST_LOG(warning) << "Unable to ensure display for encoder probing. Probe may fail.";
    }

    bool encoder_probe_succeeded = false;
    auto cleanup_encoder_probe_display = util::fail_guard([&encoder_probe_display_result, &encoder_probe_succeeded]() {
      VDISPLAY::cleanup_ensure_display(encoder_probe_display_result, encoder_probe_succeeded, true);
    });
#endif

    bool encoder_probe_failed = video::probe_encoders();

#ifdef _WIN32
    // If the probe failed and there's no active display (headless with VDD),
    // wait for the display to become available via DXGI and retry.
    if (encoder_probe_failed) {
      BOOST_LOG(info) << "Startup encoder probe failed; waiting for display activation before retry.";
      constexpr auto kDisplayActivationTimeout = std::chrono::seconds(5);
      const auto deadline = std::chrono::steady_clock::now() + kDisplayActivationTimeout;
      bool display_activated = false;
      while (std::chrono::steady_clock::now() < deadline) {
        if (VDISPLAY::has_active_physical_display() ||
            !VDISPLAY::enumerateSudaVDADisplays().empty()) {
          display_activated = true;
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }
      if (display_activated) {
        BOOST_LOG(info) << "Display became active; retrying startup encoder probe.";
        encoder_probe_failed = video::probe_encoders();
      }
    }

    encoder_probe_succeeded = !encoder_probe_failed;
#endif

    if (encoder_probe_failed) {
      BOOST_LOG(error) << "Failed to probe encoders during startup.";
    }
  };

  startup_probe();

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

  // Drain any active WebRTC sessions before joining server threads so capture,
  // media, and input teardown finish while the process is still fully alive.
  webrtc_stream::shutdown_all_sessions();

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
