"""Exercise actual legacy boot/session recovery with a recording controller.

Only OS/storage-loading collaborators are replaced. Recovery, eligibility,
stable-read/quiet-period gates, cancellation, and filesystem tier promotion are
extracted from production. Hard-coded clock/sleep leaves use a virtual clock.
--legacy-source supports before/after negative controls; --emit-only prepares
the translation unit and extraction manifest without compiling or running it.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile


def function(source, signature):
    tokens = re.findall(r"[A-Za-z_]\w*|::|[^\w\s]", signature)
    match = re.search(r"\s*".join(re.escape(token) for token in tokens), source)
    if match is None:
        raise ValueError(f"Production function not found: {signature}")
    start = match.start()
    # Default arguments such as std::stop_token st = {} contain braces before
    # the function body. Finish the parameter list before looking for it.
    end = source.index("(", start) + 1
    parentheses = 1
    while parentheses:
        parentheses += (source[end] == "(") - (source[end] == ")")
        end += 1
    end = source.index("{", end) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
parser.add_argument("--legacy-source", type=Path)
parser.add_argument("--compiler", default="g++")
parser.add_argument("--emit-only", type=Path)
parser.add_argument("--cases", help="Comma-separated case names; defaults to every case")
args = parser.parse_args()
path = args.legacy_source or args.repo / "tools/display_settings_helper.cpp"
legacy = path.read_text()
manifest = {"source": str(path), "source_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "extractions": {}}


def extract(signature):
    original = function(legacy, signature)
    transformed = original.replace("std::chrono::steady_clock", "FixtureClock").replace(
        "std::this_thread::sleep_for", "fixture_sleep")
    manifest["extractions"][signature] = {
        "original_sha256": hashlib.sha256(original.encode()).hexdigest(),
        "compiled_sha256": hashlib.sha256(transformed.encode()).hexdigest(),
        "clock_leaf_substitutions": original.count("std::chrono::steady_clock")
        + original.count("std::this_thread::sleep_for"),
    }
    return transformed


controller_methods = "\n".join(extract(signature) for signature in (
    "static std::string normalize_device_id(", "static bool contains_ci(",
    "static bool equals_ci(", "static bool is_virtual_display_device(",
    "static bool is_active_display_device(",
))
if "live_desktop_to_preserve(" in legacy:
    controller_methods += extract(
        "std::optional<display_device::DisplaySettingsSnapshot> live_desktop_to_preserve(")
service_methods = "\n".join(extract(signature) for signature in (
    "static display_device::ActiveTopology canonical_topology(",
    "static bool equal_snapshots_strict(", "bool read_stable_snapshot(",
    "bool quiet_period(", "bool promote_current_snapshot_to_previous(",
    "void clear_session_restore_snapshots_after_golden(",
    "bool apply_session_snapshot_from_path(", "bool try_restore_once_if_valid(",
))
service_methods += "\ntemplate<typename CancelPredicate>\n" + extract("static bool wait_with_cancel(")
boot = legacy[legacy.index('Display helper started in restore mode (--restore flag)'):]
assignment = re.search(r"state\.preserve_active_physical_primary\.store\([^;]+;", boot)
boot_assignment = assignment.group() if assignment else "(void) state;"
# A missing pre-fix startup flag must cause a runtime regression, not an
# extraction/compilation failure. The old recovery methods never read this stub.
field = re.search(r"std::atomic<bool> preserve_active_physical_primary\s*\{[^}]*\};", legacy)
flag_declaration = field.group() if field else "std::atomic<bool> preserve_active_physical_primary {false};"
manifest["boot_assignment"] = boot_assignment
manifest["flag_declaration"] = flag_declaration

program = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>
using namespace std::chrono_literals;
namespace fs = std::filesystem;
struct FixtureClock {
  static inline std::chrono::steady_clock::time_point value {};
  static auto now() { return value; }
};
void fixture_sleep(std::chrono::milliseconds duration) { FixtureClock::value += duration; }
struct NullLog { template<class T> NullLog &operator<<(const T &) { return *this; } };
#define BOOST_LOG(level) NullLog {}
namespace display_device {
  using ActiveTopology = std::vector<std::vector<std::string>>;
  enum class DeviceEnumerationDetail { Minimal };
  struct EnumeratedDevice {
    std::string m_device_id, m_display_name, m_friendly_name;
    struct Edid { std::string m_manufacturer_id; };
    std::optional<Edid> m_edid;
    std::optional<int> m_info;
  };
  using EnumeratedDeviceList = std::vector<EnumeratedDevice>;
  struct DisplaySettingsSnapshot {
    ActiveTopology m_topology;
    std::map<std::string, int> m_modes;
    std::map<std::string, std::optional<bool>> m_hdr_states;
    std::string m_primary_device;
    std::map<std::string, std::pair<int, int>> m_origins;
    bool operator==(const DisplaySettingsSnapshot &) const = default;
  };
  bool equalDisplayModes(const auto &a, const auto &b) { return a == b; }
}
using Snapshot = display_device::DisplaySettingsSnapshot;
Snapshot snapshot(std::initializer_list<std::string> ids) {
  Snapshot result;
  for (const auto &id : ids) { result.m_topology.push_back({id}); result.m_modes[id] = 60; }
  if (!ids.size()) return result;
  result.m_primary_device = *ids.begin();
  return result;
}
struct DisplayController {
  using layout_rotation_map_t = std::map<std::string, int>;
  struct LoadedSnapshot {
    Snapshot snapshot;
    int snapshot_version = 2;
    bool has_layout_data = false;
    layout_rotation_map_t layout_rotations;
  };
  static constexpr int snapshot_layout_version_latest = 2;
  Snapshot live;
  display_device::EnumeratedDeviceList devices;
  std::map<fs::path, LoadedSnapshot> files;
  std::vector<std::string> exclusions;
  layout_rotation_map_t live_rotations;
  std::vector<Snapshot> applied;
  mutable int captures = 0;
  std::function<void(int)> on_capture;
  bool enumeration_known = true;
  Snapshot snapshot() const {
    ++captures;
    if (on_capture) on_capture(captures);
    return live;
  }
  bool is_topology_valid(const Snapshot &s) const {
    std::set<std::string> ids;
    if (s.m_topology.empty()) return false;
    for (const auto &group : s.m_topology) {
      if (group.empty() || group.size() > 2) return false;
      for (const auto &id : group) if (id.empty() || !ids.insert(id).second) return false;
    }
    return true;
  }
  auto enumerate_nonempty_devices_known(display_device::DeviceEnumerationDetail) const
      -> std::optional<display_device::EnumeratedDeviceList> {
    if (!enumeration_known || devices.empty()) return std::nullopt;
    return devices;
  }
  auto snapshot_exclusions_copy() const { return exclusions; }
  auto missing_devices_for_topology(const display_device::ActiveTopology &topology) const {
    std::vector<std::string> missing;
    for (const auto &group : topology) for (const auto &id : group) {
      if (std::none_of(devices.begin(), devices.end(), [&](const auto &d) {
            return normalize_device_id(d.m_device_id) == normalize_device_id(id);
          })) missing.push_back(id);
    }
    return missing;
  }
  std::optional<LoadedSnapshot> load_display_settings_snapshot_with_metadata(const fs::path &path) const {
    const auto found = files.find(path);
    if (!fs::exists(path) || found == files.end()) return std::nullopt;
    return found->second;
  }
  std::optional<Snapshot> load_display_settings_snapshot(const fs::path &path) const {
    const auto loaded = load_display_settings_snapshot_with_metadata(path);
    if (!loaded) return std::nullopt;
    return loaded->snapshot;
  }
  bool validate_topology_with_os(const auto &) const { return true; }
  std::string signature(const Snapshot &) const { return "recorded"; }
  bool current_layout_matches(const layout_rotation_map_t &expected) const { return live_rotations == expected; }
  bool apply_snapshot(const Snapshot &s, const layout_rotation_map_t *rotations = nullptr) {
    applied.push_back(s); live = s;
    if (rotations) live_rotations = *rotations;
    return true;
  }
''' + controller_methods + r'''
};
struct ServiceState {
  DisplayController controller;
  fs::path session_current_path, session_previous_path, golden_path;
  std::atomic<bool> always_restore_from_golden {false}, prefer_golden_if_current_missing {true};
  std::atomic<bool> restore_requested {true}, restore_attempted_unconfirmed {false}, session_saved {true};
  std::atomic<bool> exit_after_revert {false};
  std::atomic<uint64_t> restore_cancel_generation {7};
  std::atomic<long long> last_session_restore_success_ms {0};
''' + flag_declaration + r'''
  int golden_attempts = 0, golden_fallbacks = 0;
  void reset_pending_golden_session_fallbacks() { golden_fallbacks = 0; }
  int note_pending_golden_session_fallback() { return ++golden_fallbacks; }
  void register_unresolved_golden_restore_request(const char *) {}
  void note_golden_restore_issue(const char *) {}
  bool apply_golden_and_confirm(std::stop_token, uint64_t) {
    ++golden_attempts;
    const auto loaded = controller.load_display_settings_snapshot_with_metadata(golden_path);
    if (!loaded) return false;
    controller.apply_snapshot(loaded->snapshot);
    clear_session_restore_snapshots_after_golden();
    return true;
  }
''' + service_methods + r'''
};
void configure_boot(ServiceState &state) {
''' + boot_assignment + r'''
}
std::string contents(const fs::path &p) {
  std::ifstream input(p);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
struct Fixture {
  ServiceState state;
  fs::path directory;
  Fixture(const fs::path &root, const std::string &name) : directory(root / name) {
    FixtureClock::value = {};
    fs::create_directories(directory);
    state.session_current_path = directory / "current.json";
    state.session_previous_path = directory / "previous.json";
    state.golden_path = directory / "golden.json";
    state.controller.live = snapshot({"A", "B", "C"});
    for (const auto &id : {"A", "B", "C", "DUMMY"}) {
      state.controller.devices.push_back({id, "DISPLAY_" + std::string(id), "Physical", {}, 1});
    }
  }
  void save(const fs::path &path, const Snapshot &s, const std::string &marker) {
    state.controller.files[path] = {s, 2, true, {{"B", 90}}};
    std::ofstream(path) << marker;
  }
  bool run() { return state.try_restore_once_if_valid({}, 7); }
};
void preserves_boot_history(const fs::path &root, bool previous) {
  Fixture f(root, previous ? "previous" : "current");
  configure_boot(f.state);
  f.save(f.state.session_current_path, previous ? snapshot({"MISSING"}) : snapshot({"B", "C"}), "current-marker");
  f.save(f.state.session_previous_path, previous ? snapshot({"B", "C"}) : snapshot({"MISSING"}), "previous-marker");
  const auto before = f.state.controller.live;
  assert(f.run());
  assert(f.state.controller.applied.empty());
  assert(f.state.controller.live == before);
  assert(contents(f.state.session_current_path) == "current-marker");
  assert(contents(f.state.session_previous_path) == "previous-marker");
}
void drift_or_cancel(const fs::path &root, bool cancel) {
  Fixture f(root, cancel ? "cancel" : "drift");
  configure_boot(f.state);
  f.save(f.state.session_current_path, snapshot({"B", "C"}), "current-marker");
  f.save(f.state.session_previous_path, snapshot({"MISSING"}), "previous-marker");
  f.state.controller.on_capture = [&](int capture) {
    if (cancel) f.state.restore_cancel_generation.store(8);
    else if (capture > 1) ++f.state.controller.live.m_modes["A"];
  };
  assert(!f.run());
  assert(f.state.controller.applied.empty());
  assert(contents(f.state.session_current_path) == "current-marker");
  assert(contents(f.state.session_previous_path) == "previous-marker");
}
void invalid_saved_topology(const fs::path &root) {
  Fixture f(root, "invalid_saved");
  configure_boot(f.state);
  auto invalid = snapshot({"B", "C"});
  invalid.m_topology.push_back({"B"});
  f.save(f.state.session_current_path, invalid, "current-marker");
  f.save(f.state.session_previous_path, snapshot({"MISSING"}), "previous-marker");
  assert(!f.run());
  assert(f.state.controller.applied.empty());
  assert(contents(f.state.session_current_path) == "current-marker");
  assert(contents(f.state.session_previous_path) == "previous-marker");
}
void exact_restore_control(const fs::path &root, const std::string &kind) {
  Fixture f(root, kind);
  configure_boot(f.state);
  auto baseline = snapshot({"B", "C"});
  if (kind == "ordinary" || kind == "exclusive") {
    f.state.preserve_active_physical_primary.store(false);
    // Ordinary IPC REVERT also sets this: it is not proof of boot recovery.
    f.state.exit_after_revert.store(true);
    if (kind == "exclusive") baseline = snapshot({"B"});
  } else if (kind == "golden" || kind == "golden_missing") {
    f.state.always_restore_from_golden.store(true);
    if (kind == "golden") f.save(f.state.golden_path, snapshot({"B"}), "golden-marker");
  } else if (kind == "excluded") {
    f.state.controller.exclusions = {" a "};
  } else if (kind == "primary_in_baseline") {
    baseline = snapshot({"A", "B"});
  } else if (kind == "dummy" || kind == "partial_baseline") {
    f.state.controller.live = kind == "dummy" ? snapshot({"DUMMY"}) : snapshot({"DUMMY", "B"});
    baseline = snapshot({"A", "B", "C"});
    for (auto &device : f.state.controller.devices) {
      if (device.m_device_id == "A" || device.m_device_id == "C") {
        device.m_display_name.clear(); device.m_info.reset();
      }
    }
  } else if (kind == "virtual" || kind == "virtual_primary") {
    f.state.controller.devices.push_back({"V", "DISPLAY_V", "Sunshine Virtual Display", {}, 1});
    f.state.controller.live.m_topology.push_back({"V"});
    f.state.controller.live.m_modes["V"] = 60;
    if (kind == "virtual_primary") f.state.controller.live.m_primary_device = "V";
  } else if (kind == "unknown_enumeration") {
    f.state.controller.enumeration_known = false;
  } else if (kind == "missing_primary_mode") {
    f.state.controller.live.m_modes.erase("A");
  } else if (kind == "inactive_primary") {
    auto &a = f.state.controller.devices.front(); a.m_display_name.clear(); a.m_info.reset();
  } else if (kind == "invalid_live") {
    f.state.controller.live.m_topology.clear();
  } else { assert(false && "unknown control"); }
  f.save(f.state.session_current_path, baseline, "current-marker");
  f.save(f.state.session_previous_path, snapshot({"MISSING"}), "previous-marker");
  assert(f.run());
  assert(!f.state.controller.applied.empty());
  assert(f.state.controller.live == (kind == "golden" ? snapshot({"B"}) : baseline));
  assert(!fs::exists(f.state.session_current_path));
  if (kind == "golden") assert(!fs::exists(f.state.session_previous_path));
  else assert(contents(f.state.session_previous_path) == "current-marker");
  assert(f.state.always_restore_from_golden.load() == (kind == "golden" || kind == "golden_missing"));
}
int main(int argc, char **argv) {
  assert(argc == 3);
  const fs::path root(argv[1]);
  const std::string name(argv[2]);
  if (name == "boot_current") preserves_boot_history(root, false);
  else if (name == "boot_previous") preserves_boot_history(root, true);
  else if (name == "drift") drift_or_cancel(root, false);
  else if (name == "cancel") drift_or_cancel(root, true);
  else if (name == "invalid_saved") invalid_saved_topology(root);
  else exact_restore_control(root, name);
  std::cout << name << ": pass\n";
}
'''

cases = ["boot_current", "boot_previous", "drift", "cancel", "invalid_saved", "ordinary", "exclusive",
         "golden", "golden_missing", "excluded", "primary_in_baseline", "dummy",
         "partial_baseline", "virtual", "virtual_primary", "unknown_enumeration",
         "missing_primary_mode", "inactive_primary", "invalid_live"]
if args.cases:
    selected = args.cases.split(",")
    if any(name not in cases for name in selected):
        parser.error("Unknown case")
    cases = selected
if args.emit_only:
    args.emit_only.write_text(program)
    args.emit_only.with_suffix(".json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Prepared {len(cases)} cases; no compiler or test process invoked")
else:
    with tempfile.TemporaryDirectory(prefix="legacy-boot-primary-") as temporary:
        directory = Path(temporary)
        source = directory / "fixture.cpp"
        source.write_text(program)
        binary = directory / "fixture"
        subprocess.run([args.compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pthread",
                        str(source), "-o", str(binary)], check=True)
        results = []
        for case in cases:
            result = subprocess.run([str(binary), str(directory), case], capture_output=True, text=True)
            results.append({"case": case, "exit": result.returncode,
                            "stdout": result.stdout, "stderr": result.stderr})
        print(json.dumps({"cases": len(results), "failures": sum(r["exit"] != 0 for r in results),
                          "results": results}, indent=2))
        raise SystemExit(any(result["exit"] != 0 for result in results))
