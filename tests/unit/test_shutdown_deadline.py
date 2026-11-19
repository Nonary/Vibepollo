"""Exercise the production deadline even when the logger cannot make progress."""
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
    if args.revision:
        text = subprocess.check_output(['git', '-C', str(root), 'show',
                                        f'{args.revision}:src/main.cpp'], text=True)
    else:
        text = (root / 'src/main.cpp').read_text()
    start = text.index('  class shutdown_deadline_t {')
    deadline = text[start:text.index('\n}  // namespace', start)]
    # Accelerate only elapsed time; keep the actual worker, condition variable,
    # completion serialization and emergency-exit ordering.
    deadline = deadline.replace('std::chrono::seconds(15)', 'std::chrono::milliseconds(100)')
    deadline = deadline.replace('std::chrono::seconds(10)', 'std::chrono::milliseconds(100)')
    program = r'''
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string_view>
#include <system_error>
#include <thread>
#include "src/jthread.h"
#include "src/shutdown_phase.h"
using namespace std::literals;
bool blocked_log;
struct logger_t {
  template<class T> logger_t &operator<<(const T &) {
    if (blocked_log) for (;;) std::this_thread::sleep_for(1s);
    return *this;
  }
} logger;
#define BOOST_LOG(level) logger
namespace logging { void log_flush() { logger << "flush"; } }
namespace lifetime {
  constexpr int desired_exit_code = 23;
  void debug_trap() { std::_Exit(24); }
}
@DEADLINE@
int main(int argc, char **argv) {
  if (argc != 2) return 2;
  const std::string_view mode = argv[1];
  blocked_log = mode == "machine" || mode == "signal" || mode == "complete";
  std::atomic_bool requested {mode == "signal"};
  shutdown_phase::tracker_t phase;
  shutdown_deadline_t watchdog {&requested, mode != "ordinary", phase};
  if (mode != "signal") watchdog.arm();
  if (mode == "complete") {
    watchdog.complete();
    std::this_thread::sleep_for(200ms);
    return 0;
  }
  std::this_thread::sleep_for(2s);
  return 99;
}
'''.replace('@DEADLINE@', deadline)
    with tempfile.TemporaryDirectory(prefix='shutdown-deadline-') as directory:
        source = Path(directory) / 'deadline.cpp'
        binary = Path(directory) / 'deadline'
        source.write_text(program)
        subprocess.run([args.compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror',
                        '-pthread', '-I', str(root), str(source), '-o', str(binary)], check=True)
        for case, expected in [('machine', 23), ('signal', 23), ('ordinary', 24), ('complete', 0)]:
            result = subprocess.run([str(binary), case], timeout=3)
            if result.returncode != expected:
                raise AssertionError(f'{case}: expected exit {expected}, got {result.returncode}')
            print(f'PASS: shutdown deadline {case}')


if __name__ == '__main__':
    main()
