"""Execute the production Windows VDISPLAY recovery facade with fake backends."""
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve()
compiler = sys.argv[2]
source = (root / "src/platform/windows/virtual_display.cpp").read_text()
facade_start = source.index("  std::vector<TrackedDisplayCleanupTarget> tracked_display_cleanup_targets() {", source.index("namespace VDISPLAY {", source.index("namespace {")))

def extract(signature, start):
    begin = source.index(signature, start)
    brace = source.index("{", begin)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[begin:end]

functions = [
    extract("  std::vector<TrackedDisplayCleanupTarget> tracked_display_cleanup_targets() {", facade_start),
    extract("  bool tracked_display_cleanup_target_matches(const TrackedDisplayCleanupTarget &target) {", facade_start),
    extract("  bool remove_tracked_display_cleanup_target(const TrackedDisplayCleanupTarget &target) {", facade_start),
    extract("  std::optional<bool> is_any_managed_virtual_display_output(const std::string &output_identifier) {", facade_start),
]
assert all("use_sunshine_driver" not in function for function in functions)

for backend_path, backend_name in [
    ("src/platform/windows/virtual_display_sunshine.cpp", "sunshine"),
    ("src/platform/windows/virtual_display_sudovda.cpp", "sudovda"),
]:
    backend_source = (root / backend_path).read_text()
    remove_start = backend_source.index(
        "bool remove_tracked_display_cleanup_target(const VDISPLAY::TrackedDisplayCleanupTarget &target)"
    )
    remove_body_start = backend_source.index("{", remove_start)
    depth, remove_end = 1, remove_body_start + 1
    while depth:
        depth += (backend_source[remove_end] == "{") - (backend_source[remove_end] == "}")
        remove_end += 1
    remove_body = backend_source[remove_start:remove_end]
    assert f"ensure_display_backend_e::{backend_name}" in remove_body
    assert "std::lock_guard<std::recursive_mutex> operation_lock" in remove_body
    assert "tracked_display_cleanup_target_matches(target.guid_bytes, target.device_id)" in remove_body
    if backend_name == "sudovda":
        assert "return remove_virtual_display_impl(guid, true, {}, false);" in remove_body
    else:
        assert "return removeVirtualDisplay(guid);" in remove_body

program = r'''
#include <array>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <boost/algorithm/string/predicate.hpp>
enum class ensure_display_backend_e : std::uint8_t { none, sunshine, sudovda };
struct TrackedDisplayCleanupTarget {
  std::array<std::uint8_t, 16> guid_bytes{};
  std::string device_id;
  ensure_display_backend_e backend = ensure_display_backend_e::none;
};
namespace fake {
  bool selected_sunshine = true;
  unsigned sunshine_matches = 0, sudovda_matches = 0;
  unsigned sunshine_removals = 0, sudovda_removals = 0;
  std::optional<bool> sunshine_classification = false;
  std::optional<bool> sudovda_classification = false;
  std::vector<TrackedDisplayCleanupTarget> sunshine_targets, sudovda_targets;
  bool exact_match(const TrackedDisplayCleanupTarget &target, ensure_display_backend_e backend) {
    return target.backend == backend && !target.device_id.empty() && target.guid_bytes[0] == 42;
  }
}
namespace VDISPLAY_SUNSHINE {
  std::vector<TrackedDisplayCleanupTarget> tracked_display_cleanup_targets() { return fake::sunshine_targets; }
  bool tracked_display_cleanup_target_matches(const std::array<std::uint8_t, 16> &guid, const std::string &id) {
    ++fake::sunshine_matches;
    return !id.empty() && guid[0] == 42;
  }
  bool remove_tracked_display_cleanup_target(const TrackedDisplayCleanupTarget &target) {
    if (target.backend != ensure_display_backend_e::sunshine ||
        !tracked_display_cleanup_target_matches(target.guid_bytes, target.device_id)) return false;
    ++fake::sunshine_removals; return true;
  }
  std::optional<bool> classify_virtual_display_output(const std::string &) { return fake::sunshine_classification; }
}
namespace VDISPLAY_SUDOVDA {
  std::vector<TrackedDisplayCleanupTarget> tracked_display_cleanup_targets() { return fake::sudovda_targets; }
  bool tracked_display_cleanup_target_matches(const std::array<std::uint8_t, 16> &guid, const std::string &id) {
    ++fake::sudovda_matches;
    return !id.empty() && guid[0] == 42;
  }
  bool remove_tracked_display_cleanup_target(const TrackedDisplayCleanupTarget &target) {
    if (target.backend != ensure_display_backend_e::sudovda ||
        !tracked_display_cleanup_target_matches(target.guid_bytes, target.device_id)) return false;
    ++fake::sudovda_removals; return true;
  }
  std::optional<bool> classify_virtual_display_output(const std::string &) { return fake::sudovda_classification; }
}
namespace VDISPLAY {
''' + "\n".join(functions) + r'''
}
int main() {
  TrackedDisplayCleanupTarget sunshine, sudovda;
  sunshine.guid_bytes[0] = sudovda.guid_bytes[0] = 42;
  sunshine.device_id = sudovda.device_id = "same-device-id";
  sunshine.backend = ensure_display_backend_e::sunshine;
  sudovda.backend = ensure_display_backend_e::sudovda;
  fake::sunshine_targets = {sunshine}; fake::sudovda_targets = {sudovda};

  const auto targets = VDISPLAY::tracked_display_cleanup_targets();
  assert(targets.size() == 2);
  assert(targets[0].backend == ensure_display_backend_e::sunshine);
  assert(targets[1].backend == ensure_display_backend_e::sudovda);

  // Selection may change after matching; removal remains pinned to the
  // captured backend and does its own exact identity revalidation there.
  fake::selected_sunshine = true;
  assert(VDISPLAY::tracked_display_cleanup_target_matches(targets[0]));
  fake::selected_sunshine = false;
  assert(VDISPLAY::remove_tracked_display_cleanup_target(targets[0]));
  assert(fake::sunshine_removals == 1 && fake::sudovda_removals == 0);
  assert(VDISPLAY::remove_tracked_display_cleanup_target(targets[1]));
  assert(fake::sunshine_removals == 1 && fake::sudovda_removals == 1);

  auto unpinned = sunshine;
  unpinned.backend = ensure_display_backend_e::none;
  assert(!VDISPLAY::tracked_display_cleanup_target_matches(unpinned));
  assert(!VDISPLAY::remove_tracked_display_cleanup_target(unpinned));

  // An opposite-driver virtual path remains virtual regardless of the active
  // config selection, and unknown/incomplete classifications fail closed.
  fake::sunshine_classification = false;
  fake::sudovda_classification = true;
  fake::selected_sunshine = true;
  assert(VDISPLAY::is_any_managed_virtual_display_output("opposite-driver").value());
  fake::sudovda_classification = false;
  assert(VDISPLAY::is_any_managed_virtual_display_output("same-device-id").value());
  fake::sudovda_classification = false;
  assert(VDISPLAY::is_any_managed_virtual_display_output("known-physical").has_value());
  assert(!*VDISPLAY::is_any_managed_virtual_display_output("known-physical"));
  fake::sunshine_classification = std::nullopt;
  assert(!VDISPLAY::is_any_managed_virtual_display_output("unresolved").has_value());
}
'''
with tempfile.TemporaryDirectory(prefix="wake-recovery-backend-") as directory:
    generated = pathlib.Path(directory) / "test.cpp"
    binary = pathlib.Path(directory) / "test"
    generated.write_text(program)
    subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pthread",
                    "-I", str(root), str(generated), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)


def extract_backend_classifier(path):
    backend_source = (root / path).read_text()
    signature = "std::optional<bool> classify_virtual_display_output(const std::string &output_identifier)"
    begin = backend_source.index(signature)
    brace = backend_source.index("{", begin)
    depth, end = 1, brace + 1
    while depth:
        depth += (backend_source[end] == "{") - (backend_source[end] == "}")
        end += 1
    return backend_source[begin:end]


sunshine_classifier = extract_backend_classifier("src/platform/windows/virtual_display_sunshine.cpp")
sudovda_classifier = extract_backend_classifier("src/platform/windows/virtual_display_sudovda.cpp")
classifier_program = r'''
#include <cassert>
#include <optional>
#include <string>
#include <vector>
namespace display_device {
  enum class DeviceEnumerationDetail { Minimal };
  struct Edid {};
  struct Device {
    std::string m_device_id, m_display_name, m_monitor_device_path, m_friendly_name;
    std::optional<Edid> m_edid;
    bool sunshine_managed = false, sudovda_managed = false;
  };
}
namespace fake {
  std::optional<std::vector<display_device::Device>> devices = std::vector<display_device::Device>{};
}
namespace platf::display_helper {
  struct Coordinator {
    static Coordinator &instance() { static Coordinator value; return value; }
    std::optional<std::vector<display_device::Device>> enumerate_devices(display_device::DeviceEnumerationDetail) {
      return fake::devices;
    }
  };
}
namespace VDISPLAY_SUNSHINE {
  bool equals_ci(const std::string &left, const std::string &right) { return left == right; }
  bool is_virtual_display_device(const display_device::Device &device) { return device.sunshine_managed; }
''' + sunshine_classifier + r'''
}
namespace VDISPLAY_SUDOVDA {
  bool equals_ci(const std::string &left, const std::string &right) { return left == right; }
  bool is_virtual_display_device(const display_device::Device &device) { return device.sudovda_managed; }
''' + sudovda_classifier + r'''
}
int main() {
  using display_device::Device;
  using display_device::Edid;
  fake::devices = std::vector<Device>{{"physical-a", "DISPLAY1", "\\\\?\\monitor", "Physical display", Edid{}, false, false},
                                      {"sun-vd", "SUN", "\\\\?\\sun", "Sunshine display", Edid{}, true, false},
                                      {"sudo-vd", "SUDO", "\\\\?\\sudo", "SudoVDA display", Edid{}, false, true},
                                      {"metadata-missing", "", "", "", {}, false, false}};
  assert(VDISPLAY_SUNSHINE::classify_virtual_display_output("physical-a") == false);
  assert(VDISPLAY_SUDOVDA::classify_virtual_display_output("physical-a") == false);
  assert(VDISPLAY_SUNSHINE::classify_virtual_display_output("sun-vd") == true);
  assert(VDISPLAY_SUDOVDA::classify_virtual_display_output("sun-vd") == false);
  assert(VDISPLAY_SUNSHINE::classify_virtual_display_output("sudo-vd") == false);
  assert(VDISPLAY_SUDOVDA::classify_virtual_display_output("sudo-vd") == true);
  assert(!VDISPLAY_SUNSHINE::classify_virtual_display_output("metadata-missing").has_value());
  assert(!VDISPLAY_SUDOVDA::classify_virtual_display_output("metadata-missing").has_value());
  assert(!VDISPLAY_SUNSHINE::classify_virtual_display_output("not-enumerated").has_value());
  assert(!VDISPLAY_SUDOVDA::classify_virtual_display_output("").has_value());
  const auto known_physical = fake::devices->front();
  for (unsigned missing = 1; missing < 8; ++missing) {
    auto partial = known_physical;
    if (missing & 1) partial.m_monitor_device_path.clear();
    if (missing & 2) partial.m_friendly_name.clear();
    if (missing & 4) partial.m_edid.reset();
    fake::devices = std::vector<Device>{partial};
    assert(!VDISPLAY_SUNSHINE::classify_virtual_display_output("physical-a").has_value());
    assert(!VDISPLAY_SUDOVDA::classify_virtual_display_output("physical-a").has_value());
  }
  fake::devices = std::vector<Device>{{"sun-vd", "SUN", "", "", {}, true, false}};
  assert(VDISPLAY_SUNSHINE::classify_virtual_display_output("sun-vd") == true);
  fake::devices = std::nullopt;
  assert(!VDISPLAY_SUNSHINE::classify_virtual_display_output("physical-a").has_value());
  assert(!VDISPLAY_SUDOVDA::classify_virtual_display_output("physical-a").has_value());
}
'''
with tempfile.TemporaryDirectory(prefix="wake-recovery-classifier-") as directory:
    generated = pathlib.Path(directory) / "test.cpp"
    binary = pathlib.Path(directory) / "test"
    generated.write_text(classifier_program)
    subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pthread",
                    "-I", str(root), str(generated), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)


backend_source = (root / "src/platform/windows/virtual_display_sudovda.cpp").read_text()
open_signature = (
    "static DRIVER_STATUS open_vdisplay_device_impl(\n"
    "    std::stop_token stop_token,\n"
    "    bool allow_reinstall,\n"
    "    bool allow_driver_recovery)"
)
open_begin = backend_source.index(open_signature)
open_brace = backend_source.index("{", open_begin)
depth, open_end = 1, open_brace + 1
while depth:
    depth += (backend_source[open_end] == "{") - (backend_source[open_end] == "}")
    open_end += 1
open_impl = backend_source[open_begin:open_end]
assert "allow_driver_recovery &&" in open_impl
open_program = r'''
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <optional>
#include <stop_token>
namespace VDISPLAY_SUDOVDA {
  enum class DRIVER_STATUS { UNKNOWN, OK, FAILED, VERSION_INCOMPATIBLE };
  enum class RestartCooldownBehavior { wait };
  using HANDLE = int;
  constexpr HANDLE INVALID_HANDLE_VALUE = -1;
  HANDLE SUDOVDA_DRIVER_HANDLE = INVALID_HANDLE_VALUE;
  int SUVDA_INTERFACE_GUID = 0;
  uint64_t g_driver_handle_generation = 0;
  std::optional<int> g_current_render_adapter_request;
  std::recursive_mutex g_virtual_display_operation_mutex;
  int open_calls = 0, repair_calls = 0, succeed_on_open = 0;
  HANDLE OpenDevice(int *) {
    ++open_calls;
    return succeed_on_open != 0 && open_calls >= succeed_on_open ? 7 : INVALID_HANDLE_VALUE;
  }
  bool driver_handle_responsive(HANDLE) { return true; }
  void closeVDisplayDevice() { SUDOVDA_DRIVER_HANDLE = INVALID_HANDLE_VALUE; }
  bool wait_for_monitor_stop(std::stop_token, std::chrono::milliseconds) { return false; }
  bool ensure_driver_is_ready_impl(RestartCooldownBehavior, std::stop_token, bool) {
    ++repair_calls;
    return true;
  }
  bool CheckProtocolCompatible(HANDLE) { return true; }
''' + open_impl + r'''
}
int main() {
  using namespace VDISPLAY_SUDOVDA;
  // The automatic passive reopen exhausts its bounded transport retries and
  // returns without trying adapter recovery/reinstall.
  assert(open_vdisplay_device_impl({}, false, false) == DRIVER_STATUS::FAILED);
  assert(open_calls == 6 && repair_calls == 0);

  // Ordinary/default removal retains the established recovery path. Once the
  // fake adapter repair succeeds, a subsequent passive open can complete.
  open_calls = 0;
  repair_calls = 0;
  succeed_on_open = 7;
  assert(open_vdisplay_device_impl({}, true, true) == DRIVER_STATUS::OK);
  assert(repair_calls == 1 && open_calls == 7);
}
'''
with tempfile.TemporaryDirectory(prefix="wake-recovery-sudovda-open-") as directory:
    generated = pathlib.Path(directory) / "test.cpp"
    binary = pathlib.Path(directory) / "test"
    generated.write_text(open_program)
    subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pthread",
                    str(generated), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
