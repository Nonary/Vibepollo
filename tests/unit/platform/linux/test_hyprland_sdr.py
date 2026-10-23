"""Compile production KMS factory and SDR authority check with inert DRM edges."""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(sys.argv[1]).resolve()
compiler = sys.argv[2] if len(sys.argv) > 2 else 'g++'
source = (root / 'src/platform/linux/kmsgrab.cpp').read_text()

def function(marker):
    start = source.index(marker)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

program = r'''
#include "src/platform/linux/kmsgrab_hdr.h"
#include <cassert>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <drm_mode.h>
#include <xf86drmMode.h>
using namespace std::literals;
struct NullLog { template<class T> NullLog& operator<<(const T&) { return *this; } };
#define BOOST_LOG(level) NullLog{}
namespace video { struct config_t {}; }
namespace platf {
enum class mem_type_e { unknown, cuda, vaapi, vulkan, system };
struct display_t { virtual ~display_t() = default; };
namespace fake {
inline std::map<unsigned, std::optional<std::uint64_t>> properties = {{1,0}, {2,10}};
inline hdr_output_metadata metadata = {};
inline bool unreadable=false, null_data=false, direct_fails=false, no_connector=false;
inline unsigned length=sizeof(metadata);
inline bool disconnected=false, no_encoder=false, wrong_crtc=false, null_property=false;
inline int reads=0;
}
namespace kms {
struct blob_t { unsigned length; void *data; };
using prop_blob_t=std::shared_ptr<blob_t>;
prop_blob_t drmModeGetPropertyBlob(int, std::uint64_t) {
  ++fake::reads;
  if (fake::unreadable) return {};
  return std::make_shared<blob_t>(blob_t{fake::length, fake::null_data ? nullptr : &fake::metadata});
}
struct card_t {
  struct { int el=7; } fd;
  struct connector_t { int connection; unsigned encoder_id; };
  struct encoder_t { unsigned crtc_id; };
  std::optional<connector_t> connector(unsigned) {
    return connector_t{fake::disconnected ? DRM_MODE_DISCONNECTED : DRM_MODE_CONNECTED, fake::no_encoder ? 0u : 4u};
  }
  std::optional<encoder_t> encoder(unsigned) { return encoder_t{fake::wrong_crtc ? 8u : 7u}; }
  using properties_t=std::vector<std::pair<std::shared_ptr<int>,std::uint64_t>>;
  properties_t connector_props(unsigned id) {
    if (!fake::properties[id]) return {};
    return {{fake::null_property ? nullptr : std::make_shared<int>(id), *fake::properties[id]}};
  }
  std::optional<std::uint64_t> prop_value_by_name(const properties_t& properties, std::string_view name) {
    assert(name=="HDR_OUTPUT_METADATA");
    if (properties.empty()) return {};
    assert(properties.front().first);
    return properties.front().second;
  }
};
inline std::mutex display_mutex;
}
}
'''
program += (root / 'src/platform/linux/session_backend.h').read_text().replace('#pragma once', '')
program += r'''
namespace platf::kms {
struct display_vram_t : display_t {
  card_t card;
  hdr::generation_t hdr_metadata_generation;
  std::optional<unsigned> connector_id;
  unsigned crtc_id=7;
  explicit display_vram_t(mem_type_e) {}
  int init(const std::string& name, const video::config_t&) {
    connector_id=fake::no_connector ? std::nullopt : std::optional<unsigned>{name=="DP-1" ? 1u : 2u};
    return fake::direct_fails ? -1 : 0;
  }
'''
program += function('      bool sdr_capture_allowed(')
program += r'''
};
struct display_ram_t : display_vram_t {
  using display_vram_t::display_vram_t;
  int init(const std::string& name, const video::config_t& config) {
    auto old=fake::direct_fails;
    fake::direct_fails=false;
    auto result=display_vram_t::init(name,config);
    fake::direct_fails=old;
    return result;
  }
};
}
namespace platf {
static std::vector<std::string> kms_display_names_unlocked(mem_type_e) { return {"DP-2", "DP-1"}; }
'''
program += function('  std::shared_ptr<display_t> kms_display(')
program += r'''
}
int main() {
  using namespace platf;
  setenv("VIBEPOLLO_MACHINE_HOST","1",1);
  setenv("VIBEPOLLO_SESSION_BACKEND","hyprland-kms",1);
  fake::metadata.hdmi_metadata_type1.eotf=2;
  const video::config_t config;
  // HDR is enumerated first, but an explicitly selected SDR connector works.
  for (auto type : {mem_type_e::system,mem_type_e::vaapi,mem_type_e::cuda,mem_type_e::vulkan}) {
    for (bool fallback : {false,true}) {
      fake::direct_fails=fallback;
      assert(kms_display(type,"DP-1",config));
      assert(!kms_display(type,"DP-2",config));
    }
  }
  fake::direct_fails=false;
  kms::display_vram_t selected(mem_type_e::system);
  selected.init("DP-2",config);
  // Only standard SDR metadata, or an explicit zero property, establishes SDR.
  for (unsigned eotf : {0u,1u,2u,3u,255u}) {
    fake::metadata.hdmi_metadata_type1.eotf=eotf;
    assert(selected.sdr_capture_allowed()==(eotf==0));
  }
  fake::metadata.hdmi_metadata_type1.eotf=0;
  assert(kms_display(mem_type_e::system,"DP-2",config));
  // Reuse the same blob ID: current content, not cached IDs/is_hdr(), governs.
  fake::metadata.hdmi_metadata_type1.eotf=2;
  assert(!selected.sdr_capture_allowed());
  fake::metadata.hdmi_metadata_type1.eotf=0;
  assert(selected.sdr_capture_allowed());
  for (int bad=0;bad<11;++bad) {
    fake::unreadable=bad==0;
    fake::null_data=bad==1;
    fake::length=bad==2 ? sizeof(hdr_output_metadata)-1 : sizeof(hdr_output_metadata);
    fake::metadata.metadata_type=bad==3 ? 1 : 0;
    fake::metadata.hdmi_metadata_type1.metadata_type=bad==4 ? 1 : 0;
    fake::properties[2]=bad==5 ? std::nullopt : std::optional<std::uint64_t>{10};
    fake::no_connector=bad==6;
    fake::disconnected=bad==7;
    fake::no_encoder=bad==8;
    fake::wrong_crtc=bad==9;
    fake::null_property=bad==10;
    assert(!kms_display(mem_type_e::system,"DP-2",config));
  }
  // The root opt-in is required: KWin preserves its existing capture policy.
  setenv("VIBEPOLLO_SESSION_BACKEND","kwin",1);
  assert(kms_display(mem_type_e::system,"DP-2",config));
  std::cout << "PASS: production KMS factory rejects selected HDR/unknown SDR state, direct/RAM fallback, every HDR EOTF and same-ID metadata changes; KWin unchanged\n";
}
'''
with tempfile.TemporaryDirectory(prefix='hyprland-sdr-') as work:
    work = Path(work)
    cpp, binary = work/'sdr.cpp', work/'sdr'
    cpp.write_text(program)
    flags = subprocess.check_output(['pkg-config','--cflags','libdrm'], text=True).split()
    subprocess.run([compiler,'-I',str(root),'-std=c++23','-Wall','-Wextra','-Werror',*flags,str(cpp),'-o',str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
