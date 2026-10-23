"""Execute production init lookup, factory, getters and encoder setup pre-capture.

DRM/topology/device allocation are deterministic boundaries. Blob IDs retain
immutable distinct payloads: cached PQ 10 remains readable while current SDR is
zero or EOTF0 blob 11. This does not test a native codec, RTSP transaction, whole
KMS initialization or atomic metadata-to-scanout behavior.
"""
from pathlib import Path
import argparse
import os
import shlex
import subprocess
import tempfile


def function(source, marker, last=False):
    start = source.rindex(marker) if last else source.index(marker)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


PRELUDE = r'''
#include "src/platform/linux/kmsgrab_hdr.h"
#include "src/platform/linux/session_backend.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <iostream>
using namespace std::literals;
struct NullLog { template<class T> NullLog& operator<<(const T&){return *this;} };
#define BOOST_LOG(level) NullLog{}
struct SS_HDR_METADATA {
  struct XY { unsigned short x,y; } displayPrimaries[3],whitePoint;
  unsigned short maxDisplayLuminance{},minDisplayLuminance{},maxContentLightLevel{},maxFrameAverageLightLevel{};
};
namespace video {
@@COLOR_TYPES@@
struct config_t { bool prefer_sdr_10bit=false,force_sdr=false,rtx_hdr_active=false; int dynamicRange=1,encoderCscMode=2,chromaSamplingType=0; };
@@COLOR_FUNCTIONS@@
}
namespace probe {
 struct observation_t { std::optional<uint64_t> hdr;bool fail_list=false,null_property=false; };
 std::vector<observation_t> plan;
 observation_t current;
 unsigned property_lists=0,device_calls=0,captures=0,cases=0;
 bool direct_fails=false;
 std::map<uint32_t,unsigned> blob_reads;
 std::map<uint32_t,hdr_output_metadata> blobs;
 void reset(observation_t initial,observation_t factory) {
  plan={initial,factory};property_lists=device_calls=captures=0;blob_reads.clear();
  blobs.clear();
  for(auto [id,eotf]:std::vector<std::pair<unsigned,unsigned>>{{10,2},{11,0},{13,0},{14,0},{15,0},{16,1},{17,3},{18,255}}){
   auto &raw=blobs[id];raw={};raw.hdmi_metadata_type1.eotf=eotf;
   raw.hdmi_metadata_type1.max_display_mastering_luminance=1000;
   raw.hdmi_metadata_type1.max_cll=900;
  }
  blobs[13].metadata_type=1;
 }
}
extern "C" drmModePropertyBlobPtr drmModeGetPropertyBlob(int fd,uint32_t id) {
 assert(fd==7);++probe::blob_reads[id];
 if(!probe::blobs.contains(id))return nullptr;
 auto *b=new drmModePropertyBlobRes{};b->length=id==14?sizeof(hdr_output_metadata)-1:sizeof(hdr_output_metadata);
 b->data=id==15?nullptr:new hdr_output_metadata(probe::blobs[id]);return b;
}
extern "C" void drmModeFreePropertyBlob(drmModePropertyBlobPtr blob) {
 delete static_cast<hdr_output_metadata*>(blob->data);delete blob;
}
namespace platf {
 enum class mem_type_e { unknown,cuda,vaapi,vulkan,system };
 enum class pix_fmt_e { any };
 struct encode_device_t {
  virtual ~encode_device_t()=default;
  virtual bool supports_rtx_hdr() const {return false;}
  video::sunshine_colorspace_t colorspace;
  bool rtx_hdr_active{},hdr_metadata_valid{};SS_HDR_METADATA hdr_metadata{};
 };
 struct display_t {
  virtual ~display_t()=default;
  virtual bool is_hdr()=0;
  virtual bool get_hdr_metadata(SS_HDR_METADATA&)=0;
  std::unique_ptr<encode_device_t> make_avcodec_encode_device(pix_fmt_e){assert(probe::captures==0);++probe::device_calls;return std::make_unique<encode_device_t>();}
  std::unique_ptr<encode_device_t> make_deferred_avcodec_encode_device(pix_fmt_e p){return make_avcodec_encode_device(p);}
  std::unique_ptr<encode_device_t> make_nvenc_encode_device(pix_fmt_e p){return make_avcodec_encode_device(p);}
  std::unique_ptr<encode_device_t> make_amf_encode_device(pix_fmt_e p){return make_avcodec_encode_device(p);}
 };
 namespace kms {
  std::mutex display_mutex;
  using prop_t=std::unique_ptr<drmModePropertyRes>;
  using obj_prop_t=std::unique_ptr<drmModeObjectProperties>;
  struct prop_blob_t {
   drmModePropertyBlobPtr p;prop_blob_t(drmModePropertyBlobPtr value):p(value){}
   ~prop_blob_t(){if(p)drmModeFreePropertyBlob(p);}
   explicit operator bool()const{return p!=nullptr;}auto operator->()const{return p;}
  };
  obj_prop_t drmModeObjectGetProperties(int fd,unsigned id,unsigned type){
   assert(fd==7&&id==1&&type==DRM_MODE_OBJECT_CONNECTOR);
   assert(probe::property_lists<probe::plan.size());probe::current=probe::plan[probe::property_lists++];
   if(probe::current.fail_list)return {};
   static std::vector<uint32_t> ids;static std::vector<uint64_t> values;ids.clear();values.clear();
   if(probe::current.hdr){ids={1};values={*probe::current.hdr};}
   auto p=std::make_unique<drmModeObjectProperties>();p->count_props=ids.size();p->props=ids.data();p->prop_values=values.data();return p;
  }
  prop_t drmModeGetProperty(int fd,unsigned id){
   assert(fd==7&&id==1);if(probe::current.null_property)return {};
   auto p=std::make_unique<drmModePropertyRes>();std::strcpy(p->name,"HDR_OUTPUT_METADATA");return p;
  }
  struct card_t {
   struct {int el=7;}fd;
   struct connector_t {int connection=DRM_MODE_CONNECTED;unsigned encoder_id=4;};
   struct encoder_t {unsigned crtc_id=7;};
   std::optional<connector_t> connector(unsigned id){assert(id==1);return connector_t{};}
   std::optional<encoder_t> encoder(unsigned id){assert(id==4);return encoder_t{};}
@@PROPERTY_METHODS@@
  };
  struct display_vram_t:display_t {
   card_t card;std::optional<unsigned>connector_id=1;unsigned crtc_id=7;
   std::optional<uint64_t>hdr_metadata_blob_id;hdr::generation_t hdr_metadata_generation;
   explicit display_vram_t(mem_type_e){}
   int init(const std::string &name,const video::config_t&){
    assert(name=="DP-1");if(probe::direct_fails)return -1;
@@INIT_LOOKUP@@
    return 0;
   }
@@STRICT@@
@@GETTERS@@
  };
  struct display_ram_t:display_vram_t {
   using display_vram_t::display_vram_t;
   int init(const std::string&name,const video::config_t&config){
    const bool old=probe::direct_fails;probe::direct_fails=false;
    auto result=display_vram_t::init(name,config);probe::direct_fails=old;return result;
   }
  };
 }
 static std::vector<std::string>kms_display_names_unlocked(mem_type_e){return {"DP-1"};}
@@FACTORY@@
}
namespace video {
 struct formats_t {virtual ~formats_t()=default;platf::pix_fmt_e pix_fmt_yuv444_10bit{},pix_fmt_yuv444_8bit{},pix_fmt_10bit{},pix_fmt_8bit{};};
 struct encoder_platform_formats_avcodec:formats_t{};
 struct encoder_platform_formats_nvenc:formats_t{};
 struct encoder_platform_formats_amf:formats_t{};
 struct encoder_t {
  std::shared_ptr<formats_t>platform_formats=std::make_shared<encoder_platform_formats_avcodec>();unsigned flags{};
  struct codec_t {const char*name="inert";};codec_t codec_from_config(const config_t&)const{return {};}
 };
 constexpr unsigned YUV444_SUPPORT=1;
 namespace logging {auto bracket(auto value){return value;}}
@@MAKE_ENCODE_DEVICE@@
}
'''
POSTLUDE=r'''
void startup(probe::observation_t initial,probe::observation_t current,bool admitted,bool backend_hyprland,bool expected_hdr) {
 using namespace platf;
 for(auto type:{mem_type_e::system,mem_type_e::vaapi,mem_type_e::cuda,mem_type_e::vulkan})
 for(bool fallback:{false,true})for(int range:{1,0}){
  probe::reset(initial,current);probe::direct_fails=fallback;
  setenv("@@BRAND@@_MACHINE_HOST","1",1);
  setenv("@@BRAND@@_SESSION_BACKEND",backend_hyprland?"hyprland-kms":"kwin",1);
  video::config_t config;config.dynamicRange=range;
  auto base=kms_display(type,"DP-1",config);
  assert(bool(base)==admitted);assert(probe::captures==0);
  if(!base){assert(probe::device_calls==0);++probe::cases;continue;}
  auto capture=std::dynamic_pointer_cast<kms::display_vram_t>(base);assert(capture);
  assert(capture->hdr_metadata_blob_id==initial.hdr);
  assert(probe::property_lists==(backend_hyprland?2u:1u));
  // Run initial getters, then the actual encoder-device selection, before any
  // capture. Cached PQ remains readable at ID 10 throughout this sequence.
  const bool actual_initial_hdr=capture->is_hdr();
  SS_HDR_METADATA metadata{};const bool actual_metadata=capture->get_hdr_metadata(metadata);
  if(actual_metadata)assert(metadata.maxDisplayLuminance==1000);
  const auto reads=probe::blob_reads;
  std::vector<std::thread>getters;
  for(int i=0;i<4;++i)getters.emplace_back([&]{for(int j=0;j<25;++j){assert(capture->is_hdr()==actual_initial_hdr);SS_HDR_METADATA m{};assert(capture->get_hdr_metadata(m)==actual_metadata);}});
  for(auto &thread:getters)thread.join();
  auto device=video::make_encode_device(*base,video::encoder_t{},config,false);
  assert(device&&probe::device_calls==1&&probe::captures==0);
  if(actual_initial_hdr!=expected_hdr||actual_metadata!=expected_hdr){
   std::cerr<<"startup disagreement: dynamicRange="<<range<<" initial_hdr="<<actual_initial_hdr
            <<" metadata="<<actual_metadata<<" device_hdr="<<video::colorspace_is_hdr(device->colorspace)
            <<" device_hdr_metadata="<<device->hdr_metadata_valid<<" device_calls="<<probe::device_calls
            <<" captures="<<probe::captures<<"\n";
  }
  assert(actual_initial_hdr==expected_hdr&&actual_metadata==expected_hdr);
  assert(video::colorspace_is_hdr(device->colorspace)==(expected_hdr&&range>0));
  assert(device->hdr_metadata_valid==(expected_hdr&&range>0));
  assert(probe::blob_reads==reads); // Repeated getters use one immutable seed.
  if(backend_hyprland){assert(!expected_hdr);assert(probe::blob_reads[10]==0);}
  ++probe::cases;
 }
}
int main(){
 using probe::observation_t;
 for(auto cached:{observation_t{10},observation_t{12},observation_t{std::nullopt},observation_t{0},observation_t{11}})
 for(auto current:{observation_t{0},observation_t{11}})startup(cached,current,true,true,false);
 for(auto cached:{observation_t{10},observation_t{12},observation_t{std::nullopt}})
 for(auto current:{observation_t{12},observation_t{13},observation_t{14},observation_t{15},observation_t{16},observation_t{17},observation_t{18},observation_t{10},observation_t{std::nullopt},observation_t{0,true},observation_t{0,false,true}})
 startup(cached,current,false,true,false);
 startup(observation_t{10},observation_t{0},true,false,true);
 startup(observation_t{std::nullopt},observation_t{0},true,false,false);
 startup(observation_t{11},observation_t{0},true,false,false);
 std::cout<<"PASS: "<<probe::cases<<" production startup sequences; immutable cached PQ/current zero or EOTF0; initial getters + encoder selection before captures; concurrent getter/unknown/rejection/KWin controls\n";
}
'''


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source',type=Path)
    parser.add_argument('compiler',nargs='?',default='c++')
    parser.add_argument('--output-dir',type=Path)
    parser.add_argument('--sanitizers',action='store_true')
    args=parser.parse_args()
    root=args.source.resolve()
    kms=(root/'src/platform/linux/kmsgrab.cpp').read_text()
    video=(root/'src/video.cpp').read_text()
    types=(root/'src/video_colorspace.h').read_text()
    colors=(root/'src/video_colorspace.cpp').read_text()
    marker='                auto connector_props = card.connector_props(*connector_id);\n'
    start=kms.index(marker)
    end=kms.index('\n',kms.index('hdr_metadata_blob_id = card.prop_value_by_name',start))
    properties='\n'.join(function(kms,marker)for marker in (
        '      std::vector<std::pair<prop_t, std::uint64_t>> props(',
        '      std::vector<std::pair<prop_t, std::uint64_t>> connector_props(',
        '      std::optional<std::uint64_t> prop_value_by_name(',
    ))
    replacements={
        '@@COLOR_TYPES@@':function(types,'  enum class colorspace_e {')+';\n'+function(types,'  struct sunshine_colorspace_t {')+';',
        '@@COLOR_FUNCTIONS@@':function(colors,'  bool colorspace_is_hdr(')+'\n'+function(colors,'  sunshine_colorspace_t colorspace_from_client_config('),
        '@@PROPERTY_METHODS@@':properties,
        '@@INIT_LOOKUP@@':kms[start:end],
        '@@STRICT@@':function(kms,'      bool sdr_capture_allowed('),
        '@@GETTERS@@':function(kms,'      bool is_hdr() {')+'\n'+function(kms,'      bool get_hdr_metadata(SS_HDR_METADATA &metadata) {'),
        '@@FACTORY@@':function(kms,'  std::shared_ptr<display_t> kms_display('),
        '@@MAKE_ENCODE_DEVICE@@':function(video,'  std::unique_ptr<platf::encode_device_t> make_encode_device(',last=True),
        '@@BRAND@@':'VIBEPOLLO' if 'VIBEPOLLO_MACHINE_HOST' in (root/'src/platform/linux/session_backend.h').read_text() else 'VIBESHINE',
    }
    program=PRELUDE+POSTLUDE
    for marker,body in replacements.items():program=program.replace(marker,body)
    with tempfile.TemporaryDirectory(prefix='kms-startup-')as temporary:
        output=args.output_dir.resolve()if args.output_dir else Path(temporary)
        output.mkdir(parents=True,exist_ok=True)
        cpp,binary=output/'startup.cpp',output/'startup'
        cpp.write_text(program)
        flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','libdrm'],text=True))
        command=[args.compiler,'-std=c++23','-pthread','-Wall','-Wextra','-Werror','-Wno-sign-compare','-I',str(root),str(cpp),*flags,'-o',str(binary)]
        if args.sanitizers:command[1:1]=['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer']
        result=subprocess.run(command,capture_output=True,text=True)
        (output/'compile.log').write_text(result.stdout+result.stderr)
        if result.returncode:print(result.stdout+result.stderr);return result.returncode
        result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=30,env=os.environ.copy())
        (output/'result.log').write_text(result.stdout+result.stderr)
        print(result.stdout+result.stderr,end='')
        return result.returncode


if __name__=='__main__':raise SystemExit(main())
