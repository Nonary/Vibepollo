"""Execute the production RFI/IDR and repeat branches with fake capture/driver edges.

This is a CPU control-flow regression, not a live stream or GPU test. Source
extraction keeps the session fallback and loop decisions under test verbatim;
assertions inspect encoded output and capture waits, not source text.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def block(text, marker):
    start = text.index(marker)
    opening = text.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def fixture(root):
    video = (root / 'src/video.cpp').read_text()
    header = (root / 'src/video.h').read_text()
    interface = next(line.strip() for line in header.splitlines()
                     if 'virtual ' in line and 'invalidate_ref_frames(' in line)
    request_start = video.index('      bool requested_idr_frame = std::exchange(retry_idr, false);')
    requests = video[request_start:video.index(
        '      std::optional<std::chrono::steady_clock::time_point> frame_timestamp;', request_start)]
    wait_start = video.index('if (!requested_idr_frame || images->peek()) {', request_start)
    wait_gate = video[wait_start:video.index('{', wait_start) + 1]
    repeat = block(video[wait_start:], 'if (!converted.encode) {')
    sessions = ''
    for backend in ('nvenc', 'amf'):
        session = block(video, f'  class {backend}_encode_session_t:')
        invalidation_start = session.index('invalidate_ref_frames(int64_t')
        method_start = session.rfind('\n', 0, invalidation_start) + 1
        method = block(session, session[method_start:invalidation_start] + 'invalidate_ref_frames(')
        request = block(session, 'void request_idr_frame() override {')
        sessions += f'''
struct {backend}_session : Interface {{
  Device *device;
  bool force_idr = false;
  explicit {backend}_session(Device *value) : device(value) {{}}
  {method}
  {request}
}};
'''
    return r'''
#include "src/video_policy.h"
#include <cstdint>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>
using namespace std::chrono_literals;
namespace policy = video::policy;
void check(bool ok, const char *message) {
  if (!ok) throw std::runtime_error(message);
}
struct Driver {
  std::deque<bool> answers;
  unsigned calls = 0;
  bool invalidate_ref_frames(int64_t first, int64_t last) {
    check(first == 90 && last == 90, "forward the client's range unchanged");
    ++calls;
    bool success = answers.front();
    answers.pop_front();
    return success;
  }
};
struct Device { Driver *nvenc; Driver *amf; };
struct Interface {
  virtual ~Interface() = default;
  virtual void request_idr_frame() = 0;
''' + interface + '\n};\n' + sessions + r'''
struct Events {
  unsigned pending;
  bool peek() const { return pending != 0; }
  std::optional<std::pair<int64_t, int64_t>> pop(std::chrono::milliseconds = 0ms) {
    if (!pending) return {};
    --pending;
    return std::pair<int64_t, int64_t>{90, 90};
  }
};
struct Images {
  unsigned repeats;
  int waited_ms = 0;
  unsigned pops = 0;
  bool peek() const { return repeats != 0; }
  bool pop(int budget_ms) {
    ++pops;
    if (repeats) { --repeats; return true; }
    waited_ms += budget_ms;
    return false;
  }
};
struct Result { int wait_ms; bool idr; unsigned invalidations; unsigned pops; };
template<class Session>
Result run(std::deque<bool> answers, bool explicit_idr, unsigned repeats) {
  Events invalidations{static_cast<unsigned>(answers.size())}, idrs{explicit_idr ? 1u : 0u};
  auto *invalidate_ref_frames_events = &invalidations;
  auto *idr_events = &idrs;
  Driver driver{std::move(answers)};
  Device device{&driver, &driver};
  Session encoder{&device};
  auto *session = &encoder;
  Images capture{repeats};
  auto *images = &capture;
  struct { unsigned repeated_inputs = 0; } loop_stats;
  bool retry_idr = false;
  // The converted output is already cached. Only capture/driver edges are fake.
  for (unsigned iteration = 0; iteration < repeats + 2; ++iteration) {
''' + requests + '\n' + wait_gate + r'''
      if (images->pop(100)) {
        struct Converted { bool repeats_previous; std::optional<std::chrono::steady_clock::time_point> frame_timestamp, host_processing_timestamp; };
        const auto converted = policy::resolve_converted_input(Converted{true, {}, {}}, {}, {});
''' + repeat + r'''
      }
    }
    check(loop_stats.repeated_inputs == repeats, "drain every queued duplicate");
    // Observe the IDR submitted to the encoder and the accumulated capture wait.
    return {capture.waited_ms, encoder.force_idr, driver.calls, capture.pops};
  }
  throw std::runtime_error("recovery did not submit a frame");
}
template<class Session> void scenarios() {
  for (unsigned repeats : {1u, 3u, 0u}) {
    auto fallback = run<Session>({false}, false, repeats);
    if (fallback.wait_ms != 0) {
      std::cerr << "RFI fallback after " << repeats << " queued repeats waited " << fallback.wait_ms << "ms\n";
    }
    check(fallback.idr && fallback.wait_ms == 0, "RFI fallback must encode IDR without capture wait");
    check(fallback.invalidations == 1 && fallback.pops == repeats, "fallback consumes once without an empty pop");
    auto success = run<Session>({true}, false, repeats);
    check(!success.idr && success.wait_ms == 100, "successful invalidation preserves normal capture cadence");
    auto explicit_request = run<Session>({}, true, repeats);
    check(explicit_request.idr && explicit_request.wait_ms == 0, "explicit IDR remains immediate");
    for (const auto &answers : {std::deque<bool>{false, true}, std::deque<bool>{true, false}}) {
      auto mixed = run<Session>(answers, false, repeats);
      check(mixed.idr && mixed.wait_ms == 0 && mixed.invalidations == 2, "coalescing must retain fallback and process every range");
    }
    auto both = run<Session>({true}, true, repeats);
    check(both.idr && both.wait_ms == 0 && both.invalidations == 1, "explicit IDR and successful RFI coexist");
  }
}
int main() {
  try {
    scenarios<nvenc_session>();
    scenarios<amf_session>();
    std::cout << "PASS: 36 NVENC/AMF recovery scenarios; capture waits and IDR decisions verified\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', default='c++')
    parser.add_argument('--repo-root', type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='video-rfi-retry-') as directory:
        source = Path(directory) / 'fixture.cpp'
        binary = Path(directory) / 'fixture'
        source.write_text(fixture(args.repo_root))
        subprocess.run([args.compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror',
                        '-I', str(args.repo_root), str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
