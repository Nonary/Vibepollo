#!/usr/bin/env python3
"""Execute the production machine-host status handler with cleanup sentinels."""

import argparse
import pathlib
import subprocess
import sys
import tempfile


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("repository_root", type=pathlib.Path)
parser.add_argument("--compiler", default="c++")
parser.add_argument("--json-include", type=pathlib.Path, required=True)
args = parser.parse_args()
root = args.repository_root.resolve()
source = (root / "src/confighttp.cpp").read_text()
start = source.index("  void getSessionStatus(resp_https_t response, req_https_t request) {")
end = source.index("\n  }", start) + len("\n  }")
handler = source[start:end]

prefix = r'''#include <cassert>
#include <string>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using resp_https_t = int *;
using req_https_t = int;
static bool authenticated = true;
static int response_count = 0;
static int passive_stream_queries = 0;
static int destructive_stream_queries = 0;
static int current_app_queries = 0;
static int destructive_app_queries = 0;
static json response_body;
static bool authenticate(int *, int) { return authenticated; }
static void print_req(int) {}
static void send_response(int *, const json &body) { ++response_count; response_body = body; }
namespace rtsp_stream {
  int session_count_no_cleanup() { ++passive_stream_queries; return 2; }
  int session_count() { ++destructive_stream_queries; return 2; }
}
namespace webrtc_stream { int active_session_count() { return 1; } }
struct process_t {
  int current_app_id() { ++current_app_queries; return 44; }
  int running() { ++destructive_app_queries; return 1; }
  std::string get_last_run_app_name() { return "Example"; }
};
static process_t proc_instance;
namespace proc { process_t &proc = proc_instance; }
namespace video { bool last_encoder_probe_failed() { return false; } }
'''

main = r'''
int main() {
  int response = 0;
  authenticated = false;
  getSessionStatus(&response, 0);
  assert(response_count == 0);
  assert(passive_stream_queries == 0 && destructive_stream_queries == 0);
  assert(current_app_queries == 0 && destructive_app_queries == 0);

  authenticated = true;
  getSessionStatus(&response, 0);
  assert(response_count == 1);
  assert(passive_stream_queries == 1 && destructive_stream_queries == 0);
  assert(current_app_queries == 1 && destructive_app_queries == 0);
  assert(response_body.at("activeSessions") == 3);
  assert(response_body.at("appRunning") == true);
  assert(response_body.at("appName") == "Example");
  assert(response_body.at("paused") == false);
}
'''


def run(handler_text: str, should_pass: bool) -> None:
    with tempfile.TemporaryDirectory(prefix="machine-status-polling-") as directory:
        directory = pathlib.Path(directory)
        cpp = directory / "status.cpp"
        binary = directory / "status"
        cpp.write_text(prefix + handler_text + main)
        command = [args.compiler, "-std=c++20"]
        if args.json_include:
            command += ["-I", str(args.json_include)]
        compile_result = subprocess.run(
            command + [str(cpp), "-o", str(binary)],
            capture_output=True,
            text=True,
        )
        if compile_result.returncode:
            raise AssertionError(f"status harness compilation failed:\n{compile_result.stderr}")
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        if should_pass and result.returncode:
            raise AssertionError(f"production status behavior failed:\n{result.stderr}")
        if not should_pass and result.returncode == 0:
            raise AssertionError("status behavior harness did not detect destructive-query mutation")


run(handler, should_pass=True)
mutated = handler.replace("session_count_no_cleanup()", "session_count()")
mutated = mutated.replace("current_app_id()", "running()")
if mutated == handler:
    raise AssertionError("expected destructive-query mutation did not apply")
run(mutated, should_pass=False)
