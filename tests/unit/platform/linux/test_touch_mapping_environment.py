"""Replay the production touch mapping caller with isolated launch environments.

Compile the actual associate_output body. Only the touchscreen enumeration and
subprocess boundary are modeled; the real retry policy and environment lookup
remain in use. --source-tree permits a red/green run against the unchanged tree.
"""
import argparse
import pathlib
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source-tree', type=pathlib.Path,
                    default=pathlib.Path(__file__).resolve().parents[4])
args = parser.parse_args()
root = args.source_tree.resolve()
source = (root / 'src/platform/linux/input/inputtino_touch.cpp').read_text()
start = source.index('    void associate_output(const touch_port_t &port) {')
brace = source.index('{', start)
depth, end = 1, brace + 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
method = source[start:end]
program = r'''
#include "src/platform/linux/input/inputtino_touch_mapping.h"
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

struct GSubprocess {};
using GSubprocessFlags = int;
constexpr int G_SUBPROCESS_FLAGS_STDOUT_SILENCE = 1;
constexpr int G_SUBPROCESS_FLAGS_STDERR_SILENCE = 2;
std::vector<std::vector<std::string>> launches;
GSubprocess child;
int mapping_result = 0;
std::ostringstream logs;
#define BOOST_LOG(level) logs

template<class... Args>
GSubprocess *g_subprocess_new(GSubprocessFlags, void *, Args... args) {
  const char *arguments[] = {args...};
  std::vector<std::string> command;
  for (const auto *argument : arguments) {
    if (!argument) break;
    command.emplace_back(argument);
  }
  launches.push_back(std::move(command));
  return &child;
}
void g_subprocess_force_exit(GSubprocess *) {}
void g_object_unref(GSubprocess *) {}
int wait_for_mapping(GSubprocess *) { return mapping_result; }

namespace platf::touch {
struct touch_port_t { std::string output_name; };
struct touchscreen_t {
  std::vector<std::string> get_nodes() { return {"/dev/input/event27"}; }
};
struct client_input_raw_t { std::optional<touchscreen_t> touch {std::in_place}; };
struct worker_t {
  client_input_raw_t *raw;
  mapping_retry_t mapping_retry;
  std::string mapped_output;
  bool stopping = false;
  std::mutex mutex;
  GSubprocess *active_process = nullptr;
  int released = 0;
  void release_contacts() { ++released; }
''' + method + r'''
};
}

int main() {
  using namespace platf::touch;
  client_input_raw_t raw;
  unsetenv("WAYLAND_DISPLAY");
  setenv("VIBEPOLLO_MACHINE_HOST", "1", 1);
  worker_t machine {&raw};
  machine.associate_output({"Virtual-1"});
  assert(launches.size() == 1);
  assert((launches.back() == std::vector<std::string> {
    "/usr/libexec/vibeshine/vibepollo-session-exec", "touch-map", "event27", "Virtual-1"}));
  assert(machine.mapped_output == "Virtual-1");
  assert(machine.released == 1);
  machine.associate_output({"Virtual-1"});
  assert(launches.size() == 1); // Retry throttles already-mapped output.
  machine.associate_output({"Virtual-2"});
  assert(launches.size() == 2);
  assert(launches.back().back() == "Virtual-2");
  assert(machine.released == 2);

  launches.clear();
  unsetenv("VIBEPOLLO_MACHINE_HOST");
  setenv("WAYLAND_DISPLAY", "wayland-0", 1);
  worker_t standalone {&raw};
  standalone.associate_output({"DP-3"});
  assert(launches.size() == 1);
  assert((launches.back() == std::vector<std::string> {
    "/usr/libexec/vibeshine/vibepollo-touch-map", "event27", "DP-3"}));

  launches.clear();
  unsetenv("WAYLAND_DISPLAY");
  worker_t no_session {&raw};
  no_session.associate_output({"Virtual-1"});
  assert(launches.empty());
  assert(no_session.mapping_retry.output.empty());
  setenv("WAYLAND_DISPLAY", "wayland-1", 1);
  no_session.associate_output({"Virtual-1"});
  assert(launches.size() == 1); // Missing local session cannot latch failure.

  launches.clear();
  unsetenv("WAYLAND_DISPLAY");
  setenv("VIBEPOLLO_MACHINE_HOST", "1", 1);
  worker_t empty_target {&raw};
  empty_target.associate_output({""});
  assert(launches.empty()); // Broker mode still requires a captured target.
  assert(empty_target.released == 0);
  std::cout << "Production native-touch mapping caller: machine-host/standalone/retry/output-change checks passed\n";
}
'''
with tempfile.TemporaryDirectory(prefix='touch-mapping-environment-') as temporary:
    directory = pathlib.Path(temporary)
    cpp = directory / 'caller.cpp'
    binary = directory / 'caller'
    cpp.write_text(program)
    subprocess.run(['c++', '-std=c++20', '-I', str(root), str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
