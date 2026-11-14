#!/usr/bin/env python3
"""Behavioral regression tests for the production Dolby session worker.

The C++ fixture includes and executes the same worker and capture-cleanup
templates used by audio.cpp. A controllable encoder injects initialization and
encode failures; a second session verifies shared packet delivery remains alive.
No FFmpeg library, audio device, GPU, or running host is required.
"""
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[2]
compiler = sys.argv[2] if len(sys.argv) > 2 else "c++"
source = (root / "src/audio.cpp").read_text()

if '#include "dolby_session_worker.h"' not in source:
    raise AssertionError("audio.cpp must include the tested production worker helpers")
if "dolby::run_encoder_worker(" not in source:
    raise AssertionError("the Dolby production path must use the tested encoder worker")
dolby_start = source.index("    if (config.use_ac3 || config.use_eac3) {")
dolby_end = source.index("    opus_t opus {", dolby_start)
if "packets->stop();" in source[dolby_start:dolby_end]:
    raise AssertionError("the Dolby production path must not stop the shared audio packet queue")
if "dolby::finish_capture_session(" not in source:
    raise AssertionError("capture shutdown must use the tested production cleanup helper")

program = r"""
#include "src/dolby_session_worker.h"

#include <atomic>
#include <cassert>
#include <deque>
#include <iostream>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

struct SampleQueue {
  bool running = true;
  std::deque<std::vector<float>> samples;

  void stop() { running = false; }
  void push(std::vector<float> sample) { samples.push_back(std::move(sample)); }
  std::optional<std::vector<float>> pop() {
    if (!running || samples.empty()) return std::nullopt;
    auto sample = std::move(samples.front());
    samples.pop_front();
    return sample;
  }
};

struct SharedPacketQueue {
  bool running = true;
  std::vector<int> packets;

  void stop() { running = false; }
  void raise(int packet) {
    if (running) packets.push_back(packet);
  }
};

struct FakeEncoder {
  bool initialize_ok = true;
  int encode_result = 1;

  bool initialize() { return initialize_ok; }
  int encode(const std::vector<float>&, std::vector<int>&) { return encode_result; }
};

struct CaptureResource {
  bool active = true;
  void release() { active = false; }
};

int main() {
  SharedPacketQueue shared_packets;

  // Session A: encoder initialization fails. The production worker must stop
  // only A's sample queue and leave the shared packet queue operational.
  SampleQueue init_failure_samples;
  FakeEncoder init_failure_encoder;
  init_failure_encoder.initialize_ok = false;
  const bool init_result = audio::dolby::run_encoder_worker(
    init_failure_samples, init_failure_encoder,
    [](auto& encoder) { return encoder.initialize(); },
    [](auto& encoder, const auto& sample, auto& packet) { return encoder.encode(sample, packet); },
    [] { return std::vector<int>{}; },
    [&](const auto&, auto packet, int bytes) { packet.push_back(bytes); shared_packets.raise(packet.back()); }
  );
  assert(!init_result);
  assert(!init_failure_samples.running);
  assert(shared_packets.running);

  // Session B: initialization succeeds but encoding fails on its first sample.
  SampleQueue encode_failure_samples;
  encode_failure_samples.push({0.25f, -0.25f});
  FakeEncoder encode_failure_encoder;
  encode_failure_encoder.encode_result = -1;
  const bool encode_result = audio::dolby::run_encoder_worker(
    encode_failure_samples, encode_failure_encoder,
    [](auto& encoder) { return encoder.initialize(); },
    [](auto& encoder, const auto& sample, auto& packet) { return encoder.encode(sample, packet); },
    [] { return std::vector<int>{}; },
    [&](const auto&, auto packet, int bytes) { packet.push_back(bytes); shared_packets.raise(packet.back()); }
  );
  assert(!encode_result);
  assert(!encode_failure_samples.running);
  assert(shared_packets.running);

  // A healthy unrelated session can still deliver packets after both failures.
  shared_packets.raise(101);
  shared_packets.raise(102);
  assert(shared_packets.running);
  assert((shared_packets.packets == std::vector<int>{101, 102}));

  // The successful production path still publishes encoded packets.
  SampleQueue success_samples;
  success_samples.push({0.5f, -0.5f});
  FakeEncoder success_encoder;
  const bool success_result = audio::dolby::run_encoder_worker(
    success_samples, success_encoder,
    [](auto& encoder) { return encoder.initialize(); },
    [](auto& encoder, const auto& sample, auto& packet) { return encoder.encode(sample, packet); },
    [] { return std::vector<int>{}; },
    [&](const auto&, auto packet, int bytes) { packet.push_back(bytes); shared_packets.raise(packet.back()); }
  );
  assert(success_result);
  assert(shared_packets.running);
  assert((shared_packets.packets == std::vector<int>{101, 102, 1}));

  // The production cleanup helper must stop the worker queue, join its thread,
  // wait for shutdown, and only then release the failed session's capture.
  auto capture_samples = std::make_shared<SampleQueue>();
  std::atomic<bool> worker_finished {false};
  std::thread capture_worker([&] { worker_finished.store(true); });
  CaptureResource capture;
  bool shutdown_observed = false;
  audio::dolby::finish_capture_session(
    capture_samples, capture_worker,
    [&] { shutdown_observed = true; },
    [&] { assert(shutdown_observed); capture.release(); }
  );
  assert(!capture_samples->running);
  assert(!capture_worker.joinable());
  assert(worker_finished.load());
  assert(shutdown_observed);
  assert(!capture.active);

  std::cout << "Dolby init/encode failures isolated; unrelated delivery and capture cleanup verified\n";
}
"""

with tempfile.TemporaryDirectory(prefix="dolby-session-isolation-") as temporary:
    cpp = pathlib.Path(temporary) / "test.cpp"
    executable = pathlib.Path(temporary) / ("test.exe" if pathlib.Path(compiler).name.lower() in {"cl", "cl.exe", "clang-cl", "clang-cl.exe"} else "test")
    cpp.write_text(program)
    compiler_name = pathlib.Path(compiler).name.lower()
    if compiler_name in {"cl", "cl.exe", "clang-cl", "clang-cl.exe"}:
        compile_command = [
            compiler, "/nologo", "/std:c++20", "/O2", "/W4", "/WX", "/EHsc",
            f"/I{root}", str(cpp), f"/Fe:{executable}",
        ]
    else:
        compile_command = [
            compiler, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror",
            f"-I{root}", str(cpp), "-o", str(executable),
        ]
    subprocess.run(compile_command, check=True)
    result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=5, check=True)
    print(result.stdout, end="")
