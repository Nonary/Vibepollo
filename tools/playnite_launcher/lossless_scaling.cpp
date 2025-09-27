#include "tools/playnite_launcher/lossless_scaling.h"

#include "src/logging.h"
#include "src/platform/windows/ipc/misc_utils.h"
#include "tools/playnite_launcher/focus_utils.h"

#include <algorithm>
#include <array>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <locale>
#include <optional>
#include <shlobj.h>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>
#include <windows.h>

using namespace std::chrono_literals;

namespace playnite_launcher::lossless {
  namespace {

    constexpr std::string_view k_lossless_profile_title = "Vibeshine";
    constexpr size_t k_lossless_max_executables = 256;
    constexpr int k_lossless_auto_delay_seconds = 10;

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
      PWSTR local = nullptr;
      std::filesystem::path p;
      if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) && local) {
        p = std::filesystem::path(local);
        p /= L"Lossless Scaling";
        p /= L"settings.xml";
      }
      if (local) {
        CoTaskMemFree(local);
      }
      return p;
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
      profile.put("AutoScale", "true");
      profile.put("AutoScaleDelay", k_lossless_auto_delay_seconds);
      if (options.target_fps && *options.target_fps > 0) {
        int target = std::clamp(*options.target_fps, 1, 480);
        profile.put("LSFG3Target", target);
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

    bool apply_backup_to_profile(boost::property_tree::ptree &profile, const lossless_scaling_profile_backup &backup) {
      bool changed = false;
      restore_string_field(profile, "AutoScale", backup.had_auto_scale, backup.auto_scale, changed);
      restore_int_field(profile, "AutoScaleDelay", backup.had_auto_scale_delay, backup.auto_scale_delay, changed);
      restore_int_field(profile, "LSFG3Target", backup.had_lsfg_target, backup.lsfg_target, changed);
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

  lossless_scaling_options read_lossless_scaling_options() {
    lossless_scaling_options opt;
    opt.enabled = parse_env_flag(std::getenv("SUNSHINE_LOSSLESS_SCALING_FRAMEGEN"));
    opt.target_fps = parse_env_int(std::getenv("SUNSHINE_LOSSLESS_SCALING_TARGET_FPS"));
    opt.rtss_limit = parse_env_int(std::getenv("SUNSHINE_LOSSLESS_SCALING_RTSS_LIMIT"));
    if (opt.enabled && !opt.rtss_limit && opt.target_fps && *opt.target_fps > 0) {
      int computed = static_cast<int>(std::lround(*opt.target_fps * 0.6));
      if (computed > 0) {
        opt.rtss_limit = computed;
      }
    }
    if (auto configured = get_lossless_scaling_env_path()) {
      if (!configured->empty()) {
        opt.configured_path = configured;
      }
    }
    return opt;
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

  bool focus_existing_instances(const lossless_scaling_runtime_state &state) {
    if (state.stopped || !state.previously_running) {
      return false;
    }
    for (DWORD pid : state.running_pids) {
      if (lossless_scaling_focus_window(pid)) {
        return true;
      }
    }
    return false;
  }

  bool should_launch_new_instance(const lossless_scaling_runtime_state &state, bool force_launch) {
    if (force_launch) {
      return true;
    }
    return state.stopped || state.previously_running;
  }

  bool focus_new_process(PROCESS_INFORMATION &pi) {
    bool focused = false;
    if (pi.hProcess) {
      WaitForInputIdle(pi.hProcess, 5000);
      for (int attempt = 0; attempt < 10 && !focused; ++attempt) {
        std::this_thread::sleep_for(200ms);
        focused = lossless_scaling_focus_window(pi.dwProcessId);
      }
      CloseHandle(pi.hProcess);
      pi.hProcess = nullptr;
    }
    if (pi.hThread) {
      CloseHandle(pi.hThread);
      pi.hThread = nullptr;
    }
    return focused;
  }

  bool launch_lossless_executable(const std::wstring &exe) {
    STARTUPINFOW si {sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNORMAL;
    PROCESS_INFORMATION pi {};
    std::wstring cmd =
      L""
      " + exe + L"
      "";
    std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back(L'\0');
    BOOL ok = CreateProcessW(exe.c_str(), cmdline.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &si, &pi);
    if (!ok) {
      return false;
    }
    bool focused = focus_new_process(pi);
    if (!focused) {
      BOOST_LOG(debug) << "Lossless Scaling: launched but could not focus window";
    }
    return true;
  }

  bool lossless_scaling_apply_global_profile(const lossless_scaling_options &options, const std::string &install_dir_utf8, const std::string &exe_path_utf8, lossless_scaling_profile_backup &backup) {
    backup = {};
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
  }

  bool lossless_scaling_restore_global_profile(const lossless_scaling_profile_backup &backup) {
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
  }

  void lossless_scaling_restart_foreground(const lossless_scaling_runtime_state &state, bool force_launch) {
    if (focus_existing_instances(state)) {
      return;
    }
    if (!should_launch_new_instance(state, force_launch)) {
      return;
    }
    auto exe = discover_lossless_scaling_exe(state);
    if (!exe || exe->empty() || !std::filesystem::exists(*exe)) {
      BOOST_LOG(debug) << "Lossless Scaling: executable path not resolved for relaunch";
      return;
    }
    if (launch_lossless_executable(*exe)) {
      BOOST_LOG(info) << "Lossless Scaling: relaunched at " << platf::dxgi::wide_to_utf8(*exe);
    } else {
      BOOST_LOG(warning) << "Lossless Scaling: relaunch failed, error=" << GetLastError();
    }
  }

}  // namespace playnite_launcher::lossless
