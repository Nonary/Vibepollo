"""Compile actual snapshot adapters against a recording display controller.

The Windows API boundary is modeled; no display or driver is accessed. Use
--prepare-only PATH to inspect the generated translation unit without compiling.
"""
import argparse
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
    brace = source.index("{", start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def program(root):
    v2 = (root / "src/platform/windows/display_helper_v2/win_display_settings.cpp").read_text()
    legacy = (root / "tools/display_settings_helper.cpp").read_text()
    adapters = "\n".join(function(v2, signature) for signature in [
        "bool WinDisplaySettings::apply_snapshot_settings(const Snapshot &snapshot)",
        "bool WinDisplaySettings::apply_snapshot_hdr_states(const Snapshot &snapshot)",
        "bool WinDisplaySettings::apply_snapshot_with_layouts(const Snapshot &snapshot, const codec::layout_rotation_map_t *layout_rotations)",
    ])
    legacy_adapter = function(legacy, "bool apply_snapshot(const display_device::DisplaySettingsSnapshot &snap, const layout_rotation_map_t *layout_rotations = nullptr)")
    return r'''
#include <algorithm>
#include <cassert>
#include <chrono>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "src/platform/windows/display_snapshot_restore.h"
using namespace std::chrono_literals;
namespace display_device {
  enum class DisplayRecoveryBehavior { Skip };
  struct DisplayRecoveryBehaviorGuard { explicit DisplayRecoveryBehaviorGuard(DisplayRecoveryBehavior) {} };
  struct DisplaySettingsSnapshot {
    std::vector<std::vector<std::string>> m_topology {{"A"}, {"B"}};
    std::map<std::string, int> m_modes {{"A", 120}, {"B", 60}};
    std::map<std::string, std::optional<bool>> m_hdr_states {{"A", false}, {"B", true}};
    std::string m_primary_device = "A";
    std::map<std::string, int> m_origins {{"A", 0}, {"B", 1920}};
  };
}
using Snapshot = display_device::DisplaySettingsSnapshot;
using HdrMap = decltype(Snapshot::m_hdr_states);
using Topology = decltype(Snapshot::m_topology);
using layout_rotation_map_t = std::map<std::string, int>;
namespace codec { using layout_rotation_map_t = ::layout_rotation_map_t; }
struct Log { template<class T> Log &operator<<(const T &) { return *this; } };
#define BOOST_LOG(level) Log{}
template<class T> std::string format_map_keys(const T &) { return {}; }
std::string format_topology(const Topology &) { return {}; }
enum class ApplyStatus { Ok, VerificationFailed };
struct Device {
  std::vector<std::string> calls;
  std::string failed, thrown;
  HdrMap hdr;
  Topology topology;
  bool ignore_hdr = false;
  bool call(const std::string &stage) {
    calls.push_back(stage);
    if (thrown == stage) throw std::runtime_error(stage);
    return failed != stage;
  }
  bool setTopology(const Topology &value) {
    if (!call("topology")) return false;
    topology = value;
    return true;
  }
  Topology getCurrentTopology() { return topology; }
  bool isTopologyTheSame(const Topology &a, const Topology &b) { return a == b; }
  bool setDisplayModesExactTemporary(const auto &) { return call("modes"); }
  bool setAsPrimary(const std::string &) { hdr.clear(); return call("primary"); }
  bool setDisplayOrigin(const std::string &id, int) { hdr.clear(); return call("origin:" + id); }
  bool setHdrStates(const HdrMap &states) {
    if (!call("hdr")) return false;
    if (!ignore_hdr) for (const auto &[id, state] : states) hdr[id] = state;
    return true;
  }
  HdrMap getCurrentHdrStates(const std::set<std::string> &) {
    return call("hdr-readback") ? hdr : HdrMap {};
  }
  bool rotate() { hdr.clear(); return call("rotation"); }
};
struct WinDisplaySettings {
  std::shared_ptr<Device> display_device_ = std::make_shared<Device>();
  bool initialized = true;
  bool ensure_initialized() { return initialized; }
  ApplyStatus apply_topology(const Topology &topology) {
    return display_device_->setTopology(topology) ? ApplyStatus::Ok : ApplyStatus::VerificationFailed;
  }
  bool apply_layout_rotations(const layout_rotation_map_t &) { return display_device_->rotate(); }
  bool apply_snapshot_settings(const Snapshot &);
  bool apply_snapshot_hdr_states(const Snapshot &);
  bool apply_snapshot_with_layouts(const Snapshot &, const layout_rotation_map_t *);
};
ADAPTERS
struct Legacy {
  std::shared_ptr<Device> m_dd = std::make_shared<Device>();
  bool initialized = true;
  bool ensure_initialized() { return initialized; }
  bool apply_layout_rotations(const layout_rotation_map_t &) { return m_dd->rotate(); }
LEGACY
};
int main() {
  int cases = 0;
  const layout_rotation_map_t rotations {{"A", 90}};
  for (const bool legacy : {false, true}) {
    auto apply = [&](Device &device, const Snapshot &snapshot, bool rotate = true, bool initialized = true) {
      auto shared = std::shared_ptr<Device>(&device, [](Device *) {});
      if (legacy) {
        Legacy adapter {shared, initialized};
        return adapter.apply_snapshot(snapshot, rotate ? &rotations : nullptr);
      }
      WinDisplaySettings adapter {shared, initialized};
      return adapter.apply_snapshot_with_layouts(snapshot, rotate ? &rotations : nullptr);
    };
    for (const bool rotate : {false, true}) {
      Device device;
      Snapshot snapshot;
      assert(apply(device, snapshot, rotate));
      auto expected = std::vector<std::string> {"topology", "modes", "primary", "origin:A", "origin:B"};
      if (rotate) expected.push_back("rotation");
      expected.insert(expected.end(), {"hdr", "hdr-readback"});
      assert(device.calls == expected);
      assert(device.hdr == snapshot.m_hdr_states);
      ++cases;
    }
    for (const auto &failure : {"topology", "modes", "primary", "origin:A", "origin:B", "rotation", "hdr", "hdr-readback"}) {
      Device device;
      device.failed = failure;
      assert(!apply(device, Snapshot {}));
      assert(device.calls.back() == failure);
      ++cases;
    }
    for (const auto &thrown : {"topology", "modes", "primary", "origin:A", "rotation", "hdr", "hdr-readback"}) {
      Device device;
      device.thrown = thrown;
      assert(!apply(device, Snapshot {}));
      assert(device.calls.back() == thrown);
      ++cases;
    }
    Device ineffective;
    ineffective.ignore_hdr = true;
    assert(!apply(ineffective, Snapshot {}));
    ++cases;
    Device uninitialized;
    assert(!apply(uninitialized, Snapshot {}, true, false));
    assert(uninitialized.calls.empty());
    ++cases;
    Snapshot unknown;
    unknown.m_hdr_states = {{"A", std::nullopt}};
    Device no_color;
    assert(apply(no_color, unknown));
    assert(no_color.calls.back() == "rotation");
    ++cases;
  }
  std::cout << cases << " recording-controller adapter cases passed\n";
}
'''.replace("ADAPTERS", adapters).replace("LEGACY", legacy_adapter)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repo", nargs="?", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--compiler", default="g++")
    parser.add_argument("--prepare-only", type=Path)
    args = parser.parse_args()
    root = args.repo.resolve()
    source = program(root)
    if args.prepare_only:
        args.prepare_only.parent.mkdir(parents=True, exist_ok=True)
        args.prepare_only.write_text(source)
        return
    with tempfile.TemporaryDirectory(prefix="display-snapshot-backend-") as temporary:
        directory = Path(temporary)
        cpp = directory / "test.cpp"
        cpp.write_text(source)
        binary = directory / "test"
        subprocess.run([args.compiler, "-std=c++23", "-Wall", "-Wextra", "-Werror", "-I", str(root), str(cpp), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
