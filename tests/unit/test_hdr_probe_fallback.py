"""Compile the production HDR probe block with deterministic backend failures."""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
source = (Path(sys.argv[1]) if len(sys.argv) > 1 else root / 'src/video.cpp').read_text()
start = source.index('    // Test HDR and YUV444 support')
end = source.index('    encoder.h264[encoder_t::VUI_PARAMETERS]', start)
block = source[start:end]
with tempfile.TemporaryDirectory(prefix='hdr-probe-') as directory:
    path = Path(directory)
    (path / 'test.cpp').write_text(r'''
#include <array>
#include <cassert>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
struct config_t { int width,height,framerate,bitrate,slices,numRefFrames,encoderCscMode,videoFormat,dynamicRange,mode,chromaSamplingType; };
struct codec_t {
 std::string name;
 std::array<bool,3> values{true,true,true};
 bool& operator[](int flag) { return values[flag]; }
};
struct encoder_t {
 enum {PASSED,DYNAMIC_RANGE,YUV444};
 codec_t h264{"h264"},hevc{"hevc"},av1{"av1"};
 int flags=1;
 codec_t& codec_from_config(const config_t& config) { return config.videoFormat==1?hevc:av1; }
};
constexpr int YUV444_SUPPORT=1;
struct display {
 bool hdr=false;
 std::string adapter="gpu";
 bool is_codec_supported(std::string_view, const config_t& config) { return !config.dynamicRange || hdr; }
 std::optional<std::string> capture_adapter_id() { return adapter; }
};
int validate_config(const std::shared_ptr<display>&,const encoder_t&,const config_t& config) {
 // Main10 works; optional HEVC/AV1 4:4:4 does not.
 return config.dynamicRange && config.chromaSamplingType ? -1 : 0;
}
#define BOOST_LOG(level) std::cerr
bool probe(encoder_t& encoder, bool supports_hdr, bool fail_surface, bool wrong_adapter, int& resets) {
 auto disp=std::make_shared<display>(); disp->hdr=supports_hdr;
 auto cached_probe_display=disp;
 std::optional<std::string> required_adapter="gpu";
 auto reset_probe_display=[&](const config_t&) {
  ++resets;
  if (fail_surface || !supports_hdr) disp.reset();
  else if(wrong_adapter) disp->adapter="other";
 };
''' + block + r'''
 return true;
}
int main() {
 {
  encoder_t encoder; int resets=0;
  assert(probe(encoder,false,false,false,resets));
  assert(resets==0);
  for(auto* codec:{&encoder.h264,&encoder.hevc,&encoder.av1}) {
   assert((*codec)[encoder_t::PASSED]);
   assert(!(*codec)[encoder_t::DYNAMIC_RANGE]);
  }
  assert(encoder.h264[encoder_t::YUV444]);
  assert(!encoder.hevc[encoder_t::YUV444] && !encoder.av1[encoder_t::YUV444]);
 }
 {
  encoder_t encoder; int resets=0;
  assert(probe(encoder,true,false,false,resets)); assert(resets==1);
  assert(encoder.hevc[encoder_t::DYNAMIC_RANGE] && encoder.av1[encoder_t::DYNAMIC_RANGE]);
  assert(!encoder.h264[encoder_t::DYNAMIC_RANGE]);
  assert(!encoder.hevc[encoder_t::YUV444] && !encoder.av1[encoder_t::YUV444]);
 }
 for(bool wrong_adapter:{false,true}) {
  encoder_t encoder; int resets=0;
  assert(!probe(encoder,true,!wrong_adapter,wrong_adapter,resets));
 }
}
''')
    subprocess.run(['g++', '-std=c++17', str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('PASS: SDR fallback, Main10 without 4:4:4, failed surface, and adapter isolation')
