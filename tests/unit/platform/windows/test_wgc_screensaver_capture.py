#!/usr/bin/env python3
"""Replay production WGC factories/recovery branches with inert Windows/GPU boundaries."""
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve()
compiler = sys.argv[2] if len(sys.argv) > 2 else "c++"
source = (root / "src/platform/windows/display_wgc.cpp").read_text()
header = (root / "src/platform/windows/display.h").read_text()

# These fakes cannot load the Windows SDK; retain its Winsock include contract.
for text in (source, header):
    winsock = text.index("#include <winsock2.h>")
    for sdk in ("d3d11.h", "d3d11_4.h", "dxgi1_2.h", "dwmapi.h", "winrt/windows.graphics.capture.h"):
        include = "#include <" + sdk + ">"
        if include in text:
            assert winsock < text.index(include), "Winsock must precede Windows SDK/WinRT headers"


def block(text, marker):
    start = text.index(marker)
    opening = text.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


code = r'''
#include <cassert>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include "src/platform/windows/wgc_screensaver_recovery.h"
using namespace std::chrono_literals;
using clock_type = std::chrono::steady_clock;
clock_type::time_point fake_now {};
struct FakeClock { using time_point = clock_type::time_point; static auto now() { return fake_now; } };
void at(std::chrono::milliseconds time) { fake_now = clock_type::time_point(time); }
bool secure = false, grace = false, dxgi_init_ok = true, wgc_init_ok = true;
int dxgi_inits = 0, wgc_inits = 0, base_snapshots = 0, spi_queries = 0;
std::optional<bool> saver = false;
std::ostringstream logs;
#define BOOST_LOG(level) logs
struct LUID {};
namespace video { struct config_t {}; }
namespace platf::dxgi {
  bool is_secure_desktop_active() { return secure; }
}
namespace replay {
namespace std {
  using namespace ::std;
  namespace chrono { using namespace ::std::chrono; using steady_clock = ::FakeClock; }
}
namespace wgc_screensaver = ::platf::dxgi::wgc_screensaver;
std::optional<bool> query_screen_saver_running() { ++spi_queries; return saver; }
bool is_secure_desktop_active() { return secure; }
bool recent_wgc_desktop_switch_grace_active() { return grace; }
struct adapter_luid_override_guard { explicit adapter_luid_override_guard(std::optional<LUID>) {} };
std::optional<LUID> get_last_wgc_adapter_luid() { return std::nullopt; }
enum class capture_e { ok, timeout, reinit, error, interrupted };
capture_e base_status = capture_e::ok;
using pull_free_image_cb_t = int;
namespace platf { struct img_t {}; namespace dxgi { using ::platf::dxgi::is_secure_desktop_active; } }
struct display_t { bool dxgi = false; virtual ~display_t() = default; };
struct display_ddup_vram_t : display_t {
  int init(const ::video::config_t &, const std::string &, const std::optional<LUID> &) {
    dxgi = true; ++dxgi_inits; return dxgi_init_ok ? 0 : 1;
  }
  virtual capture_e snapshot(const pull_free_image_cb_t &, std::shared_ptr<platf::img_t> &, std::chrono::milliseconds, bool) {
    ++base_snapshots; return base_status;
  }
};
using display_ddup_ram_t = display_ddup_vram_t;
'''
for mode in ("vram", "ram"):
    code += block(header, f"  class temp_dxgi_{mode}_t:") + ";\n"
    code += f'''
struct display_wgc_ipc_{mode}_t : display_t {{
  int init(const ::video::config_t &, const std::string &, const std::optional<LUID> &) {{
    ++wgc_inits; return wgc_init_ok ? 0 : 1;
  }}
  static std::shared_ptr<display_t> create(const ::video::config_t &, const std::string &, const std::optional<LUID> &);
}};
'''
code += block(source, "    struct wgc_dxgi_fallback_state_t") + ";\n"
for marker in (
    "    wgc_screensaver::recovery_t &screensaver_recovery_for(",
    "    wgc_screensaver::recovery_t &poll_screensaver(",
    "    bool screensaver_requires_wgc_reinit(",
    "    std::optional<wgc_dxgi_fallback_state_t> get_wgc_dxgi_fallback_state(",
    "    void log_wgc_dxgi_fallback_reason(",
):
    code += block(source, marker) + "\n"
for mode in ("vram", "ram"):
    code += block(source, f"  std::shared_ptr<display_t> display_wgc_ipc_{mode}_t::create(") + "\n"
    code += block(source, f"  capture_e temp_dxgi_{mode}_t::snapshot(") + "\n"
code += r'''
template<class Wgc, class Dxgi> void run(const std::string &prefix) {
  ::video::config_t config;
  std::shared_ptr<platf::img_t> image;
  const auto make = [&](const std::string &output) { return Wgc::create(config, output, std::nullopt); };
  // Static output, timeouts, normal desktop event churn: no saver evidence.
  secure = grace = false; saver = false; at(0ms);
  auto normal = make(prefix + "normal"); assert(normal && !normal->dxgi);
  for (int i = 1; i < 100; ++i) {
    at(i * 500ms); saver = i % 2 ? std::optional<bool>(false) : std::nullopt;
    assert(!screensaver_requires_wgc_reinit(prefix + "normal"));
  }
  // Startup while saver runs, then confirmed dismissal and fresh WGC once.
  saver = true; at(0ms); auto active = make(prefix + "active");
  assert(active && active->dxgi);
  auto *dxgi = dynamic_cast<Dxgi *>(active.get()); assert(dxgi);
  saver = false; at(500ms); assert(dxgi->snapshot(0, image, 1ms, true) == capture_e::ok);
  at(750ms); assert(dxgi->snapshot(0, image, 1ms, true) == capture_e::ok);
  at(2000ms); assert(dxgi->snapshot(0, image, 1ms, true) == capture_e::reinit);
  auto resumed = make(prefix + "active"); assert(resumed && !resumed->dxgi);
  at(3000ms); assert(!screensaver_requires_wgc_reinit(prefix + "active"));
  // Saver-only DXGI initialization failure falls through in the same call.
  saver = true; at(0ms); dxgi_init_ok = false;
  const auto before = dxgi_inits;
  auto failed = make(prefix + "init-failed"); assert(failed && !failed->dxgi);
  at(1000ms); auto failed_again = make(prefix + "init-failed");
  assert(failed_again && !failed_again->dxgi && dxgi_inits == before + 1);
  dxgi_init_ok = true;
  // A DXGI acquisition failure gets WGC recovery, not stream termination/retry loop.
  at(0ms); auto broken = make(prefix + "capture-failed"); assert(broken && broken->dxgi);
  base_status = capture_e::error;
  assert(dynamic_cast<Dxgi *>(broken.get())->snapshot(0, image, 1ms, true) == capture_e::reinit);
  base_status = capture_e::ok;
  auto repaired = make(prefix + "capture-failed"); assert(repaired && !repaired->dxgi);
  // Unknown expires the saver lease; later true cannot repeat this attempt.
  at(0ms); auto unknown = make(prefix + "unknown"); assert(unknown && unknown->dxgi);
  saver.reset(); at(2000ms);
  assert(dynamic_cast<Dxgi *>(unknown.get())->snapshot(0, image, 1ms, true) == capture_e::reinit);
  auto unknown_wgc = make(prefix + "unknown"); assert(unknown_wgc && !unknown_wgc->dxgi);
  saver = true; at(2500ms); assert(!screensaver_requires_wgc_reinit(prefix + "unknown"));
  at(2750ms); assert(!screensaver_requires_wgc_reinit(prefix + "unknown"));
  // Mid-session recovery works even if real frame metadata keeps advancing.
  saver = false; at(0ms); auto live = make(prefix + "live"); assert(live && !live->dxgi);
  saver = true; at(1000ms); assert(!screensaver_requires_wgc_reinit(prefix + "live"));
  at(1250ms); assert(screensaver_requires_wgc_reinit(prefix + "live"));
  // Probe changed before factory: do not use stale positive state.
  saver.reset(); at(1260ms); auto changed = make(prefix + "live"); assert(changed && !changed->dxgi);
  saver = true; at(1750ms); assert(!screensaver_requires_wgc_reinit(prefix + "live"));
  saver = false; at(2000ms); assert(!screensaver_requires_wgc_reinit(prefix + "live"));
  at(2250ms); assert(screensaver_requires_wgc_reinit(prefix + "live"));
  auto post_exit = make(prefix + "live"); assert(post_exit && !post_exit->dxgi);
  at(2500ms); assert(!screensaver_requires_wgc_reinit(prefix + "live"));
  // Existing secure/grace admission is independent of unknown/false saver state.
  saver.reset(); secure = true; at(0ms);
  auto locked = make(prefix + "secure"); assert(locked && locked->dxgi);
  dxgi_init_ok = false; assert(!make(prefix + "secure-failed")); dxgi_init_ok = true;
  base_status = capture_e::error;
  assert(dynamic_cast<Dxgi *>(locked.get())->snapshot(0, image, 1ms, true) == capture_e::error);
  base_status = capture_e::ok; secure = false; grace = true;
  auto transitioning = make(prefix + "grace"); assert(transitioning && transitioning->dxgi);
  at(2000ms);
  assert(dynamic_cast<Dxgi *>(transitioning.get())->snapshot(0, image, 1ms, true) == capture_e::ok);
  grace = false;
}
}
int main() {
  replay::run<replay::display_wgc_ipc_vram_t, replay::temp_dxgi_vram_t>("vram-");
  replay::run<replay::display_wgc_ipc_ram_t, replay::temp_dxgi_ram_t>("ram-");
}
'''
with tempfile.TemporaryDirectory(prefix="wgc-screensaver-capture-") as directory:
    path = pathlib.Path(directory)
    (path / "capture.cpp").write_text(code)
    subprocess.run([compiler, "-std=c++23", "-Wall", "-Wextra", "-Werror", "-I", str(root), str(path / "capture.cpp"), "-o", str(path / "capture")], check=True)
    subprocess.run([str(path / "capture")], check=True)
print("Production VRAM/RAM factories and DXGI snapshots: saver, failure, unknown, secure/grace and return scenarios passed")
