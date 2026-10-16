"""CPU regression fixture for shared capture demand, not native FPS evidence.

Compile the actual async subscription callback, display-create/reset calls and
Windows rate initialization/selection with a fake display. Count distinct
capture IDs separately from simulated encoded packets; never open a display.
"""
from pathlib import Path
import argparse
import subprocess
import sys
import tempfile


def block(text, marker):
    start = text.index(marker)
    opening = text.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end + 1]


def fixture(root):
    video = (root / 'src/video.cpp').read_text()
    capture = block(video, '  void captureThread(')
    callback = block(capture, 'auto push_captured_image_callback = ')
    create = next(line.strip() for line in capture.splitlines() if 'disp = platf::display(' in line)
    reset = next(line.strip() for line in capture.splitlines() if 'reset_display(disp,' in line)
    initial_loop = capture.index('while (capture_ctx_queue->running()) {')
    initial = capture[capture.index('{', initial_loop) + 1:capture.index('if (capture_config.capture_source', initial_loop)]
    reinit = capture.index('case platf::capture_e::reinit:')
    retry_loop = capture.index('while (capture_ctx_queue->running()) {', capture.index('std::size_t display_retry_failures = 0;', reinit))
    retry = capture[capture.index('{', retry_loop) + 1:capture.index('// Release the display before', retry_loop)]
    # The existing production reinit must release the old producer first.
    assert capture.index('disp.reset();', retry_loop) < capture.index(reset, retry_loop)
    windows = (root / 'src/platform/windows/display_base.cpp').read_text()
    rate_init = windows[windows.index('client_frame_rate = config.framerate;'):windows.index('\n    HRESULT status;', windows.index('client_frame_rate = config.framerate;'))]
    rate_adjust = block(windows, 'auto adjust_client_frame_rate = ')
    pacing_method = ''
    if 'auto capture_pacing_for_display = ' in capture:
        pacing = block(capture, 'auto capture_pacing_for_display = ')
        body = pacing[pacing.index('{') + 1:pacing.rfind('}')]
        # Exercise the Windows branch with the fake DXGI display below, even
        # when the compiler host is Linux. Other capture code stays verbatim.
        body = body.replace('#ifdef _WIN32', '').replace('#endif', '')
        pacing_method = 'video::capture_pacing_context_t capture_pacing_for_display() { using video::capture_pacing_context_t;' + body + '}\n'
    parity = ''
    if 'capture_pacing_context_t' in (root / 'src/video_capture_source.h').read_text():
        parity = r'''
  int comparisons = 0;
  for (DXGI_RATIONAL refresh : {DXGI_RATIONAL {0,1}, {60,1}, {60000,1001}, {120000,1001}, {144000,1001}}) {
    const int rounded = refresh.Numerator ? static_cast<int>(std::lround(double(refresh.Numerator) / refresh.Denominator)) : 0;
    for (int requested : {24, 30, 60, 90, 120, 144, 240}) {
      for (int strict : {0, 2398, 5994, 5995, 6000}) {
        platf::desktop_refresh = refresh; platf::desktop_refresh_rounded = rounded;
        config_t config {requested, strict, requested * 1000};
        platf::display_t display(config);
        const auto actual = display.rate();
        const auto selected = video::capture_pacing(config, {{static_cast<int>(refresh.Numerator), static_cast<int>(refresh.Denominator)}, rounded, true});
        check(std::int64_t(actual.Numerator) * selected.denominator == std::int64_t(selected.numerator) * actual.Denominator,
              "demand rate matches actual Windows adjustment");
        ++comparisons;
      }
    }
  }
  std::cout << "Windows effective-rate parity comparisons=" << comparisons << '\n';
'''
    return r'''
#include "src/video_capture_source.h"
#include "src/video_policy.h"
#include <cmath>
#include <iostream>
#include <future>
#include <set>
#include <vector>
using namespace std::chrono_literals;
using UINT = unsigned;
struct DXGI_RATIONAL { UINT Numerator, Denominator; };
struct AVRational { int num, den; };
namespace video {
AVRational framerateX100_to_rational(int value) {
  auto rate = policy::framerate_x100_to_rational(value);
  return {rate.numerator, rate.denominator};
}
}
#define BOOST_LOG(level) std::cerr
struct config_t {
  int framerate = 30;
  int framerateX100 = 0;
  int encodingFramerate = 30000;
  int width = 1920;
  video::capture_source_e capture_source = video::capture_source_e::active_output;
};
namespace platf {
struct img_t { int capture_id; };
int active = 0, peak_active = 0, generations = 0;
DXGI_RATIONAL desktop_refresh {0, 1};
int desktop_refresh_rounded = 0;
struct display_t {
  int client_frame_rate = 0;
  DXGI_RATIONAL client_frame_rate_strict {0, 0};
  DXGI_RATIONAL display_refresh_rate {0, 1};
  int display_refresh_rate_rounded = 0;
  explicit display_t(const config_t &config) {
    display_refresh_rate = desktop_refresh;
    display_refresh_rate_rounded = desktop_refresh_rounded;
''' + rate_init + r'''
    ++active; peak_active = std::max(peak_active, active); ++generations;
  }
  ~display_t() { --active; }
  void request_refresh() {}
  DXGI_RATIONAL rate() {
''' + rate_adjust + r'''
    return adjust_client_frame_rate();
  }
};
namespace dxgi { using display_base_t = display_t; }
std::shared_ptr<display_t> display(int, const std::string &, const config_t &config) {
  return std::make_shared<display_t>(config);
}
}
void reset_display(std::shared_ptr<platf::display_t> &disp, int type, const std::string &name, const config_t &config) {
  disp = platf::display(type, name, config);
}
using image_event_t = safe::event_t<std::shared_ptr<platf::img_t>>;
struct capture_ctx_t { std::shared_ptr<image_event_t> images; config_t config; };
using video::capture_config_for_consumers;
using video::same_capture_pacing;
using video::await_capture_config;
int failures = 0;
void check(bool value, const char *label) {
  if (!value) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
}
struct run_t {
  std::vector<capture_ctx_t> capture_ctxs;
  std::shared_ptr<safe::queue_t<capture_ctx_t>> capture_ctx_queue = std::make_shared<safe::queue_t<capture_ctx_t>>(30);
  std::shared_ptr<platf::display_t> disp;
  config_t producer_config {};
  struct formats_t { int dev_type = 0; } formats;
  struct encoder_t { formats_t *platform_formats; } encoder {&formats};
  std::vector<std::string> display_names {"requested-output"};
  int display_p = 0;
  bool event_driven_capture;
  bool artificial_reinit = false;
  auto add(int fps, int x100 = 0) {
    auto images = std::make_shared<image_event_t>();
    config_t config {fps, x100, fps * 1000};
    if (capture_ctxs.empty() && !disp) capture_ctxs.push_back({images, config});
    else check(capture_ctx_queue->try_raise(capture_ctx_t {images, config}), "admit consumer");
    return images;
  }
  explicit run_t(bool event_driven, DXGI_RATIONAL refresh = {0, 1}, int rounded = 0): event_driven_capture(event_driven) {
    check(platf::active == 0, "previous producer released");
    platf::peak_active = 0; platf::generations = 0;
    platf::desktop_refresh = refresh; platf::desktop_refresh_rounded = rounded;
  }
''' + pacing_method + r'''
  void start() {
''' + initial + create + r'''
  }
  void recreate() {
''' + retry + r'''
    disp.reset();
''' + reset + r'''
  }
  bool tick() {
    auto switch_display_event = std::make_shared<safe::event_t<int>>();
    std::uint64_t switch_display_generation = 0;
    std::optional<std::string> pending_switch_output;
    const auto manual_switch_selection = video::policy::capture_selection_e::process_preferred;
''' + callback + r'''
    auto img = std::make_shared<platf::img_t>(platf::img_t {platf::generations});
    const bool continued = push_captured_image_callback(std::move(img), true);
    if (artificial_reinit) {
      check(!continued, "demand change leaves capture before recreation");
      recreate();
      artificial_reinit = false;
    }
    return continued;
  }
  bool at(unsigned n, unsigned d = 1) {
    const auto actual = disp->rate();
    return std::uint64_t(actual.Numerator) * d == std::uint64_t(n) * actual.Denominator;
  }
  int distinct_capture_ids(int simulated_packets) {
    const auto rate = disp->rate();
    std::set<std::uint64_t> ids;
    std::set<std::uint64_t> capture_timestamps_ns;
    for (int i = 0; i < simulated_packets; ++i) {
      // Sample at packet-interval midpoints. Repeated packets keep the previous
      // capture ID/timestamp; their count cannot establish fresh-source cadence.
      const auto capture_id = std::uint64_t(2 * i + 1) * rate.Numerator / (2ULL * simulated_packets * rate.Denominator);
      ids.insert(capture_id);
      capture_timestamps_ns.insert(capture_id * 1000000000ULL * rate.Denominator / rate.Numerator);
    }
    check(ids.size() == capture_timestamps_ns.size(), "fresh capture IDs and timestamps agree independently of packet count");
    return static_cast<int>(ids.size());
  }
};
int main() {
''' + parity + r'''
  for (bool event_driven : {false, true}) {
    {
      run_t run(event_driven);
      auto first = run.add(30); run.start(); first->stop();
      check(run.tick(), "empty callback must retain worker while admission queue stays open");
      auto pending = run.add(60, 6000); run.tick();
      check(run.at(60), "late join after empty callback still receives capture");
    }
    {
      run_t run(event_driven);
      auto first = run.add(30); auto pending = run.add(60, 6000);
      first->stop(); run.start();
      check(bool(run.disp), "stopped initial subscriber must not strand pending live join");
      if (run.disp) check(run.at(60), "pending live join supplies initial demand");
      check(pending->running(), "admitted pending subscriber stays live");
    }
    {
      run_t run(event_driven);
      auto first = run.add(30); run.start(); auto pending = run.add(60, 6000);
      first->stop(); run.capture_ctxs.clear(); run.recreate();
      check(run.at(60), "empty retry vector must admit queued live join");
      check(pending->running(), "retry keeps pending join live");
      check(platf::peak_active == 1, "pending retry never overlaps producers");
    }
    {
      run_t run(event_driven, {120000, 1001}, 120);
      auto automatic = run.add(60); run.start();
      check(run.at(60000, 1001), "automatic60 adjusts to actual fractional desktop");
      auto strict = run.add(60, 6000); run.tick();
      check(run.at(60), "strict60 join promotes automatic59.94 to exact60");
      strict->stop(); run.tick();
      check(run.at(60000, 1001), "strict departure restores automatic adjustment");
      check(platf::peak_active == 1, "auto/strict transitions keep one producer");
    }
    {
      run_t run(event_driven, {120000, 1001}, 120);
      auto strict = run.add(60, 6000); run.start(); auto automatic = run.add(60); run.tick();
      check(run.at(60) && platf::generations == 1, "strict-first join preserves exact60 without recreation");
      strict->stop(); run.tick();
      check(run.at(60000, 1001), "strict-first departure adopts automatic59.94");
    }
    {
      run_t run(event_driven, {120000, 1001}, 120);
      auto automatic = run.add(60); run.start(); auto strict = run.add(60, 5995); run.tick();
      check(run.at(5995, 100), "strict59.95 exceeds adjusted auto60 even with lower nominal rate");
      run.recreate();
      check(run.at(5995, 100), "fractional retry selects effective maximum");
    }
    {
      run_t run(event_driven, {120, 1}, 120);
      auto automatic = run.add(60); run.start(); auto strict = run.add(60, 6000); run.tick();
      check(run.at(60) && platf::generations == 1, "integral desktop keeps equal effective auto/strict pace");
    }
    {
      run_t run(event_driven);
      auto low = run.add(30); run.start();
      check(run.at(30), "solo30");
      auto high = run.add(60); run.tick();
      std::cout << "30->60 event_driven=" << event_driven << " simulated_packets=60 distinct_capture_ids=" << run.distinct_capture_ids(60) << '\n';
      check(run.at(60), "30->60 raises capture rate");
      check(run.distinct_capture_ids(60) == 60, "60 distinct source IDs rather than repeated packets");
      low->stop(); run.tick();
      check(run.at(60), "first low-rate client departure retains high-rate demand");
      run.recreate();
      check(run.at(60), "display recreation recomputes live demand");
      check(run.capture_ctxs.front().config.framerate == 60, "encoder config preserved after first departure");
      check(platf::peak_active == 1, "one producer during promotion and recreation");
    }
    {
      run_t run(event_driven);
      auto high = run.add(60); run.start(); auto low = run.add(30); run.tick();
      check(run.at(60), "60->30 preserves maximum demand");
      check(platf::generations == 1, "lower join does not recreate");
      high->stop(); run.tick();
      check(run.at(30), "fast first consumer departure lowers demand");
      check(run.distinct_capture_ids(60) == 30, "lower demand still distinguishes repeated packets");
      check(platf::peak_active == 1, "one producer during departure");
    }
    {
      run_t run(event_driven);
      auto first = run.add(60); run.start(); auto second = run.add(60); run.tick();
      check(run.at(60) && platf::generations == 1, "equal-rate join does not recreate");
      first->stop(); run.tick();
      check(run.at(60) && platf::generations == 1, "equal first departure does not recreate");
    }
    {
      run_t run(event_driven);
      auto fractional = run.add(60, 5994); run.start();
      check(run.at(60000, 1001), "fractional solo rate");
      auto integer = run.add(60); run.tick();
      check(run.at(60), "integer60 exceeds 59.94 despite equal integer fields");
      integer->stop(); run.tick();
      check(run.at(60000, 1001), "restore fractional demand after faster departure");
      check(run.capture_ctxs.front().config.framerateX100 == 5994, "original fractional encoder config retained");
      check(platf::peak_active == 1, "fractional transitions never overlap producers");
    }
    {
      run_t run(event_driven);
      auto fractional = run.add(120, 11988); run.start();
      check(run.at(120000, 1001), "119.88 solo rate");
      auto integer = run.add(120); run.tick();
      check(run.at(120), "120 exceeds 119.88 despite equal integer fields");
      integer->stop(); run.tick();
      check(run.at(120000, 1001), "restore 119.88 after faster departure");
      check(platf::peak_active == 1, "119.88 transitions never overlap producers");
    }
    {
      run_t run(event_driven);
      auto first = run.add(24, 2397); run.start(); auto second = run.add(24, 2398); run.tick();
      check(run.at(24000, 1001) && platf::generations == 1, "equivalent fractional aliases avoid recreation");
    }
  }
  check(platf::active == 0, "final producer released");
  if (!failures) std::cout << "PASS: 26 source-derived scenarios; pending admission, effective Windows pacing and single producer checked\n";
  return failures ? 1 : 0;
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('root', type=Path)
    parser.add_argument('compiler', nargs='?', default='g++')
    parser.add_argument('--include-directory', action='append', default=[])
    args = parser.parse_args(sys.argv[1:])
    root = args.root.resolve()
    compiler = args.compiler
    include_flags = ['-I' + directory for value in args.include_directory
                     for directory in value.split(';') if directory]
    text = fixture(root)
    # Before the repair these helpers do not exist. They are unused by the
    # shipping callback, allowing the exact same fixture to expose its behavior.
    if 'capture_config_for_consumers' not in (root / 'src/video_capture_source.h').read_text():
        text = text.replace('using video::capture_config_for_consumers;', '').replace('using video::same_capture_pacing;', '')
    if 'await_capture_config' not in (root / 'src/video_capture_source.h').read_text():
        text = text.replace('using video::await_capture_config;', '')
    with tempfile.TemporaryDirectory(prefix='capture-demand-') as directory:
        source = Path(directory) / 'fixture.cpp'
        source.write_text(text)
        binary = Path(directory) / 'fixture'
        subprocess.run([compiler, '-std=c++23', '-pthread', '-I' + str(root), *include_flags, str(source),
                        str(root / 'src/video_policy.cpp'), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
