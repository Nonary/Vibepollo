#!/usr/bin/env python3
"""Compile the production process-launch guard before its first mutation.

The remainder of execute is replaced by a reached-body sentinel. App lookup and
logging are deterministic adapters; this does not run commands or a host service.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

from test_delayed_mouse_release import ROOT, function


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--process-source', type=Path, default=ROOT / 'src/process.cpp')
    args = parser.parse_args()
    source = args.process_source.read_text()
    target = 'int proc_t::execute(const ctx_t &app,' in source
    signature = ('int proc_t::execute(const ctx_t &app,' if target else 'int proc_t::execute(int app_id,')
    execute = function(source, signature)
    boundary = '#ifdef _WIN32' if target else '    // Ensure starting from a clean slate'
    prefix = execute[:execute.index(boundary)]
    argument = 'const ctx_t &app' if target else 'int app_id'
    call = 'process.app' if target else '42'
    admission = (ROOT / 'src/host_stream_control.h').exists()
    code = r'''
#include <cassert>
#include <iostream>
#include <memory>
#include <optional>
#define BOOST_LOG(level) std::cerr
namespace rtsp_stream { struct launch_session_t {}; }
namespace proc {
struct ctx_t { bool remote_monitor_profile = false; };
struct proc_t {
  ctx_t app;
  int body_reached = 0;
  std::optional<ctx_t> resolve_app(int) { return app; }
  int execute(ARGUMENT, std::shared_ptr<rtsp_stream::launch_session_t> launch_session, bool config_read_gate_held);
};
PREFIX
  ++body_reached;
  return 77;
}
}
int main() {
  proc::proc_t process;
  process.app.remote_monitor_profile = true;
  assert(process.execute(CALL, {}, false) == 400);
  assert(process.body_reached == 0);
  process.app.remote_monitor_profile = false;
  assert(process.execute(CALL, {}, false) == 77);
  assert(process.body_reached == 1);
ADMISSION_TESTS
  std::cout << "PASS: profile rejected before process mutation\nPASS: ordinary app continues to existing body\n";
}
'''.replace('ARGUMENT', argument).replace('PREFIX', prefix).replace('CALL', call)
    admission_tests = r'''
  // Execute the composed production prefix while real terminal ownership is closed.
  assert(host_stream_control::terminate([] { return true; }, [&] {
    process.app.remote_monitor_profile = true;
    assert(process.execute(CALL, {}, false) == 503);
    process.app.remote_monitor_profile = false;
    assert(process.execute(CALL, {}, false) == 503);
    assert(process.body_reached == 1);
  }));
  assert(process.execute(CALL, {}, false) == 77);
  assert(process.body_reached == 2);
  std::cout << "PASS: real startup admission precedes profile lookup and reopens after terminal cleanup\n";
'''.replace('CALL', call)
    code = code.replace('ADMISSION_TESTS', admission_tests if admission else '')
    if admission:
        code = '#include "src/host_stream_control.h"\n' + code
    with tempfile.TemporaryDirectory(prefix='monitor-profile-launch-') as temp:
        cpp, binary = Path(temp) / 'test.cpp', Path(temp) / 'test'
        cpp.write_text(code)
        subprocess.run(['c++', '-std=c++20', '-pthread', '-I', str(ROOT), str(cpp), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
