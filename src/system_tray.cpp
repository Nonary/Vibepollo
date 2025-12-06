/**
 * @file src/system_tray.cpp
 * @brief Definitions for the system tray icon and notification system.
 */
// macros
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1

  #if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include "platform/windows/utils.h"

    #include <Windows.h>
    #include <accctrl.h>
    #include <aclapi.h>
    #define TRAY_ICON WEB_DIR "images/apollo.ico"
    #define TRAY_ICON_PLAYING WEB_DIR "images/apollo-playing.ico"
    #define TRAY_ICON_PAUSING WEB_DIR "images/apollo-pausing.ico"
  #define TRAY_ICON_LOCKED WEB_DIR "images/apollo-locked.ico"
#elif defined(__linux__) || defined(linux) || defined(__linux)
  #define TRAY_ICON SUNSHINE_TRAY_PREFIX "-tray"
  #define TRAY_ICON_PLAYING SUNSHINE_TRAY_PREFIX "-playing"
  #define TRAY_ICON_PAUSING SUNSHINE_TRAY_PREFIX "-pausing"
    #define TRAY_ICON_LOCKED SUNSHINE_TRAY_PREFIX "-locked"
  #elif defined(__APPLE__) || defined(__MACH__)
    #define TRAY_ICON WEB_DIR "images/logo-apollo-16.png"
    #define TRAY_ICON_PLAYING WEB_DIR "images/apollo-playing-16.png"
    #define TRAY_ICON_PAUSING WEB_DIR "images/apollo-pausing-16.png"
    #define TRAY_ICON_LOCKED WEB_DIR "images/apollo-locked-16.png"
    #include <dispatch/dispatch.h>
  #endif

  #define TRAY_MSG_NO_APP_RUNNING "Reload Apps"

  // standard includes
  #include <atomic>
  #include <csignal>
  #include <cwchar>
  #include <string>
  #include <thread>

  // lib includes
  #include <boost/filesystem.hpp>
  #include <tray/src/tray.h>

  // local includes
  #include "config.h"
  #include "confighttp.h"
  #include "logging.h"
  #include "network.h"
  #include "platform/common.h"
  #include "process.h"
  #include "src/entry_handler.h"
  #include "update.h"

using namespace std::literals;

// system_tray namespace
namespace system_tray {
  static std::atomic<bool> tray_initialized = false;
  static std::atomic<bool> tray_thread_running = false;
  static std::atomic<bool> tray_thread_should_exit = false;
  static std::thread tray_thread;

  static int init_tray();

  void tray_open_ui_cb([[maybe_unused]] struct tray_menu *item) {
    BOOST_LOG(info) << "Opening UI from system tray"sv;
    launch_ui();
  }

  void
    tray_force_stop_cb(struct tray_menu *item) {
    BOOST_LOG(info) << "Force stop from system tray"sv;
    proc::proc.terminate(true);
  }

  void tray_restart_cb([[maybe_unused]] struct tray_menu *item) {
    BOOST_LOG(info) << "Restarting from system tray"sv;

    proc::proc.terminate();
    platf::restart();
  }

  void tray_quit_cb([[maybe_unused]] struct tray_menu *item) {
    BOOST_LOG(info) << "Quitting from system tray"sv;

    proc::proc.terminate();

  #ifdef _WIN32
    // If we're running in a service, return a special status to
    // tell it to terminate too, otherwise it will just respawn us.
    if (GetConsoleWindow() == nullptr) {
      lifetime::exit_sunshine(ERROR_SHUTDOWN_IN_PROGRESS, true);
      return;
    }
  #endif

    lifetime::exit_sunshine(0, true);
  }

  // Tray menu
  static struct tray tray = {
    .icon = TRAY_ICON,
    .tooltip = PROJECT_NAME,
    .menu =
      (struct tray_menu[]) {
        // todo - use boost/locale to translate menu strings
        {.text = "Open Apollo", .cb = tray_open_ui_cb},
        {.text = "-"},
        // { .text = "-" },
        // { .text = "Donate",
        //   .submenu =
        //   (struct tray_menu[]) {
        //   { .text = "GitHub Sponsors", .cb = tray_donate_github_cb },
        //   { .text = "MEE6", .cb = tray_donate_mee6_cb },
        //   { .text = "Patreon", .cb = tray_donate_patreon_cb },
        //   { .text = "PayPal", .cb = tray_donate_paypal_cb },
        //   { .text = nullptr } } },
        // { .text = "-" },
        {.text = TRAY_MSG_NO_APP_RUNNING, .cb = tray_force_stop_cb},
        {.text = "Check for Update", .cb = [](tray_menu *) {
           BOOST_LOG(info) << "Manual update check requested from tray"sv;
           update::trigger_check(true);
         }},

        {.text = "Restart", .cb = tray_restart_cb},
        {.text = "Quit", .cb = tray_quit_cb},
        {.text = nullptr}
      },
    .iconPathCount = 4,
    .allIconPaths = {TRAY_ICON, TRAY_ICON_LOCKED, TRAY_ICON_PLAYING, TRAY_ICON_PAUSING},
  };

  static int init_tray() {
  #ifdef _WIN32
    // If we're running as SYSTEM, Explorer.exe will not have permission to open our thread handle
    // to monitor for thread termination. If Explorer fails to open our thread, our tray icon
    // will persist forever if we terminate unexpectedly. To avoid this, we will modify our thread
    // DACL to add an ACE that allows SYNCHRONIZE access to Everyone.
    {
      PACL old_dacl;
      PSECURITY_DESCRIPTOR sd;
      auto error = GetSecurityInfo(GetCurrentThread(), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &old_dacl, nullptr, &sd);
      if (error != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "GetSecurityInfo() failed: "sv << error;
        return 1;
      }

      auto free_sd = util::fail_guard([sd]() {
        LocalFree(sd);
      });

      SID_IDENTIFIER_AUTHORITY sid_authority = SECURITY_WORLD_SID_AUTHORITY;
      PSID world_sid;
      if (!AllocateAndInitializeSid(&sid_authority, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &world_sid)) {
        error = GetLastError();
        BOOST_LOG(warning) << "AllocateAndInitializeSid() failed: "sv << error;
        return 1;
      }

      auto free_sid = util::fail_guard([world_sid]() {
        FreeSid(world_sid);
      });

      EXPLICIT_ACCESS ea {};
      ea.grfAccessPermissions = SYNCHRONIZE;
      ea.grfAccessMode = GRANT_ACCESS;
      ea.grfInheritance = NO_INHERITANCE;
      ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
      ea.Trustee.ptstrName = (LPSTR) world_sid;

      PACL new_dacl;
      error = SetEntriesInAcl(1, &ea, old_dacl, &new_dacl);
      if (error != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "SetEntriesInAcl() failed: "sv << error;
        return 1;
      }

      auto free_new_dacl = util::fail_guard([new_dacl]() {
        LocalFree(new_dacl);
      });

      error = SetSecurityInfo(GetCurrentThread(), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, new_dacl, nullptr);
      if (error != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "SetSecurityInfo() failed: "sv << error;
        return 1;
      }
    }

    // Wait for the shell to be initialized before registering the tray icon.
    // This ensures the tray icon works reliably after a logoff/logon cycle.
    while (GetShellWindow() == nullptr) {
      Sleep(1000);
    }

    auto wait_for_default_desktop = []() {
      constexpr int attempts = 60;
      for (int attempt = 0; attempt < attempts; ++attempt) {
        HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS | DESKTOP_ENUMERATE);
        if (desktop != nullptr) {
          auto close_desktop = util::fail_guard([desktop]() {
            CloseDesktop(desktop);
          });

          WCHAR desktop_name[256] = {};
          DWORD required_length = 0;
          if (GetUserObjectInformationW(desktop, UOI_NAME, desktop_name, sizeof(desktop_name), &required_length)) {
            if (_wcsicmp(desktop_name, L"Default") == 0) {
              return true;
            }
          }
        }

        Sleep(1000);
      }

      return false;
    };

    if (!wait_for_default_desktop()) {
      BOOST_LOG(warning) << "Timed out waiting for interactive desktop; system tray may not appear"sv;
    } else {
      BOOST_LOG(debug) << "Interactive desktop ready for tray initialization"sv;
    }
  #endif

    int attempt = 0;
    int tray_init_result = -1;
    while (tray_init_result < 0 && attempt < 30) {
      tray_init_result = tray_init(&tray);
      if (tray_init_result >= 0) {
        break;
      }
#ifdef _WIN32
      auto last_error = GetLastError();
      BOOST_LOG(warning) << "Failed to create system tray (attempt "sv << attempt + 1 << ", error " << last_error << ')';
#else
      BOOST_LOG(warning) << "Failed to create system tray (attempt "sv << attempt + 1 << ')';
#endif
      std::this_thread::sleep_for(2s);
      ++attempt;
    }

    if (tray_init_result < 0) {
      BOOST_LOG(warning) << "Failed to create system tray after retries"sv;
      return 1;
    }

    BOOST_LOG(info) << "System tray created"sv;
    tray_initialized = true;
    return 0;
  }

  int system_tray() {
    if (init_tray() != 0) {
      return 1;
    }

    while (tray_loop(1) == 0) {
      BOOST_LOG(debug) << "System tray loop"sv;
    }

    tray_exit();
    tray_initialized = false;
    return 0;
  }

  void run_tray() {
    // create the system tray
  #if defined(__APPLE__) || defined(__MACH__)
    // macOS requires that UI elements be created on the main thread
    // creating tray using dispatch queue does not work, although the code doesn't actually throw any (visible) errors

    // dispatch_async(dispatch_get_main_queue(), ^{
    //   system_tray();
    // });

    BOOST_LOG(info) << "system_tray() is not yet implemented for this platform."sv;
  #else  // Windows, Linux
    // create tray in separate thread
    std::thread tray_thread(system_tray);
    tray_thread.detach();
  #endif
  }

  int end_tray() {
    tray_initialized = false;
    tray_exit();
    return 0;
  }

  // Persistent storage for tooltip/notification strings to avoid dangling pointers
  static std::string s_tooltip;
  static std::string s_notification_text;
  static std::string s_last_playing_app;

  void update_tray_playing(std::string app_name) {
    if (!tray_initialized) {
      return;
    }

    if (!app_name.empty() && app_name == s_last_playing_app && tray.icon == TRAY_ICON_PLAYING) {
      return;
    }

    tray.notification_title = nullptr;
    tray.notification_text = nullptr;
    tray.notification_cb = nullptr;
    tray.notification_icon = nullptr;
    tray.icon = TRAY_ICON_PLAYING;

    char msg[256];
    static char force_close_msg[256];
    snprintf(msg, std::size(msg), "%s launched.", app_name.c_str());
    snprintf(force_close_msg, std::size(force_close_msg), "Force close [%s]", app_name.c_str());
  #ifdef _WIN32
    auto msg_acp = utf8ToAcp(msg);
    auto force_msg_acp = utf8ToAcp(force_close_msg);
    strncpy(msg, msg_acp.c_str(), std::size(msg) - 1);
    msg[std::size(msg) - 1] = '\0';
    strncpy(force_close_msg, force_msg_acp.c_str(), std::size(force_close_msg) - 1);
    force_close_msg[std::size(force_close_msg) - 1] = '\0';
  #endif
    s_notification_text = msg;
    s_tooltip = "Streaming started for " + app_name;
    tray.notification_title = "App launched";
    tray.notification_text = s_notification_text.c_str();
    tray.notification_icon = TRAY_ICON_PLAYING;
    tray.tooltip = s_tooltip.c_str();
    tray.menu[2].text = force_close_msg;
    s_last_playing_app = app_name;
    tray_update(&tray);
  }

  void update_tray_pausing(std::string app_name) {
    if (!tray_initialized) {
      return;
    }

    tray.notification_cb = nullptr;
    tray.notification_icon = nullptr;
    tray.notification_text = nullptr;
    tray.notification_title = nullptr;
    tray.icon = TRAY_ICON_PAUSING;

    char msg[256];
    snprintf(msg, std::size(msg), "Streaming paused for %s", app_name.c_str());
  #ifdef _WIN32
    auto msg_acp = utf8ToAcp(msg);
    strncpy(msg, msg_acp.c_str(), std::size(msg) - 1);
    msg[std::size(msg) - 1] = '\0';
  #endif
    s_notification_text = msg;
    tray.notification_title = "Stream Paused";
    tray.notification_text = s_notification_text.c_str();
    tray.notification_icon = TRAY_ICON_PAUSING;
    tray.tooltip = s_notification_text.c_str();
    tray_update(&tray);
  }

  void update_tray_stopped(std::string app_name) {
    if (!tray_initialized) {
      return;
    }

    tray.notification_cb = nullptr;
    tray.notification_icon = nullptr;
    tray.notification_text = nullptr;
    tray.notification_title = nullptr;
    tray.icon = TRAY_ICON;

    char msg[256];
    snprintf(msg, std::size(msg), "Streaming stopped for %s", app_name.c_str());
  #ifdef _WIN32
    auto msg_acp = utf8ToAcp(msg);
    strncpy(msg, msg_acp.c_str(), std::size(msg) - 1);
    msg[std::size(msg) - 1] = '\0';
  #endif
    s_notification_text = msg;
    tray.notification_icon = TRAY_ICON;
    tray.notification_title = "Application Stopped";
    tray.notification_text = s_notification_text.c_str();
    tray.tooltip = PROJECT_NAME;
    tray.menu[2].text = TRAY_MSG_NO_APP_RUNNING;
    s_last_playing_app.clear();
    tray_update(&tray);
  }

  void
    update_tray_launch_error(std::string app_name, int exit_code) {
    if (!tray_initialized) {
      return;
    }

    tray.notification_title = nullptr;
    tray.notification_text = nullptr;
    tray.notification_cb = nullptr;
    tray.notification_icon = nullptr;
    tray.icon = TRAY_ICON;

    char msg[256];
    snprintf(msg, std::size(msg), "Application %s exited too fast with code %d. Click here to terminate the stream.", app_name.c_str(), exit_code);
  #ifdef _WIN32
    auto msg_acp = utf8ToAcp(msg);
    strncpy(msg, msg_acp.c_str(), std::size(msg) - 1);
    msg[std::size(msg) - 1] = '\0';
  #endif
    s_notification_text = msg;
    tray.notification_icon = TRAY_ICON;
    tray.notification_title = "Launch Error";
    tray.notification_text = s_notification_text.c_str();
    tray.notification_cb = []() {
      BOOST_LOG(info) << "Force stop from notification"sv;
      proc::proc.terminate();
    };
    tray.tooltip = PROJECT_NAME;
    s_last_playing_app.clear();
    tray_update(&tray);
  }

  void update_tray_require_pin() {
    if (!tray_initialized) {
      return;
    }

    tray.notification_title = nullptr;
    tray.notification_text = nullptr;
    tray.notification_cb = nullptr;
    tray.notification_icon = nullptr;
    tray.icon = TRAY_ICON;

    tray.notification_title = "Incoming Pairing Request";
    tray.notification_text = "Click here to complete the pairing process";
    tray.notification_icon = TRAY_ICON_LOCKED;
    tray.tooltip = PROJECT_NAME;
    tray.notification_cb = []() {
      launch_ui("/clients");
    };
    tray_update(&tray);
  }

  void
    update_tray_paired(std::string device_name) {
    if (!tray_initialized) {
      return;
    }

    tray.notification_title = nullptr;
    tray.notification_text = nullptr;
    tray.notification_cb = nullptr;
    tray.notification_icon = nullptr;

    char msg[256];
    snprintf(msg, std::size(msg), "Device %s paired Succesfully. Please make sure you have access to the device.", device_name.c_str());
  #ifdef _WIN32
    auto msg_acp = utf8ToAcp(msg);
    strncpy(msg, msg_acp.c_str(), std::size(msg) - 1);
    msg[std::size(msg) - 1] = '\0';
  #endif
    tray.notification_title = "Device Paired Succesfully";
    tray.notification_text = msg;
    tray.notification_icon = TRAY_ICON;
    tray.tooltip = PROJECT_NAME;
    tray_update(&tray);
  }

  void
    update_tray_client_connected(std::string client_name) {
    if (!tray_initialized) {
      return;
    }

    tray.notification_title = nullptr;
    tray.notification_text = nullptr;
    tray.notification_cb = nullptr;
    tray.notification_icon = nullptr;
    tray.icon = TRAY_ICON;

    char msg[256];
    snprintf(msg, std::size(msg), "%s has connected to the session.", client_name.c_str());
  #ifdef _WIN32
    auto msg_acp = utf8ToAcp(msg);
    strncpy(msg, msg_acp.c_str(), std::size(msg) - 1);
    msg[std::size(msg) - 1] = '\0';
  #endif
    tray.notification_title = "Client Connected";
    tray.notification_text = msg;
    tray.notification_icon = TRAY_ICON;
    tray.tooltip = PROJECT_NAME;
    tray_update(&tray);
  }

  void update_tray_vigem_missing() {
    if (!tray_initialized) {
      return;
    }

    tray.notification_title = nullptr;
    tray.notification_text = nullptr;
    tray.notification_cb = nullptr;
    tray.notification_icon = nullptr;
    tray.icon = TRAY_ICON;

    tray.notification_title = "Gamepad Input Unavailable";
    tray.notification_text = "ViGEm is not installed. Click for setup info";
    tray.notification_icon = TRAY_ICON;
    tray.tooltip = PROJECT_NAME;
    tray.notification_cb = []() {
      launch_ui("/");
    };
    tray_update(&tray);
  }

  // Threading functions available on all platforms
  static void tray_thread_worker() {
    BOOST_LOG(info) << "System tray thread started"sv;

    // Initialize the tray in this thread
    if (init_tray() != 0) {
      BOOST_LOG(error) << "Failed to initialize tray in thread"sv;
      tray_thread_running = false;
      return;
    }

    tray_thread_running = true;

    // Run the event loop
    while (!tray_thread_should_exit) {
      tray_loop(1);
    }

    // Cleanup
    tray_exit();
    tray_initialized = false;
    tray_thread_running = false;
    BOOST_LOG(info) << "System tray thread ended"sv;
  }

  int init_tray_threaded() {
    if (tray_thread_running) {
      BOOST_LOG(warning) << "Tray thread is already running"sv;
      return 1;
    }

  #ifdef _WIN32
    std::string tmp_str = "Open Apollo (" + config::nvhttp.sunshine_name + ":" + std::to_string(net::map_port(confighttp::PORT_HTTPS)) + ")";
    static const std::string title_str = utf8ToAcp(tmp_str);
  #else
    static const std::string title_str = "Open Apollo (" + config::nvhttp.sunshine_name + ":" + std::to_string(net::map_port(confighttp::PORT_HTTPS)) + ")";
  #endif
    tray.menu[0].text = title_str.c_str();

    if (config::sunshine.hide_tray_controls) {
      tray.menu[1].text = nullptr;
    }

    tray_thread_should_exit = false;

    try {
      tray_thread = std::thread(tray_thread_worker);

      // Wait for the thread to start and initialize
      const auto start_time = std::chrono::steady_clock::now();
      while (!tray_thread_running && !tray_thread_should_exit) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        // Timeout after 10 seconds
        if (std::chrono::steady_clock::now() - start_time > std::chrono::seconds(10)) {
          BOOST_LOG(error) << "Tray thread initialization timeout"sv;
          tray_thread_should_exit = true;
          if (tray_thread.joinable()) {
            tray_thread.join();
          }
          return 1;
        }
      }

      if (!tray_thread_running) {
        BOOST_LOG(error) << "Tray thread failed to start"sv;
        if (tray_thread.joinable()) {
          tray_thread.join();
        }
        return 1;
      }

      BOOST_LOG(info) << "System tray thread initialized successfully"sv;
      return 0;
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "Failed to create tray thread: " << e.what();
      return 1;
    }
  }

  int end_tray_threaded() {
    if (!tray_thread_running) {
      return 0;
    }

    BOOST_LOG(info) << "Stopping system tray thread"sv;
    tray_thread_should_exit = true;

    if (tray_thread.joinable()) {
      tray_thread.join();
    }

    return 0;
  }

  void tray_notify(const char *title, const char *text, void (*cb)()) {
    if (!tray_initialized) {
      return;
    }

    tray.notification_title = nullptr;
    tray.notification_text = nullptr;
    tray.notification_cb = nullptr;
    tray.notification_icon = nullptr;
    tray.icon = TRAY_ICON;
    tray_update(&tray);

    tray.icon = TRAY_ICON;
    tray.notification_title = title;
    s_notification_text = text ? std::string {text} : std::string {};
    tray.notification_text = s_notification_text.c_str();
    tray.notification_icon = TRAY_ICON;
    tray.tooltip = PROJECT_NAME;
    tray.notification_cb = cb;
    tray_update(&tray);
  }

}  // namespace system_tray

#endif
