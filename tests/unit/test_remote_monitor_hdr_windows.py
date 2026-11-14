#!/usr/bin/env python3
"""Exercise the production exact-target HDR apply/readback with fake platform edges."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler', default='g++')
    parser.add_argument('--source-dir', type=Path, default=Path(__file__).resolve().parents[2] / 'src')
    args = parser.parse_args()
    integration = (args.source_dir / 'platform/windows/display_helper_integration.cpp').read_text()
    nvhttp = (args.source_dir / 'nvhttp.cpp').read_text()
    windows = nvhttp[nvhttp.index('    void register_remote_monitor_runtime()'):nvhttp.index('    void register_remote_monitor_runtime()', nvhttp.index('    void register_remote_monitor_runtime()') + 1)]
    assert 'mode.hdr = hdr_requested;' in windows
    assert 'false, 1, mode.hdr, false, true, true' in windows
    assert 'apply_remote_monitor_hdr_state(*device, mode.hdr, allow_mutation)' in windows
    assert windows.count('.hdr_enabled = state.hdr_enabled') == 2
    assert 'hdr_capture_required(*launch_session, config::video)' in nvhttp
    assert 'if (rtsp_stream::hdr_capture_required(*launch_session, config::video) && !monitor.hdr_enabled)' in nvhttp
    code = r'''
#include <cassert>
#include <chrono>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
namespace display_device { enum class HdrState { Disabled, Enabled }; }
namespace display_helper::borrow {
struct native_mutation_scope_t { static bool enter(bool) { return true; } };
}
#define BOOST_LOG(x) std::cerr
struct Display {
 using State = display_device::HdrState;
 std::map<std::string, std::optional<State>> states;
 bool set_ok = true; int reads = 0, sets = 0, delay = 0;
 std::optional<State> pending;
 std::string target;
 auto getCurrentHdrStates(const std::set<std::string>& ids) {
   ++reads; assert(ids.size() == 1); target = *ids.begin();
   if (pending && delay-- <= 0) { states[target] = pending; pending.reset(); }
   std::map<std::string, std::optional<State>> result;
   if (states.contains(target)) result[target] = states[target];
   return result;
 }
 bool setHdrStates(const std::map<std::string, std::optional<State>>& values) {
   ++sets; assert(values.size() == 1 && values.begin()->first == target);
   if (!set_ok) return false;
   pending = values.begin()->second; return true;
 }
};
struct Context { std::shared_ptr<Display> display; };
std::shared_ptr<Display> display;
auto make_settings_manager() -> std::optional<Context> { return Context{display}; }
std::optional<bool> unsupported;
std::optional<bool> remote_monitor_hdr_unsupported(const std::string &) { return unsupported; }
''' + function(integration, '  bool apply_remote_monitor_hdr_state(') + r'''
int main() {
 using S = display_device::HdrState;
 display = std::make_shared<Display>();
 display->states["target"] = S::Disabled;
 display->states["peer"] = S::Enabled;
 display->delay = 2;
 assert(apply_remote_monitor_hdr_state("target", true, true));
 assert(display->sets == 1 && display->reads >= 3);
 assert(display->states["target"] == S::Enabled && display->states["peer"] == S::Enabled);
 int sets = display->sets;
 assert(apply_remote_monitor_hdr_state("target", true, true));
 assert(display->sets == sets);
 // A shared captured target may be observed but never toggled.
 assert(!apply_remote_monitor_hdr_state("target", false, false));
 assert(display->sets == sets);
 assert(apply_remote_monitor_hdr_state("target", true, false));
 assert(display->sets == sets);
 display->set_ok = false;
 assert(!apply_remote_monitor_hdr_state("target", false, true));
 assert(display->states["target"] == S::Enabled);
 display->states["target"] = std::nullopt;
 unsupported = true;
 assert(apply_remote_monitor_hdr_state("target", false, false));
 unsupported = false;
 assert(!apply_remote_monitor_hdr_state("target", false, false));
 unsupported = std::nullopt;
 assert(!apply_remote_monitor_hdr_state("target", false, false));
 assert(!apply_remote_monitor_hdr_state("target", true, true));
 display->states.erase("target");
 unsupported = true;
 assert(!apply_remote_monitor_hdr_state("target", false, false));
 assert(!apply_remote_monitor_hdr_state("", true, true));
}
'''
    with tempfile.TemporaryDirectory() as tmp:
        source = Path(tmp) / 'test.cpp'
        binary = Path(tmp) / 'test'
        source.write_text(code)
        subprocess.run([args.compiler, '-std=c++20', '-pthread', str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print('Remote Monitor HDR exact-target apply/readback regression passed')


if __name__ == '__main__':
    main()
