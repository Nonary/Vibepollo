"""Compile and exercise the production HeartbeatMonitor class in isolation."""
import pathlib
import subprocess
import sys
import tempfile


root = pathlib.Path(sys.argv[1]).resolve()
compiler = sys.argv[2]
runtime = (root / "src/platform/windows/display_helper_v2/runtime_support.h").read_text()
start = runtime.index("  class HeartbeatMonitor {")
open_brace = runtime.index("{", start)
depth = 0
end = None
for index in range(open_brace, len(runtime)):
    if runtime[index] == "{":
        depth += 1
    elif runtime[index] == "}":
        depth -= 1
        if depth == 0:
            end = runtime.index(";", index) + 1
            break
assert end is not None, "could not extract the production HeartbeatMonitor class"
heartbeat_class = runtime[start:end]

test_source = r"""
#include <chrono>
#include <mutex>
#include <optional>

namespace display_helper::v2 {
  class IClock {
  public:
    virtual ~IClock() = default;
    virtual std::chrono::steady_clock::time_point now() = 0;
  };
""" + heartbeat_class + r"""
  class FakeClock final : public IClock {
  public:
    std::chrono::steady_clock::time_point now() override { return now_; }
    void advance(std::chrono::steady_clock::duration duration) { now_ += duration; }
  private:
    std::chrono::steady_clock::time_point now_ {};
  };
}

int main() {
  using namespace std::chrono;
  display_helper::v2::FakeClock clock;
  display_helper::v2::HeartbeatMonitor heartbeat(clock);
  heartbeat.arm();
  clock.advance(seconds(31));
  if (heartbeat.check_timeout()) return 1; // Starts a 2-minute recovery deadline.
  clock.advance(seconds(60));
  heartbeat.record_liveness_ping();
  clock.advance(seconds(59));
  if (heartbeat.check_timeout()) return 2;
  clock.advance(seconds(1));
  if (!heartbeat.check_timeout()) return 3; // Liveness must not move that deadline.
  return 0;
}
"""

with tempfile.TemporaryDirectory(prefix="vibeshine-recovery-liveness-") as temporary:
    source = pathlib.Path(temporary) / "heartbeat_test.cpp"
    executable = pathlib.Path(temporary) / "heartbeat_test"
    source.write_text(test_source)
    subprocess.run([compiler, "-std=c++23", str(source), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
