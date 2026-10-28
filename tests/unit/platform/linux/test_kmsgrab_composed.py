"""Exercise production DRM property readers, init lookup, factory and refresh.

The extracted production boundaries run in sequence. Only the DRM APIs,
connector/encoder topology and frame-consumption edge are inert; this is not a
native KMS pipeline, physical ioctl failure or read-to-frame atomicity test.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(sys.argv[1]).resolve()
compiler = sys.argv[2] if len(sys.argv) > 2 else "g++"
source = (root / "src/platform/linux/kmsgrab.cpp").read_text()


def function(marker):
    start = source.index(marker)
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


init_marker = "                auto connector_props = card.connector_props(*connector_id);\n"
init_start = source.index(init_marker)
init_end = source.index("\n", source.index("hdr_metadata_blob_id = card.prop_value_by_name", init_start))
init_lookup = source[init_start:init_end]
refresh_start = source.index("      inline capture_e refresh(")
refresh_end = source.index("        if (pending_exported_frame) {", refresh_start)
refresh_prefix = source[refresh_start:refresh_end]

program = r'''
#include "src/platform/linux/kmsgrab_hdr.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <drm_mode.h>
#include <xf86drmMode.h>
using namespace std::literals;
struct NullLog { template<class T> NullLog& operator<<(const T&) { return *this; } };
#define BOOST_LOG(level) NullLog{}
namespace video { struct config_t {}; }
namespace egl { struct owned_surface_t { struct {} sd; }; }
using platf::capture_e;
namespace platf {
enum class mem_type_e { unknown, cuda, vaapi, vulkan, system };
struct display_t { virtual ~display_t() = default; };
namespace fake {
enum class read_e { zero, sdr_blob, pq_blob, gamma_blob, hlg_blob, unknown_blob, malformed_blob, null_only, null_and_zero, failed_list, missing_hdr };
inline std::vector<read_e> plan;
inline read_e current;
inline unsigned reads=0, property_reads=0, blob_reads=0, frame_boundaries=0;
inline std::vector<std::uint32_t> ids;
inline std::vector<std::uint64_t> values;
inline hdr_output_metadata metadata={};
inline bool direct_fails=false;
void reset(std::vector<read_e> sequence) {
  plan=std::move(sequence);
  reads=property_reads=blob_reads=frame_boundaries=0;
  metadata={}; // Valid standard metadata with EOTF 0.
}
}
namespace kms {
inline std::mutex display_mutex;
using prop_t=std::unique_ptr<drmModePropertyRes>;
using obj_prop_t=std::unique_ptr<drmModeObjectProperties>;
using prop_blob_t=std::unique_ptr<drmModePropertyBlobRes>;
obj_prop_t drmModeObjectGetProperties(int fd, unsigned id, unsigned type) {
  assert(fd==7 && id==1 && type==DRM_MODE_OBJECT_CONNECTOR);
  assert(fake::reads<fake::plan.size());
  fake::current=fake::plan[fake::reads++];
  fake::metadata={};
  if(fake::current==fake::read_e::pq_blob) fake::metadata.hdmi_metadata_type1.eotf=2;
  if(fake::current==fake::read_e::gamma_blob) fake::metadata.hdmi_metadata_type1.eotf=1;
  if(fake::current==fake::read_e::hlg_blob) fake::metadata.hdmi_metadata_type1.eotf=3;
  if(fake::current==fake::read_e::malformed_blob) fake::metadata.metadata_type=1;
  if (fake::current==fake::read_e::failed_list) return {};
  if (fake::current==fake::read_e::missing_hdr) {
    fake::ids={2};
    fake::values={0};
  } else if (fake::current==fake::read_e::null_and_zero) {
    fake::ids={2,1};
    fake::values={0,0};
  } else {
    fake::ids={1};
    const bool has_blob=fake::current==fake::read_e::sdr_blob || fake::current==fake::read_e::pq_blob ||
      fake::current==fake::read_e::gamma_blob || fake::current==fake::read_e::hlg_blob ||
      fake::current==fake::read_e::unknown_blob || fake::current==fake::read_e::malformed_blob;
    fake::values={has_blob ? 10u : 0u};
  }
  auto properties=std::make_unique<drmModeObjectProperties>();
  properties->count_props=fake::ids.size();
  properties->props=fake::ids.data();
  properties->prop_values=fake::values.data();
  return properties;
}
prop_t drmModeGetProperty(int fd, unsigned id) {
  assert(fd==7);
  ++fake::property_reads;
  if (fake::current==fake::read_e::null_only ||
      (fake::current==fake::read_e::null_and_zero && id==2)) return {};
  auto property=std::make_unique<drmModePropertyRes>();
  std::strcpy(property->name,id==1 ? "HDR_OUTPUT_METADATA" : "link-status");
  return property;
}
prop_blob_t drmModeGetPropertyBlob(int fd, std::uint64_t id) {
  assert(fd==7 && id==10);
  ++fake::blob_reads;
  if(fake::current==fake::read_e::unknown_blob) return {};
  auto blob=std::make_unique<drmModePropertyBlobRes>();
  blob->length=sizeof(fake::metadata);
  blob->data=&fake::metadata;
  return blob;
}
struct card_t {
  struct { int el=7; } fd;
  struct connector_t { int connection=DRM_MODE_CONNECTED; unsigned encoder_id=4; };
  struct encoder_t { unsigned crtc_id=7; };
  std::optional<connector_t> connector(unsigned id) { assert(id==1); return connector_t{}; }
  std::optional<encoder_t> encoder(unsigned id) { assert(id==4); return encoder_t{}; }
'''
for marker in (
    "      std::vector<std::pair<prop_t, std::uint64_t>> props(",
    "      std::vector<std::pair<prop_t, std::uint64_t>> connector_props(",
    "      std::optional<std::uint64_t> prop_value_by_name(",
):
    program += function(marker) + "\n"
program += "};\n}\n}\n"
program += (root / "src/platform/linux/session_backend.h").read_text().replace("#pragma once", "")
program += r'''
namespace platf::kms {
struct display_vram_t : display_t {
  card_t card;
  std::optional<unsigned> connector_id=1;
  unsigned crtc_id=7;
  std::optional<std::uint64_t> hdr_metadata_blob_id;
  hdr::generation_t hdr_metadata_generation;
  explicit display_vram_t(mem_type_e) {}
  int init(const std::string& name, const video::config_t&) {
    assert(name=="DP-1");
    if (fake::direct_fails) return -1;
'''
program += init_lookup + "\n    return 0;\n  }\n"
program += function("      bool sdr_capture_allowed(") + "\n" + function("      bool is_hdr() {") + "\n"
program += refresh_prefix + r'''
    (void)sd;
    (void)frame_timestamp;
    ++fake::frame_boundaries;
    return capture_e::ok; // Crossing the pending-frame-consumption boundary.
  }
};
struct display_ram_t : display_vram_t {
  using display_vram_t::display_vram_t;
  int init(const std::string& name, const video::config_t& config) {
    const bool old=fake::direct_fails;
    fake::direct_fails=false;
    const int result=display_vram_t::init(name,config);
    fake::direct_fails=old;
    return result;
  }
};
}
namespace platf {
static std::vector<std::string> kms_display_names_unlocked(mem_type_e) { return {"DP-1"}; }
'''
program += function("  std::shared_ptr<display_t> kms_display(")
program += r'''
}
extern "C" drmModePropertyBlobPtr drmModeGetPropertyBlob(int fd,uint32_t id) {
  auto value=platf::kms::drmModeGetPropertyBlob(fd,id);
  if(!value) return nullptr;
  auto *owned=new drmModePropertyBlobRes(*value);
  owned->data=new hdr_output_metadata(*static_cast<hdr_output_metadata*>(value->data));
  return owned;
}
extern "C" void drmModeFreePropertyBlob(drmModePropertyBlobPtr blob) {
  delete static_cast<hdr_output_metadata*>(blob->data); delete blob;
}
using platf::fake::read_e;
void sequence(read_e initial, read_e second, bool expect_frame) {
  using namespace platf;
  for (auto type : {mem_type_e::system,mem_type_e::vaapi,mem_type_e::cuda,mem_type_e::vulkan}) {
    for (bool fallback : {false,true}) {
      fake::reset({initial,read_e::zero,read_e::zero,second});
      fake::direct_fails=fallback;
      auto base=kms_display(type,"DP-1",video::config_t{});
      assert(base);
      auto capture=std::dynamic_pointer_cast<kms::display_vram_t>(base);
      assert(capture && fake::reads==2);
      assert(!capture->is_hdr());
      if (initial==read_e::zero) {
        assert(capture->hdr_metadata_blob_id && *capture->hdr_metadata_blob_id==0);
      } else {
        // The actual initial lookup cached nullopt. The actual strict factory
        // check then recovered a complete, present-zero SDR property.
        assert(!capture->hdr_metadata_blob_id);
      }
      egl::owned_surface_t surface;
      std::optional<std::chrono::steady_clock::time_point> timestamp;
      const auto result=capture->refresh(surface,timestamp);
      assert(fake::reads==4);
      assert((result==capture_e::ok)==expect_frame);
      assert(fake::frame_boundaries==(expect_frame ? 1u : 0u));
      // Refusing a frame is insufficient if the same observation stages HDR
      // for the next encoder. Rejected strict observations must stage nothing.
      assert(!capture->hdr_metadata_generation.commit_pending());
    }
  }
}
int main(int argc, char **argv) {
  using namespace platf;
  assert(argc==2);
  setenv("VIBEPOLLO_MACHINE_HOST","1",1);
  setenv("VIBEPOLLO_SESSION_BACKEND","hyprland-kms",1);
  const std::string_view boundary=argv[1];
  std::cout << "RUN: production property boundary " << boundary << std::endl;
  if (boundary=="init") {
    for (auto initial : {read_e::null_only,read_e::failed_list,read_e::missing_hdr}) {
      fake::reset({initial});
      fake::direct_fails=false;
      kms::display_vram_t capture(mem_type_e::system);
      assert(capture.init("DP-1",video::config_t{})==0);
      assert(!capture.hdr_metadata_blob_id && fake::reads==1);
    }
    fake::reset({read_e::null_and_zero});
    kms::display_vram_t capture(mem_type_e::system);
    assert(capture.init("DP-1",video::config_t{})==0);
    assert(capture.hdr_metadata_blob_id && *capture.hdr_metadata_blob_id==0);
  } else if (boundary=="refresh") {
    sequence(read_e::zero,read_e::null_and_zero,false);
  } else if (boundary=="second-list" || boundary=="second-missing") {
    const auto second=boundary=="second-list" ? read_e::failed_list : read_e::missing_hdr;
    for (auto initial : {read_e::null_only,read_e::failed_list,read_e::missing_hdr}) {
      sequence(initial,second,false);
    }
  } else if (boundary=="second-state") {
    for(auto second : {read_e::pq_blob,read_e::gamma_blob,read_e::hlg_blob,read_e::unknown_blob,read_e::malformed_blob}) {
      sequence(read_e::zero,second,false);
    }
  } else if (boundary=="healthy") {
    // Present zero is known SDR, not the missing optional. It reaches frames.
    sequence(read_e::zero,read_e::zero,true);
    fake::reset({read_e::sdr_blob,read_e::sdr_blob,read_e::sdr_blob,read_e::sdr_blob});
    fake::direct_fails=false;
    auto base=kms_display(mem_type_e::system,"DP-1",video::config_t{});
    auto capture=std::dynamic_pointer_cast<kms::display_vram_t>(base);
    assert(capture && capture->hdr_metadata_blob_id && *capture->hdr_metadata_blob_id==10);
    assert(!capture->is_hdr());
    egl::owned_surface_t surface;
    std::optional<std::chrono::steady_clock::time_point> timestamp;
    assert(capture->refresh(surface,timestamp)==capture_e::ok);
    assert(fake::reads==4 && fake::blob_reads==3 && fake::frame_boundaries==1);
  } else if (boundary=="kwin") {
    setenv("VIBEPOLLO_SESSION_BACKEND","kwin",1);
    // Preserve inherited KWin ID-comparison semantics, including unknown IDs.
    for (auto read : {read_e::failed_list,read_e::missing_hdr,read_e::zero,read_e::sdr_blob}) {
      fake::reset({read,read});
      fake::direct_fails=false;
      auto base=kms_display(mem_type_e::system,"DP-1",video::config_t{});
      auto capture=std::dynamic_pointer_cast<kms::display_vram_t>(base);
      assert(capture && fake::reads==1);
      egl::owned_surface_t surface;
      std::optional<std::chrono::steady_clock::time_point> timestamp;
      assert(capture->refresh(surface,timestamp)==capture_e::ok);
      assert(fake::reads==2 && fake::frame_boundaries==1);
    }
    fake::reset({read_e::zero,read_e::null_and_zero});
    auto base=kms_display(mem_type_e::system,"DP-1",video::config_t{});
    auto capture=std::dynamic_pointer_cast<kms::display_vram_t>(base);
    assert(capture);
    egl::owned_surface_t surface;
    std::optional<std::chrono::steady_clock::time_point> timestamp;
    assert(capture->refresh(surface,timestamp)==capture_e::ok);
    assert(fake::reads==2 && fake::frame_boundaries==1);
  } else {
    assert(false && "unknown test boundary");
  }
  std::cout << "PASS: production property boundary " << boundary << "\n";
}
'''

with tempfile.TemporaryDirectory(prefix="hyprland-property-") as directory:
    cpp = Path(directory) / "boundary.cpp"
    binary = Path(directory) / "boundary"
    cpp.write_text(program)
    flags = subprocess.check_output(["pkg-config", "--cflags", "libdrm"], text=True).split()
    subprocess.run(
        [compiler, "-std=c++23", "-Wall", "-Wextra", "-Werror", "-Wno-sign-compare",
         "-fsanitize=undefined", "-fno-sanitize-recover=undefined", *flags,
         "-I",str(root),str(cpp), "-o", str(binary)], check=True
    )
    boundaries = (sys.argv[3],) if len(sys.argv) > 3 else (
        "init", "refresh", "second-list", "second-missing", "second-state", "healthy", "kwin"
    )
    for boundary in boundaries:
        subprocess.run([str(binary), boundary], check=True)
