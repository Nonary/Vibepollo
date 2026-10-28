"""Compile production NVIDIA lifetime/recovery code with platform boundaries.

The journal seam uses real temporary files. NVAPI, Windows environment calls
and log delivery are deterministic, without a GPU or Windows runtime claim.
"""
import pathlib
import re
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve()
compiler = sys.argv[2]
directory = root / 'src/platform/windows/nvprefs'


def without_includes(text):
    return re.sub(r'^\s*#(?:include[^\n]*|pragma once)\n', '', text, flags=re.M)


driver = (directory / 'driver_settings.cpp').read_text()
start = driver.index('  driver_settings_t::~driver_settings_t()')
destructor = driver[start:driver.index('\n  bool driver_settings_t::init()', start)]
start = driver.index('  bool driver_settings_t::destroy()')
destroy = driver[start:driver.index('\n  bool driver_settings_t::load_settings()', start)]
program = r'''
#include <cassert>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
namespace fs = std::filesystem;
bool logging_alive = true, error_log_throws = false;
std::string fail_at, throw_at;
std::map<std::string, int> calls;
std::set<fs::path> locks;
std::function<void()> on_log;
fs::path program_data;
int global_setting = 11, pending_setting = 11;
bool point(const std::string &name) {
  assert(logging_alive);
  ++calls[name];
  if (throw_at == name) { throw std::runtime_error("fixture: " + name); }
  return fail_at != name;
}
using NvDRSSessionHandle = void *;
constexpr int NVAPI_OK = 0;
int NvAPI_DRS_DestroySession(NvDRSSessionHandle) { return point("destroy") ? 0 : -1; }
int NvAPI_Unload() { return point("unload") ? 0 : -1; }
constexpr int MAX_PATH = 1024, ERROR_ALREADY_EXISTS = 183;
unsigned long GetEnvironmentVariableW(const wchar_t *, wchar_t *buffer, unsigned long size) {
  if (!point("environment")) { return 0; }
  const auto value = program_data.wstring();
  assert(value.size() + 1 < size);
  std::wcscpy(buffer, value.c_str());
  return value.size();
}
template<class Char> bool CreateDirectoryW(const Char *name, void *) { return fs::create_directory(fs::path(name)); }
int GetLastError() { return ERROR_ALREADY_EXISTS; }
namespace nvprefs {
void info_message(const std::string &) {
  point("info_log");
  if (on_log) { std::exchange(on_log, {})(); }
}
void info_message(const std::wstring &) { point("info_log"); }
void error_message(const std::string &) {
  point("error_log");
  if (error_log_throws) { throw std::runtime_error("fixture log failure"); }
}
struct undo_data_t {
  int original = 11;
  void merge(const undo_data_t &) {}
};
class undo_file_t {
  fs::path path;
  explicit undo_file_t(fs::path value): path(std::move(value)) { assert(locks.insert(path).second); }
public:
  undo_file_t(undo_file_t &&other) noexcept: path(std::exchange(other.path, {})) {}
  undo_file_t &operator=(undo_file_t &&other) noexcept {
    if (!path.empty()) { locks.erase(path); }
    path = std::exchange(other.path, {});
    return *this;
  }
  ~undo_file_t() { if (!path.empty()) { assert(locks.erase(path) == 1); } }
  static std::optional<undo_file_t> open_existing_file(fs::path path, bool &denied) {
    denied = false;
    point("open");
    if (!fs::exists(path)) { return {}; }
    if (locks.count(path)) { denied = true; return {}; }
    return undo_file_t(std::move(path));
  }
  static std::optional<undo_file_t> create_new_file(fs::path path) {
    point("create");
    if (fs::exists(path)) { return {}; }
    std::ofstream(path) << "";
    return undo_file_t(std::move(path));
  }
  bool write_undo_data(const undo_data_t &data) {
    point("write");
    std::ofstream(path) << data.original << '\n';
    return true;
  }
  std::optional<undo_data_t> read_undo_data() {
    if (!point("read")) { return {}; }
    undo_data_t value;
    if (!(std::ifstream(path) >> value.original)) { return {}; }
    return value;
  }
  bool delete_file() {
    if (!point("delete")) { return false; }
    return fs::remove(path);
  }
};
}
DRIVER_HEADER
INTERFACE_HEADER
namespace nvprefs {
DRIVER_DESTRUCTOR
DRIVER_DESTROY
bool driver_settings_t::init() {
  session_handle = reinterpret_cast<void *>(1); // Also exercise partial-init failure.
  if (!point("load")) { return false; }
  pending_setting = global_setting;
  return true;
}
bool driver_settings_t::load_settings() { pending_setting = global_setting; return true; }
bool driver_settings_t::save_settings() {
  if (!point("save")) { return false; }
  global_setting = pending_setting;
  return true;
}
bool driver_settings_t::restore_global_profile_to_undo(const undo_data_t &data) {
  if (!point("restore")) { return false; }
  pending_setting = data.original;
  info_message("restored setting");
  return true;
}
bool driver_settings_t::check_and_modify_global_profile(std::optional<undo_data_t> &data) {
  data = undo_data_t{global_setting};
  pending_setting = 99;
  return true;
}
bool driver_settings_t::check_and_modify_application_profile(bool &modified) { modified = false; return true; }
}
INTERFACE_SOURCE
fs::path journal() { return program_data / "Sunshine/nvprefs_undo.json"; }
std::string bytes() { std::ifstream stream(journal()); return {std::istreambuf_iterator<char>(stream), {}}; }
void clear_faults() { fail_at.clear(); throw_at.clear(); error_log_throws = false; }
void reset() {
  assert(locks.empty());
  logging_alive = true;
  clear_faults(); calls.clear(); global_setting = pending_setting = 11;
  fs::remove_all(program_data / "Sunshine");
}
void own(nvprefs::nvprefs_interface &prefs) {
  assert(prefs.load());
  assert(prefs.modify_global_profile());
  assert(global_setting == 99 && prefs.owning_undo_file() && bytes() == "11\n");
  assert(prefs.unload());
}
void closed(nvprefs::nvprefs_interface &prefs, bool result) {
  logging_alive = false;
  const auto count = calls;
  assert(prefs.shutdown() == result && prefs.shutdown() == result);
  assert(!prefs.load() && !prefs.owning_undo_file());
  assert(!prefs.modify_application_profile() && !prefs.modify_global_profile());
  assert(!prefs.restore_global_profile() && !prefs.restore_from_and_delete_undo_file_if_exists());
  assert(prefs.unload() == result);
  assert(calls == count && locks.empty());
}
void retry() {
  logging_alive = true;
  clear_faults();
  nvprefs::nvprefs_interface next;
  assert(next.load());
  assert(next.restore_from_and_delete_undo_file_if_exists());
  assert(global_setting == 11 && !fs::exists(journal()));
  assert(next.shutdown());
}
void shutdown_case(std::string failure, bool throwing, bool log_throws = false) {
  reset();
  std::optional<nvprefs::nvprefs_interface> prefs;
  prefs.emplace(); own(*prefs);
  const auto journal_bytes = bytes();
  if (throwing) { throw_at = failure; } else { fail_at = failure; }
  error_log_throws = log_throws;
  assert(!prefs->shutdown());
  const bool recovered = failure == "destroy" || failure == "unload";
  assert(fs::exists(journal()) != recovered);
  if (!recovered) { assert(bytes() == journal_bytes); }
  closed(*prefs, false);
  const auto after = calls;
  prefs.reset(); // Same production destructor as the global, after logging's boundary.
  assert(calls == after && locks.empty());
  if (!recovered) { retry(); }
}
void recovery_case(std::string failure, bool throwing) {
  reset();
  {
    nvprefs::nvprefs_interface previous; own(previous);
    fail_at = "restore";
    assert(!previous.shutdown());
  }
  clear_faults();
  const auto journal_bytes = bytes();
  nvprefs::nvprefs_interface next;
  assert(next.load());
  if (throwing) { throw_at = failure; } else { fail_at = failure; }
  assert(!next.restore_from_and_delete_undo_file_if_exists());
  assert(bytes() == journal_bytes && locks.empty());
  clear_faults();
  assert(!next.modify_global_profile());
  assert(bytes() == journal_bytes); // An unresolved journal blocks replacement.
  assert(next.restore_from_and_delete_undo_file_if_exists());
  assert(global_setting == 11 && !fs::exists(journal()));
  assert(next.shutdown());
}
int main(int argc, char **argv) {
  assert(argc == 2); program_data = argv[1];
  static_assert(noexcept(std::declval<nvprefs::nvprefs_interface &>().shutdown()));
  static_assert(noexcept(std::declval<nvprefs::nvprefs_interface &>().~nvprefs_interface()));
  static_assert(noexcept(std::declval<nvprefs::driver_settings_t &>().destroy()));
  reset();
  {
    std::optional<nvprefs::nvprefs_interface> prefs; prefs.emplace(); own(*prefs);
    assert(prefs->shutdown());
    assert(global_setting == 11 && !fs::exists(journal()));
    closed(*prefs, true);
    const auto after = calls; prefs.reset(); assert(calls == after);
  }
  for (const auto &stage : {"environment", "load", "restore", "save", "delete", "destroy", "unload"}) {
    shutdown_case(stage, false);
    shutdown_case(stage, true);
  }
  shutdown_case("info_log", true, true);
  shutdown_case("restore", true, true);
  for (const auto &stage : {"read", "restore", "save", "delete"}) {
    recovery_case(stage, false);
    recovery_case(stage, true);
  }
  recovery_case("info_log", true);
  // A logging callback cannot recursively start a second terminal cleanup.
  reset();
  {
    nvprefs::nvprefs_interface prefs; own(prefs);
    on_log = [&] { assert(!prefs.shutdown()); };
    assert(prefs.shutdown());
    closed(prefs, true);
  }
  // Closing a failed initialization also contains native cleanup exceptions.
  reset();
  {
    nvprefs::nvprefs_interface prefs;
    throw_at = "load";
    try { prefs.load(); assert(false); } catch (const std::runtime_error &) {}
    throw_at = "destroy";
    assert(!prefs.shutdown());
    closed(prefs, false);
  }
  // No explicit boundary: destruction still never restores/logs or deletes undo.
  reset();
  {
    nvprefs::nvprefs_interface prefs; own(prefs);
    logging_alive = false;
  }
  assert(bytes() == "11\n" && locks.empty());
  retry();
  std::cout << "Production NVIDIA cleanup: success, failure/throw matrix, durable retry, repeated close and post-logger destruction passed\n";
}
'''
for marker, value in (
    ('DRIVER_HEADER', without_includes((directory / 'driver_settings.h').read_text())),
    ('INTERFACE_HEADER', without_includes((directory / 'nvprefs_interface.h').read_text())),
    ('DRIVER_DESTRUCTOR', destructor), ('DRIVER_DESTROY', destroy),
    ('INTERFACE_SOURCE', without_includes((directory / 'nvprefs_interface.cpp').read_text())),
):
    program = program.replace(marker, value)
with tempfile.TemporaryDirectory(prefix='nvprefs-cleanup-') as temporary:
    path = pathlib.Path(temporary)
    (path / 'test.cpp').write_text(program)
    binary = path / 'test'
    subprocess.run([compiler, '-std=c++23', '-Wall', '-Wextra', '-Werror', str(path / 'test.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary), str(path)], check=True, timeout=15)
