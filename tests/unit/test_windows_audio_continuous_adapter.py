#!/usr/bin/env python3
"""Compile the production WASAPI sample/drain methods against fake device edges."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--compiler', default='c++')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
source = (root / 'src/platform/windows/audio.cpp').read_text()
sample_start = source.index('    capture_e sample(', source.index('class mic_wasapi_t'))
sample = source[sample_start:source.index('    int init(', sample_start)]
fill_start = source.index('    capture_e refresh_mute()', sample_start)
fill = source[fill_start:source.index('  public:\n    handle_t audio_event;', fill_start)]
code = r'''
#include "src/platform/windows/audio_continuous_buffer.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
using namespace std::literals;
using DWORD = unsigned long; // Windows DWORD differs from uint32_t on MSVC
using UINT64 = std::uint64_t;
using BYTE = unsigned char;
using HRESULT = int;
using BOOL = int;
constexpr HRESULT S_OK=0, AUDCLNT_E_DEVICE_INVALIDATED=-1;
constexpr DWORD WAIT_OBJECT_0=0, WAIT_TIMEOUT=258, FALSE=0;
constexpr DWORD AUDCLNT_BUFFERFLAGS_SILENT=2, AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY=1, AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR=4;
#define SUCCEEDED(x) ((x)>=0)
#define FAILED(x) ((x)<0)
#define CHECK(x) do { if (!(x)) { std::cerr << "failed: " #x << " line " << __LINE__ << '\n'; std::exit(1); } } while(false)
struct LARGE_INTEGER { std::int64_t QuadPart; };
constexpr std::int64_t origin = 9000000000000;
std::int64_t clock_ticks = origin;
bool spurious_events = false, fail_wait = false;
int wait_count=0;
bool QueryPerformanceCounter(LARGE_INTEGER *out) {out->QuadPart=clock_ticks; return true;}
bool QueryPerformanceFrequency(LARGE_INTEGER *out) {out->QuadPart=10000000; return true;}
DWORD WaitForSingleObjectEx(int, DWORD timeout, DWORD) {
  ++wait_count;
  if (fail_wait) return 999;
  // Even spurious events cost some elapsed time; they never earn silence.
  clock_ticks += timeout ? (spurious_events ? 10000 : timeout*10000) : 0;
  return spurious_events ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
}
std::ostringstream logs;
#define BOOST_LOG(level) logs
namespace util {
  struct hex_t { std::string_view to_string_view() { return "status"; } };
  hex_t hex(int) { return {}; }
}
struct Packet {
  std::uint32_t frames; DWORD flags; UINT64 qpc; std::vector<float> samples; UINT64 position=0;
};
struct Capture {
  std::deque<Packet> packets;
  int releases=0;
  HRESULT status=S_OK;
  HRESULT GetNextPacketSize(std::uint32_t *out) {
    *out=packets.empty()?0:packets.front().frames; return status;
  }
  HRESULT GetBuffer(BYTE **out, std::uint32_t *frames, DWORD *flags, UINT64 *position, UINT64 *qpc) {
    auto &p=packets.front(); *out=reinterpret_cast<BYTE*>(p.samples.data());
    *frames=p.frames; *flags=p.flags; *qpc=p.qpc; *position=p.position; return status;
  }
  HRESULT ReleaseBuffer(std::uint32_t) { ++releases; packets.pop_front(); return S_OK; }
};
namespace platf::audio {
  enum class capture_e {ok, timeout, reinit, error};
  struct mic_t { virtual capture_e sample(std::vector<float>&)=0; virtual ~mic_t()=default; };
  struct mic_wasapi_t : mic_t {
SAMPLE
FILL
    struct { int get() {return 0;} } audio_event;
    struct {bool changed=false; bool check_default_render_device_changed(){return std::exchange(changed,false);} } endpt_notification;
    std::optional<std::function<void()>> default_endpt_changed_cb;
    bool follow_default_device=true;
    DWORD default_latency_ms=10;
    struct Volume {
      BOOL muted=0;
      HRESULT status=S_OK;
      HRESULT GetMute(BOOL *out) { *out=muted; return status; }
    } volume;
    Volume *endpoint_volume=nullptr;
    struct Callback { std::atomic<std::uint64_t> state{1}; } callback;
    Callback *mute_callback=&callback;
    bool endpoint_muted=false;
    std::uint64_t mute_state=0;
    Capture capture;
    Capture *audio_capture=&capture;
    std::array<float, 10000> sample_buf{};
    float *sample_buf_pos=sample_buf.data();
    int channels=2;
    std::optional<audio_detail::continuous_buffer_t> continuous_buffer;
    bool timestamp_warning_logged=false;
  };
}
int main() {
  using platf::audio::mic_wasapi_t; using platf::audio::capture_e;
  std::vector<float> output(480);
  // Start muted despite audible WASAPI packets; drain and release normally.
  mic_wasapi_t guarded;
  guarded.endpoint_volume=&guarded.volume;
  guarded.volume.muted=1;
  guarded.capture.packets.push_back({480,0,origin,std::vector<float>(960,99)});
  CHECK(guarded.sample(output)==capture_e::ok && output.front()==0);
  CHECK(guarded.capture.releases==1);
  // The queued half-packet is erased even if mute and unmute both arrive
  // between samples. Subsequent unmuted PCM preserves its original gain.
  guarded.volume.muted=0;
  guarded.callback.state=4;
  CHECK(guarded.sample(output)==capture_e::ok && output.front()==0);
  guarded.capture.packets.push_back({240,0,origin,std::vector<float>(480,7)});
  CHECK(guarded.sample(output)==capture_e::ok && output.front()==7);
  guarded.volume.status=AUDCLNT_E_DEVICE_INVALIDATED;
  CHECK(guarded.sample(output)==capture_e::reinit);
  guarded.volume.status=-2;
  CHECK(guarded.sample(output)==capture_e::error);
  // Continuous capture stores silence and preserves its output deadlines.
  mic_wasapi_t paced;
  paced.endpoint_volume=&paced.volume;
  paced.volume.muted=1;
  paced.continuous_buffer.emplace(48000,2,240,480,origin);
  paced.capture.packets.push_back({480,0,origin,std::vector<float>(960,99)});
  CHECK(paced.sample(output)==capture_e::ok && output.front()==0);
  paced.volume.muted=0;
  CHECK(paced.sample(output)==capture_e::ok && output.front()==0);
  CHECK(paced.capture.releases==1);
  // Pre-mute audible data already queued in the continuous ring must also
  // disappear when both edges occur between capture calls.
  mic_wasapi_t queued;
  queued.endpoint_volume=&queued.volume;
  queued.callback.state=0;
  queued.continuous_buffer.emplace(48000,2,240,480,origin);
  clock_ticks=origin;
  queued.capture.packets.push_back({480,0,origin,std::vector<float>(960,8)});
  CHECK(queued.sample(output)==capture_e::ok && output.front()==8);
  queued.callback.state=4;
  CHECK(queued.sample(output)==capture_e::ok && output.front()==0);
  clock_ticks=origin;
  mic_wasapi_t mic;
  mic.continuous_buffer.emplace(48000,2,240,480,origin);
  CHECK(mic.sample(output)==capture_e::ok);
  CHECK(clock_ticks == origin+300000); // first packet deadline includes 25ms margin
  CHECK(output.front()==0);
  // Timed-out event, but pending capture queue must still be drained. Clip
  // the already-emitted first half, keeping the second half in its time slot.
  mic.capture.packets.push_back({480,0,origin,std::vector<float>(960,3)});
  CHECK(mic.sample(output)==capture_e::ok);
  CHECK(output.front()==3 && output.back()==3 && mic.capture.releases==1);
  CHECK(clock_ticks==origin+350000);
  CHECK(mic.sample(output)==capture_e::ok);
  CHECK(output.front()==0 && clock_ticks==origin+400000);
  // Persistent uncertain timestamps must not restart capture or busy loop.
  for(int n=0;n<20;++n) {
    mic.capture.packets.push_back({240,AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR,0,std::vector<float>(480,9)});
    CHECK(mic.sample(output)==capture_e::ok);
    CHECK(output.front()==0);
  }
  CHECK(mic.capture.releases==21 && mic.timestamp_warning_logged);
  auto warnings=logs.str();
  CHECK(warnings.find("invalid capture timestamps") == warnings.rfind("invalid capture timestamps"));
  // A subsequent reliable packet resumes on the existing output clock.
  auto first=clock_ticks-250000;
  mic.capture.packets.push_back({240,AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY,static_cast<UINT64>(first),std::vector<float>(480,7)});
  CHECK(mic.sample(output)==capture_e::ok && output.front()==7);
  // Spurious signals do not accelerate the stream.
  spurious_events=true;
  auto before=clock_ticks;
  CHECK(mic.sample(output)==capture_e::ok);
  CHECK(clock_ticks-before==50000);
  spurious_events=false;
  CHECK(wait_count<1000);
  // Endpoint invalidation/replacement and wait errors retain normal behavior.
  mic.capture.status=AUDCLNT_E_DEVICE_INVALIDATED;
  CHECK(mic.sample(output)==capture_e::reinit);
  mic.capture.status=S_OK;
  mic.endpt_notification.changed=true;
  CHECK(mic.sample(output)==capture_e::reinit);
  fail_wait=true;
  CHECK(mic.sample(output)==capture_e::error);
  fail_wait=false;
  // Existing event-driven path does not synthesize silence and still handles
  // silent packets whose data pointer may be null.
  mic_wasapi_t normal;
  CHECK(normal.sample(output)==capture_e::timeout);
  normal.capture.packets.push_back({240,0,0,std::vector<float>(480,5)});
  CHECK(normal.sample(output)==capture_e::ok && output.front()==5);
  normal.capture.packets.push_back({240,AUDCLNT_BUFFERFLAGS_SILENT,0,{}});
  CHECK(normal.sample(output)==capture_e::ok && output.front()==0);
  // Production GetBuffer must forward the device position. Timestamp jitter
  // across otherwise contiguous packets cannot alter the sample sequence.
  clock_ticks=origin;
  mic_wasapi_t jittered;
  jittered.continuous_buffer.emplace(48000,2,240,480,origin);
  for (int n=0;n<8;++n) {
    std::vector<float> values(960);
    for (int i=0;i<960;++i) values[i]=float(n*960+i+1);
    auto jitter=n==0?0:(n%2?9000:-9000);
    jittered.capture.packets.push_back({480,n==0?AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY:0,
      static_cast<UINT64>(origin+n*100000+jitter),std::move(values),static_cast<UINT64>(n)*480});
  }
  int expected=1;
  for (int n=0;n<16;++n) {
    CHECK(jittered.sample(output)==capture_e::ok);
    for (float value:output) CHECK(value==float(expected++));
  }
  CHECK(jittered.capture.releases==8);
  std::cout << "production WASAPI adapter tests passed\n";
}
'''.replace('SAMPLE', sample).replace('FILL', fill)
code = '#include <utility>\n' + code
with tempfile.TemporaryDirectory(prefix='windows-audio-adapter-') as tmp:
    path=Path(tmp)
    (path/'adapter.cpp').write_text(code)
    subprocess.run([args.compiler,'-std=c++23','-DNDEBUG','-O2','-Wall','-Wextra','-Werror','-I',str(root),str(path/'adapter.cpp'),'-o',str(path/'adapter')],check=True)
    subprocess.run([str(path/'adapter')],check=True)
