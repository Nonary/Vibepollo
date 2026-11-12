"""Compile migrated AMF retry, NVENC gating and packet-size policy with driver fakes."""
from pathlib import Path
import argparse
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--compiler', default='g++')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
amf = (root / 'src/amf/amf_d3d11.cpp').read_text()
start = amf.index('    // Some AMD drivers regress on pre-RDNA')
end = amf.index('\n    initialization_failure = lifecycle::initialization_failure_e::unsupported_format;\n    if (video_format != 0)', start)
retry = amf[start:end]
rtsp = (root / 'src/rtsp.cpp').read_text()
start = rtsp.index('      // Limit the packetsize')
packet_cap = rtsp[start:rtsp.index('      config.minRequiredFecPackets', start)]
nvenc = (root / 'src/nvenc/nvenc_base.cpp').read_text()
aq = next(line for line in nvenc.splitlines() if 'enableTemporalAQ = ' in line)
video = (root / 'src/video.cpp').read_text()
for codec in ('av1_vaapi', 'hevc_vaapi', 'h264_vaapi'):
    end = video.index('"' + codec + '"s')
    previous = video.rfind('// Common options', 0, end)
    options = video[previous:end]
    assert '{"low_power"s, 1}' in options and '{"low_power"s, 0}' in options
assert 'bool temporal_aq = false;' in (root / 'src/nvenc/nvenc_config.h').read_text()
program = r'''
#include <algorithm>
#include <cassert>
#include <iostream>
#include <memory>
#include <optional>
#include <string_view>
using namespace std::literals;
#define BOOST_LOG(level) if (false) std::cerr
using amf_int64 = long long;
constexpr int AMF_OK=0, AMF_NOT_SUPPORTED=1, AMF_INVALID_ARG=2, S_OK=0;
constexpr int AMF_VIDEO_ENCODER_USAGE_ULTRA_LOW_LATENCY=1;
namespace lifecycle { enum class initialization_failure_e {device_or_capture,unsupported_format,encoder_create,encoder_init}; }
struct amf_config { std::optional<int> usage; };
struct Driver;
struct Encoder { Driver *owner; int Init(int,int,int); void Terminate(); };
struct Driver {
 int attempts=0, terminations=0, failures=0, stage=0, removed=0;
 int last_usage=-1;
 int GetDeviceRemovedReason() { return removed; }
 int CreateComponent(int,int,std::unique_ptr<Encoder>* encoder) {
  ++attempts; *encoder=std::make_unique<Encoder>(Encoder{this});
  return stage==0 && attempts<=failures ? 3 : AMF_OK;
 }
};
int Encoder::Init(int,int,int) { return owner->stage==2 && owner->attempts<=owner->failures ? 3 : AMF_OK; }
void Encoder::Terminate() { ++owner->terminations; }
struct Host {
 Driver driver; Driver *factory=&driver, *device=&driver;
 std::unique_ptr<Encoder> encoder;
 int context=0, video_format=0, buffer_format=0, surface_format=0, encode_width=0, encode_height=0;
 struct { int bit_depth=8; } colorspace;
 struct { int width=1920,height=1080; } client_config;
 lifecycle::initialization_failure_e initialization_failure{};
 int get_codec_id(){return 0;} int get_amf_format(int,int){return 0;}
 bool configure_encoder(const amf_config &config, auto&, auto&) {
  driver.last_usage=config.usage.value_or(-1);
  return !(driver.stage==1 && driver.attempts<=driver.failures);
 }
 bool initialize(const amf_config &config) {
  int res=AMF_OK;
@RETRY@
  return true;
 }
};
namespace config {
 constexpr int PACKETSIZE_MIN=200, PACKETSIZE_MAX=65535, PACKETSIZE_SMALL=500, PACKETSIZE_LARGE=1456;
 struct { int packetsize=0; } stream;
}
int cap_packet(int requested,int limit) {
 struct {int packetsize;} config{requested}; config::stream.packetsize=limit;
@PACKET_CAP@
 return config.packetsize;
}
bool temporal_aq(bool enabled,bool supported) {
 struct {bool temporal_aq;} config{enabled};
 struct {struct {bool enableTemporalAQ=false;} rcParams;} enc_config;
 constexpr int NV_ENC_CAPS_SUPPORT_TEMPORAL_AQ=1;
 auto get_encoder_cap=[&](int){return supported;};
@AQ@
 return enc_config.rcParams.enableTemporalAQ;
}
int main() {
 for(int stage=0; stage<3; ++stage) {
  Host host; host.driver.stage=stage; host.driver.failures=1;
  assert(host.initialize({1})); assert(host.driver.attempts==2);
  assert(host.driver.terminations==1); assert(host.driver.last_usage==2);
  Host lost; lost.driver.stage=stage; lost.driver.failures=1; lost.driver.removed=1;
  assert(!lost.initialize({1})); assert(lost.driver.attempts==1);
  assert(lost.initialization_failure==lifecycle::initialization_failure_e::device_or_capture);
  Host exhausted; exhausted.driver.stage=stage; exhausted.driver.failures=99;
  assert(!exhausted.initialize({1})); assert(exhausted.driver.attempts==2);
  assert(exhausted.driver.terminations==2);
 }
 for(int usage : {0,2,3}) {Host host;host.driver.failures=1;assert(!host.initialize({usage}));assert(host.driver.attempts==1);}
 for(int codec : {1,2}) {Host host;host.video_format=codec;host.driver.failures=1;assert(!host.initialize({1}));assert(host.driver.attempts==1);}
 Host healthy;assert(healthy.initialize({1}));assert(healthy.driver.attempts==1);
 for(int limit : {0,199,65536,2000}) assert(cap_packet(1392,limit)==1392);
 assert(cap_packet(1392,500)==500);assert(cap_packet(1392,200)==200);assert(cap_packet(200,500)==200);
 assert(!temporal_aq(false,false));assert(!temporal_aq(false,true));assert(!temporal_aq(true,false));assert(temporal_aq(true,true));
 std::cout << "AMF retry cleanup/device-loss/usage, packet cap, NVENC AQ gating and VAAPI option checks passed\n";
}
'''.replace('@RETRY@', retry).replace('@PACKET_CAP@', packet_cap).replace('@AQ@', aq)
with tempfile.TemporaryDirectory(prefix='encoder-sync-') as temporary:
    cpp = Path(temporary) / 'test.cpp'
    exe = Path(temporary) / 'test'
    cpp.write_text(program)
    subprocess.run([args.compiler, '-std=c++23', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
