#!/usr/bin/env python3
"""Execute the actual close handler with transport/process side-effect sentinels."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('repository_root', type=Path)
parser.add_argument('--compiler', default='c++')
parser.add_argument('--json-include', required=True)
args = parser.parse_args()
source = (args.repository_root / 'src/confighttp.cpp').read_text()
start = source.index('  void closeApp(resp_https_t response, req_https_t request) {')
end = source.index('\n  }', start) + len('\n  }')
handler = source[start:end]
harness = r'''
#include <cassert>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
struct content_t : std::istringstream {
  explicit content_t(const std::string &s): std::istringstream(s), length(s.size()) {}
  std::size_t size() const { return length; }
  std::size_t length;
};
struct request_t { explicit request_t(const std::string &s): content(s) {} content_t content; };
using req_https_t = std::shared_ptr<request_t>;
using resp_https_t = int;
static bool authorized = true, content_type_ok = true;
static int errors = 0, responses = 0;
static std::vector<std::string> effects;
bool validateContentType(resp_https_t, req_https_t, const char *) { return content_type_ok; }
bool authenticate(resp_https_t, req_https_t) { return authorized; }
void print_req(req_https_t) {}
void bad_request(resp_https_t, req_https_t, const char *) { ++errors; }
void send_response(resp_https_t, const nlohmann::json &j) { assert(j.at("status") == true); ++responses; }
namespace rtsp_stream { void terminate_sessions(bool preserve) { assert(!preserve); effects.push_back("rtsp"); } }
namespace webrtc_stream { void shutdown_all_sessions() { effects.push_back("webrtc"); } }
namespace proc {
struct process_t { void terminate(bool force) { effects.push_back(force ? "force" : "graceful"); } } proc;
}
'''
main = r'''
int main() {
  for (const std::string body : {"", "null", "{}", "{\"force\":false}"}) {
    effects.clear();
    closeApp(0, std::make_shared<request_t>(body));
    assert((effects == std::vector<std::string>{"graceful"}));
  }
  effects.clear();
  closeApp(0, std::make_shared<request_t>("{\"force\":true}"));
  assert((effects == std::vector<std::string>{"rtsp", "webrtc", "force"}));
  for (const std::string body : {"broken", "[]", "{\"force\":\"true\"}", "{\"force\":1}"}) {
    effects.clear();
    const int before = errors;
    closeApp(0, std::make_shared<request_t>(body));
    assert(effects.empty() && errors == before + 1);
  }
  effects.clear();
  const int before = responses;
  authorized = false;
  closeApp(0, std::make_shared<request_t>("{\"force\":true}"));
  authorized = true;
  content_type_ok = false;
  closeApp(0, std::make_shared<request_t>("{\"force\":true}"));
  assert(effects.empty() && responses == before);
}
'''
with tempfile.TemporaryDirectory(prefix='app-close-test-') as directory:
    cpp, binary = Path(directory) / 'test.cpp', Path(directory) / 'test'
    cpp.write_text(harness + handler + main)
    subprocess.run([args.compiler, '-std=c++20', '-I', args.json_include,
                    str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
print('PASS: close handler authorization, body validation, transport drain and graceful compatibility')
