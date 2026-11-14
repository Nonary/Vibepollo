"""Compile the production final recovery-validation chain with CPU-only leaves.

Usage: python3 tests/unit/test_display_recovery_confirmation.py [repo]
       [--compiler CXX] [--windows-source before.cpp]
       [--display-device-root PATH] [--include DEPENDENCY_INCLUDE ...]
       [--generate-only --output generated.cpp]

The Windows-source override is a negative control: it exercises the unchanged
final backend predicate against the same partial-snapshot regression cases.
Snapshot capture and initialization are controlled leaves. Snapshot types, mode
comparison, codec, snapshot service and validation operation are compiled from
production sources; no Windows display APIs or physical monitor claims are made.
"""
import argparse
import pathlib
import subprocess
import sys
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("repo", nargs="?", type=pathlib.Path,
                    default=pathlib.Path(__file__).resolve().parents[2])
parser.add_argument("--compiler", default="g++")
parser.add_argument("--windows-source", type=pathlib.Path)
parser.add_argument("--display-device-root", type=pathlib.Path)
parser.add_argument("--include", type=pathlib.Path, action="append", default=[],
                    help="Dependency include directory (JSON and Boost); repeat as needed")
parser.add_argument("--generate-only", action="store_true",
                    help="Write the production-wrapper fixture without compiling or running it")
parser.add_argument("--output", type=pathlib.Path,
                    help="Generated C++ output path (requires --generate-only)")
args = parser.parse_args()
if args.generate_only != (args.output is not None):
    parser.error("--generate-only and --output must be supplied together")
display_device = args.display_device_root or args.repo / "third-party/libdisplaydevice"
core = args.repo / "src/platform/windows/display_helper_v2"


def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


windows = (args.windows_source or core / "win_display_settings.cpp").read_text()

program = r'''
#include <algorithm>
#include <cassert>
#include <chrono>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include "src/platform/windows/display_helper_v2/operations.h"
#include "src/platform/windows/display_helper_v2/snapshot_codec.h"
namespace display_helper::v2 {
class WinDisplaySettings final : public IDisplaySettings {
public:
  Snapshot current;
  bool initialized = true, throw_capture = false;
  int captures = 0;
  bool ensure_initialized() { return initialized; }
  Snapshot capture_snapshot() override {
    ++captures;
    if (throw_capture) throw std::runtime_error("capture unavailable");
    return current;
  }
  ApplyStatus apply(const SingleDisplayConfiguration &) override { return ApplyStatus::Fatal; }
  ApplyStatus apply_topology(const ActiveTopology &) override { return ApplyStatus::Fatal; }
  EnumeratedDeviceList enumerate(display_device::DeviceEnumerationDetail) override { return {}; }
  ActiveTopology capture_topology() override { return current.m_topology; }
  bool validate_topology(const ActiveTopology &) override { return false; }
  bool apply_snapshot(const Snapshot &) override { return false; }
  bool configuration_matches(const SingleDisplayConfiguration &) override { return false; }
  bool set_display_origin(const std::string &, const display_device::Point &) override { return false; }
  bool is_topology_same(const ActiveTopology &, const ActiveTopology &) override { return false; }
  bool snapshot_matches_current(const Snapshot &snapshot) override;
};
WINDOWS
}  // namespace display_helper::v2
using namespace display_helper::v2;
namespace codec = display_helper::v2::codec;
class Clock final : public IClock {
public:
  std::function<void()> on_sleep;
  std::chrono::steady_clock::time_point now() override { return {}; }
  void sleep_for(std::chrono::milliseconds duration) override {
    assert(duration == std::chrono::milliseconds(250));
    if (on_sleep) on_sleep();
  }
};
int main() {
  WinDisplaySettings display;
  SnapshotService service(display);
  Clock clock;
  RecoveryValidationOperation validation(service, clock);
  CancellationSource cancellation;
  auto token = cancellation.token();
  Snapshot requested;
  requested.m_topology = {{"LG"}};
  requested.m_modes["LG"] = {display_device::Resolution {1920, 1080}, display_device::Rational {59997, 1000}};
  requested.m_hdr_states["LG"] = std::nullopt;
  requested.m_origins["LG"] = {0, 0};
  display.current = requested;
  display.current.m_hdr_states["LG"] = display_device::HdrState::Disabled;
  display.current.m_primary_device = "LG";
  const Snapshot observed = display.current;
  const auto passes = [&](const Snapshot &saved) { return validation.run(saved, token); };
  const auto check = [](bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
  };
  // Matches the original log: mode/origin are restored, while unknown HDR and
  // an absent saved primary are not settings that can be written or cleared.
  check(!codec::equal_snapshots_strict(observed, requested), "strict control must differ");
  check(passes(requested), "partial saved snapshot must complete final recovery validation");
  check(display.current.m_primary_device == "LG", "validation must not mutate primary");
  check(display.current.m_hdr_states.at("LG") == display_device::HdrState::Disabled, "validation must not mutate HDR");
  auto old_schema = requested;
  old_schema.m_origins.clear();
  check(passes(old_schema), "legacy omitted origins must be unrequested");
  auto no_hdr = requested;
  no_hdr.m_hdr_states.clear();
  check(passes(no_hdr), "omitted HDR map must be unrequested");
  auto required = requested;
  required.m_primary_device = "LG";
  required.m_hdr_states["LG"] = display_device::HdrState::Disabled;
  check(passes(required), "specified primary and HDR must still match");
  required.m_hdr_states["LG"] = display_device::HdrState::Enabled;
  check(!passes(required), "wrong specified HDR must fail");
  display.current.m_hdr_states["LG"] = std::nullopt;
  check(!passes(required), "unknown observed HDR must not satisfy specified HDR");
  display.current.m_hdr_states.clear();
  check(!passes(required), "missing observed HDR must fail");
  display.current = observed;
  required = requested;
  required.m_primary_device = "OTHER";
  check(!passes(required), "wrong specified primary must fail");
  display.current.m_primary_device.clear();
  required.m_primary_device = "LG";
  check(!passes(required), "missing specified primary must fail");
  display.current = observed;
  display.current.m_modes["LG"] = {display_device::Resolution {3840, 2160}, display_device::Rational {59997, 1000}};
  check(!passes(requested), "wrong requested mode must fail");
  display.current.m_modes.clear();
  check(!passes(requested), "missing mode must fail");
  display.current = observed;
  display.current.m_modes.at("LG").m_refresh_rate = display_device::Rational {119994, 2000};
  check(passes(requested), "equivalent rational refresh must match");
  display.current.m_modes.at("LG").m_refresh_rate = display_device::Rational {60, 1};
  check(!passes(requested), "different exact refresh frequency must fail");
  display.current.m_modes.at("LG").m_refresh_rate = display_device::Rational {59997, 0};
  check(!passes(requested), "invalid refresh denominator must fail");
  display.current = observed;
  display.current.m_topology = {{"LG", "TV"}};
  check(!passes(requested), "wrong topology must fail");
  display.current = observed;
  display.current.m_origins["LG"] = {1, 0};
  check(!passes(requested), "wrong requested origin must fail");
  display.current.m_origins.clear();
  check(!passes(requested), "missing requested origin must fail");
  display.current = observed;
  auto no_modes = requested;
  no_modes.m_modes.clear();
  check(!passes(no_modes), "empty saved modes must fail");
  auto multi = requested;
  multi.m_topology = {{"LG"}, {"TV", "RECEIVER"}};
  display.current.m_topology = {{"RECEIVER", "TV"}, {"LG"}};
  check(passes(multi), "topology enumeration order is irrelevant");
  display.current = observed;
  display.initialized = false;
  check(!passes(requested), "initialization failure must fail closed");
  display.initialized = true;
  display.throw_capture = true;
  check(!passes(requested), "capture exception must fail closed");
  display.throw_capture = false;
  int captures_before = display.captures;
  cancellation.cancel();
  check(!passes(requested), "cancelled validation must fail");
  check(display.captures == captures_before, "cancellation must avoid capture");
  token = cancellation.token();
  clock.on_sleep = [&] { cancellation.cancel(); };
  check(!passes(requested), "cancellation during settle must fail");
  check(display.captures == captures_before, "settle cancellation must avoid capture");
  token = cancellation.token();
  clock.on_sleep = [&] { display.current.m_modes["LG"] = {display_device::Resolution {3840, 2160}, display_device::Rational {59997, 1000}}; };
  check(!passes(requested), "mode changed during final settle must fail");
  std::cout << "production final recovery validation: passed\n";
}
'''
program = program.replace("WINDOWS", function(windows, "bool WinDisplaySettings::snapshot_matches_current("))
if args.generate_only:
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(program)
    sys.exit(0)
with tempfile.TemporaryDirectory(prefix="recovery-confirmation-") as temp:
    source = pathlib.Path(temp) / "test.cpp"
    binary = pathlib.Path(temp) / "test"
    source.write_text(program)
    sources = [core / f"{name}.cpp" for name in [
        "diagnostics", "text_storage", "topology_policy", "golden_health",
        "operations", "snapshot", "snapshot_codec"]]
    sources += [display_device / "src/windows/types.cpp"]
    sources += [display_device / f"src/common/{name}.cpp" for name in [
        "types", "json", "json_serializer", "logging"]]
    includes = [args.repo, args.repo / "tests/support/display_helper_core",
                display_device / "src/common/include", display_device / "src/windows/include",
                *args.include]
    subprocess.run([args.compiler, "-std=c++23", "-pthread", "-UNDEBUG",
                    "-DSUNSHINE_TEST_PORTABLE_WINDOWS_TYPES",
                    *[f"-I{path}" for path in includes], str(source),
                    *map(str, sources), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
