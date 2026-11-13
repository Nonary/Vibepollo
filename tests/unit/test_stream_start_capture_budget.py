"""Exercise production budget publication, readback and capture retry sleeps."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler', default='g++')
    parser.add_argument('--video-revision')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    helper = (root / 'src/platform/windows/display_helper_integration.cpp').read_text()
    start = helper.index('      g_stream_start_deadline_us.store(')
    publish = helper[start:helper.index('\n      const std::function<bool()> startup_cancellation_predicate', start)]
    start = helper.index('  std::optional<std::chrono::milliseconds> remaining_stream_start_budget() {')
    remaining = helper[start:helper.index('\n  bool reactivate_owned_virtual_display(', start)]
    video = (subprocess.check_output(['git', '-C', str(root), 'show',
                                     f'{args.video_revision}:src/video.cpp'], text=True)
             if args.video_revision else (root / 'src/video.cpp').read_text())
    start = video.index('  void reset_display(')
    end = video.index('\n  /**', start)
    retry = video[start:end].replace('std::chrono::steady_clock::now()', 'Clock::now()')
    retry = retry.replace('std::this_thread::sleep_for(delay);', 'sleep_for(delay);')
    program = r'''
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#define _WIN32
using namespace std::literals;
struct Clock {
  static inline std::chrono::steady_clock::time_point time {1s};
  static auto now() { return time; }
};
std::vector<std::chrono::milliseconds> sleeps;
void sleep_for(std::chrono::milliseconds delay) { sleeps.push_back(delay); Clock::time+=delay; }
namespace display_helper_integration {
  std::atomic<std::int64_t> g_stream_start_deadline_us {0};
  enum class ApplyRetryPolicy {Full, StreamStart};
  std::int64_t now_steady_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
  }
  void publish(ApplyRetryPolicy retry_policy, std::chrono::steady_clock::time_point startup_deadline) {
@PUBLISH@
  }
@REMAINING@
  int age=0;
  int ms_since_last_apply() { return age; }
}
namespace capture_recovery {
  struct observations_t { int begin_capture() { return 7; } } state;
  auto &observations() { return state; }
}
namespace platf {
  using mem_type_e=int;
  using adapter_id_t=int;
  struct display_t { int recovery_capture_instance=0; };
  bool available=false;
  int attempts=0;
  std::shared_ptr<display_t> display(auto, auto, auto, auto) {
    ++attempts; return available ? std::make_shared<display_t>() : nullptr;
  }
}
struct config_t {};
@RETRY@
void check(bool condition, const char *message) {
  if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
void setup() {
  Clock::time=std::chrono::steady_clock::time_point {1s};
  sleeps.clear(); platf::attempts=0; platf::available=false;
  display_helper_integration::age=0;
}
int main() {
  using namespace display_helper_integration;
  setup();
  publish(ApplyRetryPolicy::StreamStart, Clock::now()+250ms);
  check(remaining_stream_start_budget()==250ms,"published stream-start budget");
  std::shared_ptr<platf::display_t> output;
  reset_display(output,0,"display",{});
  check(Clock::now()==std::chrono::steady_clock::time_point{1250ms},"capture sleeps must stop at shared deadline");
  check(platf::attempts==5 && sleeps==std::vector{250ms,0ms,0ms,0ms,0ms},"bounded retry ladder");
  check(remaining_stream_start_budget()==0ms,"expired budget is clamped");
  setup();
  publish(ApplyRetryPolicy::StreamStart, Clock::now()-1ms);
  reset_display(output,0,"display",{});
  check(platf::attempts==2 && sleeps==std::vector{0ms,0ms},"expired budget must not extend retries or sleep");
  setup();
  publish(ApplyRetryPolicy::Full, Clock::now()+10s);
  check(!remaining_stream_start_budget(),"full apply clears the bounded budget");
  reset_display(output,0,"display",{});
  check(sleeps==std::vector{300ms,400ms,500ms,600ms,700ms},"unbounded apply keeps full settling ladder");
  setup(); age=6000;
  reset_display(output,0,"display",{});
  check(sleeps==std::vector{200ms,300ms},"outside post-apply window keeps normal retries");
  setup(); platf::available=true;
  publish(ApplyRetryPolicy::StreamStart, Clock::now()+250ms);
  reset_display(output,0,"display",{});
  check(output && output->recovery_capture_instance==7 && sleeps.empty(),"successful capture keeps recovery tracking");
  std::cout << "PASS: budget publication, expiry, capped retries, full apply and capture success\n";
}
'''.replace('@PUBLISH@', publish).replace('@REMAINING@', remaining).replace('@RETRY@', retry)
    with tempfile.TemporaryDirectory(prefix='stream-start-budget-') as directory:
        source = Path(directory) / 'budget.cpp'
        binary = Path(directory) / 'budget'
        source.write_text(program)
        subprocess.run([args.compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror',
                        str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
