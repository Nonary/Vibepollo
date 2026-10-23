"""Compile production diagnostic branches with inert Windows boundary fakes."""
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve()
compiler = sys.argv[2] if len(sys.argv) > 2 else "g++-15"


def block(source, marker):
    start = source.index(marker)
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


pump = (root / "src/platform/windows/display_helper_v2/win_event_pump.cpp").read_text()
monitor = (root / "src/platform/windows/display_output_monitor.cpp").read_text()
program = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include "src/color_profile_policy.h"
#include "src/platform/windows/device_notification_diagnostics.h"
std::vector<std::string> logs;
struct Log {
  std::ostringstream text;
  template<class T> Log &operator<<(const T &value) { text << value; return *this; }
  ~Log() { logs.push_back(text.str()); }
};
#define BOOST_LOG(level) Log{}
#define CALLBACK
using HWND = void *;
using UINT = unsigned;
using DWORD = std::uint32_t;
using WPARAM = std::uintptr_t;
using LPARAM = std::intptr_t;
using LONG_PTR = std::intptr_t;
using LRESULT = std::intptr_t;
struct GUID { DWORD a; std::uint16_t b, c; unsigned char d[8]; bool operator==(const GUID &) const = default; };
struct DEV_BROADCAST_HDR { DWORD dbch_size, dbch_devicetype, reserved; };
struct DEV_BROADCAST_DEVICEINTERFACE_W { DWORD dbcc_size, dbcc_devicetype, reserved; GUID dbcc_classguid; wchar_t dbcc_name[1]; };
struct DEV_BROADCAST_PORT_W { DWORD dbcp_size, dbcp_devicetype, reserved; wchar_t dbcp_name[1]; };
struct DEV_BROADCAST_VOLUME { DWORD dbcv_size, dbcv_devicetype, reserved, dbcv_unitmask; std::uint16_t dbcv_flags; };
struct CREATESTRUCTW { void *lpCreateParams; };
struct POWERBROADCAST_SETTING { GUID PowerSetting; DWORD DataLength; unsigned char Data[4]; };
constexpr GUID GUID_MONITOR_POWER_ON {};
enum { WM_NCCREATE=1, WM_DISPLAYCHANGE, WM_DEVICECHANGE, WM_POWERBROADCAST, WM_DESTROY,
  DBT_DEVICEARRIVAL=0x8000, DBT_DEVICEREMOVECOMPLETE=0x8004, DBT_DEVNODES_CHANGED=7,
  DBT_DEVTYP_DEVICEINTERFACE=5, DBT_DEVTYP_PORT=3, DBT_DEVTYP_VOLUME=2,
  PBT_APMRESUMEAUTOMATIC=18, PBT_POWERSETTINGCHANGE=0x8013, GWLP_USERDATA=0, TRUE=1 };
LONG_PTR user_data = 0;
LONG_PTR SetWindowLongPtrW(HWND, int, LONG_PTR value) { return user_data = value; }
LONG_PTR GetWindowLongPtrW(HWND, int) { return user_data; }
LRESULT DefWindowProcW(HWND, UINT, WPARAM, LPARAM) { return 123; }
void PostQuitMessage(int) {}
auto fake_now = std::chrono::steady_clock::time_point {};
namespace display_helper::v2 {
  constexpr GUID kMonitorInterfaceGuid = {0xe6f07b5f,0xee97,0x4a90,{0xb0,0x76,0x33,0xf5,0x7b,0xf4,0xea,0xa7}};
  enum class DisplayEvent { DisplayChange, DeviceArrival, DeviceRemoval, PowerResume };
  struct WinEventPump {
    std::function<void(DisplayEvent)> callback_;
    std::atomic<HWND> hwnd_ {nullptr};
    struct { bool observe(bool, bool) { return false; } } monitor_power_policy_;
    device_diagnostics::context_t device_diagnostics_;
    static LRESULT wnd_proc(HWND, UINT, WPARAM, LPARAM);
    void signal(DisplayEvent);
    void log_device_event(WPARAM, LPARAM) noexcept;
    void cleanup_notifications() {}
  };
'''
program += block(pump, "  LRESULT CALLBACK WinEventPump::wnd_proc")
program += block(pump, "  void WinEventPump::signal")
program += block(pump, "  void WinEventPump::log_device_event").replace("device_diagnostics::context_t::clock::now()", "fake_now")
program += r'''
}
namespace winrt::Windows::Graphics::Display {
  enum class AdvancedColorKind { StandardDynamicRange, WideColorGamut, HighDynamicRange };
}
using Kind = winrt::Windows::Graphics::Display::AdvancedColorKind;
struct Notifications {
  std::atomic_bool reinit {false}, dirty {false};
  std::condition_variable_any cv;
};
struct ColorInfo { Kind kind; Kind CurrentAdvancedColorKind() const { return kind; } };
struct Sender {
  Kind kind;
  bool fail = false;
  ColorInfo GetAdvancedColorInfo() const { if (fail) throw 1; return {kind}; }
};
auto make_callback(std::shared_ptr<Notifications> notifications, bool captured_hdr) {
  const std::uint64_t observation_id = 7;
'''
program += block(monitor, "const auto color_changed =") + "; return color_changed; }\n"
program += "void initial_color(std::shared_ptr<Notifications> notifications, bool captured_hdr, Kind initial_kind) {\n"
program += "const bool initial_hdr = initial_kind == Kind::HighDynamicRange;\n"
program += "const std::uint64_t observation_id = 7;\n"
program += block(monitor, "if (initial_hdr != captured_hdr") + "\n}\n"
program += r'''
enum class capture_e { ok, timeout, reinit, error, interrupted };
namespace platf { using ::capture_e; }
struct Ipc {
  bool stop=false, swap=false, reinit=false, initialized=true, descriptor=true;
  bool stop_requested() { return stop; }
  bool should_swap_to_dxgi() { return swap; }
  bool should_reinit() { return reinit; }
  bool is_initialized() { return initialized; }
  bool set_cursor_visible(bool) { return true; }
  void initialize_if_needed() {}
  bool peek_shared_texture_desc(int &) { return descriptor; }
};
'''
wgc = (root / "src/platform/windows/display_wgc.cpp").read_text()
for mode, end_marker in (("vram", "    timeout = effective_wgc_timeout"), ("ram", "    winrt::com_ptr<ID3D11Texture2D> gpu_tex;")):
    start = wgc.index(f"  capture_e display_wgc_ipc_{mode}_t::snapshot(")
    start = wgc.index("    if (!_ipc_session)", start)
    end = wgc.index(end_marker, start)
    program += f"capture_e {mode}_preamble(Ipc *_ipc_session) {{\nconst bool cursor_visible = true;\n" + wgc[start:end] + "return capture_e::ok; }\n"
program += "capture_e descriptor(Ipc *_ipc_session) { int desc;\n"
program += block(wgc, "if (!_ipc_session->peek_shared_texture_desc(desc))") + " return capture_e::ok; }\n"
base = (root / "src/platform/windows/display_base.cpp").read_text()
before = block(base, "if (output_monitor && output_monitor->reinit_requested())")
after = block(base[base.index(before) + len(before):], "if (output_monitor && output_monitor->reinit_requested())")
program += r'''
struct Monitor { bool requested; bool reinit_requested() { return requested; } unsigned diagnostic_id() { return 7; } };
unsigned releases = 0;
void release_snapshot() { ++releases; }
'''
program += "capture_e before_snapshot(Monitor *output_monitor) {\n" + before + "return capture_e::ok; }\n"
program += "capture_e after_snapshot(Monitor *output_monitor, capture_e status) {\n" + after + "return status; }\n"
video = (root / "src/video.cpp").read_text()
program += "std::pair<capture_e, bool> manual_switch(capture_e status, bool artificial_reinit) {\n"
program += block(video, "if (artificial_reinit && status != platf::capture_e::error)") + "return {status, artificial_reinit}; }\n"
program += r'''
bool contains(const std::string &value) {
  return std::any_of(logs.begin(), logs.end(), [&](const auto &line) { return line.find(value) != std::string::npos; });
}
int main() {
  using namespace display_helper;
  using namespace display_helper::v2;
  // Compile the actual return branches: diagnostics must preserve priorities,
  // return values and release behavior even when multiple flags are present.
  for (auto preamble : {vram_preamble, ram_preamble}) {
    Ipc ipc;
    assert(preamble(nullptr) == capture_e::error);
    assert(preamble(&ipc) == capture_e::ok);
    ipc.stop = ipc.swap = ipc.reinit = true;
    assert(preamble(&ipc) == capture_e::interrupted);
    ipc.stop = false;
    assert(preamble(&ipc) == capture_e::reinit);
    ipc.swap = false;
    assert(preamble(&ipc) == capture_e::reinit);
    ipc.reinit = false; ipc.initialized = false;
    assert(preamble(&ipc) == capture_e::reinit);
  }
  assert(contains("source=wgc-vram reason=dxgi-fallback"));
  assert(contains("source=wgc-ram reason=ipc-reinit-flag"));
  Ipc ipc;
  assert(descriptor(&ipc) == capture_e::ok);
  ipc.descriptor = false;
  assert(descriptor(&ipc) == capture_e::reinit && contains("reason=missing-texture-descriptor"));
  Monitor observed {false};
  assert(before_snapshot(nullptr) == capture_e::ok);
  assert(before_snapshot(&observed) == capture_e::ok);
  assert(after_snapshot(&observed, capture_e::timeout) == capture_e::timeout && releases == 0);
  observed.requested = true;
  assert(before_snapshot(&observed) == capture_e::reinit);
  assert(after_snapshot(&observed, capture_e::error) == capture_e::reinit && releases == 1);
  assert(contains("phase=before-snapshot") && contains("phase=after-snapshot"));
  assert(manual_switch(capture_e::ok, true) == std::make_pair(capture_e::reinit, false));
  assert(manual_switch(capture_e::error, true) == std::make_pair(capture_e::error, true));
  assert(contains("source=explicit-display-switch"));
  logs.clear();
  // A burst of WinRT events retains the old sticky behavior and logs only its
  // first cause. An unreadable event still requests a DXGI refresh, not reinit.
  auto notifications = std::make_shared<Notifications>();
  auto callback = make_callback(notifications, true);
  callback(Sender {Kind::HighDynamicRange});
  assert(!notifications->reinit && notifications->dirty && logs.empty());
  notifications->dirty = false;
  callback(Sender {Kind::StandardDynamicRange, true});
  assert(!notifications->reinit && notifications->dirty && logs.empty());
  for (int i=0; i<100; ++i) callback(Sender {Kind::WideColorGamut});
  assert(notifications->reinit && logs.size() == 1);
  assert(contains("reason=winrt-color-event") && contains("observed_kind=1"));
  initial_color(notifications, true, Kind::StandardDynamicRange);
  assert(logs.size() == 1);
  notifications = std::make_shared<Notifications>();
  initial_color(notifications, false, Kind::HighDynamicRange);
  assert(notifications->reinit && contains("reason=winrt-initial-color"));

  WinEventPump pump;
  user_data = reinterpret_cast<LONG_PTR>(&pump);
  std::vector<DisplayEvent> routed;
  pump.callback_ = [&](auto event) { routed.push_back(event); };
  auto send = [&](WPARAM event, void *payload) {
    return pump.wnd_proc(nullptr, WM_DEVICECHANGE, event, reinterpret_cast<LPARAM>(payload));
  };
  auto tick = [&] { fake_now += std::chrono::seconds(11); };
  logs.clear();
  const std::wstring private_name = L"\\\\?\\DISPLAY#SDD5BBA#PRIVATE_SERIAL_123#{private-instance}";
  std::vector<wchar_t> storage((offsetof(DEV_BROADCAST_DEVICEINTERFACE_W, dbcc_name) + (private_name.size()+1)*sizeof(wchar_t) + sizeof(wchar_t)-1)/sizeof(wchar_t));
  auto *item = reinterpret_cast<DEV_BROADCAST_DEVICEINTERFACE_W *>(storage.data());
  item->dbcc_size = storage.size()*sizeof(wchar_t);
  item->dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
  item->dbcc_classguid = kMonitorInterfaceGuid;
  std::copy(private_name.begin(), private_name.end(), item->dbcc_name);
  item->dbcc_name[private_name.size()] = 0;
  assert(send(DBT_DEVICEREMOVECOMPLETE, item) == 123);
  send(DBT_DEVICEARRIVAL, item);
  assert(routed[0] == DisplayEvent::DeviceRemoval && routed[1] == DisplayEvent::DeviceArrival);
  assert(logs.size() == 2 && contains("class=monitor family=managed-virtual model=SDD5BBA identity=1"));
  for (const auto &line : logs) {
    assert(line.size() < 256 && line.find("PRIVATE") == std::string::npos && line.find("private-instance") == std::string::npos);
  }
  tick();
  DEV_BROADCAST_VOLUME volume {sizeof(volume), DBT_DEVTYP_VOLUME, 0, 1<<3, 0};
  send(DBT_DEVICEARRIVAL, &volume);
  assert(contains("type=volume") && routed.back() == DisplayEvent::DeviceArrival);
  tick();
  struct Port { DWORD size, type, reserved; wchar_t name[16]; } port {sizeof(port),DBT_DEVTYP_PORT,0,L"PRIVATE_PORT"};
  send(DBT_DEVICEREMOVECOMPLETE, &port);
  assert(contains("type=port") && !contains("PRIVATE_PORT") && routed.back() == DisplayEvent::DeviceRemoval);
  tick();
  send(DBT_DEVICEARRIVAL, nullptr);
  DEV_BROADCAST_HDR invalid {sizeof(DWORD), DBT_DEVTYP_DEVICEINTERFACE, 0};
  send(DBT_DEVICEARRIVAL, &invalid);
  DEV_BROADCAST_HDR short_interface {sizeof(short_interface), DBT_DEVTYP_DEVICEINTERFACE, 0};
  send(DBT_DEVICEARRIVAL, &short_interface);
  assert(contains("payload=missing") && contains("payload=invalid"));
  tick();
  std::vector<wchar_t> long_storage(2048, L'x');
  auto *long_item = reinterpret_cast<DEV_BROADCAST_DEVICEINTERFACE_W *>(long_storage.data());
  long_item->dbcc_size = long_storage.size()*sizeof(wchar_t);
  long_item->dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
  long_item->dbcc_classguid = kMonitorInterfaceGuid;
  send(DBT_DEVICEARRIVAL, long_item);
  assert(contains("identity=0 payload=unterminated-or-long"));
  assert(device_diagnostics::monitor_model(L"\\\\?\\DISPLAY#AUSAA06#private") == "AUSAA06");
  assert(device_diagnostics::monitor_model(L"\\\\?\\DISPLAY#BAD\n123#private") == "unknown");

  // The diagnostic budget never suppresses callback delivery or changes type.
  pump.device_diagnostics_ = {};
  logs.clear(); routed.clear();
  for (int i=0; i<100; ++i) send(DBT_DEVICEARRIVAL, item);
  assert(logs.size() == 8 && routed.size() == 100);
  tick(); send(DBT_DEVICEREMOVECOMPLETE, item);
  assert(logs.size() == 9 && contains("suppressed=92") && routed.size() == 101);
  device_diagnostics::context_t identities;
  for (unsigned i=1; i<=32; ++i) assert(identities.identity(L'M',std::to_wstring(i)) == i);
  assert(identities.identity(L'M', L"33") == 0);
  assert(identities.identity(L'M', L"1") == 1);
  assert(identities.identity(L'M', std::wstring(513, L'x')) == 0);
}
'''

with tempfile.TemporaryDirectory(prefix="capture-restart-diagnostics-") as temp:
    path = pathlib.Path(temp)
    source, binary = path / "diagnostics.cpp", path / "diagnostics"
    source.write_text(program)
    subprocess.run([compiler, "-std=c++23", "-Wall", "-Wextra", "-Werror", "-I", str(root), str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("Production WinRT/device diagnostics: routing, privacy, bounds and rate-limit scenarios passed")
