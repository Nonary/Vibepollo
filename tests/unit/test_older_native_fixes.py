"""Compile actual encoder initialization and platform functions with deterministic OS seams."""
from pathlib import Path
import argparse, subprocess, tempfile
parser = argparse.ArgumentParser()
parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[2])
parser.add_argument('--compiler', default='clang++')
parser.add_argument('--only', choices=['videotoolbox', 'thread_name', 'wayland_mode'], action='append')
args = parser.parse_args()
root=args.repo
programs={}
video=(root/'src/video.cpp').read_text()
start=video.index('  encoder_t videotoolbox {')
block=video[start:video.index('\n#endif',start)]
programs['videotoolbox']=r'''
#include <algorithm>
#include <cassert>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
using namespace std::literals;
constexpr int AV_HWDEVICE_TYPE_VIDEOTOOLBOX=1,AV_HWDEVICE_TYPE_NONE=0,
 AV_PIX_FMT_VIDEOTOOLBOX=2,AV_PIX_FMT_NV12=3,AV_PIX_FMT_P010=4,AV_PIX_FMT_NONE=-1;
void vt_init_avcodec_hardware_input_buffer() {}
namespace config { struct { struct {int vt_allow_sw=1,vt_require_sw=0,vt_realtime=1;} vt;} video; }
struct encoder_platform_formats_avcodec { template<class... Ts> encoder_platform_formats_avcodec(Ts...) {} };
struct option_t { std::string name; std::variant<int,int*,std::string> value; };
struct codec_t {
 std::vector<option_t> common_options,sdr_options,hdr_options,yuv444_sdr_options,yuv444_hdr_options,fallback_options;
 std::string name;
};
enum {DEFAULT=0,PARALLEL_ENCODING=2};
struct encoder_t { std::string_view name; std::unique_ptr<encoder_platform_formats_avcodec> formats; codec_t av1,hevc,h264; int flags; };
@BLOCK@
int main() {
 const auto has=[](const auto &opts, const auto &name) {return std::ranges::any_of(opts,[&](const auto &o){return o.name==name;});};
 assert(!has(videotoolbox.h264.common_options,"max_ref_frames"));
 assert(has(videotoolbox.hevc.common_options,"max_ref_frames"));
 assert(has(videotoolbox.av1.common_options,"max_ref_frames"));
 assert(has(videotoolbox.h264.fallback_options,"flags"));
 assert(videotoolbox.flags&PARALLEL_ENCODING);
 for(const auto *codec:{&videotoolbox.av1,&videotoolbox.hevc,&videotoolbox.h264}){
  assert(has(codec->common_options,"allow_sw"));
  assert(has(codec->common_options,"require_sw"));
  assert(has(codec->common_options,"realtime"));
 }
}
'''.replace('@BLOCK@',block)
common=r'''
#include <cassert>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
using namespace std::literals;
struct log_t {template<class T> log_t &operator<<(const T &) {return *this;}} logger;
#define BOOST_LOG(level) logger
'''
linux=(root/'src/platform/linux/misc.cpp').read_text()
start=linux.index('  void set_thread_name(')
body=linux[start:linux.index('\n  void enable_mouse_keys()',start)]
programs['thread_name']=common+r'''
std::string captured;
int pthread_self(){return 1;}
void pthread_setname_np(int,const char *name) {captured=name;}
@BODY@
int main(){
 set_thread_name("short");assert(captured=="short");
 set_thread_name("fifteen-letters");assert(captured=="fifteen-letters");
 set_thread_name("video::capture_sync");assert(captured=="video::capture_");
 set_thread_name("");assert(captured.empty());
}
'''.replace('@BODY@',body)
wayland=(root/'src/platform/linux/wayland.cpp').read_text()
start=wayland.index('  void monitor_t::wl_mode(')
body=wayland[start:wayland.index('\n  void monitor_t::listen(',start)]
programs['wayland_mode']=common+r'''
struct wl_output{};
constexpr unsigned WL_OUTPUT_MODE_CURRENT=1,WL_OUTPUT_MODE_PREFERRED=2;
struct monitor_t {
 struct {int width=-1,height=-1,logical_width=900,logical_height=600,offset_x=41,offset_y=42;} viewport;
 void wl_mode(wl_output *,std::uint32_t,std::int32_t,std::int32_t,std::int32_t);
};
@BODY@
int main(){
 monitor_t monitor;
 monitor.wl_mode(nullptr,WL_OUTPUT_MODE_PREFERRED,1280,720,60000);
 assert(monitor.viewport.width==-1 && monitor.viewport.height==-1);
 monitor.wl_mode(nullptr,WL_OUTPUT_MODE_CURRENT,1920,1080,60000);
 monitor.wl_mode(nullptr,0,2560,1440,60000);
 assert(monitor.viewport.width==1920 && monitor.viewport.height==1080);
 monitor.wl_mode(nullptr,WL_OUTPUT_MODE_CURRENT|WL_OUTPUT_MODE_PREFERRED,3840,2160,60000);
 assert(monitor.viewport.width==3840 && monitor.viewport.height==2160);
 assert(monitor.viewport.logical_width==900 && monitor.viewport.logical_height==600);
 assert(monitor.viewport.offset_x==41 && monitor.viewport.offset_y==42);
}
'''.replace('@BODY@',body)
failed=[]
with tempfile.TemporaryDirectory(prefix='small-native-') as tmp:
 for name,program in programs.items():
  if args.only and name not in args.only:continue
  cpp=Path(tmp)/(name+'.cpp');cpp.write_text(program);binary=Path(tmp)/name
  build=subprocess.run([args.compiler,'-std=c++23','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-fsanitize=address,undefined',str(cpp),'-o',str(binary)])
  if build.returncode:failed.append(name+':compile');continue
  check=subprocess.run([str(binary)])
  print(('PASS' if check.returncode==0 else 'FAIL')+': production '+name,flush=True)
  if check.returncode:failed.append(name+':behavior')
if failed:raise SystemExit('Failures: '+', '.join(failed))
