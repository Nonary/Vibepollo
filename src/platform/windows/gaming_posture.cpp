/**
 * @file src/platform/windows/gaming_posture.cpp
 * @brief Windows backend for the gaming posture, the Xbox full screen experience (see src/tablet_posture.h).
 *
 * Windows calls the Xbox full screen experience ("Xbox mode") its gaming posture. The shell's
 * GamingPostureController enters and leaves it, and gamemode.dll exposes the state through
 * IsGamingFullScreenExperienceSupported/Active and a switch, SetGamingFullScreenExperience, with
 * CanSetGamingFullScreenExperience telling whether the switch is available. It is once the user has a
 * home app (Settings > Gaming > Xbox mode, normally the Xbox app); without one nothing can enter.
 *
 * Windows refuses the switch (E_ACCESSDENIED) for a process that does not run as the logged-on user,
 * also while such a process impersonates that user, and reports the experience as inactive to it. A
 * host service therefore starts this executable once more as the user, with gaming_switch_arg: that
 * instance reads the state, switches only when it differs, confirms the new state and reports it as
 * its exit code. There is no blind toggle (the Win+F11 shortcut is not used). When the experience is
 * not supported, has no home app, or does not change, the caller keeps the host in tablet mode.
 *
 * Entering and leaving ask for confirmation. Windows remembers the answers in the user's hive under
 * GamingConfiguration\SystemDialogResults; the machine hive is not consulted. The answers are
 * supplied there only while the host switches and removed afterwards; a journal file lets a
 * restarted host remove them after a crash. A choice the user made is never touched.
 */
// standard includes
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

// platform includes
#include <winsock2.h>
#include <windows.h>

// local includes
#include "misc.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/tablet_posture.h"

using namespace std::literals;

namespace tablet_posture::backend {
  bool apply_gaming(bool enter);

  void recover_gaming();
}  // namespace tablet_posture::backend

namespace tablet_posture::backend {
  namespace {
    constexpr auto dialog_results_key = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\GamingConfiguration\\SystemDialogResults";

    struct dialog_answer_t {
      const wchar_t *name;
      DWORD answer;
    };

    /// The confirmations Windows shows around a switch, each with the answer that switches at once.
    /// Which of them appear depends on the device form Windows believes it runs on.
    constexpr std::array<dialog_answer_t, 5> dialog_answers {{
      {L"EnterGamingPostureConfirmation_NoReboot", 1},
      // Handheld device form: "Enter at startup", "Start now", "Stay on desktop". 2 is "Start now" (build 26300).
      {L"EnterGamingPostureConfirmation_Config", 2},
      {L"PostEntryGamingPostureConfirmation", 1},
      {L"ExitGamingPostureConfirmation_Minimal", 1},
      {L"ExitGamingPostureConfirmation", 1},
    }};

    constexpr auto switch_timeout = 8s;
    constexpr auto switch_poll = 100ms;

    using state_fn_t = BOOL(WINAPI *)();
    using switch_fn_t = HRESULT(WINAPI *)(BOOL);

    struct gaming_api_t {
      state_fn_t supported = nullptr;
      state_fn_t active = nullptr;
      state_fn_t can_set = nullptr;
      switch_fn_t set = nullptr;
    };

    /// The gaming experience API set is missing on older Windows, so it is bound at run time.
    const gaming_api_t &gaming_api() {
      static const gaming_api_t api = [] {
        gaming_api_t loaded;
        if (const auto module = LoadLibraryW(L"api-ms-win-gaming-experience-l1-1-0.dll")) {
          loaded.supported = reinterpret_cast<state_fn_t>(reinterpret_cast<void *>(GetProcAddress(module, "IsGamingFullScreenExperienceSupported")));
          loaded.active = reinterpret_cast<state_fn_t>(reinterpret_cast<void *>(GetProcAddress(module, "IsGamingFullScreenExperienceActive")));
          loaded.can_set = reinterpret_cast<state_fn_t>(reinterpret_cast<void *>(GetProcAddress(module, "CanSetGamingFullScreenExperience")));
          loaded.set = reinterpret_cast<switch_fn_t>(reinterpret_cast<void *>(GetProcAddress(module, "SetGamingFullScreenExperience")));
        }
        return loaded;
      }();
      return api;
    }

    std::filesystem::path journal_path() {
      return platf::appdata() / "gaming_posture.journal";
    }

    std::string narrow(const wchar_t *text) {
      std::string out;
      for (; *text; ++text) {
        out.push_back(static_cast<char>(*text));
      }
      return out;
    }

    /// The logged-on user's hive, or this process' own when the host already runs as that user. Close with close_user_hive().
    HKEY open_user_hive() {
      HKEY hive = nullptr;
      if (const HANDLE token = platf::retrieve_users_token(false)) {
        (void) platf::impersonate_current_user(token, [&]() {
          if (RegOpenCurrentUser(KEY_READ | KEY_WRITE, &hive) != ERROR_SUCCESS) {
            hive = nullptr;
          }
        });
        CloseHandle(token);
      }
      return hive ? hive : HKEY_CURRENT_USER;
    }

    void close_user_hive(HKEY hive) {
      if (hive != HKEY_CURRENT_USER) {
        RegCloseKey(hive);
      }
    }

    /// Remove the confirmation answers this host supplied, as listed in the journal.
    void withdraw_answers() {
      std::error_code ec;
      if (!std::filesystem::exists(journal_path(), ec)) {
        return;
      }
      std::vector<std::string> supplied;
      {
        std::ifstream in(journal_path());
        for (std::string line; std::getline(in, line);) {
          if (!line.empty()) {
            supplied.push_back(line);
          }
        }
      }
      HKEY key = nullptr;
      bool clean = true;
      const HKEY hive = open_user_hive();
      if (RegOpenKeyExW(hive, dialog_results_key, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        for (const auto &dialog : dialog_answers) {
          if (std::find(supplied.begin(), supplied.end(), narrow(dialog.name)) == supplied.end()) {
            continue;
          }
          const auto status = RegDeleteValueW(key, dialog.name);
          clean = clean && (status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND);
        }
        RegCloseKey(key);
      }
      close_user_hive(hive);
      if (clean) {
        std::filesystem::remove(journal_path(), ec);
      } else {
        BOOST_LOG(warning) << "Gaming posture: could not remove the confirmation answers; they stay journaled for the next start"sv;
      }
    }

    /// Answer the switch confirmations in the user's hive where no answer exists yet, journaling each one first.
    void supply_answers() {
      HKEY key = nullptr;
      const HKEY hive = open_user_hive();
      const auto status = RegCreateKeyExW(hive, dialog_results_key, 0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr);
      close_user_hive(hive);
      if (status != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "Gaming posture: could not open the confirmation answers; Windows may ask before switching"sv;
        return;
      }
      for (const auto &dialog : dialog_answers) {
        if (RegQueryValueExW(key, dialog.name, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
          continue;  // Already answered, by the user or by an earlier switch of ours.
        }
        std::ofstream(journal_path(), std::ios::app) << narrow(dialog.name) << '\n';
        RegSetValueExW(key, dialog.name, 0, REG_DWORD, reinterpret_cast<const BYTE *>(&dialog.answer), sizeof(dialog.answer));
      }
      RegCloseKey(key);
    }

    bool wait_for_active(bool wanted) {
      const auto &api = gaming_api();
      const auto deadline = std::chrono::steady_clock::now() + switch_timeout;
      while (std::chrono::steady_clock::now() < deadline) {
        if ((api.active() != FALSE) == wanted) {
          return true;
        }
        std::this_thread::sleep_for(switch_poll);
      }
      return (api.active() != FALSE) == wanted;
    }

    /// Verdict of a switch, also the exit code of the switch instance.
    enum switch_result_e : int {
      switched = 0,  ///< The host is in the wanted state.
      unchanged = 1,  ///< Windows did not change state in time; a confirmation may be waiting on screen.
      bad_request = 2,
      unsupported = 3,
      no_home_app = 4,  ///< The user chose no home app, so Windows offers no switch.
    };

    /// Switch from this process, which must run as the logged-on user. The call into Windows does not
    /// return while a confirmation is open, so it runs on its own thread and the state wait bounds it.
    switch_result_e switch_in_process(bool enter) {
      const auto &api = gaming_api();
      if (!api.supported || !api.active || !api.set || api.supported() == FALSE) {
        return unsupported;
      }
      if ((api.active() != FALSE) == enter) {
        return switched;
      }
      if (!api.can_set || api.can_set() == FALSE) {
        return no_home_app;
      }
      std::thread([set = api.set, enter] {
        set(enter ? TRUE : FALSE);
      }).detach();
      return wait_for_active(enter) ? switched : unchanged;
    }

    /// Start this executable as the logged-on user to make the switch there, and wait for its verdict.
    switch_result_e switch_as_user(bool enter) {
      wchar_t executable[MAX_PATH] = {};
      const DWORD length = GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
      if (length == 0 || length >= ARRAYSIZE(executable)) {
        return unchanged;
      }
      std::string cmd = "\"" + platf::to_utf8(executable) + "\" ";
      cmd += tablet_posture::gaming_switch_arg;
      cmd += enter ? " enter" : " leave";

      platf::bp::environment env = platf::bp::this_process::env();
      auto working_dir = boost::filesystem::path();
      std::error_code ec;
      auto child = platf::run_command(false, false, cmd, working_dir, env, nullptr, ec, nullptr);
      if (ec) {
        BOOST_LOG(warning) << "Gaming posture: could not start the switch as the logged-on user: "sv << ec.message();
        return unchanged;
      }
      // The instance gives up after switch_timeout; allow for its start on top of that.
      const auto deadline = std::chrono::steady_clock::now() + switch_timeout + 4s;
      while (child.running(ec) && !ec) {
        if (std::chrono::steady_clock::now() >= deadline) {
          child.detach();
          return unchanged;
        }
        std::this_thread::sleep_for(switch_poll);
      }
      const int code = child.exit_code();
      return code >= switched && code <= no_home_app ? static_cast<switch_result_e>(code) : unchanged;
    }
  }  // namespace

  bool apply_gaming(bool enter) {
    const auto &api = gaming_api();
    if (!api.supported || !api.set || api.supported() == FALSE) {
      if (enter) {
        BOOST_LOG(warning) << "Gaming posture: this Windows does not offer the Xbox full screen experience"sv;
      }
      return !enter;
    }

    supply_answers();

    // Only a host that runs as the logged-on user is offered the switch itself.
    const bool own_switch = api.can_set && api.can_set() != FALSE;
    const auto result = own_switch ? switch_in_process(enter) : switch_as_user(enter);

    if (result != switched) {
      const auto action = enter ? "enter"sv : "leave"sv;
      if (result == no_home_app) {
        BOOST_LOG(warning) << "Gaming posture: Windows cannot "sv << action
                           << " the Xbox full screen experience: the logged-on user has no home app. "sv
                           << "Choose one under Settings > Gaming > Xbox mode (the Xbox app)."sv;
      } else if (result == unsupported) {
        BOOST_LOG(warning) << "Gaming posture: Windows does not offer the Xbox full screen experience to the logged-on user"sv;
      } else {
        BOOST_LOG(warning) << "Gaming posture: Windows did not "sv << action
                           << " the Xbox full screen experience. A confirmation may be waiting on the host screen."sv;
      }
    }
    if (!enter || result != switched) {
      // Nothing more to confirm once the host is back on the desktop, or when it never left it.
      withdraw_answers();
    }
    return result == switched;
  }

  void recover_gaming() {
    // A leave supplies its answers again, so nothing a previous host process left behind is needed.
    withdraw_answers();
  }
}  // namespace tablet_posture::backend

namespace tablet_posture {
  int run_gaming_switch(int argc, char **argv) {
    const std::string_view verb = argc > 2 ? argv[2] : "";
    if (verb != "enter"sv && verb != "leave"sv) {
      return backend::bad_request;
    }
    return backend::switch_in_process(verb == "enter"sv);
  }
}  // namespace tablet_posture
