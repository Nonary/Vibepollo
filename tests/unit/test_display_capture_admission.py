"""Exercise the production startup boundary with delayed display configuration.

Compile the actual apply wrapper with deterministic helper/transport fakes so
Windows startup ordering and failure behavior can also be checked on Linux.
"""
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve()
compiler = sys.argv[2] if len(sys.argv) > 2 else "g++-15"
source = (root / "src/platform/windows/display_helper_integration.cpp").read_text()
start = source.index("  bool apply(\n")
brace = source.index("{", start)
depth, end = 1, brace + 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
apply = source[start:end]

# Compile the real launch/resume/WebRTC decision blocks too: APPLY must retain
# its honest failure result without turning a display preference into a rejected
# connection (in particular before the user can remotely unlock Windows).
nvhttp = (root / "src/nvhttp.cpp").read_text()
request_marker = "auto request = display_helper_integration::helpers::build_request_from_session(config::video, *launch_session);"
launch_start = nvhttp.rindex(request_marker, 0, nvhttp.index("// Wait for display setup"))
launch_end = nvhttp.index("// Apply a per-client HDR profile", launch_start)
launch_block = nvhttp[launch_start:launch_end]
resume_start = nvhttp.index(request_marker, launch_end)
resume_end = nvhttp.index("// Apply a per-client HDR profile", resume_start)
resume_block = nvhttp[resume_start:resume_end]
webrtc = (root / "src/webrtc_stream.cpp").read_text()
webrtc_start = webrtc.index(request_marker, webrtc.index("Display helper: applying WebRTC display request"))
webrtc_end = webrtc.index("        }\n#elif defined(__linux__)", webrtc_start)
webrtc_block = webrtc[webrtc_start:webrtc_end]
process = (root / "src/process.cpp").read_text()
skip_start = process.index("    const bool skip_display_revert = launch_session")
skip_end = process.index(";", skip_start) + 1
skip_revert = process[skip_start:skip_end]

program = r'''
#include <cassert>
#include <chrono>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#define BOOST_LOG(level) std::clog
using namespace std::chrono_literals;
namespace display_helper_integration {
  enum class ApplyRetryPolicy { Full, StreamStart };
  enum class ApplyVerificationStatus { Verified, Unknown, Failed };
  struct ApplyVerificationTicket {
    bool uses_v2_helper = false;
    std::chrono::steady_clock::time_point startup_deadline {};
  };
  struct Session {
    std::string virtual_display_device_id;
    bool virtual_display = true;
    bool display_config_preapplied = false;
    bool display_config_preapply_attempted = false;
  };
  struct Overrides { std::optional<std::string> device_id_override; };
  struct DisplayApplyRequest { Session *session = nullptr; Overrides session_overrides; };
  constexpr auto kStreamStartApplyVerificationTimeout = 100ms;
  constexpr auto kApplyVerificationTimeout = 100ms;
  namespace fake {
    bool applied = true, v2 = true, hdr = false, cancelled = false, has_request = true;
    bool cancel_on_verify = false;
    ApplyVerificationStatus verification = ApplyVerificationStatus::Verified;
    std::chrono::milliseconds blank_delay {0};
    std::vector<std::string> events;
    void reset() {
      applied = v2 = true;
      has_request = true;
      hdr = cancelled = cancel_on_verify = false;
      verification = ApplyVerificationStatus::Verified;
      blank_delay = 0ms;
      events.clear();
    }
  }
  namespace statefile { void remember_virtual_display_device(const std::string &) {} }
  bool cancellation_requested(const std::function<bool()> &cancelled) {
    return cancelled && cancelled();
  }
  std::mutex &pending_apply_execution_mutex() { static std::mutex mutex; return mutex; }
  bool lock_pending_apply_execution(std::unique_lock<std::mutex> &lock,
      const std::function<bool()> &cancelled, std::chrono::steady_clock::time_point) {
    if (cancelled()) return false;
    lock.lock();
    return true;
  }
  void clear_pending_apply_queue_locked() {}
  bool apply_internal(const DisplayApplyRequest &, bool, ApplyVerificationTicket *ticket,
      const std::function<bool()> &, ApplyRetryPolicy, std::chrono::steady_clock::time_point deadline, bool) {
    fake::events.emplace_back("apply");
    fake::hdr = false;
    std::this_thread::sleep_for(fake::blank_delay);
    fake::hdr = true;
    fake::events.emplace_back("hdr-restored");
    if (ticket) { ticket->uses_v2_helper = fake::v2; ticket->startup_deadline = deadline; }
    return fake::applied;
  }
  ApplyVerificationStatus wait_for_apply_verification(const ApplyVerificationTicket &ticket,
      std::chrono::milliseconds) {
    assert(fake::hdr);
    fake::events.emplace_back("verify");
    if (fake::cancel_on_verify) fake::cancelled = true;
    if (std::chrono::steady_clock::now() >= ticket.startup_deadline) return ApplyVerificationStatus::Unknown;
    return fake::verification;
  }
  namespace helpers {
    std::optional<DisplayApplyRequest> build_request_from_session(int, Session &session) {
      if (!fake::has_request) return std::nullopt;
      return DisplayApplyRequest {&session, {}};
    }
  }
  bool apply(const DisplayApplyRequest &, ApplyVerificationTicket *, std::function<bool()>,
      ApplyRetryPolicy, std::chrono::steady_clock::time_point = {}, bool = false);
'''
program += apply + r'''
}
namespace config { int video = 0; }
struct Tree {
  template<class T> void put(const char *, const T &) { assert(false && "display setup rejected the stream"); }
};
'''
for name, block in (("launch", launch_block), ("resume", resume_block), ("webrtc", webrtc_block)):
    result_type = "std::optional<std::string>" if name == "webrtc" else "void"
    program += f"{result_type} {name}_startup(display_helper_integration::Session *launch_session) {{\n"
    program += r'''
  Tree tree;
  const auto display_startup_deadline = std::chrono::steady_clock::now() + 100ms;
  const std::function<bool()> display_startup_cancelled = [] { return display_helper_integration::fake::cancelled; };
'''
    program += block
    program += '  display_helper_integration::fake::events.emplace_back("capture");\n'
    if name == "webrtc":
        program += "  return std::nullopt;\n"
    program += "}\n"
program += "bool preserves_startup_setup(display_helper_integration::Session *launch_session) {\n"
program += skip_revert + "\nreturn skip_display_revert;\n}\n"
program += r'''
int main() {
  using namespace display_helper_integration;
  const DisplayApplyRequest request;
  auto start_stream = [&](std::chrono::steady_clock::time_point deadline = {}) {
    if (!apply(request, nullptr, [] { return fake::cancelled; }, ApplyRetryPolicy::StreamStart, deadline, false)) return false;
    fake::events.emplace_back("probe");
    assert(fake::hdr);
    fake::events.emplace_back("capture");
    return true;
  };
  // The original race: the helper temporarily switches HDR off during setup.
  fake::reset();
  fake::blank_delay = 20ms;
  assert(start_stream());
  assert((fake::events == std::vector<std::string>{"apply", "hdr-restored", "verify", "probe", "capture"}));
  // APPLY reports setup failure honestly; callers still admit the stream below.
  for (auto status : {ApplyVerificationStatus::Unknown, ApplyVerificationStatus::Failed}) {
    fake::reset(); fake::verification = status;
    assert(!start_stream());
    assert(fake::events.back() == "verify");
  }
  fake::reset(); fake::applied = false;
  assert(!start_stream());
  assert(fake::events.size() == 2);
  // The synchronous legacy result already covers HDR blanking and verification.
  fake::reset(); fake::v2 = false; fake::blank_delay = 20ms;
  assert(start_stream());
  assert((fake::events == std::vector<std::string>{"apply", "hdr-restored", "probe", "capture"}));
  // Completion outside the caller's budget is not a successful startup.
  fake::reset(); fake::v2 = false; fake::blank_delay = 10ms;
  assert(!start_stream(std::chrono::steady_clock::now() + 1ms));
  fake::reset(); fake::cancelled = true;
  assert(!start_stream()); assert(fake::events.empty());
  fake::reset(); fake::cancel_on_verify = true;
  assert(!start_stream()); assert(fake::events.back() == "verify");
  fake::reset();
  assert(!start_stream(std::chrono::steady_clock::now() - 1ms));
  assert(fake::events.empty());
  // Recovery and other Full callers still own their separate verification.
  fake::reset(); fake::verification = ApplyVerificationStatus::Failed;
  assert(apply(request, nullptr, {}, ApplyRetryPolicy::Full, {}, false));
  assert(fake::events.size() == 2);
  // Existing callers requesting a ticket receive the successful capture proof.
  fake::reset(); ApplyVerificationTicket ticket;
  assert(apply(request, &ticket, {}, ApplyRetryPolicy::StreamStart, {}, false));
  assert(ticket.uses_v2_helper);
  // Reboot/login-screen failures, verification failures/timeouts, and missing
  // requests all reach capture through every real startup decision block.
  for (int scenario = 0; scenario < 6; ++scenario) {
    for (int path = 0; path < 3; ++path) {
      fake::reset();
      if (scenario == 1) fake::applied = false; // lock-screen deferred APPLY
      if (scenario == 2) fake::verification = ApplyVerificationStatus::Failed;
      if (scenario == 3) fake::verification = ApplyVerificationStatus::Unknown;
      if (scenario == 4) fake::has_request = false;
      if (scenario == 5) fake::blank_delay = 110ms; // setup exceeds startup budget
      Session session;
      if (path == 0) launch_startup(&session);
      if (path == 1) resume_startup(&session);
      if (path == 2) assert(!webrtc_startup(&session));
      assert(fake::events.back() == "capture");
      if (path == 0) {
        assert(session.display_config_preapplied == (scenario == 0));
        assert(preserves_startup_setup(&session));
      }
    }
  }
  Session idle_session;
  assert(!preserves_startup_setup(&idle_session));
  assert(!preserves_startup_setup(nullptr));
}
'''
with tempfile.TemporaryDirectory(prefix="vibeshine-capture-admission-") as temp:
    path = pathlib.Path(temp)
    fixture = path / "admission.cpp"
    binary = path / "admission"
    fixture.write_text(program)
    subprocess.run([compiler, "-std=c++23", "-pthread", str(fixture), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("Display capture admission scenarios passed")

# Exercise the actual background monitor rather than a duplicate color policy.
# Replace only DXGI enumeration and Windows metrics; retain the worker lifecycle.
monitor_header = (root / "src/platform/windows/display_output_monitor.h").read_text()
monitor_header = monitor_header.replace("#pragma once\n", "")
monitor_header = monitor_header.replace("#include <windows.h>\n", "").replace("#include <dxgi1_6.h>\n", "")
monitor_source = (root / "src/platform/windows/display_output_monitor.cpp").read_text()
monitor_methods = monitor_source[monitor_source.index("  display_output_monitor_t::display_output_monitor_t"):]
monitor_program = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <memory>
#include <optional>
#include <thread>
#include "src/platform/windows/wgc_capture_policy.h"
#define BOOST_LOG(level) std::clog
using namespace std::chrono_literals;
struct LUID {};
struct RECT { int left, top, right, bottom; };
struct DXGI_OUTPUT_DESC {
  wchar_t DeviceName[32] {};
  int Monitor = 1, Rotation = 0;
  bool AttachedToDesktop = true;
  RECT DesktopCoordinates {0, 0, 1920, 1080};
};
enum { SM_XVIRTUALSCREEN, SM_YVIRTUALSCREEN, SM_CXVIRTUALSCREEN, SM_CYVIRTUALSCREEN };
namespace fake {
  std::atomic_bool hdr {false}, available {true};
  std::atomic_int revision {0}, samples {0}, width {1920};
  void reset(bool initial_hdr) {
    hdr = initial_hdr; available = true; revision = samples = 0; width = 1920;
  }
}
int GetSystemMetrics(int metric) {
  if (metric == SM_CXVIRTUALSCREEN) return fake::width;
  if (metric == SM_CYVIRTUALSCREEN) return 1080;
  return 0;
}
'''
monitor_program += monitor_header + r'''
namespace platf::dxgi {
  struct Factory {
    int revision;
    bool IsCurrent() const { return revision == fake::revision; }
  };
  struct output_sample_t {
    std::shared_ptr<Factory> factory;
    DXGI_OUTPUT_DESC desc {};
    bool hdr_valid = true, hdr = false;
  };
  std::optional<output_sample_t> sample_output(LUID, const wchar_t *) {
    ++fake::samples;
    if (!fake::available) return std::nullopt;
    return output_sample_t {std::make_shared<Factory>(Factory {fake::revision}), {}, true, fake::hdr};
  }
'''
monitor_program += monitor_methods + r'''
template<class Predicate> void await(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!predicate() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
  assert(predicate());
}
int main() {
  using platf::dxgi::display_output_monitor_t;
  // Both directions follow the real display state, including 10-bit SDR sources.
  for (bool initial_hdr : {false, true}) {
    fake::reset(initial_hdr);
    display_output_monitor_t monitor({}, {}, true, initial_hdr, 0, 0, 1920, 1080);
    await([] { return fake::samples > 0; });
    assert(!monitor.reinit_requested());
    fake::hdr = !initial_hdr; ++fake::revision;
    await([&] { return monitor.reinit_requested(); });
  }
  // A topology notification with the same source color needs no frame hold/reinit.
  fake::reset(true);
  {
    display_output_monitor_t monitor({}, {}, true, true, 0, 0, 1920, 1080);
    await([] { return fake::samples > 0; });
    ++fake::revision;
    await([] { return fake::samples > 1; });
    assert(!monitor.reinit_requested());
    fake::available = false; ++fake::revision;
    await([&] { return monitor.reinit_requested(); });
  }
  // Neighbouring output changes still refresh absolute-input coordinates.
  fake::reset(false);
  {
    display_output_monitor_t monitor({}, {}, true, false, 0, 0, 1920, 1080);
    await([] { return fake::samples > 0; });
    fake::width = 2560;
    await([&] { return monitor.reinit_requested(); });
  }
}
'''
with tempfile.TemporaryDirectory(prefix="vibeshine-color-monitor-") as temp:
    path = pathlib.Path(temp)
    fixture = path / "monitor.cpp"
    binary = path / "monitor"
    fixture.write_text(monitor_program)
    subprocess.run([compiler, "-std=c++23", "-pthread", "-I", str(root), str(fixture), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("Display monitor HDR/SDR reinitialization scenarios passed")
