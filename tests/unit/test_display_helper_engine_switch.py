"""Exercise the production engine-switch branch with deterministic process seams."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler', default='g++')
    parser.add_argument('--revision')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    path = 'src/platform/windows/display_helper_integration.cpp'
    text = (subprocess.check_output(['git', '-C', str(root), 'show', f'{args.revision}:{path}'], text=True)
            if args.revision else (root / path).read_text())
    start = text.index('        if (!engine_matches) {')
    end = text.index('\n      } else {\n        // Process exited', start)
    branch = text[start:end]
    program = r'''
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <optional>
using namespace std::literals;
using DWORD = unsigned long;
constexpr DWORD WAIT_OBJECT_0=0, WAIT_TIMEOUT=258, WAIT_FAILED=0xffffffff;
constexpr int kHelperForceKillWaitMs=100, kShutdownHelperPingTimeoutMs=5, kHelperPingTimeoutMs=10;
std::optional<bool> g_running_helper_legacy;
int terminations, fast_pings, launches, failures;
bool cancelled, reset_ok, wait_ok, sleep_ok, ping_ok;
DWORD wait_status;
struct logger_t { template<class T> logger_t &operator<<(const T &) { return *this; } } logger;
#define BOOST_LOG(level) logger
void note_helper_start_failure(const char *) { ++failures; }
void terminate_helper_process() { ++terminations; }
bool cancellation_requested(const std::function<bool()> &) { return cancelled; }
bool operation_deadline_expired(auto) { return false; }
bool sleep_with_cancellation(auto, auto, auto) { return sleep_ok; }
bool wait_for_process_with_cancellation(auto, auto, auto, DWORD &result, auto) {
  result=wait_status; return wait_ok;
}
void GetExitCodeProcess(auto, DWORD *code) { *code=0; }
DWORD GetLastError() { return 6; }
namespace platf::display_helper_client {
  bool reset_connection_cancellable(auto, auto) { return reset_ok; }
  bool send_ping_fast(auto, auto) { ++fast_pings; return ping_ok; }
  bool send_ping_cancellable(auto, auto, auto) { return ping_ok; }
  bool send_ping() { return ping_ok; }
}
bool existing_helper(bool engine_matches, bool force_restart) {
  const bool legacy_engine=false, shutdown_class_caller=false;
  int h=1, pid=2;
  std::function<bool()> cancellation_predicate;
  auto operation_deadline=std::chrono::steady_clock::time_point::max();
@BRANCH@
  ++launches;
  return true;
}
void check(bool result, const char *message) {
  if (!result) { std::cerr << message << '\n'; std::exit(1); }
}
void reset() {
  terminations=fast_pings=launches=failures=0;
  cancelled=false; reset_ok=wait_ok=sleep_ok=ping_ok=true;
  wait_status=WAIT_OBJECT_0; g_running_helper_legacy=true;
}
int main() {
  for (auto status : {WAIT_TIMEOUT,WAIT_FAILED}) {
    reset(); wait_status=status;
    check(!existing_helper(false,true),"failed engine termination must fail closed");
    check(terminations==1 && fast_pings==0 && launches==0 && failures==1,
          "failed switch must neither ping nor launch");
    check(!g_running_helper_legacy,"failed switch must invalidate engine tracking");
  }
  reset();
  check(existing_helper(false,true),"successful switch should continue to launch");
  check(terminations==1 && fast_pings==0 && launches==1,"a different engine must be relaunched");
  reset(); wait_ok=false;
  check(!existing_helper(false,true) && launches==0,"cancelled process wait must not relaunch");
  reset(); reset_ok=false;
  check(!existing_helper(false,true) && terminations==0,"failed authority reset must not terminate");
  reset(); sleep_ok=false;
  check(!existing_helper(false,true) && launches==0,"cancelled switch settling must not launch");
  reset();
  check(existing_helper(true,false) && terminations==0 && launches==0,"matching live helper is reused");
  reset();
  check(existing_helper(true,true) && fast_pings==1 && terminations==0 && launches==0,
        "matching forced helper may pass its fast ping");
  reset(); ping_ok=false;
  check(existing_helper(true,true) && terminations==1 && launches==1,"failed fast ping restarts matching helper");
  std::cout << "PASS: engine timeout/failure, relaunch, cancellation, authority reset and reuse\n";
}
'''.replace('@BRANCH@', branch)
    with tempfile.TemporaryDirectory(prefix='helper-engine-') as directory:
        source = Path(directory) / 'engine.cpp'
        binary = Path(directory) / 'engine'
        source.write_text(program)
        subprocess.run([args.compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror',
                        str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
