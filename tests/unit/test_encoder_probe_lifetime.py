#!/usr/bin/env python3
"""Execute production validate_encoder with deterministic GPU/codec boundaries.

No Windows or GPU APIs execute. The source body and any legacy thread-local
cache declarations are compiled unchanged, to check actual ownership on exit.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repository", type=Path)
    parser.add_argument("compiler", nargs="?", default="c++")
    parser.add_argument("--source-file", type=Path)
    args = parser.parse_args()
    source = (args.source_file or args.repository / "src/video.cpp").read_text()
    start = source.index("  bool validate_encoder(")
    end = source.index("{", start)
    depth = 1
    while depth:
        end += 1
        depth += (source[end] == "{") - (source[end] == "}")
    body = source[start:end + 1]
    legacy_cache = "\n".join(re.findall(
        r"^  static thread_local .* cached_(?:probe_display|display_type).*;$",
        source, re.MULTILINE))
    prefix = r'''
#include <bitset>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#define _WIN32
#define BOOST_LOG(level) log_output
using namespace std::literals;
std::ostringstream log_output;
struct config_t {
  int width, height, framerate, bitrate, slices, refs, csc, videoFormat,
      dynamicRange, unused, chromaSamplingType;
};
int live = 0, created = 0, peak = 0;
bool hdr = false, unsupported = false, invalid = false, throws = false;
namespace platf {
  using adapter_id_t = int;
  enum class mem_type_e { system, gpu };
  struct display_t {
    display_t() { ++created; peak = std::max(peak, ++live); }
    ~display_t() { --live; }
    std::optional<adapter_id_t> capture_adapter_id() { return 7; }
    bool is_codec_supported(const std::string &, const config_t &config) {
      return !unsupported && (!config.dynamicRange || hdr);
    }
  };
}
namespace util {
  template<class F> struct guard_t {
    F callback;
    bool enabled = true;
    ~guard_t() { if (enabled) callback(); }
    void disable() { enabled = false; }
  };
  template<class F> guard_t<F> fail_guard(F callback) { return {callback}; }
}
namespace config {
  namespace flag { constexpr int FORCE_VIDEO_HEADER_REPLACE = 0; }
  struct { std::bitset<1> flags; } sunshine;
}
namespace video {
  constexpr int H264_ONLY = 1, YUV444_SUPPORT = 2;
  enum validate_flag_e { VUI_PARAMS = 1 };
  int active_hevc_mode = 1, active_av1_mode = 1;
  struct encoder_t {
    enum flag_e { VUI_PARAMETERS, REF_FRAMES_RESTRICT, PASSED, YUV444, DYNAMIC_RANGE };
    struct codec_t {
      std::string name;
      std::bitset<5> capabilities;
      auto operator[](flag_e flag) { return capabilities[flag]; }
    };
    struct formats_t { platf::mem_type_e dev_type = platf::mem_type_e::gpu; } formats;
    formats_t *platform_formats = &formats;
    std::string name = "fake";
    int flags = 0;
    codec_t h264 {"h264", {}}, hevc {"hevc", {}}, av1 {"av1", {}};
    codec_t &codec_from_config(const config_t &config) {
      return config.videoFormat == 0 ? h264 : config.videoFormat == 1 ? hevc : av1;
    }
  };
  std::shared_ptr<platf::display_t> make_synthetic_probe_display(
    platf::mem_type_e, const config_t &, const std::optional<int> &) {
    return std::make_shared<platf::display_t>();
  }
  int validate_config(const std::shared_ptr<platf::display_t> &, const encoder_t &, const config_t &) {
    if (throws) throw std::runtime_error("driver failure");
    return invalid ? -1 : VUI_PARAMS;
  }
'''
    suffix = r'''
}
void require(bool value, const char *message) {
  if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
int main() {
  using namespace video;
  encoder_t encoder;
  std::optional<int> observed;
  auto run = [&]() { return validate_encoder(encoder, false, 7, &observed, ""); };
  require(run(), "SDR probe must succeed");
  require(live == 0, "SDR success retained a probe GPU source on the calling thread");
  require(observed == 7, "adapter identity must remain published as a value");
  unsupported = true;
  require(!run(), "unsupported H264 must fail");
  require(live == 0, "unsupported encoder retained a probe GPU source");
  unsupported = false;
  invalid = true;
  require(!run(), "invalid H264 must fail");
  require(live == 0, "validation failure retained a probe GPU source");
  invalid = false;
  throws = true;
  try { run(); require(false, "expected injected driver exception"); }
  catch (const std::runtime_error &) {}
  require(live == 0, "exception retained a probe GPU source");
  throws = false;
  hdr = true;
  active_hevc_mode = active_av1_mode = 0;
  const int before = created;
  require(run(), "HDR probe must succeed");
  require(created == before + 2, "SDR/HDR should use exactly two sources");
  require(peak == 1, "replaced SDR source must be released before HDR allocation");
  require(live == 0, "HDR success retained a probe GPU source");
  for (int i = 0; i < 100; ++i) require(run() && live == 0, "repeated probes retain resources");
  std::cout << "Encoder probe lifetime regressions passed\n";
}
'''
    with tempfile.TemporaryDirectory() as directory:
        cpp = Path(directory) / "probe.cpp"
        exe = Path(directory) / "probe"
        cpp.write_text(prefix + legacy_cache + "\n" + body + suffix)
        subprocess.run([args.compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        str(cpp), "-pthread", "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
