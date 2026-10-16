#!/usr/bin/env python3
"""Replay production WGC startup with deterministic Windows API results on Linux.

Compile the actual SystemInitializer and required startup guards. Only the
Windows API/device/capture boundaries are stubbed, so priority denial cannot
silently become fatal again or turn required initialization failures into success.
Pass --source-file to prove the regression against an earlier helper revision.
"""

import argparse
from pathlib import Path
import subprocess
import tempfile


def declaration(source, marker):
    """Extract a complete top-level enum or class without changing its body."""
    start = source.index(marker)
    return source[start:source.index("\n};", start) + len("\n};")]


def guard(source, condition):
    """Extract one production startup guard, including its failure return."""
    start = source.index("  if (" + condition + ") {")
    return source[start:source.index("\n  }", start) + len("\n  }")]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repository", type=Path)
    parser.add_argument("compiler", nargs="?", default="c++")
    parser.add_argument("--source-file", type=Path)
    args = parser.parse_args()
    path = args.source_file or args.repository / "tools/sunshine_wgc_capture.cpp"
    source = path.read_text()
    entry = source[source.index("int main(int argc, char *argv[]) {"):]
    startup = "\n".join(guard(entry, condition) for condition in (
        "!system_initializer.initialize_all()",
        "!d3d11_manager.initialize_all(g_config.adapter_luid)",
        "!wgc_capture_manager.create_or_adjust_frame_pool(initial_frame_buffer_size)",
        "!wgc_capture_manager.create_capture_session()",
    ))
    code = r'''
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
using LONG = std::int32_t;
using HRESULT = LONG;
using DWORD = unsigned long;
using BOOL = bool;
using HANDLE = void *;
using HMODULE = void *;
using DPI_AWARENESS_CONTEXT = void *;
#define WINAPI
#define __stdcall
#define SUCCEEDED(status) ((status) >= 0)
#define FAILED(status) ((status) < 0)
#define BOOST_LOG(level) log_output
constexpr LONG denied = -1073741790;
constexpr LONG invalid_parameter = -1073741811;
constexpr int THREAD_PRIORITY_HIGHEST = 2;
constexpr int AVRT_PRIORITY_HIGH = 2;
constexpr int PROCESS_PER_MONITOR_DPI_AWARE = 2;
constexpr DPI_AWARENESS_CONTEXT DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = nullptr;
std::ostringstream log_output;
struct State {
  bool module = true, api = true, dpi = true, thread = true, mmcss = true;
  bool mmcss_priority = true, winrt = true, device = true, frame_pool = true, session = true;
  LONG realtime = 0, high = 0;
  int winrt_calls = 0, device_calls = 0, frame_pool_calls = 0, session_calls = 0;
  std::vector<LONG> requests;
} state;
HMODULE GetModuleHandleA(const char *name) {
  return std::string(name) == "gdi32.dll" && !state.module ? nullptr : &state;
}
BOOL set_dpi(DPI_AWARENESS_CONTEXT) { return state.dpi; }
LONG set_priority(HANDLE, LONG priority) {
  state.requests.push_back(priority);
  return priority == 5 ? state.realtime : state.high;
}
void *GetProcAddress(HMODULE, const char *name) {
  if (std::string(name) == "SetProcessDpiAwarenessContext") return reinterpret_cast<void *>(&set_dpi);
  return state.api ? reinterpret_cast<void *>(&set_priority) : nullptr;
}
HRESULT SetProcessDpiAwareness(int) { return state.dpi ? 0 : denied; }
HANDLE GetCurrentThread() { return &state; }
HANDLE GetCurrentProcess() { return &state; }
BOOL SetThreadPriority(HANDLE, int) { return state.thread; }
DWORD GetLastError() { return 5; }
HANDLE AvSetMmThreadCharacteristicsW(const wchar_t *, DWORD *) { return state.mmcss ? &state : nullptr; }
BOOL AvSetMmThreadPriority(HANDLE, int) { return state.mmcss_priority; }
struct safe_mmcss_handle {
  safe_mmcss_handle(std::nullptr_t) {}
  void reset(HANDLE) {}
};
namespace winrt {
  enum class apartment_type { multi_threaded };
  void init_apartment(apartment_type) {
    ++state.winrt_calls;
    if (!state.winrt) throw std::runtime_error("WinRT initialization failed");
  }
}
// PRIORITY_DECLARATIONS
// SYSTEM_INITIALIZER
struct {
  bool initialize_all(int) { ++state.device_calls; return state.device; }
} d3d11_manager;
struct {
  bool create_or_adjust_frame_pool(unsigned) { ++state.frame_pool_calls; return state.frame_pool; }
  bool create_capture_session() { ++state.session_calls; return state.session; }
} wgc_capture_manager;
struct { int adapter_luid = 0; } g_config;
constexpr unsigned initial_frame_buffer_size = 1;
int start() {
  SystemInitializer system_initializer;
// STARTUP_GUARDS
  return 0;
}
int failures = 0;
void check(bool ok, const char *message) {
  if (!ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
void reset() { state = {}; log_output.str(""); log_output.clear(); }
bool logged(const char *text) { return log_output.str().find(text) != std::string::npos; }
int main() {
  reset();
  SystemInitializer realtime;
  check(realtime.initialize_all(), "REALTIME success permits initialization");
  check(realtime.is_gpu_priority_set(), "REALTIME success records boost");
  check(state.requests == std::vector<LONG>{5}, "REALTIME success never requests HIGH");
  check(logged("GPU scheduling priority set to REALTIME"), "REALTIME success is logged");

  reset(); state.realtime = denied;
  SystemInitializer high;
  check(high.initialize_all(), "REALTIME denial falls back without aborting");
  check(high.is_gpu_priority_set(), "HIGH fallback records boost");
  check(state.requests == std::vector<LONG>({5, 4}), "fallback requests REALTIME then HIGH");
  check(logged("Failed to set GPU scheduling priority to REALTIME: -1073741790"), "denied status remains visible");
  check(logged("GPU scheduling priority set to HIGH"), "HIGH fallback is logged");

  reset(); state.realtime = invalid_parameter;
  SystemInitializer other_failure;
  check(other_failure.initialize_all() && other_failure.is_gpu_priority_set(), "other optional API errors also permit HIGH fallback");

  reset(); state.realtime = denied; state.high = denied;
  SystemInitializer unboosted;
  check(unboosted.initialize_all(), "both priority requests denied remain nonfatal");
  check(!unboosted.is_gpu_priority_set(), "both denials do not claim a boost");
  check(state.requests == std::vector<LONG>({5, 4}), "both denials make only two requests");
  check(logged("Failed to set GPU scheduling priority to HIGH: -1073741790"), "HIGH denial remains visible");
  check(logged("continuing capture at the existing priority"), "unboosted continuation is logged");
  check(state.winrt_calls == 1, "priority denial still initializes WinRT");
  check(start() == 0 && state.session_calls == 1, "unboosted startup reaches capture session");

  for (bool missing_module : {false, true}) {
    reset(); state.module = !missing_module; state.api = missing_module;
    SystemInitializer unavailable;
    check(unavailable.initialize_all(), "unavailable optional API remains nonfatal");
    check(!unavailable.is_gpu_priority_set() && state.requests.empty(), "unavailable API does not claim/request a boost");
    check(logged(missing_module ? "Failed to get gdi32.dll handle" : "D3DKMTSetProcessSchedulingPriorityClass not available"), "unavailable API is logged");
  }

  reset(); state.dpi = false; state.mmcss_priority = false;
  check(start() == 0, "existing best-effort DPI and MMCSS relative priority stay nonfatal");
  for (int failure = 0; failure < 5; ++failure) {
    reset(); state.realtime = denied; state.high = denied;
    if (failure == 0) state.thread = false;
    if (failure == 1) state.mmcss = false;
    if (failure == 2) state.device = false;
    if (failure == 3) state.frame_pool = false;
    if (failure == 4) state.session = false;
    check(start() == 1, "required system/device/frame-pool/session failure stays fatal");
    if (failure <= 1) check(state.device_calls == 0, "required system failure prevents device setup");
    if (failure <= 2) check(state.frame_pool_calls == 0, "failed device/system prevents frame-pool setup");
    if (failure <= 3) check(state.session_calls == 0, "failed frame-pool/device/system prevents capture session");
  }
  reset(); state.winrt = false;
  bool threw = false;
  try { start(); } catch (const std::runtime_error &) { threw = true; }
  check(threw && state.device_calls == 0, "WinRT initialization exception is not hidden");
  if (!failures) std::cout << "WGC GPU priority startup regressions passed\n";
  return failures ? 1 : 0;
}
'''
    enum = declaration(source, "enum class D3DKMT_SchedulingPriorityClass")
    enum += "\nusing PD3DKMTSetProcessSchedulingPriorityClass = LONG(__stdcall *)(HANDLE, LONG);\n"
    code = code.replace("// PRIORITY_DECLARATIONS", enum)
    code = code.replace("// SYSTEM_INITIALIZER", declaration(source, "class SystemInitializer {"))
    code = code.replace("// STARTUP_GUARDS", startup)
    with tempfile.TemporaryDirectory(prefix="wgc-gpu-priority-") as temporary:
        cpp = Path(temporary) / "startup.cpp"
        binary = Path(temporary) / "startup"
        cpp.write_text(code)
        subprocess.run([args.compiler, "-std=c++23", "-Wall", "-Wextra", "-Werror",
                        str(cpp), "-o", str(binary)], check=True, timeout=20)
        subprocess.run([str(binary)], check=True, timeout=5)


if __name__ == "__main__":
    main()
