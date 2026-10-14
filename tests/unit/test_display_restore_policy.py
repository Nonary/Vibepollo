"""Exercise the production boot restore policy and host persistence with real JSON.

The host writer is compiled with filesystem collaborators in place of the host
runtime. Both boot startup assignments are compiled from the helper source.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
compiler = sys.argv[1] if len(sys.argv) > 1 else "g++"


def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


writer = function((root / "src/state_storage.cpp").read_text(),
                  "  void save_display_restore_preference(")
paths = (root / "tools/display_helper_paths.h").read_text()
command_line_reader = function(paths, "  inline std::optional<std::filesystem::path> restore_state_file_from_command_line()")
task_arguments = function(paths, "  inline std::wstring restore_task_arguments()")
legacy = (root / "tools/display_settings_helper.cpp").read_text()
v2 = (root / "tools/display_settings_helper_v2.cpp").read_text()
legacy_boot = legacy[legacy.index('Display helper started in restore mode'):]
v2_boot = v2[v2.index('Display helper v2 running in restore mode.'):]
legacy_assignment = re.search(r"state\.always_restore_from_golden\.store\([\s\S]+?;", legacy_boot).group()
v2_assignment = re.search(r"revert\.always_restore_from_golden = [\s\S]+?;", v2_boot).group()
assert legacy_boot.index(legacy_assignment) < legacy_boot.index("state.ensure_restore_polling(")
assert v2_boot.index(v2_assignment) < v2_boot.index("queue.push(revert)")
# Both initial load and hot reload must save the resolved configuration value.
config = (root / "src/config.cpp").read_text()
assert config.count("statefile::save_display_restore_preference(video.dd.always_restore_from_golden);") == 2

integration = (root / "src/platform/windows/display_helper_integration.cpp").read_text()
assert "restore_state_file_argument(" in integration
assert "platf::from_utf8(config::nvhttp.vibeshine_file_state)" in integration
for task_source in [legacy, (root / "src/platform/windows/display_helper_v2/win_scheduled_task_manager.cpp").read_text()]:
    assert "put_Arguments(_bstr_t(display_helper_paths::restore_task_arguments().c_str()))" in task_source

with tempfile.TemporaryDirectory(prefix="display-restore-policy-") as temporary:
    directory = Path(temporary)
    source = directory / "test.cpp"
    source.write_text(r'''#include <cassert>
#include <atomic>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include "src/platform/windows/display_restore_policy.h"
#include "src/state_storage_policy.h"
using namespace std::literals;
namespace fs = std::filesystem;
namespace pt = boost::property_tree;
namespace policy = statefile::policy;
std::string state_path;
int writes = 0;
void migrate_recent_state_keys() {}
const std::string &vibeshine_state_path() { return state_path; }
std::mutex &state_mutex() { static std::mutex m; return m; }
policy::load_result_e load_tree_for_update(const fs::path &path, pt::ptree &tree) {
  if (!fs::exists(path)) return policy::load_result_e::missing;
  pt::read_json(path.string(), tree);
  return policy::load_result_e::loaded;
}
pt::ptree &ensure_root(pt::ptree &tree) {
  if (!tree.get_child_optional("root")) tree.add_child("root", pt::ptree{});
  return tree.get_child("root");
}
policy::read_result_t read_file(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {policy::read_status_e::missing, {}};
  return {policy::read_status_e::loaded, std::string(std::istreambuf_iterator<char>(in), {})};
}
void write_tree(const fs::path &path, const pt::ptree &tree) {
  policy::write_json_atomic(path.string(), tree,
    [](const std::string &target, const std::string &contents) {
      std::ofstream out(target, std::ios::binary); out << contents; return out.good();
    }, read_file);
  ++writes;
}
#define BOOST_LOG(level) std::cerr
''' + writer + r'''
std::vector<fs::path> config_roots, search_roots;
std::optional<fs::path> explicit_state_file;
std::vector<fs::path> executable_config_search_roots() { return config_roots; }
const wchar_t *GetCommandLineW() { return L"fake Windows process command line"; }
wchar_t **CommandLineToArgvW(const wchar_t *, int *argc) {
  static std::wstring argument;
  static wchar_t program[] = L"helper.exe";
  static wchar_t *arguments[2];
  arguments[0] = program;
  *argc = 1;
  if (explicit_state_file) {
    argument = L"--restore-state-file=" + explicit_state_file->wstring();
    arguments[1] = argument.data(); *argc = 2;
  }
  return arguments;
}
void LocalFree(void *) {}
namespace display_helper_paths {
std::vector<fs::path> executable_config_search_roots() { return config_roots; }
''' + command_line_reader + task_arguments + r'''
}
bool legacy_policy() {
  struct { std::atomic<bool> always_restore_from_golden {false}; } state;
''' + legacy_assignment + r'''
  return state.always_restore_from_golden.load();
}
bool v2_policy() {
  struct { std::optional<bool> always_restore_from_golden; } revert;
''' + v2_assignment + r'''
  assert(revert.always_restore_from_golden.has_value());
  return *revert.always_restore_from_golden;
}
void expect_policy(bool expected) {
  assert(legacy_policy() == expected);
  assert(v2_policy() == expected);
}
void put(const fs::path &directory, const std::string &content) {
  std::ofstream out(directory / "vibeshine_state.json"); out << content;
}
int main(int argc, char **argv) {
  assert(argc == 2);
  const fs::path primary = fs::path(argv[1]) / "config";
  const fs::path stale = fs::path(argv[1]) / "snapshots";
  fs::create_directories(primary); fs::create_directories(stale);
  config_roots = {primary}; search_roots = {stale, primary};
  state_path = (primary / "vibeshine_state.json").string();
  expect_policy(true); // Older install/no state matches host default.
  put(primary, R"({"root":{"display_helper_engine":"legacy","other":"keep"}})");
  save_display_restore_preference(false);
  expect_policy(false); // Actual property_tree writer/boot-reader round trip.
  pt::ptree tree; pt::read_json(state_path, tree);
  assert(tree.get<std::string>("root.other") == "keep");
  assert(tree.get<std::string>("root.display_helper_engine") == "legacy");
  const int first_writes = writes;
  save_display_restore_preference(false); assert(writes == first_writes);
  save_display_restore_preference(true); expect_policy(true);
  save_display_restore_preference(false); expect_policy(false); // Setting reload.
  put(primary, R"({"root":{"display_restore_prefer_golden":0}})");
  save_display_restore_preference(false); expect_policy(false); // Normalize noncanonical scalar.
  put(primary, R"({"root":{"display_restore_prefer_golden":"0"}})");
  save_display_restore_preference(false); expect_policy(false);
  put(stale, R"({"root":{"display_restore_prefer_golden":true}})");
  expect_policy(false); // Authoritative config wins over old per-user state.
  put(primary, R"({"root":{"display_restore_prefer_golden":false}})"); expect_policy(false);
  put(primary, R"({"root":{"display_restore_prefer_golden":true}})"); expect_policy(true);
  put(primary, R"({"root":{"display_restore_prefer_golden":"false"}})"); expect_policy(false);
  put(stale, R"({"root":{"display_restore_prefer_golden":false}})");
  for (const auto &json : {
         R"({"root":{}})", R"({"root":{"display_restore_prefer_golden":"invalid"}})",
         R"({"root":{"display_restore_prefer_golden":0}})",
         R"({"root":{"display_restore_prefer_golden":[false]}})",
         R"({"root":{"display_restore_prefer_golden":{"value":false}}})"}) {
    put(primary, json); expect_policy(true); // Never revive stale policy for default/invalid key.
  }
  put(primary, "{corrupt"); expect_policy(false); // Next valid root is usable.
  put(stale, "{corrupt"); expect_policy(true);
  fs::remove(primary / "vibeshine_state.json");
  fs::remove(stale / "vibeshine_state.json"); expect_policy(true);
  // Explicit configured auxiliary path has priority over every standard file.
  const auto custom = fs::path(argv[1]) / "custom state.json";
  explicit_state_file = custom;
  state_path = custom.string();
  put(primary, R"({"root":{"display_restore_prefer_golden":true}})");
  save_display_restore_preference(false); expect_policy(false);
  assert(display_helper_paths::restore_state_file_from_command_line() == explicit_state_file);
  assert(display_helper_paths::restore_task_arguments() ==
         L"--restore " + display_helper::restore_state_file_argument(custom));
  fs::copy_file(custom, custom.string() + ".bak");
  std::ofstream(custom) << "{corrupt"; expect_policy(false);
  fs::remove(custom); expect_policy(false); // Backup also covers missing primary.
  fs::remove(custom.string() + ".bak"); expect_policy(true); // No unrelated fallback.
  save_display_restore_preference(false); expect_policy(false);
  save_display_restore_preference(true); expect_policy(true);
  fs::copy_file(custom, custom.string() + ".bak");
  save_display_restore_preference(false); expect_policy(false); // Primary wins stale backup.
  const auto quoted = display_helper::restore_state_file_argument(fs::path("C:\\\\Host folder\\\\custom state.json"));
  assert(quoted.front() == L'"' && quoted.back() == L'"');
  assert(quoted.find(L"--restore-state-file=") != std::wstring::npos);
}

''')
    binary = directory / "test"
    subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(root),
                    str(source), str(root / "src/state_storage_policy.cpp"), "-o", str(binary)], check=True)
    subprocess.run([str(binary), str(directory)], check=True)
