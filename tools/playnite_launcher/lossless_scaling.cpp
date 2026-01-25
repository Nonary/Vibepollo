#include "tools/playnite_launcher/lossless_scaling.h"

#include "src/logging.h"
#include "src/platform/windows/ipc/misc_utils.h"
#include "src/platform/windows/misc.h"
#include "src/utility.h"
#include "tools/playnite_launcher/focus_utils.h"

#include <algorithm>
#include <array>
#include <boost/algorithm/string.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <locale>
#include <memory>
#include <optional>
#include <Psapi.h>
#include <shlobj.h>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <UserEnv.h>
#include <utility>
#include <vector>
#include <windows.h>
#include <winrt/base.h>

using namespace std::chrono_literals;

namespace playnite_launcher::lossless {
  lossless_scaling_runtime_state capture_lossless_scaling_state();
  namespace {

    constexpr std::string_view k_lossless_profile_title = "Vibeshine";
    constexpr size_t k_lossless_max_executables = 256;
    constexpr auto k_lossless_observation_duration = std::chrono::seconds(10);
    constexpr auto k_lossless_poll_interval = std::chrono::milliseconds(250);
    constexpr int k_sharpness_min = 1;
    constexpr int k_sharpness_max = 10;
    constexpr int k_flow_scale_min = 0;
    constexpr int k_flow_scale_max = 100;
    constexpr double k_resolution_factor_min = 1.0;
    constexpr double k_resolution_factor_max = 10.0;
    constexpr int k_max_frame_latency = 1;

    std::filesystem::path lossless_scaling_settings_path();

    template<typename Fn>
    auto run_with_user_context(Fn &&fn) -> decltype(fn()) {
      if (platf::dxgi::is_running_as_system()) {
        winrt::handle user_token {platf::dxgi::retrieve_users_token(false)};
        if (user_token) {
          if (!ImpersonateLoggedOnUser(user_token.get())) {
            BOOST_LOG(warning) << "Lossless Scaling: impersonation failed, error=" << GetLastError();
          } else {
            auto revert_guard = util::fail_guard([&]() {
              constexpr int kMaxRevertAttempts = 3;
              for (int attempt = 0; attempt < kMaxRevertAttempts; ++attempt) {
                if (RevertToSelf()) {
                  return;
                }
                DWORD err = GetLastError();
                BOOST_LOG(error) << "Lossless Scaling: RevertToSelf attempt " << (attempt + 1) << " failed, error=" << err;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
              }
              BOOST_LOG(fatal) << "Lossless Scaling: giving up after repeated RevertToSelf failures";
            });
            auto result = fn();
            return result;
          }
        } else {
          BOOST_LOG(debug) << "Lossless Scaling: no active user token, using service context";
        }
      }
      return fn();
    }

    std::filesystem::path known_folder_path_for_token(HANDLE token) {
      PWSTR local = nullptr;
      std::filesystem::path path;
      HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, token, &local);
      if (FAILED(hr) || !local) {
        if (local) {
          CoTaskMemFree(local);
        }
        return path;
      }
      path = std::filesystem::path(local);
      CoTaskMemFree(local);
      path /= L"Lossless Scaling";
      path /= L"settings.xml";
      return path;
    }

    bool parse_env_flag(const char *value) {
      if (!value) {
        return false;
      }
      std::string v(value);
      std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      return v == "1" || v == "true" || v == "yes";
    }

    std::optional<int> parse_env_int(const char *value) {
      if (!value || !*value) {
        return std::nullopt;
      }
      try {
        int v = std::stoi(value);
        if (v > 0) {
          return v;
        }
      } catch (...) {
      }
      return std::nullopt;
    }

    std::optional<int> parse_env_int_allow_zero(const char *value) {
      if (!value || !*value) {
        return std::nullopt;
      }
      try {
        return std::stoi(value);
      } catch (...) {
        return std::nullopt;
      }
    }

    std::optional<bool> parse_env_flag_optional(const char *value) {
      if (!value || !*value) {
        return std::nullopt;
      }
      return parse_env_flag(value);
    }

    std::optional<std::string> parse_env_string(const char *value) {
      if (!value || !*value) {
        return std::nullopt;
      }
      std::string converted(value);
      boost::algorithm::trim(converted);
      if (converted.empty()) {
        return std::nullopt;
      }
      return converted;
    }

    struct lossless_hotkey_t {
      WORD key = 0;
      std::vector<WORD> modifiers;
    };

    bool is_extended_key(WORD vk) {
      switch (vk) {
        case VK_LWIN:
        case VK_RWIN:
        case VK_RMENU:
        case VK_RCONTROL:
        case VK_INSERT:
        case VK_DELETE:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_UP:
        case VK_DOWN:
        case VK_LEFT:
        case VK_RIGHT:
        case VK_DIVIDE:
        case VK_APPS:
          return true;
        default:
          return false;
      }
    }

    std::optional<WORD> parse_hotkey_key(const std::string &value) {
      std::string text = boost::algorithm::trim_copy(value);
      if (text.empty()) {
        return std::nullopt;
      }
      boost::algorithm::to_upper(text);
      if (text.size() == 1) {
        char c = text.front();
        if (c >= 'A' && c <= 'Z') {
          return static_cast<WORD>(c);
        }
        if (c >= '0' && c <= '9') {
          return static_cast<WORD>(c);
        }
      }
      if (text.size() > 1 && text.front() == 'F') {
        try {
          int fkey = std::stoi(text.substr(1));
          if (fkey >= 1 && fkey <= 24) {
            return static_cast<WORD>(VK_F1 + (fkey - 1));
          }
        } catch (...) {
        }
      }
      struct NamedKey {
        const char *name;
        WORD vk;
      };
      static const std::array<NamedKey, 20> k_named_keys {{
        {"SPACE", VK_SPACE},
        {"TAB", VK_TAB},
        {"ESC", VK_ESCAPE},
        {"ESCAPE", VK_ESCAPE},
        {"ENTER", VK_RETURN},
        {"RETURN", VK_RETURN},
        {"BACK", VK_BACK},
        {"BACKSPACE", VK_BACK},
        {"INSERT", VK_INSERT},
        {"DELETE", VK_DELETE},
        {"HOME", VK_HOME},
        {"END", VK_END},
        {"PAGEUP", VK_PRIOR},
        {"PGUP", VK_PRIOR},
        {"PAGEDOWN", VK_NEXT},
        {"PGDN", VK_NEXT},
        {"UP", VK_UP},
        {"DOWN", VK_DOWN},
        {"LEFT", VK_LEFT},
        {"RIGHT", VK_RIGHT},
      }};
      for (const auto &entry : k_named_keys) {
        if (text == entry.name) {
          return entry.vk;
        }
      }
      if (text.rfind("NUMPAD", 0) == 0 && text.size() == 7 && std::isdigit(text[6])) {
        int num = text[6] - '0';
        return static_cast<WORD>(VK_NUMPAD0 + num);
      }
      return std::nullopt;
    }

    std::vector<WORD> parse_hotkey_modifiers(const std::string &value) {
      std::unordered_set<WORD> mods;
      std::string token;
      for (char ch : value) {
        if (std::isspace(static_cast<unsigned char>(ch)) || ch == '+' || ch == ',' || ch == ';' || ch == '|') {
          if (!token.empty()) {
            std::string lower = boost::algorithm::to_lower_copy(token);
            if (lower == "alt" || lower == "menu") {
              mods.insert(VK_MENU);
            } else if (lower == "control" || lower == "ctrl") {
              mods.insert(VK_CONTROL);
            } else if (lower == "shift") {
              mods.insert(VK_SHIFT);
            } else if (lower == "win" || lower == "windows" || lower == "logo") {
              mods.insert(VK_LWIN);
            }
            token.clear();
          }
          continue;
        }
        token.push_back(ch);
      }
      if (!token.empty()) {
        std::string lower = boost::algorithm::to_lower_copy(token);
        if (lower == "alt" || lower == "menu") {
          mods.insert(VK_MENU);
        } else if (lower == "control" || lower == "ctrl") {
          mods.insert(VK_CONTROL);
        } else if (lower == "shift") {
          mods.insert(VK_SHIFT);
        } else if (lower == "win" || lower == "windows" || lower == "logo") {
          mods.insert(VK_LWIN);
        }
      }
      std::vector<WORD> ordered;
      ordered.reserve(mods.size());
      const std::array<WORD, 4> order {VK_CONTROL, VK_MENU, VK_SHIFT, VK_LWIN};
      for (auto vk : order) {
        if (mods.find(vk) != mods.end()) {
          ordered.push_back(vk);
        }
      }
      return ordered;
    }

    bool send_hotkey_input(const lossless_hotkey_t &hotkey) {
      if (!hotkey.key) {
        return false;
      }
      std::vector<INPUT> inputs;
      inputs.reserve((hotkey.modifiers.size() * 2) + 2);
      auto append_key = [&](WORD vk, bool up) {
        INPUT input {};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = vk;
        input.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
        if (is_extended_key(vk)) {
          input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        }
        inputs.push_back(input);
      };
      for (auto vk : hotkey.modifiers) {
        append_key(vk, false);
      }
      append_key(hotkey.key, false);
      append_key(hotkey.key, true);
      for (auto it = hotkey.modifiers.rbegin(); it != hotkey.modifiers.rend(); ++it) {
        append_key(*it, true);
      }
      if (inputs.empty()) {
        return false;
      }
      UINT sent = SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
      if (sent != inputs.size()) {
        BOOST_LOG(warning) << "Lossless Scaling: SendInput sent " << sent << " of " << inputs.size();
        return false;
      }
      return true;
    }

    HWND focus_game_window(DWORD pid);
    bool click_window_center(HWND hwnd);

    bool apply_hotkey_for_pid(const lossless_hotkey_t &hotkey, DWORD pid, bool click_before_send, int attempts) {
      if (attempts <= 0) {
        return false;
      }
      for (int attempt = 0; attempt < attempts; ++attempt) {
        HWND hwnd = nullptr;
        if (pid) {
          hwnd = focus_game_window(pid);
          if (hwnd) {
            std::this_thread::sleep_for(75ms);
            if (click_before_send) {
              click_window_center(hwnd);
            }
            std::this_thread::sleep_for(50ms);
          }
        }
        bool sent = send_hotkey_input(hotkey);
        if (pid) {
          bool focused = focus::confirm_foreground_pid(pid);
          BOOST_LOG(info) << "Lossless Scaling: hotkey attempt " << (attempt + 1) << "/" << attempts
                          << " pid=" << pid << " focused=" << (focused ? "true" : "false")
                          << " sent=" << (sent ? "true" : "false");
        } else {
          BOOST_LOG(info) << "Lossless Scaling: hotkey attempt " << (attempt + 1) << "/" << attempts
                          << " pid=none sent=" << (sent ? "true" : "false");
        }
        if (sent) {
          return true;
        }
        std::this_thread::sleep_for(150ms);
      }
      return false;
    }

    std::optional<lossless_hotkey_t> read_lossless_hotkey() {
      auto worker = [&]() -> std::optional<lossless_hotkey_t> {
        auto settings_path = lossless_scaling_settings_path();
        if (settings_path.empty()) {
          return std::nullopt;
        }
        boost::property_tree::ptree tree;
        try {
          boost::property_tree::read_xml(settings_path.string(), tree);
        } catch (...) {
          return std::nullopt;
        }
        auto hotkey_text = tree.get_optional<std::string>("Settings.Hotkey");
        if (!hotkey_text || hotkey_text->empty()) {
          return std::nullopt;
        }
        auto key = parse_hotkey_key(*hotkey_text);
        if (!key) {
          BOOST_LOG(warning) << "Lossless Scaling: unrecognized hotkey '" << *hotkey_text << "'";
          return std::nullopt;
        }
        lossless_hotkey_t hotkey {};
        hotkey.key = *key;
        if (auto mods = tree.get_optional<std::string>("Settings.HotkeyModifierKeys")) {
          hotkey.modifiers = parse_hotkey_modifiers(*mods);
        }
        return hotkey;
      };
      return run_with_user_context(worker);
    }

    std::optional<int> clamp_optional_int(std::optional<int> value, int min_value, int max_value) {
      if (!value) {
        return std::nullopt;
      }
      return std::clamp(*value, min_value, max_value);
    }

    std::optional<double> parse_env_double(const char *value) {
      if (!value || !*value) {
        return std::nullopt;
      }
      try {
        double v = std::stod(value);
        if (std::isfinite(v) && v > 0.0) {
          return v;
        }
      } catch (...) {
      }
      return std::nullopt;
    }

    std::optional<double> clamp_optional_double(std::optional<double> value, double min_value, double max_value) {
      if (!value) {
        return std::nullopt;
      }
      return std::clamp(*value, min_value, max_value);
    }

    void finalize_lossless_options(lossless_scaling_options &options) {
      if (options.enabled && !options.rtss_limit && options.target_fps && *options.target_fps > 0) {
        int computed = *options.target_fps / 2;
        if (computed > 0) {
          options.rtss_limit = computed;
        }
      }
      if (options.anime4k_type) {
        boost::algorithm::to_upper(*options.anime4k_type);
      }
    }

    void lowercase_inplace(std::wstring &value) {
      std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
        return std::towlower(c);
      });
    }

    std::optional<std::filesystem::path> utf8_to_path(const std::string &input) {
      if (input.empty()) {
        return std::nullopt;
      }
      try {
        std::wstring wide = platf::dxgi::utf8_to_wide(input);
        if (wide.empty()) {
          return std::nullopt;
        }
        return std::filesystem::path(wide);
      } catch (...) {
        return std::nullopt;
      }
    }

    std::optional<std::filesystem::path> normalize_directory(std::optional<std::filesystem::path> path) {
      if (!path || path->empty()) {
        return std::nullopt;
      }
      std::error_code ec;
      auto canonical = std::filesystem::weakly_canonical(*path, ec);
      if (!ec && !canonical.empty()) {
        path = canonical;
      }
      if (!std::filesystem::is_directory(*path, ec)) {
        return std::nullopt;
      }
      return path;
    }

    std::optional<std::filesystem::path> parent_directory_from_utf8(const std::string &exe_utf8) {
      auto exe_path = utf8_to_path(exe_utf8);
      if (!exe_path) {
        return std::nullopt;
      }
      exe_path = exe_path->parent_path();
      return normalize_directory(exe_path);
    }

    std::optional<std::filesystem::path> lossless_resolve_base_dir(const std::string &install_dir_utf8, const std::string &exe_path_utf8) {
      auto install_dir = normalize_directory(utf8_to_path(install_dir_utf8));
      if (install_dir) {
        return install_dir;
      }
      return parent_directory_from_utf8(exe_path_utf8);
    }

    bool lossless_path_within(const std::filesystem::path &candidate, const std::filesystem::path &base) {
      if (candidate.empty() || base.empty()) {
        return false;
      }
      std::error_code ec;
      auto rel = std::filesystem::relative(candidate, base, ec);
      if (ec) {
        return false;
      }
      for (const auto &part : rel) {
        if (part == L"..") {
          return false;
        }
      }
      return true;
    }

    void add_executable(const std::filesystem::path &candidate, bool require_exists, std::unordered_set<std::wstring> &seen, std::vector<std::wstring> &executables) {
      if (executables.size() >= k_lossless_max_executables) {
        return;
      }
      if (require_exists) {
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec) || !std::filesystem::is_regular_file(candidate, ec)) {
          return;
        }
      }
      auto ext = candidate.extension().wstring();
      lowercase_inplace(ext);
      if (ext != L".exe") {
        return;
      }
      auto name = candidate.filename().wstring();
      if (name.empty()) {
        return;
      }
      auto key = name;
      lowercase_inplace(key);
      if (seen.insert(key).second) {
        executables.push_back(name);
      }
    }

    void scan_directory_for_executables(const std::filesystem::path &base, std::unordered_set<std::wstring> &seen, std::vector<std::wstring> &executables) {
      if (base.empty()) {
        return;
      }
      std::error_code ec;
      auto options = std::filesystem::directory_options::skip_permission_denied;
      std::filesystem::recursive_directory_iterator it(base, options, ec);
      std::filesystem::recursive_directory_iterator end;
      for (; it != end && executables.size() < k_lossless_max_executables; it.increment(ec)) {
        if (ec) {
          ec.clear();
          continue;
        }
        if (it->is_regular_file(ec)) {
          add_executable(it->path(), true, seen, executables);
        }
      }
    }

    void add_explicit_executable(const std::optional<std::filesystem::path> &explicit_exe, const std::filesystem::path &base_dir, std::unordered_set<std::wstring> &seen, std::vector<std::wstring> &executables) {
      if (!explicit_exe) {
        return;
      }
      if (!base_dir.empty() && !lossless_path_within(*explicit_exe, base_dir)) {
        return;
      }
      add_executable(*explicit_exe, true, seen, executables);
    }

    void sort_executable_names(std::vector<std::wstring> &executables) {
      std::sort(executables.begin(), executables.end(), [](std::wstring a, std::wstring b) {
        lowercase_inplace(a);
        lowercase_inplace(b);
        return a < b;
      });
    }

    std::vector<std::wstring> lossless_collect_executable_names(const std::filesystem::path &base_dir, const std::optional<std::filesystem::path> &explicit_exe) {
      std::vector<std::wstring> executables;
      std::unordered_set<std::wstring> seen;
      scan_directory_for_executables(base_dir, seen, executables);
      add_explicit_executable(explicit_exe, base_dir, seen, executables);
      sort_executable_names(executables);
      return executables;
    }

    std::wstring join_executable_filter(const std::vector<std::wstring> &exe_names) {
      std::wstring filter;
      for (const auto &name : exe_names) {
        std::wstring lowered = name;
        lowercase_inplace(lowered);
        if (lowered.empty()) {
          continue;
        }
        if (!filter.empty()) {
          filter.push_back(L';');
        }
        filter.append(lowered);
      }
      return filter;
    }

    std::string lossless_build_filter(const std::vector<std::wstring> &exe_names) {
      if (exe_names.empty()) {
        return std::string();
      }
      auto filter = join_executable_filter(exe_names);
      if (filter.empty()) {
        return std::string();
      }
      try {
        return platf::dxgi::wide_to_utf8(filter);
      } catch (...) {
        return std::string();
      }
    }

    std::optional<std::filesystem::path> get_lossless_scaling_env_path() {
      const char *env = std::getenv("SUNSHINE_LOSSLESS_SCALING_EXE");
      if (!env || !*env) {
        return std::nullopt;
      }
      try {
        std::wstring wide = platf::dxgi::utf8_to_wide(env);
        if (wide.empty()) {
          return std::nullopt;
        }
        return std::filesystem::path(wide);
      } catch (...) {
        return std::nullopt;
      }
    }

    std::optional<std::wstring> exe_from_env_path() {
      auto path = get_lossless_scaling_env_path();
      if (path && std::filesystem::exists(*path)) {
        return path->wstring();
      }
      return std::nullopt;
    }

    std::optional<std::wstring> exe_from_runtime(const lossless_scaling_runtime_state &state) {
      if (state.exe_path && std::filesystem::exists(*state.exe_path)) {
        return state.exe_path;
      }
      return std::nullopt;
    }

    std::filesystem::path lossless_scaling_settings_path() {
      if (platf::dxgi::is_running_as_system()) {
        winrt::handle token {platf::dxgi::retrieve_users_token(false)};
        if (token) {
          auto user_path = known_folder_path_for_token(token.get());
          if (!user_path.empty()) {
            return user_path;
          }
          BOOST_LOG(debug) << "Lossless Scaling: failed to resolve LocalAppData via user token, falling back";
        }
      }
      return known_folder_path_for_token(nullptr);
    }

    std::optional<std::wstring> exe_from_settings() {
      auto settings = lossless_scaling_settings_path();
      if (settings.empty()) {
        return std::nullopt;
      }
      auto local_app = settings.parent_path().parent_path();
      if (local_app.empty()) {
        return std::nullopt;
      }
      std::filesystem::path candidate = local_app / L"Programs" / L"Lossless Scaling" / L"Lossless Scaling.exe";
      if (std::filesystem::exists(candidate)) {
        return candidate.wstring();
      }
      return std::nullopt;
    }

    std::optional<std::wstring> exe_from_program_files() {
      const std::array<const wchar_t *, 2> env_names {L"PROGRAMFILES", L"PROGRAMFILES(X86)"};
      for (auto env_name : env_names) {
        wchar_t buf[MAX_PATH] = {};
        DWORD len = GetEnvironmentVariableW(env_name, buf, ARRAYSIZE(buf));
        if (len == 0 || len >= ARRAYSIZE(buf)) {
          continue;
        }
        std::filesystem::path candidate = std::filesystem::path(buf) / L"Lossless Scaling" / L"Lossless Scaling.exe";
        if (std::filesystem::exists(candidate)) {
          return candidate.wstring();
        }
      }
      return std::nullopt;
    }

    void strip_xml_whitespace(boost::property_tree::ptree &node) {
      for (auto it = node.begin(); it != node.end();) {
        if (it->first == "<xmltext>") {
          it = node.erase(it);
        } else {
          strip_xml_whitespace(it->second);
          ++it;
        }
      }
    }

    std::optional<std::wstring> discover_lossless_scaling_exe(const lossless_scaling_runtime_state &state) {
      if (auto path = exe_from_env_path()) {
        return path;
      }
      if (auto path = exe_from_runtime(state)) {
        return path;
      }
      if (auto path = exe_from_settings()) {
        return path;
      }
      return exe_from_program_files();
    }

    bool is_any_executable_running(const std::vector<std::wstring> &exe_names) {
      if (exe_names.empty()) {
        return false;
      }
      for (const auto &exe_name : exe_names) {
        if (exe_name.empty()) {
          continue;
        }
        try {
          auto ids = platf::dxgi::find_process_ids_by_name(exe_name);
          if (!ids.empty()) {
            return true;
          }
        } catch (...) {
        }
      }
      return false;
    }

    bool wait_for_any_executable(const std::vector<std::wstring> &exe_names, int timeout_seconds) {
      if (exe_names.empty() || timeout_seconds <= 0) {
        return false;
      }
      auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
      while (std::chrono::steady_clock::now() < deadline) {
        if (is_any_executable_running(exe_names)) {
          BOOST_LOG(debug) << "Lossless Scaling: game executable detected in process list";
          return true;
        }
        std::this_thread::sleep_for(250ms);
      }
      BOOST_LOG(debug) << "Lossless Scaling: timeout waiting for game executable to appear";
      return false;
    }

    bool focus_main_lossless_window(DWORD pid) {
      HWND hwnd = focus::find_main_window_for_pid(pid);
      return hwnd && focus::try_focus_hwnd(hwnd);
    }

    bool focus_any_visible_window(DWORD pid) {
      struct Ctx {
        DWORD target;
        bool focused = false;
      } ctx {pid, false};

      auto proc = [](HWND hwnd, LPARAM param) -> BOOL {
        auto *ctx = reinterpret_cast<Ctx *>(param);
        DWORD owner = 0;
        GetWindowThreadProcessId(hwnd, &owner);
        if (owner == ctx->target && IsWindowVisible(hwnd)) {
          ctx->focused = focus::try_focus_hwnd(hwnd);
          if (ctx->focused) {
            return FALSE;
          }
        }
        return TRUE;
      };
      EnumWindows(proc, reinterpret_cast<LPARAM>(&ctx));
      return ctx.focused;
    }

    bool lossless_scaling_focus_window(DWORD pid) {
      if (!pid) {
        return false;
      }
      return focus_main_lossless_window(pid) || focus_any_visible_window(pid);
    }

    bool minimize_hwnd_if_visible(HWND hwnd) {
      if (!hwnd || !IsWindowVisible(hwnd)) {
        return false;
      }
      if (IsIconic(hwnd)) {
        return true;
      }
      return ShowWindowAsync(hwnd, SW_SHOWMINNOACTIVE) != 0;
    }

    bool minimize_main_lossless_window(DWORD pid) {
      return minimize_hwnd_if_visible(focus::find_main_window_for_pid(pid));
    }

    bool minimize_any_visible_window(DWORD pid) {
      struct Ctx {
        DWORD target;
        bool minimized = false;
      } ctx {pid, false};

      auto proc = [](HWND hwnd, LPARAM param) -> BOOL {
        auto *ctx = reinterpret_cast<Ctx *>(param);
        DWORD owner = 0;
        GetWindowThreadProcessId(hwnd, &owner);
        if (owner == ctx->target) {
          if (minimize_hwnd_if_visible(hwnd)) {
            ctx->minimized = true;
            return FALSE;
          }
        }
        return TRUE;
      };
      EnumWindows(proc, reinterpret_cast<LPARAM>(&ctx));
      return ctx.minimized;
    }

    bool lossless_scaling_minimize_window(DWORD pid) {
      if (!pid) {
        return false;
      }
      return minimize_main_lossless_window(pid) || minimize_any_visible_window(pid);
    }

    bool minimize_visible_windows_except(DWORD keep_pid) {
      struct Ctx {
        DWORD keep;
        bool minimized = false;
      } ctx {keep_pid, false};
      EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
        auto *ctx = reinterpret_cast<Ctx *>(param);
        if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) {
          return TRUE;
        }
        if (GetWindow(hwnd, GW_OWNER) != nullptr) {
          return TRUE;
        }
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == 0 || pid == ctx->keep) {
          return TRUE;
        }
        RECT rect {};
        if (!GetWindowRect(hwnd, &rect)) {
          return TRUE;
        }
        if ((rect.right - rect.left) <= 1 || (rect.bottom - rect.top) <= 1) {
          return TRUE;
        }
        if (ShowWindowAsync(hwnd, SW_SHOWMINNOACTIVE)) {
          ctx->minimized = true;
        }
        return TRUE;
      },
                  reinterpret_cast<LPARAM>(&ctx));
      return ctx.minimized;
    }

    HWND focus_any_visible_window_for_pid(DWORD pid) {
      struct Ctx {
        DWORD target;
        HWND focused = nullptr;
      } ctx {pid, nullptr};
      EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
        auto *ctx = reinterpret_cast<Ctx *>(param);
        DWORD owner = 0;
        GetWindowThreadProcessId(hwnd, &owner);
        if (owner == ctx->target && IsWindowVisible(hwnd)) {
          if (focus::try_focus_hwnd(hwnd)) {
            ctx->focused = hwnd;
            return FALSE;
          }
        }
        return TRUE;
      },
                  reinterpret_cast<LPARAM>(&ctx));
      return ctx.focused;
    }

    HWND focus_game_window(DWORD pid) {
      if (!pid) {
        return nullptr;
      }
      HWND hwnd = focus::find_main_window_for_pid(pid);
      if (hwnd && focus::try_focus_hwnd(hwnd)) {
        return hwnd;
      }
      return focus_any_visible_window_for_pid(pid);
    }

    bool click_window_center(HWND hwnd) {
      if (!hwnd) {
        return false;
      }
      RECT rect {};
      if (!GetWindowRect(hwnd, &rect)) {
        return false;
      }
      if ((rect.right - rect.left) <= 1 || (rect.bottom - rect.top) <= 1) {
        return false;
      }
      POINT original {};
      GetCursorPos(&original);
      int x = rect.left + (rect.right - rect.left) / 2;
      int y = rect.top + (rect.bottom - rect.top) / 2;
      SetCursorPos(x, y);
      INPUT inputs[2] {};
      inputs[0].type = INPUT_MOUSE;
      inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
      inputs[1].type = INPUT_MOUSE;
      inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
      UINT sent = SendInput(2, inputs, sizeof(INPUT));
      SetCursorPos(original.x, original.y);
      return sent == 2;
    }

    std::wstring normalize_lowercase_path(std::wstring value) {
      for (auto &ch : value) {
        if (ch == L'/') {
          ch = L'\\';
        }
        ch = static_cast<wchar_t>(std::towlower(ch));
      }
      return value;
    }

    std::optional<std::wstring> query_process_image_path_optional(DWORD pid) {
      std::wstring path;
      if (focus::get_process_image_path(pid, path)) {
        return path;
      }
      return std::nullopt;
    }

    std::optional<std::wstring> normalize_utf8_path(const std::string &path_utf8) {
      if (path_utf8.empty()) {
        return std::nullopt;
      }
      try {
        auto wide = platf::dxgi::utf8_to_wide(path_utf8);
        if (wide.empty()) {
          return std::nullopt;
        }
        return normalize_lowercase_path(wide);
      } catch (...) {
        return std::nullopt;
      }
    }

    bool path_matches_prefix(const std::wstring &path, const std::wstring &dir_prefix) {
      if (dir_prefix.empty()) {
        return false;
      }
      if (path.size() < dir_prefix.size()) {
        return false;
      }
      if (path.compare(0, dir_prefix.size(), dir_prefix) != 0) {
        return false;
      }
      if (path.size() == dir_prefix.size()) {
        return true;
      }
      return path[dir_prefix.size()] == L'\\';
    }

    bool path_matches_filter(const std::wstring &path, const std::optional<std::wstring> &install_dir_norm, const std::optional<std::wstring> &exe_path_norm) {
      if (path.empty()) {
        return false;
      }
      auto normalized = normalize_lowercase_path(path);
      if (exe_path_norm && !exe_path_norm->empty() && normalized == *exe_path_norm) {
        return true;
      }
      if (install_dir_norm && !install_dir_norm->empty()) {
        return path_matches_prefix(normalized, *install_dir_norm);
      }
      return false;
    }

    bool is_ignored_process_path(const std::wstring &path) {
      if (path.empty()) {
        return false;
      }
      std::filesystem::path fs_path(path);
      auto filename = fs_path.filename().wstring();
      if (filename.empty()) {
        return false;
      }
      auto lower = normalize_lowercase_path(filename);
      return lower == L"losslessscaling.exe" ||
             lower == L"lossless scaling.exe" ||
             lower == L"playnite.fullscreenapp.exe" ||
             lower == L"playnite.desktopapp.exe";
    }

    HWND wait_for_game_window(DWORD pid, std::chrono::milliseconds timeout) {
      if (!pid) {
        return nullptr;
      }
      auto deadline = std::chrono::steady_clock::now() + timeout;
      while (std::chrono::steady_clock::now() < deadline) {
        if (auto hwnd = focus::find_main_window_for_pid(pid)) {
          return hwnd;
        }
        std::this_thread::sleep_for(100ms);
      }
      return nullptr;
    }

    bool wait_for_lossless_ready(std::chrono::milliseconds timeout) {
      auto deadline = std::chrono::steady_clock::now() + timeout;
      while (std::chrono::steady_clock::now() < deadline) {
        auto runtime = capture_lossless_scaling_state();
        if (!runtime.running_pids.empty()) {
          return true;
        }
        std::this_thread::sleep_for(150ms);
      }
      return false;
    }

    std::vector<DWORD> enumerate_process_ids_snapshot() {
      DWORD needed = 0;
      std::vector<DWORD> pids(1024);
      while (true) {
        if (!EnumProcesses(pids.data(), static_cast<DWORD>(pids.size() * sizeof(DWORD)), &needed)) {
          return {};
        }
        if (needed < pids.size() * sizeof(DWORD)) {
          pids.resize(needed / sizeof(DWORD));
          break;
        }
        pids.resize(pids.size() * 2);
      }
      return pids;
    }

    bool sample_process_usage(DWORD pid, ULONGLONG &cpu_time, SIZE_T &working_set) {
      HANDLE handle = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
      if (!handle) {
        handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
      }
      if (!handle) {
        return false;
      }
      FILETIME creation {}, exit_time {}, kernel {}, user {};
      BOOL got_times = GetProcessTimes(handle, &creation, &exit_time, &kernel, &user);
      PROCESS_MEMORY_COUNTERS_EX pmc {};
      BOOL got_mem = GetProcessMemoryInfo(handle, reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc), sizeof(pmc));
      CloseHandle(handle);
      if (!got_times) {
        return false;
      }
      ULARGE_INTEGER kernel_int {};
      kernel_int.HighPart = kernel.dwHighDateTime;
      kernel_int.LowPart = kernel.dwLowDateTime;
      ULARGE_INTEGER user_int {};
      user_int.HighPart = user.dwHighDateTime;
      user_int.LowPart = user.dwLowDateTime;
      cpu_time = kernel_int.QuadPart + user_int.QuadPart;
      working_set = got_mem ? pmc.WorkingSetSize : 0;
      return true;
    }

    void lossless_scaling_post_wm_close(const std::vector<DWORD> &pids) {
      if (pids.empty()) {
        return;
      }

      struct EnumCtx {
        const std::vector<DWORD> *pids;
      } ctx {&pids};

      EnumWindows([](HWND hwnd, LPARAM lparam) -> BOOL {
        auto *ctx = reinterpret_cast<EnumCtx *>(lparam);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (!pid) {
          return TRUE;
        }
        if (std::find(ctx->pids->begin(), ctx->pids->end(), pid) != ctx->pids->end()) {
          PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return TRUE;
      },
                  reinterpret_cast<LPARAM>(&ctx));
    }

    std::optional<std::wstring> process_path_from_pid(DWORD pid) {
      HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
      if (!process) {
        return std::nullopt;
      }
      std::wstring buffer;
      buffer.resize(32768);
      DWORD size = static_cast<DWORD>(buffer.size());
      std::optional<std::wstring> result;
      if (QueryFullProcessImageNameW(process, 0, buffer.data(), &size) && size > 0) {
        buffer.resize(size);
        result = buffer;
      }
      CloseHandle(process);
      return result;
    }

    void collect_runtime_for_process(const wchar_t *process_name, lossless_scaling_runtime_state &state) {
      if (!process_name || !*process_name) {
        return;
      }
      try {
        auto ids = platf::dxgi::find_process_ids_by_name(process_name);
        for (DWORD pid : ids) {
          if (std::find(state.running_pids.begin(), state.running_pids.end(), pid) != state.running_pids.end()) {
            continue;
          }
          state.running_pids.push_back(pid);
          if (!state.exe_path) {
            if (auto path = process_path_from_pid(pid)) {
              state.exe_path = std::move(*path);
            }
          }
        }
      } catch (...) {
      }
    }

    struct ProfileTemplates {
      boost::property_tree::ptree *defaults = nullptr;
      boost::property_tree::ptree *first = nullptr;
    };

    ProfileTemplates find_profile_templates(boost::property_tree::ptree &profiles) {
      ProfileTemplates templates;
      for (auto &entry : profiles) {
        if (entry.first != "Profile") {
          continue;
        }
        if (!templates.first) {
          templates.first = &entry.second;
        }
        auto path_opt = entry.second.get_optional<std::string>("Path");
        if (!path_opt || path_opt->empty()) {
          templates.defaults = &entry.second;
          break;
        }
      }
      return templates;
    }

    void capture_backup_fields(const ProfileTemplates &templates, lossless_scaling_profile_backup &backup) {
      auto source = templates.defaults ? templates.defaults : templates.first;
      if (!source) {
        return;
      }
      if (auto auto_scale = source->get_optional<std::string>("AutoScale")) {
        backup.had_auto_scale = true;
        backup.auto_scale = *auto_scale;
      }
      if (auto delay = source->get_optional<int>("AutoScaleDelay")) {
        backup.had_auto_scale_delay = true;
        backup.auto_scale_delay = *delay;
      }
      if (auto target = source->get_optional<int>("LSFG3Target")) {
        backup.had_lsfg_target = true;
        backup.lsfg_target = *target;
      }
      if (auto capture = source->get_optional<std::string>("CaptureApi")) {
        backup.had_capture_api = true;
        backup.capture_api = *capture;
      }
      if (auto queue = source->get_optional<int>("QueueTarget")) {
        backup.had_queue_target = true;
        backup.queue_target = *queue;
      }
      if (auto hdr = source->get_optional<bool>("HdrSupport")) {
        backup.had_hdr_support = true;
        backup.hdr_support = *hdr;
      }
      if (auto flow = source->get_optional<int>("LSFGFlowScale")) {
        backup.had_flow_scale = true;
        backup.flow_scale = *flow;
      }
      if (auto size = source->get_optional<std::string>("LSFGSize")) {
        backup.had_lsfg_size = true;
        backup.lsfg_size = *size;
      }
      if (auto mode = source->get_optional<std::string>("LSFG3Mode1")) {
        backup.had_lsfg3_mode = true;
        backup.lsfg3_mode = *mode;
      }
      if (auto frame = source->get_optional<std::string>("FrameGeneration")) {
        backup.had_frame_generation = true;
        backup.frame_generation = *frame;
      }
      if (auto scaling_type = source->get_optional<std::string>("ScalingType")) {
        backup.had_scaling_type = true;
        backup.scaling_type = *scaling_type;
      }
      if (auto ls1_type = source->get_optional<std::string>("LS1Type")) {
        backup.had_ls1_type = true;
        backup.ls1_type = *ls1_type;
      }
      if (auto scaling_mode = source->get_optional<std::string>("ScalingMode")) {
        backup.had_scaling_mode = true;
        backup.scaling_mode = *scaling_mode;
      }
      if (auto resize_before = source->get_optional<bool>("ResizeBeforeScaling")) {
        backup.had_resize_before_scaling = true;
        backup.resize_before_scaling = *resize_before;
      }
      if (auto scaling_fit_mode = source->get_optional<std::string>("ScalingFitMode")) {
        backup.had_scaling_fit_mode = true;
        backup.scaling_fit_mode = *scaling_fit_mode;
      }
      if (auto scale_factor = source->get_optional<double>("ScaleFactor")) {
        backup.had_scale_factor = true;
        backup.scale_factor = *scale_factor;
      }
      if (auto sharpness = source->get_optional<int>("Sharpness")) {
        backup.had_sharpness = true;
        backup.sharpness = *sharpness;
      }
      if (auto ls1 = source->get_optional<int>("LS1Sharpness")) {
        backup.had_ls1_sharpness = true;
        backup.ls1_sharpness = *ls1;
      }
      if (auto anime_type = source->get_optional<std::string>("Anime4kType")) {
        backup.had_anime4k_type = true;
        backup.anime4k_type = *anime_type;
      }
      if (auto vrs = source->get_optional<bool>("VRS")) {
        backup.had_vrs = true;
        backup.vrs = *vrs;
      }
      if (auto sync = source->get_optional<std::string>("SyncMode")) {
        backup.had_sync_mode = true;
        backup.sync_mode = *sync;
      }
      if (auto latency = source->get_optional<int>("MaxFrameLatency")) {
        backup.had_max_frame_latency = true;
        backup.max_frame_latency = *latency;
      }
    }

    std::optional<std::filesystem::path> resolve_explicit_executable(const std::string &exe_path_utf8) {
      if (exe_path_utf8.empty()) {
        return std::nullopt;
      }
      auto exe = utf8_to_path(exe_path_utf8);
      if (!exe) {
        return std::nullopt;
      }
      std::error_code ec;
      auto canonical = std::filesystem::weakly_canonical(*exe, ec);
      if (!ec && !canonical.empty()) {
        exe = canonical;
      }
      if (!std::filesystem::exists(*exe, ec) || !std::filesystem::is_regular_file(*exe, ec)) {
        return std::nullopt;
      }
      return exe;
    }

    std::string build_executable_filter(const std::optional<std::filesystem::path> &base_dir, const std::optional<std::filesystem::path> &explicit_exe) {
      std::vector<std::wstring> names;
      if (base_dir || explicit_exe) {
        names = lossless_collect_executable_names(base_dir.value_or(std::filesystem::path()), explicit_exe);
      }
      return lossless_build_filter(names);
    }

    boost::property_tree::ptree clone_template_profile(const ProfileTemplates &templates) {
      if (templates.defaults) {
        return *templates.defaults;
      }
      if (templates.first) {
        return *templates.first;
      }
      return {};
    }

    bool remove_vibeshine_profiles(boost::property_tree::ptree &profiles) {
      bool removed = false;
      for (auto it = profiles.begin(); it != profiles.end();) {
        if (it->first == "Profile" && it->second.get<std::string>("Title", "") == k_lossless_profile_title) {
          it = profiles.erase(it);
          removed = true;
          continue;
        }
        ++it;
      }
      return removed;
    }

    boost::property_tree::ptree make_vibeshine_profile(const ProfileTemplates &templates, const lossless_scaling_options &options, const std::string &filter_utf8) {
      auto profile = clone_template_profile(templates);
      profile.put("Title", std::string(k_lossless_profile_title));
      profile.put("Path", filter_utf8);
      profile.put("Filter", filter_utf8);
      profile.put("AutoScale", options.legacy_auto_detect ? "true" : "false");
      profile.put("AutoScaleDelay", 0);
      profile.put("SyncMode", "OFF");
      if (options.capture_api) {
        std::string capture = *options.capture_api;
        boost::algorithm::to_upper(capture);
        profile.put("CaptureApi", capture);
      }
      if (options.queue_target) {
        profile.put("QueueTarget", std::max(0, *options.queue_target));
      }
      if (options.hdr_enabled.has_value()) {
        profile.put("HdrSupport", *options.hdr_enabled);
      }
      if (options.frame_generation_mode) {
        std::string frame_mode = *options.frame_generation_mode;
        boost::algorithm::to_upper(frame_mode);
        profile.put("FrameGeneration", frame_mode);
      } else {
        profile.put("FrameGeneration", "Off");
      }
      if (options.lsfg3_mode) {
        std::string lsfg_mode = *options.lsfg3_mode;
        boost::algorithm::to_upper(lsfg_mode);
        profile.put("LSFG3Mode1", lsfg_mode);
      }
      const std::string *scaling_type_for_opts = options.scaling_type ? &(*options.scaling_type) : nullptr;
      if (options.performance_mode.has_value()) {
        const bool perf = *options.performance_mode;
        profile.put("LSFGSize", perf ? "PERFORMANCE" : "BALANCED");
        if (scaling_type_for_opts && boost::iequals(*scaling_type_for_opts, "LS1")) {
          profile.put("LS1Type", perf ? "PERFORMANCE" : "BALANCED");
        }
      }
      profile.put("MaxFrameLatency", k_max_frame_latency);
      if (options.flow_scale) {
        int flow = std::clamp(*options.flow_scale, k_flow_scale_min, k_flow_scale_max);
        profile.put("LSFGFlowScale", flow);
      }
      if (options.target_fps && *options.target_fps > 0) {
        int target = std::clamp(*options.target_fps, 1, 480);
        profile.put("LSFG3Target", target);
      }
      double scale_factor = 1.0;
      const bool has_resolution_scale = options.resolution_scale_factor.has_value();
      if (has_resolution_scale) {
        scale_factor = std::clamp(*options.resolution_scale_factor, k_resolution_factor_min, k_resolution_factor_max);
        profile.put("ScaleFactor", scale_factor);
      }
      if (options.scaling_type) {
        profile.put("ScalingType", *options.scaling_type);
      } else if (has_resolution_scale) {
        profile.put("ScalingType", std::abs(scale_factor - 1.0) < 0.01 ? "Off" : "Auto");
      }
      if (has_resolution_scale && std::abs(scale_factor - 1.0) > 0.01) {
        profile.put("ScalingMode", "Custom");
        profile.put("ResizeBeforeScaling", true);
      }
      if (options.sharpness) {
        int sharpness = std::clamp(*options.sharpness, k_sharpness_min, k_sharpness_max);
        profile.put("Sharpness", sharpness);
      }
      if (options.ls1_sharpness) {
        int ls1 = std::clamp(*options.ls1_sharpness, k_sharpness_min, k_sharpness_max);
        profile.put("LS1Sharpness", ls1);
      }
      if (options.anime4k_type) {
        std::string type = *options.anime4k_type;
        boost::algorithm::to_upper(type);
        profile.put("Anime4kType", type);
      }
      if (options.anime4k_vrs.has_value()) {
        profile.put("VRS", *options.anime4k_vrs);
      }
      return profile;
    }

    bool insert_vibeshine_profile(const ProfileTemplates &templates, const lossless_scaling_options &options, const std::string &filter_utf8, boost::property_tree::ptree &profiles, lossless_scaling_profile_backup &backup) {
      if (filter_utf8.empty()) {
        return false;
      }
      profiles.push_back(std::make_pair("Profile", make_vibeshine_profile(templates, options, filter_utf8)));
      backup.valid = true;
      return true;
    }

    bool restore_string_field(boost::property_tree::ptree &profile, const char *key, bool had_value, const std::string &value, bool &changed) {
      auto current = profile.get_optional<std::string>(key);
      if (had_value) {
        if (!current || *current != value) {
          profile.put(key, value);
          changed = true;
        }
      } else if (current) {
        profile.erase(key);
        changed = true;
      }
      return changed;
    }

    bool restore_int_field(boost::property_tree::ptree &profile, const char *key, bool had_value, int value, bool &changed) {
      auto current = profile.get_optional<int>(key);
      if (had_value) {
        if (!current || *current != value) {
          profile.put(key, value);
          changed = true;
        }
      } else if (current) {
        profile.erase(key);
        changed = true;
      }
      return changed;
    }

    bool restore_bool_field(boost::property_tree::ptree &profile, const char *key, bool had_value, bool value, bool &changed) {
      auto current = profile.get_optional<bool>(key);
      if (had_value) {
        if (!current || *current != value) {
          profile.put(key, value);
          changed = true;
        }
      } else if (current) {
        profile.erase(key);
        changed = true;
      }
      return changed;
    }

    bool restore_double_field(boost::property_tree::ptree &profile, const char *key, bool had_value, double value, bool &changed) {
      auto current = profile.get_optional<double>(key);
      if (had_value) {
        if (!current || std::abs(*current - value) > std::numeric_limits<double>::epsilon()) {
          profile.put(key, value);
          changed = true;
        }
      } else if (current) {
        profile.erase(key);
        changed = true;
      }
      return changed;
    }

    bool apply_backup_to_profile(boost::property_tree::ptree &profile, const lossless_scaling_profile_backup &backup) {
      bool changed = false;
      restore_string_field(profile, "AutoScale", backup.had_auto_scale, backup.auto_scale, changed);
      restore_int_field(profile, "AutoScaleDelay", backup.had_auto_scale_delay, backup.auto_scale_delay, changed);
      restore_int_field(profile, "LSFG3Target", backup.had_lsfg_target, backup.lsfg_target, changed);
      restore_string_field(profile, "CaptureApi", backup.had_capture_api, backup.capture_api, changed);
      restore_int_field(profile, "QueueTarget", backup.had_queue_target, backup.queue_target, changed);
      restore_bool_field(profile, "HdrSupport", backup.had_hdr_support, backup.hdr_support, changed);
      restore_int_field(profile, "LSFGFlowScale", backup.had_flow_scale, backup.flow_scale, changed);
      restore_string_field(profile, "LSFGSize", backup.had_lsfg_size, backup.lsfg_size, changed);
      restore_string_field(profile, "LSFG3Mode1", backup.had_lsfg3_mode, backup.lsfg3_mode, changed);
      restore_string_field(profile, "FrameGeneration", backup.had_frame_generation, backup.frame_generation, changed);
      restore_string_field(profile, "ScalingType", backup.had_scaling_type, backup.scaling_type, changed);
      restore_string_field(profile, "LS1Type", backup.had_ls1_type, backup.ls1_type, changed);
      restore_string_field(profile, "ScalingMode", backup.had_scaling_mode, backup.scaling_mode, changed);
      restore_bool_field(profile, "ResizeBeforeScaling", backup.had_resize_before_scaling, backup.resize_before_scaling, changed);
      restore_string_field(profile, "ScalingFitMode", backup.had_scaling_fit_mode, backup.scaling_fit_mode, changed);
      restore_double_field(profile, "ScaleFactor", backup.had_scale_factor, backup.scale_factor, changed);
      restore_int_field(profile, "Sharpness", backup.had_sharpness, backup.sharpness, changed);
      restore_int_field(profile, "LS1Sharpness", backup.had_ls1_sharpness, backup.ls1_sharpness, changed);
      restore_string_field(profile, "Anime4kType", backup.had_anime4k_type, backup.anime4k_type, changed);
      restore_bool_field(profile, "VRS", backup.had_vrs, backup.vrs, changed);
      restore_string_field(profile, "SyncMode", backup.had_sync_mode, backup.sync_mode, changed);
      restore_int_field(profile, "MaxFrameLatency", backup.had_max_frame_latency, backup.max_frame_latency, changed);
      return changed;
    }

    bool write_settings_tree(boost::property_tree::ptree &tree, const std::filesystem::path &path) {
      strip_xml_whitespace(tree);
      try {
        boost::property_tree::xml_writer_settings<std::string> settings(' ', 2);
        boost::property_tree::write_xml(path.string(), tree, std::locale(), settings);
        return true;
      } catch (...) {
        BOOST_LOG(warning) << "Lossless Scaling: failed to write settings";
        return false;
      }
    }

  }  // namespace

  lossless_scaling_options lossless_scaling_env_loader::load() const {
    lossless_scaling_options opt;
    opt.enabled = parse_env_flag(std::getenv("SUNSHINE_LOSSLESS_SCALING_FRAMEGEN"));
    opt.target_fps = parse_env_int(std::getenv("SUNSHINE_LOSSLESS_SCALING_TARGET_FPS"));
    opt.rtss_limit = parse_env_int(std::getenv("SUNSHINE_LOSSLESS_SCALING_RTSS_LIMIT"));
    opt.active_profile = parse_env_string(std::getenv("SUNSHINE_LOSSLESS_SCALING_ACTIVE_PROFILE"));
    opt.capture_api = parse_env_string(std::getenv("SUNSHINE_LOSSLESS_SCALING_CAPTURE_API"));
    opt.queue_target = parse_env_int_allow_zero(std::getenv("SUNSHINE_LOSSLESS_SCALING_QUEUE_TARGET"));
    opt.hdr_enabled = parse_env_flag_optional(std::getenv("SUNSHINE_LOSSLESS_SCALING_HDR"));
    opt.flow_scale = clamp_optional_int(parse_env_int_allow_zero(std::getenv("SUNSHINE_LOSSLESS_SCALING_FLOW_SCALE")), k_flow_scale_min, k_flow_scale_max);
    opt.performance_mode = parse_env_flag_optional(std::getenv("SUNSHINE_LOSSLESS_SCALING_PERFORMANCE_MODE"));
    opt.resolution_scale_factor = clamp_optional_double(parse_env_double(std::getenv("SUNSHINE_LOSSLESS_SCALING_RESOLUTION_SCALE")), k_resolution_factor_min, k_resolution_factor_max);
    opt.frame_generation_mode = parse_env_string(std::getenv("SUNSHINE_LOSSLESS_SCALING_FRAMEGEN_MODE"));
    opt.lsfg3_mode = parse_env_string(std::getenv("SUNSHINE_LOSSLESS_SCALING_LSFG3_MODE"));
    opt.scaling_type = parse_env_string(std::getenv("SUNSHINE_LOSSLESS_SCALING_SCALING_TYPE"));
    opt.sharpness = clamp_optional_int(parse_env_int_allow_zero(std::getenv("SUNSHINE_LOSSLESS_SCALING_SHARPNESS")), k_sharpness_min, k_sharpness_max);
    opt.ls1_sharpness = clamp_optional_int(parse_env_int_allow_zero(std::getenv("SUNSHINE_LOSSLESS_SCALING_LS1_SHARPNESS")), k_sharpness_min, k_sharpness_max);
    opt.anime4k_type = parse_env_string(std::getenv("SUNSHINE_LOSSLESS_SCALING_ANIME4K_TYPE"));
    opt.anime4k_vrs = parse_env_flag_optional(std::getenv("SUNSHINE_LOSSLESS_SCALING_ANIME4K_VRS"));
    opt.legacy_auto_detect = parse_env_flag(std::getenv("SUNSHINE_LOSSLESS_SCALING_LEGACY_AUTO_DETECT"));
    if (auto delay = parse_env_int_allow_zero(std::getenv("SUNSHINE_LOSSLESS_SCALING_LAUNCH_DELAY"))) {
      opt.launch_delay_seconds = std::max(0, *delay);
    }
    if (auto configured = get_lossless_scaling_env_path()) {
      if (!configured->empty()) {
        opt.configured_path = configured;
      }
    }
    finalize_lossless_options(opt);
    return opt;
  }

  lossless_scaling_metadata_loader::lossless_scaling_metadata_loader(lossless_scaling_app_metadata metadata):
      _metadata(std::move(metadata)) {
  }

  lossless_scaling_options lossless_scaling_metadata_loader::load() const {
    lossless_scaling_options opt;
    opt.enabled = _metadata.enabled;
    opt.target_fps = _metadata.target_fps;
    opt.rtss_limit = _metadata.rtss_limit;
    opt.configured_path = _metadata.configured_path;
    opt.active_profile = _metadata.active_profile;
    opt.capture_api = _metadata.capture_api;
    opt.queue_target = _metadata.queue_target;
    opt.hdr_enabled = _metadata.hdr_enabled;
    opt.flow_scale = clamp_optional_int(_metadata.flow_scale, k_flow_scale_min, k_flow_scale_max);
    opt.performance_mode = _metadata.performance_mode;
    opt.resolution_scale_factor = clamp_optional_double(_metadata.resolution_scale_factor, k_resolution_factor_min, k_resolution_factor_max);
    opt.frame_generation_mode = _metadata.frame_generation_mode;
    opt.lsfg3_mode = _metadata.lsfg3_mode;
    opt.scaling_type = _metadata.scaling_type;
    opt.sharpness = clamp_optional_int(_metadata.sharpness, k_sharpness_min, k_sharpness_max);
    opt.ls1_sharpness = clamp_optional_int(_metadata.ls1_sharpness, k_sharpness_min, k_sharpness_max);
    opt.anime4k_type = _metadata.anime4k_type;
    opt.anime4k_vrs = _metadata.anime4k_vrs;
    opt.launch_delay_seconds = std::max(0, _metadata.launch_delay_seconds);
    opt.legacy_auto_detect = _metadata.legacy_auto_detect;
    finalize_lossless_options(opt);
    return opt;
  }

  lossless_scaling_options read_lossless_scaling_options() {
    lossless_scaling_env_loader loader;
    return loader.load();
  }

  lossless_scaling_options read_lossless_scaling_options(const lossless_scaling_app_metadata &metadata) {
    lossless_scaling_metadata_loader loader(metadata);
    return loader.load();
  }

  std::optional<DWORD> lossless_scaling_select_focus_pid(const std::string &install_dir_utf8, const std::string &exe_path_utf8, std::optional<DWORD> preferred_pid) {
    auto install_dir_norm = normalize_utf8_path(install_dir_utf8);
    auto exe_path_norm = normalize_utf8_path(exe_path_utf8);
    if (install_dir_norm && !install_dir_norm->empty() && install_dir_norm->back() != L'\\') {
      install_dir_norm->push_back(L'\\');
    }
    const bool has_filter = (install_dir_norm && !install_dir_norm->empty()) || (exe_path_norm && !exe_path_norm->empty());

    if (!has_filter) {
      BOOST_LOG(warning) << "Lossless Scaling: PID selection using windowed heuristic (install/exe empty)";
    }

    auto snapshot = enumerate_process_ids_snapshot();
    std::vector<DWORD> initial_candidates;
    std::unordered_set<DWORD> allowed_pids;
    initial_candidates.reserve(snapshot.size());
    for (auto pid : snapshot) {
      if (pid == 0) {
        continue;
      }
      auto path = query_process_image_path_optional(pid);
      if (!path) {
        continue;
      }
      if (is_ignored_process_path(*path)) {
        continue;
      }
      if (has_filter) {
        if (path_matches_filter(*path, install_dir_norm, exe_path_norm)) {
          initial_candidates.push_back(pid);
        }
      } else {
        if (focus::find_main_window_for_pid(pid)) {
          initial_candidates.push_back(pid);
        }
      }
    }
    if (initial_candidates.empty()) {
      return std::nullopt;
    }
    if (initial_candidates.size() == 1) {
      return initial_candidates.front();
    }
    allowed_pids.reserve(initial_candidates.size());
    for (auto pid : initial_candidates) {
      allowed_pids.insert(pid);
    }

    struct focus_process_candidate {
      DWORD pid = 0;
      ULONGLONG start_cpu = 0;
      ULONGLONG last_cpu = 0;
      SIZE_T peak_working_set = 0;
      std::wstring path;
      std::chrono::steady_clock::time_point first_seen;
      std::chrono::steady_clock::time_point last_seen;
    };

    std::unordered_map<DWORD, focus_process_candidate> candidates;
    auto deadline = std::chrono::steady_clock::now() + k_lossless_observation_duration;

    SYSTEM_INFO sys_info {};
    GetSystemInfo(&sys_info);
    double cpu_count = sys_info.dwNumberOfProcessors > 0 ? static_cast<double>(sys_info.dwNumberOfProcessors) : 1.0;

    while (std::chrono::steady_clock::now() < deadline) {
      auto now = std::chrono::steady_clock::now();
      snapshot = enumerate_process_ids_snapshot();
      for (auto pid : snapshot) {
        if (pid == 0) {
          continue;
        }
        if (!allowed_pids.empty() && allowed_pids.find(pid) == allowed_pids.end()) {
          continue;
        }
        auto &entry = candidates[pid];
        if (entry.pid == 0) {
          auto path = query_process_image_path_optional(pid);
          if (!path || is_ignored_process_path(*path)) {
            candidates.erase(pid);
            continue;
          }
          if (has_filter && !path_matches_filter(*path, install_dir_norm, exe_path_norm)) {
            candidates.erase(pid);
            continue;
          }
          entry.pid = pid;
          entry.path = *path;
          entry.first_seen = now;
          entry.last_seen = now;
        }
        ULONGLONG cpu_time = 0;
        SIZE_T working_set = 0;
        if (!sample_process_usage(pid, cpu_time, working_set)) {
          if (entry.start_cpu == 0) {
            candidates.erase(pid);
          }
          continue;
        }
        if (entry.start_cpu == 0) {
          entry.start_cpu = cpu_time;
        }
        entry.last_cpu = cpu_time;
        entry.last_seen = now;
        if (working_set > entry.peak_working_set) {
          entry.peak_working_set = working_set;
        }
      }
      std::this_thread::sleep_for(k_lossless_poll_interval);
    }

    if (candidates.empty()) {
      return initial_candidates.front();
    }

    double max_cpu_ratio = 0.0;
    double max_mem = 0.0;

    struct candidate_score {
      DWORD pid;
      std::wstring path;
      double cpu_ratio;
      double mem_mb;
      bool preferred_match;
      bool exe_match;
    };

    std::vector<candidate_score> scores;
    scores.reserve(candidates.size());

    for (auto &[pid, candidate] : candidates) {
      if (candidate.start_cpu == 0 || candidate.last_cpu < candidate.start_cpu) {
        continue;
      }
      if (candidate.last_seen <= candidate.first_seen) {
        continue;
      }
      if (candidate.path.empty()) {
        continue;
      }
      double elapsed = std::chrono::duration<double>(candidate.last_seen - candidate.first_seen).count();
      if (elapsed <= 0.1) {
        elapsed = 0.1;
      }
      double cpu_seconds = static_cast<double>(candidate.last_cpu - candidate.start_cpu) / 10000000.0;
      if (cpu_seconds < 0.0) {
        cpu_seconds = 0.0;
      }
      double cpu_ratio = cpu_seconds / (elapsed * cpu_count);
      if (cpu_ratio < 0.0) {
        cpu_ratio = 0.0;
      }
      double mem_mb = static_cast<double>(candidate.peak_working_set) / (1024.0 * 1024.0);
      auto normalized_path = normalize_lowercase_path(candidate.path);
      bool matches = install_dir_norm && !install_dir_norm->empty()
                       ? path_matches_prefix(normalized_path, *install_dir_norm)
                       : false;
      bool exe_match = exe_path_norm && !exe_path_norm->empty() && normalized_path == *exe_path_norm;
      scores.push_back({pid, candidate.path, cpu_ratio, mem_mb, matches, exe_match});
      max_cpu_ratio = std::max(max_cpu_ratio, cpu_ratio);
      max_mem = std::max(max_mem, mem_mb);
    }

    if (scores.empty()) {
      return initial_candidates.front();
    }

    bool cpu_low = max_cpu_ratio < 0.08;
    double cpu_weight = cpu_low ? 0.5 : 0.7;
    double mem_weight = 1.0 - cpu_weight;

    auto ensure_dir_prefix = [](std::wstring value) {
      if (!value.empty() && value.back() != L'\\') {
        value.push_back(L'\\');
      }
      return normalize_lowercase_path(value);
    };

    std::wstring windows_dir_norm;
    {
      wchar_t windows_dir[MAX_PATH] = {};
      UINT len = GetWindowsDirectoryW(windows_dir, ARRAYSIZE(windows_dir));
      if (len > 0 && len < ARRAYSIZE(windows_dir)) {
        windows_dir_norm = ensure_dir_prefix(std::wstring(windows_dir, len));
      }
    }

    auto has_prefix = [](const std::wstring &value, const std::wstring &prefix) {
      return !prefix.empty() && value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
    };

    const candidate_score *best = nullptr;
    double best_score = -1.0;
    DWORD root_pid = preferred_pid.value_or(0);

    for (const auto &score : scores) {
      double cpu_norm = max_cpu_ratio > 0.0 ? score.cpu_ratio / max_cpu_ratio : 0.0;
      double mem_norm = max_mem > 0.0 ? score.mem_mb / max_mem : 0.0;
      double combined = (cpu_weight * cpu_norm) + (mem_weight * mem_norm);
      if (score.preferred_match) {
        combined += 0.2;
      }
      if (score.exe_match) {
        combined += 0.25;
      }
      if (root_pid && score.pid == root_pid) {
        combined += score.preferred_match ? 0.05 : -0.05;
      }
      combined += std::min(score.cpu_ratio, 1.0) * 0.15;

      if (!windows_dir_norm.empty()) {
        auto normalized_path = normalize_lowercase_path(score.path);
        bool system_path = has_prefix(normalized_path, windows_dir_norm);
        if (system_path) {
          combined -= 0.2;
        }
        if (system_path && score.cpu_ratio < 0.02 && score.mem_mb < 48.0) {
          combined -= 0.05;
        }
      } else if (score.cpu_ratio < 0.015 && score.mem_mb < 32.0) {
        combined -= 0.05;
      }

      if (combined > best_score) {
        best_score = combined;
        best = &score;
      }
    }

    if (!best) {
      return initial_candidates.front();
    }

    BOOST_LOG(debug) << "Lossless Scaling: focus candidate PID=" << best->pid
                     << " cpu=" << best->cpu_ratio << " memMB=" << best->mem_mb;
    BOOST_LOG(info) << "Lossless Scaling: selected focus PID=" << best->pid
                    << " exe=" << platf::dxgi::wide_to_utf8(best->path);
    return best->pid;
  }

  lossless_scaling_runtime_state capture_lossless_scaling_state() {
    lossless_scaling_runtime_state state;
    const std::array<const wchar_t *, 2> names {L"Lossless Scaling.exe", L"LosslessScaling.exe"};
    for (auto name : names) {
      collect_runtime_for_process(name, state);
    }
    state.previously_running = !state.running_pids.empty();
    return state;
  }

  void lossless_scaling_stop_processes(lossless_scaling_runtime_state &state) {
    if (state.running_pids.empty()) {
      return;
    }
    lossless_scaling_post_wm_close(state.running_pids);
    for (DWORD pid : state.running_pids) {
      HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);
      if (!h) {
        continue;
      }
      DWORD wait = WaitForSingleObject(h, 4000);
      if (wait == WAIT_TIMEOUT) {
        TerminateProcess(h, 0);
        WaitForSingleObject(h, 2000);
      }
      CloseHandle(h);
    }
    state.stopped = true;
  }

    bool focus_and_minimize_existing_instances(const lossless_scaling_runtime_state &state, bool minimize_window) {
      if (state.stopped || !state.previously_running) {
        return false;
      }
      bool handled = false;
      for (DWORD pid : state.running_pids) {
        bool focused = lossless_scaling_focus_window(pid);
        bool minimized = minimize_window ? lossless_scaling_minimize_window(pid) : false;
        handled = focused || minimized || handled;
      }
      return handled;
    }

  bool should_launch_new_instance(const lossless_scaling_runtime_state &state, bool force_launch) {
    if (force_launch) {
      return true;
    }
    if (state.running_pids.empty()) {
      return true;
    }
    return state.stopped;
  }
#ifdef SUNSHINE_TESTS
  bool should_launch_new_instance_for_tests(const lossless_scaling_runtime_state &state, bool force_launch) {
    return should_launch_new_instance(state, force_launch);
  }
#endif

  static bool focus_with_retry(DWORD pid, int attempts, std::chrono::milliseconds delay) {
    if (!pid) {
      return false;
    }
    for (int attempt = 0; attempt < attempts; ++attempt) {
      if (lossless_scaling_focus_window(pid)) {
        return true;
      }
      std::this_thread::sleep_for(delay);
    }
    return false;
  }

    std::pair<bool, bool> focus_and_minimize_new_process(PROCESS_INFORMATION &pi, DWORD game_pid, bool minimize_window) {
      bool focused = false;
      bool minimized = false;
      if (pi.hProcess) {
        WaitForInputIdle(pi.hProcess, 5000);
        constexpr int kFocusRetries = 4;
        constexpr auto kRetryDelay = std::chrono::milliseconds(120);
        if (game_pid) {
          focus_with_retry(game_pid, kFocusRetries, kRetryDelay);
          std::this_thread::sleep_for(150ms);
          bool first_lossless = focus_with_retry(pi.dwProcessId, kFocusRetries, kRetryDelay);
          focused = focused || first_lossless;
          std::this_thread::sleep_for(150ms);
          focus_with_retry(game_pid, kFocusRetries, kRetryDelay);
          std::this_thread::sleep_for(150ms);
          bool second_lossless = focus_with_retry(pi.dwProcessId, kFocusRetries, kRetryDelay);
          focused = focused || second_lossless;
        } else {
          focused = focus_with_retry(pi.dwProcessId, kFocusRetries, kRetryDelay);
        }
        if (minimize_window) {
          minimized = lossless_scaling_minimize_window(pi.dwProcessId);
        }
        if (!focused) {
          focused = lossless_scaling_focus_window(pi.dwProcessId);
        }
        if (game_pid) {
          std::this_thread::sleep_for(150ms);
          focus_with_retry(game_pid, kFocusRetries, kRetryDelay);
        }
        CloseHandle(pi.hProcess);
        pi.hProcess = nullptr;
      }
      if (pi.hThread) {
        CloseHandle(pi.hThread);
        pi.hThread = nullptr;
      }
      return {focused, minimized};
    }

    bool launch_lossless_executable(const std::wstring &exe, DWORD game_pid, bool minimize_window) {
      if (exe.empty()) {
        return false;
      }
      STARTUPINFOW si {sizeof(si)};
      si.dwFlags = STARTF_USESHOWWINDOW;
      si.wShowWindow = SW_SHOWNORMAL;
      PROCESS_INFORMATION pi {};
      std::wstring cmd = L"\"" + exe + L"\"";
      std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
      cmdline.push_back(L'\0');

      auto finalize_launch = [&](PROCESS_INFORMATION &proc_info) {
        auto [focused, minimized] = focus_and_minimize_new_process(proc_info, game_pid, minimize_window);
        if (!focused) {
          BOOST_LOG(debug) << "Lossless Scaling: launched but could not focus window";
        }
        if (minimize_window && !minimized) {
          BOOST_LOG(debug) << "Lossless Scaling: launched but could not minimize window";
        }
        return true;
      };

      auto close_process_handles = [&]() {
        if (pi.hProcess) {
          CloseHandle(pi.hProcess);
          pi.hProcess = nullptr;
        }
        if (pi.hThread) {
          CloseHandle(pi.hThread);
          pi.hThread = nullptr;
        }
      };

      bool launched = false;
      if (platf::dxgi::is_running_as_system()) {
        winrt::handle user_token {platf::dxgi::retrieve_users_token(false)};
        if (user_token) {
          LPVOID raw_env = nullptr;
          if (!CreateEnvironmentBlock(&raw_env, user_token.get(), FALSE)) {
            raw_env = nullptr;
          }
          std::unique_ptr<void, decltype(&DestroyEnvironmentBlock)> env_block(raw_env, DestroyEnvironmentBlock);
          BOOL ok = FALSE;
          if (ImpersonateLoggedOnUser(user_token.get())) {
            auto revert_guard = util::fail_guard([&]() {
              if (!RevertToSelf()) {
                DWORD err = GetLastError();
                BOOST_LOG(fatal) << "Lossless Scaling: failed to revert impersonation after launch, error=" << err;
                DebugBreak();
              }
            });
            ok = CreateProcessAsUserW(
              user_token.get(),
              exe.c_str(),
              cmdline.data(),
              nullptr,
              nullptr,
              FALSE,
              CREATE_UNICODE_ENVIRONMENT,
              env_block.get(),
              nullptr,
              &si,
              &pi
            );
            if (!ok) {
              BOOST_LOG(warning) << "Lossless Scaling: CreateProcessAsUser failed, error=" << GetLastError();
            }
          } else {
            BOOST_LOG(warning) << "Lossless Scaling: impersonation failed for CreateProcessAsUser, error=" << GetLastError();
          }
          if (ok) {
            launched = true;
          } else {
            close_process_handles();
          }
        } else {
          BOOST_LOG(debug) << "Lossless Scaling: no user token available for impersonated launch";
        }
      }
      if (!launched) {
        if (!CreateProcessW(exe.c_str(), cmdline.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &si, &pi)) {
          BOOST_LOG(warning) << "Lossless Scaling: CreateProcess fallback failed, error=" << GetLastError();
          close_process_handles();
          return false;
        }
        launched = true;
      }
      bool result = launched && finalize_launch(pi);
      close_process_handles();
      return result;
    }

  bool lossless_scaling_apply_global_profile(const lossless_scaling_options &options, const std::string &install_dir_utf8, const std::string &exe_path_utf8, lossless_scaling_profile_backup &backup) {
    backup = {};
    auto worker = [&]() -> bool {
      auto settings_path = lossless_scaling_settings_path();
      if (settings_path.empty()) {
        return false;
      }
      boost::property_tree::ptree tree;
      try {
        boost::property_tree::read_xml(settings_path.string(), tree);
      } catch (...) {
        BOOST_LOG(warning) << "Lossless Scaling: failed to read settings";
        return false;
      }
      auto profiles_opt = tree.get_child_optional("Settings.GameProfiles");
      if (!profiles_opt) {
        BOOST_LOG(warning) << "Lossless Scaling: GameProfiles missing";
        return false;
      }
      auto &profiles = *profiles_opt;
      bool removed = remove_vibeshine_profiles(profiles);
      ProfileTemplates templates = find_profile_templates(profiles);
      capture_backup_fields(templates, backup);
      auto base_dir = lossless_resolve_base_dir(install_dir_utf8, exe_path_utf8);
      auto explicit_exe = resolve_explicit_executable(exe_path_utf8);
      std::string filter_utf8 = build_executable_filter(base_dir, explicit_exe);
      bool inserted = insert_vibeshine_profile(templates, options, filter_utf8, profiles, backup);
      if (!removed && !inserted) {
        return false;
      }
      return write_settings_tree(tree, settings_path);
    };
    return run_with_user_context(worker);
  }

  bool lossless_scaling_restore_global_profile(const lossless_scaling_profile_backup &backup) {
    auto worker = [&]() -> bool {
      auto settings_path = lossless_scaling_settings_path();
      if (settings_path.empty()) {
        return false;
      }
      boost::property_tree::ptree tree;
      try {
        boost::property_tree::read_xml(settings_path.string(), tree);
      } catch (...) {
        return false;
      }
      auto profiles_opt = tree.get_child_optional("Settings.GameProfiles");
      if (!profiles_opt) {
        return false;
      }
      auto &profiles = *profiles_opt;
      bool changed = remove_vibeshine_profiles(profiles);
      ProfileTemplates templates = find_profile_templates(profiles);
      if (templates.defaults && backup.valid) {
        changed |= apply_backup_to_profile(*templates.defaults, backup);
      }
      if (!changed) {
        return false;
      }
      return write_settings_tree(tree, settings_path);
    };
    return run_with_user_context(worker);
  }

  void lossless_scaling_restart_foreground(const lossless_scaling_runtime_state &state, bool force_launch, const std::string &install_dir_utf8, const std::string &exe_path_utf8, DWORD focused_game_pid, bool legacy_auto_detect) {
    focus_and_minimize_existing_instances(state, !legacy_auto_detect);
    const bool should_launch = should_launch_new_instance(state, force_launch);

    if (!install_dir_utf8.empty() || !exe_path_utf8.empty()) {
      auto base_dir = lossless_resolve_base_dir(install_dir_utf8, exe_path_utf8);
      auto explicit_exe = resolve_explicit_executable(exe_path_utf8);
      auto exe_names = lossless_collect_executable_names(base_dir.value_or(std::filesystem::path()), explicit_exe);

      if (!exe_names.empty()) {
        const char *timeout_env = std::getenv("SUNSHINE_LOSSLESS_WAIT_TIMEOUT");
        int timeout_secs = 10;
        if (auto parsed = parse_env_int(timeout_env)) {
          timeout_secs = std::clamp(*parsed, 1, 60);
        }

        BOOST_LOG(debug) << "Lossless Scaling: waiting up to " << timeout_secs << " seconds for game process to appear (checking " << exe_names.size() << " executables)";
        bool game_detected = wait_for_any_executable(exe_names, timeout_secs);
        if (game_detected) {
          BOOST_LOG(info) << "Lossless Scaling: game detected";
        }
      }
    }

    if (should_launch) {
      auto exe = discover_lossless_scaling_exe(state);
      if (!exe || exe->empty() || !std::filesystem::exists(*exe)) {
        BOOST_LOG(debug) << "Lossless Scaling: executable path not resolved for relaunch";
        return;
      }
      if (launch_lossless_executable(*exe, focused_game_pid, !legacy_auto_detect)) {
        BOOST_LOG(info) << "Lossless Scaling: relaunched at " << platf::dxgi::wide_to_utf8(*exe);
      } else {
        BOOST_LOG(warning) << "Lossless Scaling: relaunch failed, error=" << GetLastError();
        return;
      }
      if (!wait_for_lossless_ready(3s)) {
        BOOST_LOG(debug) << "Lossless Scaling: hotkey readiness wait timed out";
      } else {
        std::this_thread::sleep_for(150ms);
      }
    }

    auto hotkey = read_lossless_hotkey();
    if (!hotkey) {
      BOOST_LOG(warning) << "Lossless Scaling: no hotkey configured; skipping activation";
      return;
    }
    if (!hotkey->modifiers.empty()) {
      std::ostringstream mods;
      for (size_t i = 0; i < hotkey->modifiers.size(); ++i) {
        if (i > 0) {
          mods << "+";
        }
        mods << "0x" << std::hex << static_cast<int>(hotkey->modifiers[i]);
      }
      BOOST_LOG(debug) << "Lossless Scaling: hotkey vk=0x" << std::hex << static_cast<int>(hotkey->key)
                       << " mods=" << mods.str();
    } else {
      BOOST_LOG(debug) << "Lossless Scaling: hotkey vk=0x" << std::hex << static_cast<int>(hotkey->key)
                       << " mods=none";
    }

    DWORD target_pid = focused_game_pid;
    if (!target_pid) {
      if (auto selected = lossless_scaling_select_focus_pid(install_dir_utf8, exe_path_utf8, std::nullopt)) {
        target_pid = *selected;
      }
    }
    if (legacy_auto_detect && !target_pid) {
      for (int attempt = 0; attempt < 3 && !target_pid; ++attempt) {
        std::this_thread::sleep_for(1s);
        if (auto selected = lossless_scaling_select_focus_pid(install_dir_utf8, exe_path_utf8, std::nullopt)) {
          target_pid = *selected;
          break;
        }
      }
    }
    if (!target_pid && !install_dir_utf8.empty()) {
      try {
        auto install_dir_w = platf::dxgi::utf8_to_wide(install_dir_utf8);
        auto pids = focus::find_pids_under_install_dir_sorted(install_dir_w, false);
        if (!pids.empty()) {
          target_pid = pids.front();
          BOOST_LOG(info) << "Lossless Scaling: fallback focus PID=" << target_pid << " via installDir";
        }
      } catch (...) {
      }
    }
    if (legacy_auto_detect) {
      return;
    }

    HWND target_hwnd = nullptr;
    if (target_pid) {
      target_hwnd = focus_game_window(target_pid);
    }

    bool sent = apply_hotkey_for_pid(*hotkey, target_pid, target_hwnd != nullptr, 3);
    if (!sent) {
      BOOST_LOG(warning) << "Lossless Scaling: failed to send hotkey after retries";
    }

    if (!target_pid) {
      return;
    }

    constexpr int k_retarget_checks = 12;
    for (int attempt = 0; attempt < k_retarget_checks; ++attempt) {
      std::this_thread::sleep_for(1s);
      auto next_pid_opt = lossless_scaling_select_focus_pid(install_dir_utf8, exe_path_utf8, target_pid);
      if (!next_pid_opt || *next_pid_opt == target_pid) {
        continue;
      }
      DWORD next_pid = *next_pid_opt;
      BOOST_LOG(info) << "Lossless Scaling: retargeting from PID=" << target_pid << " to PID=" << next_pid;
      apply_hotkey_for_pid(*hotkey, target_pid, true, 2);
      HWND new_hwnd = wait_for_game_window(next_pid, 6s);
      if (new_hwnd) {
        focus::try_focus_hwnd(new_hwnd);
      } else {
        new_hwnd = focus_game_window(next_pid);
      }
      apply_hotkey_for_pid(*hotkey, next_pid, true, 2);
      target_pid = next_pid;
    }
  }

}  // namespace playnite_launcher::lossless
