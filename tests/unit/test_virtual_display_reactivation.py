"""Exercise the production Windows target-reactivation operation with CCD stubs.

The full state-machine tests cover admission/serialization. This harness covers
what crosses the native boundary: exact paths, modes, placement and failures.
"""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
text = (root / 'src/platform/windows/display_helper_v2/win_display_settings.cpp').read_text()
start = text.index('  ApplyStatus WinDisplaySettings::reactivate_virtual_target(')
brace = text.index('{', start)
end, depth = brace + 1, 1
while depth:
    depth += (text[end] == '{') - (text[end] == '}')
    end += 1
body = text[start:end]
program = r'''
#include <algorithm>
#include <cassert>
#include <climits>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <set>
#include <variant>
#include <vector>
using UINT32=std::uint32_t; using LONG=std::int32_t;
struct LUID { unsigned LowPart{}; int HighPart{}; };
struct POINTL { LONG x{}, y{}; };
struct DISPLAYCONFIG_RATIONAL { unsigned Numerator{}, Denominator{}; };
struct DISPLAYCONFIG_SOURCE_MODE { unsigned width{},height{},pixelFormat{}; POINTL position; };
struct DISPLAYCONFIG_SIZE { LONG cx{},cy{}; };
struct DISPLAYCONFIG_ADDITIONAL_SIGNAL_INFO { unsigned videoStandard{},vSyncFreqDivider{}; };
struct DISPLAYCONFIG_VIDEO_SIGNAL_INFO {
  std::uint64_t pixelRate{}; DISPLAYCONFIG_RATIONAL hSyncFreq,vSyncFreq;
  DISPLAYCONFIG_SIZE activeSize,totalSize; DISPLAYCONFIG_ADDITIONAL_SIGNAL_INFO AdditionalSignalInfo;
  unsigned scanLineOrdering{};
};
struct DISPLAYCONFIG_MODE_INFO {
  unsigned infoType{},id{}; LUID adapterId; DISPLAYCONFIG_SOURCE_MODE sourceMode;
  struct {DISPLAYCONFIG_VIDEO_SIGNAL_INFO targetVideoSignalInfo;} targetMode;
};
struct Source { LUID adapterId; unsigned id{},sourceModeInfoIdx{},cloneGroupId{},modeInfoIdx{}; };
struct Target { LUID adapterId; unsigned id{}; bool targetAvailable{}; DISPLAYCONFIG_RATIONAL refreshRate;
  unsigned rotation{}, targetModeInfoIdx{}, desktopModeInfoIdx{}, modeInfoIdx{},scanLineOrdering{};
};
struct DISPLAYCONFIG_PATH_INFO { Source sourceInfo; Target targetInfo; unsigned flags{}; };
constexpr unsigned DISPLAYCONFIG_PATH_ACTIVE=1, DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE=8;
constexpr unsigned DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE=1,DISPLAYCONFIG_MODE_INFO_TYPE_TARGET=2,DISPLAYCONFIG_PIXELFORMAT_32BPP=4;
constexpr unsigned DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE=1;
constexpr unsigned DISPLAYCONFIG_PATH_TARGET_MODE_IDX_INVALID=65535, DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID=65535, DISPLAYCONFIG_PATH_MODE_IDX_INVALID=UINT32_MAX;
constexpr unsigned SDC_VALIDATE=64,SDC_APPLY=128,SDC_USE_SUPPLIED_DISPLAY_CONFIG=32,SDC_VIRTUAL_MODE_AWARE=32768,SDC_ALLOW_CHANGES=1024;
constexpr int ERROR_SUCCESS=0;
namespace boost { bool iequals(const std::string &a,const std::string &b) { return a==b; } }
namespace display_device {
struct Resolution { unsigned m_width{},m_height{}; };
struct Rational { unsigned m_numerator{},m_denominator{}; };
using FloatingPoint=std::variant<Rational,double>;
enum class HdrState {Disabled,Enabled};
struct SingleDisplayConfiguration { std::string m_device_id; std::optional<Resolution> m_resolution;
  std::optional<FloatingPoint> m_refresh_rate; std::optional<HdrState> m_hdr_state;
};
enum class QueryType {All,Active}; enum class DisplayRecoveryBehavior {Skip};
struct DisplayRecoveryBehaviorGuard { explicit DisplayRecoveryBehaviorGuard(DisplayRecoveryBehavior) {} };
struct Data { std::vector<DISPLAYCONFIG_PATH_INFO> m_paths; std::vector<DISPLAYCONFIG_MODE_INFO> m_modes; };
namespace win_utils {
std::optional<unsigned> getSourceIndex(const DISPLAYCONFIG_PATH_INFO &p,const std::vector<DISPLAYCONFIG_MODE_INFO> &m) {
  unsigned i=(p.flags&DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE)?p.sourceInfo.sourceModeInfoIdx:p.sourceInfo.modeInfoIdx;
  return i<m.size()?std::optional<unsigned>{i}:std::nullopt;
}
}
}
using SingleDisplayConfiguration=display_device::SingleDisplayConfiguration;
enum class ApplyStatus {Ok,Retryable,InvalidRequest,VerificationFailed};
struct Display {
 std::optional<display_device::HdrState> hdr=display_device::HdrState::Disabled;
 int changes=0;
 std::map<std::string,std::optional<display_device::HdrState>> getCurrentHdrStates(std::set<std::string>) {return {{"virtual1",hdr}};}
 bool setHdrStates(std::map<std::string,std::optional<display_device::HdrState>> states) {assert(states.size()==1);hdr=states.at("virtual1");++changes;return true;}
};
struct Api {
 display_device::Data data;
 std::vector<unsigned> flags;
 int result=0; bool corrupt_peer=false,refuse_os_timing=false;
 std::string getDeviceId(const DISPLAYCONFIG_PATH_INFO &p) {return p.targetInfo.id==2?"virtual1":"physical";}
 std::optional<display_device::Data> queryDisplayConfig(display_device::QueryType) {return data;}
 int setDisplayConfig(std::vector<DISPLAYCONFIG_PATH_INFO> p,std::vector<DISPLAYCONFIG_MODE_INFO> m,unsigned f) {
  flags.push_back(f);
  const bool explicit_target=std::any_of(m.begin(),m.end(),[](const auto &mode){return mode.infoType==DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;});
  if(refuse_os_timing&&!explicit_target) return 31;
  if(result) return result;
  if(f&SDC_APPLY) {data={p,m}; if(corrupt_peer) data.m_modes[0].sourceMode.width=640;}
  return 0;
 }
};
struct WinDisplaySettings {
 Api *win_api_; Display native_display; Display *display_device_=&native_display; std::mutex settings_mutex_;
 std::map<std::string,DISPLAYCONFIG_SOURCE_MODE> verified_sources_;
 std::map<std::string,DISPLAYCONFIG_RATIONAL> verified_refreshes_;
 std::map<std::string,display_device::HdrState> verified_hdr_states_;
 bool ensure_initialized() {return true;}
 static bool refresh_rates_match(display_device::FloatingPoint a, display_device::FloatingPoint b) {
  auto x=std::get<display_device::Rational>(a),y=std::get<display_device::Rational>(b);
  return x.m_denominator && y.m_denominator && std::abs(double(x.m_numerator)/x.m_denominator-double(y.m_numerator)/y.m_denominator)<0.001;
 }
 static std::optional<double> floating_to_double(display_device::FloatingPoint v) {return std::get<double>(v);}
 ApplyStatus reactivate_virtual_target(const display_device::SingleDisplayConfiguration&,const std::function<bool()>&);
};
''' + body + r'''
Api before(bool active=false) {
 Api a;
 DISPLAYCONFIG_MODE_INFO physical;physical.sourceMode={1920,1080,4,{0,0}};
 a.data.m_modes.push_back(physical);
 DISPLAYCONFIG_PATH_INFO p; p.sourceInfo.adapterId.LowPart=10;p.sourceInfo.id=0;
 p.targetInfo.adapterId.LowPart=10;p.targetInfo.id=1;p.targetInfo.refreshRate={239760,1000};
 p.flags=DISPLAYCONFIG_PATH_ACTIVE|DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE;
 a.data.m_paths.push_back(p);
 p.sourceInfo.adapterId.LowPart=20;p.sourceInfo.id=1;p.sourceInfo.sourceModeInfoIdx=65535;
 p.targetInfo.adapterId.LowPart=20;p.targetInfo.id=2;p.targetInfo.targetAvailable=true;
 p.flags=DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE|(active?DISPLAYCONFIG_PATH_ACTIVE:0);
 a.data.m_paths.push_back(p);return a;
}
int main() {
 display_device::SingleDisplayConfiguration c;c.m_device_id="virtual1";
 c.m_resolution=display_device::Resolution{1920,1280};c.m_refresh_rate=display_device::Rational{1000000,1000};
 auto no=[] {return false;};
 {auto a=before();WinDisplaySettings d{&a};d.verified_sources_["virtual1"]={1920,1280,4,{-1920,0}};
  assert(d.reactivate_virtual_target(c,no)==ApplyStatus::Ok);assert(a.flags.size()==2);
  assert(a.flags[0]==(SDC_VALIDATE|SDC_USE_SUPPLIED_DISPLAY_CONFIG|SDC_VIRTUAL_MODE_AWARE|SDC_ALLOW_CHANGES));
  assert(a.flags[1]==(SDC_APPLY|SDC_USE_SUPPLIED_DISPLAY_CONFIG|SDC_VIRTUAL_MODE_AWARE|SDC_ALLOW_CHANGES));
  assert(a.data.m_paths.size()==2&&a.data.m_modes[0].sourceMode.width==1920);
  assert(a.data.m_paths[0].targetInfo.refreshRate.Numerator==239760);
  assert(a.data.m_modes.back().sourceMode.position.x==-1920);
  assert(a.data.m_paths.back().sourceInfo.cloneGroupId==65535);
  assert(a.data.m_paths.back().targetInfo.refreshRate.Numerator==1000000);}
 {auto a=before(true);WinDisplaySettings d{&a};assert(d.reactivate_virtual_target(c,no)==ApplyStatus::Ok);assert(a.flags.empty());}
 {auto a=before();WinDisplaySettings d{&a};assert(d.reactivate_virtual_target(c,[]{return true;})==ApplyStatus::Retryable);assert(a.flags.empty());}
 {auto a=before();a.result=31;WinDisplaySettings d{&a};assert(d.reactivate_virtual_target(c,no)==ApplyStatus::Retryable);assert(a.flags.size()==2);}
 {auto a=before();a.refuse_os_timing=true;WinDisplaySettings d{&a};assert(d.reactivate_virtual_target(c,no)==ApplyStatus::Ok);
  assert(a.flags.size()==3);assert(a.flags[0]&SDC_VALIDATE);assert(a.flags[1]&SDC_VALIDATE);assert(a.flags[2]&SDC_APPLY);
  assert(a.data.m_modes.back().infoType==DISPLAYCONFIG_MODE_INFO_TYPE_TARGET);
  assert(a.data.m_paths.back().targetInfo.targetModeInfoIdx==a.data.m_modes.size()-1);}
 {auto a=before();a.corrupt_peer=true;WinDisplaySettings d{&a};assert(d.reactivate_virtual_target(c,no)==ApplyStatus::VerificationFailed);}
 {auto a=before();WinDisplaySettings d{&a};c.m_refresh_rate=display_device::Rational{479520,1000};
  assert(d.reactivate_virtual_target(c,no)==ApplyStatus::Ok);assert(a.data.m_paths.back().targetInfo.refreshRate.Numerator==479520);}
 {auto a=before();WinDisplaySettings d{&a};d.verified_sources_["virtual1"]={2560,1440,4,{1920,0}};
  d.verified_refreshes_["virtual1"]={60000,1000};c.m_resolution.reset();c.m_refresh_rate.reset();
  assert(d.reactivate_virtual_target(c,no)==ApplyStatus::Ok);assert(a.data.m_modes.back().sourceMode.width==2560);}
 {auto a=before();WinDisplaySettings d{&a};c.m_resolution=display_device::Resolution{1920,1280};c.m_refresh_rate=display_device::Rational{1000000,1000};
  d.verified_sources_["virtual1"]={1920,1280,4,{0,0}};
  d.verified_hdr_states_["virtual1"]=display_device::HdrState::Enabled;
  assert(d.reactivate_virtual_target(c,no)==ApplyStatus::Ok);
  assert(a.data.m_modes.back().sourceMode.position.x==1920);
  assert(a.data.m_modes[0].sourceMode.position.x==0);
  assert(d.native_display.changes==1&&d.native_display.hdr==display_device::HdrState::Enabled);}
 std::cout<<"Native reactivation: target identity, healthy no-op, peer modes, fractional refresh, timing fallback and cancellation passed\n";
}
'''
with tempfile.TemporaryDirectory(prefix='native-reactivation-') as d:
    cpp = pathlib.Path(d) / 'test.cpp'
    binary = pathlib.Path(d) / 'test'
    cpp.write_text(program)
    subprocess.run(['g++', '-std=c++23', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers', str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
