"""Exercise the production event-capture loop with deterministic DRM/image fakes.

The real event loop, metadata policy, commit and refresh-request bodies run with
no changed presentation events. Native waits, image export and codecs are faked;
this is an event-resumption control, not a native capture or encoder test.
"""
from pathlib import Path
import argparse
import importlib.util
import os
import shlex
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('hdr_worker', Path(__file__).with_name('test_kmsgrab_hdr_worker.py'))
worker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(worker)

PRELUDE = r'''
#include "src/platform/linux/kmsgrab_hdr.h"
#include "src/platform/linux/kmsgrab_pacing.h"
#include <cassert>
#include <chrono>
#include <functional>
#include <iostream>
#include <sstream>
using namespace std::literals;
struct NullLog { template<class T> NullLog &operator<<(const T&) { return *this; } };
#define BOOST_LOG(x) NullLog{}
namespace probe { unsigned eotf=2, luminance=1000, cll=900, reads=0; bool fail=false; }
extern "C" drmModePropertyBlobPtr drmModeGetPropertyBlob(int,uint32_t) {
  ++probe::reads; if(probe::fail) return nullptr;
  hdr_output_metadata raw{}; raw.hdmi_metadata_type1.eotf=probe::eotf;
  raw.hdmi_metadata_type1.max_display_mastering_luminance=probe::luminance;
  raw.hdmi_metadata_type1.max_cll=probe::cll;
  auto b=new drmModePropertyBlobRes{}; b->length=sizeof(raw); b->data=new hdr_output_metadata(raw); return b;
}
extern "C" void drmModeFreePropertyBlob(drmModePropertyBlobPtr b) { delete static_cast<hdr_output_metadata*>(b->data); delete b; }
namespace platf {
 struct img_t { std::optional<std::chrono::steady_clock::time_point> frame_timestamp,host_processing_timestamp,capture_pacing_timestamp; };
 using push_captured_image_cb_t=std::function<bool(std::shared_ptr<img_t>&&,bool)>;
 using pull_free_image_cb_t=std::function<bool(std::shared_ptr<img_t>&)>;
 namespace kms {
 namespace drm_timing_trace { template<class F> void write(F f) { std::ostringstream s; f(s); } }
 auto timing_trace_ns(auto) { return 0; }
 struct event_probe_t {
  hdr::generation_t hdr_metadata_generation;
  pacing::presentation_mode_t presentation_mode{true};
  pacing::presentation_latch_t presentation_latch;
  pacing::presentation_rate_limiter_t presentation_rate_limiter{};
  bool presentation_pending=true;
  std::optional<int> pending_exported_frame=1;
  uint64_t presentation_sequence=1;
  std::optional<uint64_t> last_selected_presentation_sequence;
  std::optional<std::chrono::steady_clock::time_point> presentation_timestamp=std::chrono::steady_clock::now(), last_source_presentation_timestamp,last_capture_delivery_timestamp;
  struct logger_t { void first_point(auto){} void second_point_now_and_log(){} void collect_and_log(auto){} } sleep_overshoot_logger,source_presentation_interval_logger,capture_delivery_interval_logger,presentation_to_capture_latency_logger;
  int crtc_id=1; unsigned waits=0,dequeues=0;
  static constexpr auto PRESENT_WAIT_IDLE_TIMEOUT=0ms;
  enum class presentation_wait_e{changed,timeout,unsupported};
  enum class frame_export_e{ready,empty,unsupported};
  event_probe_t() { presentation_mode.activate(); presentation_latch.request_capture(); assert(hdr_metadata_generation.selected(7,10)); }
  presentation_wait_e wait_for_presentation(std::chrono::milliseconds) { ++waits; presentation_latch.observe_response(pacing::presentation_response_e::timeout,false,std::chrono::steady_clock::now()); return presentation_wait_e::timeout; }
  frame_export_e dequeue_presentation_frame() { ++dequeues; pending_exported_frame=1; presentation_timestamp=std::chrono::steady_clock::now(); return frame_export_e::ready; }
  void wait_until_capture_deadline(auto) {}
  capture_e snapshot(const pull_free_image_cb_t& pull,std::shared_ptr<platf::img_t>& out,std::chrono::milliseconds,bool) {
   const auto status=hdr_metadata_generation.refresh(7,10,10); if(status!=capture_e::ok) return status;
   pending_exported_frame.reset(); assert(pull(out)); out->host_processing_timestamp=std::chrono::steady_clock::now(); return capture_e::ok;
  }
'''
POSTLUDE = r'''
 };
 }
}
int main(int argc,char**argv) {
 using namespace platf;
 const std::string scenario=argc>1?argv[1]:"nonpq-churn-0";
 const bool churn=scenario.starts_with("nonpq-churn-");
 if(churn) probe::eotf=std::stoul(scenario.substr(12));
 if(scenario=="nonpq-hdr-transitions") probe::eotf=0;
 kms::event_probe_t p; bool cursor=false; unsigned delivered=0,restarts=0;
 auto pull=[](std::shared_ptr<img_t>& out){out=std::make_shared<img_t>();return true;};
 auto push=[&](std::shared_ptr<img_t>&& out,bool captured){
   assert(captured && out);
   const auto selected=p.hdr_metadata_generation.selected(7,10);
   if(!probe::fail) {
     assert(selected->is_hdr()==(probe::eotf==2));
     if(selected->is_hdr()) { assert(selected->max_display_luminance==probe::luminance); assert(selected->max_content_light_level==probe::cll); }
   }
   ++delivered; return false;
 };
 auto attempt=[&]{return p.capture_presentation_events(push,pull,&cursor);};
 auto commit=[&]{assert(p.commit_pending_capture_metadata()); assert(!p.pending_exported_frame); ++restarts;};
 if(scenario=="teardown-recovery") {
   probe::luminance=1200; assert(attempt()==capture_e::reinit); assert(delivered==0);
   assert(p.hdr_metadata_generation.selected(7,10)->max_display_luminance==1000);
   probe::luminance=1300; commit();
   assert(p.hdr_metadata_generation.selected(7,10)->max_display_luminance==1200);
   assert(attempt()==capture_e::reinit); assert(delivered==0); commit();
   probe::fail=true; assert(attempt()==capture_e::ok); assert(delivered==1);
   assert(p.hdr_metadata_generation.selected(7,10)->max_display_luminance==1300);
   p.request_refresh(); probe::fail=false; probe::luminance=1400;
   assert(attempt()==capture_e::reinit); assert(delivered==1); commit();
 }
 while(delivered<24) {
   p.request_refresh();
   if(churn) { ++probe::cll; ++probe::luminance; }
   if(scenario=="nonpq-hdr-transitions") {
     if(delivered==8) { probe::eotf=2; probe::luminance=1200; }
     if(delivered==16) probe::eotf=3;
     if(delivered==20) { probe::eotf=2; probe::luminance=1300; }
     if(probe::eotf!=2) ++probe::cll;
   }
   auto status=attempt();
   if(status==capture_e::reinit) { commit(); assert(attempt()==capture_e::ok); }
   else assert(status==capture_e::ok);
 }
 const unsigned expected_restarts=churn?0:3;
 std::cout<<"event "<<scenario<<": deliveries="<<delivered<<" restarts="<<restarts<<" reads="<<probe::reads<<" waits="<<p.waits<<" dequeues="<<p.dequeues<<"\n";
 return restarts!=expected_restarts;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('compiler', nargs='?', default='c++')
    parser.add_argument('--output-dir', type=Path)
    parser.add_argument('--sanitizers', action='store_true')
    args = parser.parse_args()
    root = args.source.resolve()
    kms = (root / 'src/platform/linux/kmsgrab.cpp').read_text()
    bodies = [worker.function(kms, marker).replace(' override', '') for marker in [
        '      bool commit_pending_capture_metadata() override {',
        '      void request_refresh() override {',
        '      std::optional<capture_e> capture_presentation_events(',
    ]]
    with tempfile.TemporaryDirectory(prefix='kms-hdr-event-') as temporary:
        output = args.output_dir.resolve() if args.output_dir else Path(temporary)
        output.mkdir(parents=True, exist_ok=True)
        cpp, binary = output / 'event.cpp', output / 'event'
        cpp.write_text(PRELUDE + '\n'.join(bodies) + POSTLUDE)
        flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', 'libdrm'], text=True))
        command = [args.compiler, '-std=c++23', '-pthread', '-I', str(root), str(cpp), *flags, '-o', str(binary)]
        if args.sanitizers:
            command[1:1] = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        compiled = subprocess.run(command, capture_output=True, text=True)
        (output / 'compile.log').write_text(compiled.stdout + compiled.stderr)
        if compiled.returncode:
            print(compiled.stdout + compiled.stderr)
            return compiled.returncode
        results, failed = [], False
        for scenario in ['nonpq-churn-0', 'nonpq-churn-1', 'nonpq-churn-3', 'nonpq-hdr-transitions', 'teardown-recovery']:
            result = subprocess.run([str(binary), scenario], capture_output=True, text=True, timeout=10, env=os.environ.copy())
            results.append(result.stdout + result.stderr)
            failed |= result.returncode != 0
        (output / 'result.log').write_text(''.join(results))
        print(''.join(results), end='')
        return int(failed)


if __name__ == '__main__':
    raise SystemExit(main())
