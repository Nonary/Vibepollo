#!/usr/bin/env python3
"""Compile the production group termination helper and exercise a TERM-resistant child."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('repository_root', type=Path)
parser.add_argument('--boost-source', type=Path, required=True)
parser.add_argument('--boost-filesystem', type=Path, required=True)
args = parser.parse_args()
source = (args.repository_root / 'src/process.cpp').read_text()
start = source.index('  void terminate_process_group(')
end = source.index('\n  boost::filesystem::path find_working_directory', start)
helper = source[start:end]
# Exercise the same timeout selection used by proc_t::terminate, not a copied policy.
selection = next(line.strip() for line in source.splitlines() if 'seconds remaining_timeout =' in line)
harness = r'''
#include "src/boost_process_shim.h"
#include <chrono>
#include <thread>
#include <iostream>
#include <cassert>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
namespace bp = boost_process_shim;
using namespace std::chrono_literals;
using namespace std::string_view_literals;
#define BOOST_LOG(level) std::cerr
namespace platf {
bool process_group_running(std::uintptr_t group) { return kill(-static_cast<pid_t>(group), 0) == 0; }
bool request_process_group_exit(std::uintptr_t group) { return kill(-static_cast<pid_t>(group), SIGTERM) == 0; }
}
'''
main = r'''
int main(int argc, char **argv) {
  for (bool immediate : {true, false}) {
    int ready[2]; assert(pipe(ready) == 0);
    const auto pid = fork(); assert(pid >= 0);
    if (pid == 0) {
      close(ready[0]); setpgid(0, 0); signal(SIGTERM, SIG_IGN);
      char byte = 'R'; write(ready[1], &byte, 1); close(ready[1]);
      for (;;) pause();
    }
    close(ready[1]); char byte; assert(read(ready[0], &byte, 1) == 1); close(ready[0]);
    bp::group group; group.set_leader(pid);
    bp::child child;
    struct { std::chrono::seconds exit_timeout {2}; } _app;
    TIMEOUT_SELECTION
    auto started = std::chrono::steady_clock::now();
    terminate_process_group(child, group, remaining_timeout);
    int status; assert(waitpid(pid, &status, 0) == pid);
    auto elapsed = std::chrono::steady_clock::now() - started;
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    if (immediate) assert(elapsed < 1s);
    else assert(elapsed >= 2s && elapsed < 4s);
  }
}
'''.replace('TIMEOUT_SELECTION', selection)
with tempfile.TemporaryDirectory(prefix='force-termination-') as directory:
    cpp, binary = Path(directory) / 'test.cpp', Path(directory) / 'test'
    cpp.write_text(harness + helper + main)
    includes = [arg for path in (args.boost_source / 'libs').glob('*/include') for arg in ('-I', str(path))]
    subprocess.run(['c++', '-std=c++20', '-pthread', '-I', str(args.repository_root), *includes, str(cpp),
                    str(args.boost_source / 'libs/process/src/error.cpp'), str(args.boost_filesystem), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
print('PASS: real process group kills TERM-resistant child immediately; graceful close waits')
