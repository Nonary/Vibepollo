"""Run the production exclusive-layout hold against a deterministic desktop.

Windows stores one layout per set of connected displays and loads it whenever
that set changes. The hold switches the extra displays off again and saves the
result for the changed set. These checks compile the production functions,
including the admission gate they share with APPLY and REVERT, against a small
model of QueryDisplayConfig and SetDisplayConfig.
"""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
source = (root / 'src/platform/windows/virtual_display_sunshine.cpp').read_text()
integration = (root / 'src/platform/windows/display_helper_integration.cpp').read_text()


def block(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


layout_observation = block(source, '    struct LayoutObservation {') + ';'
observe_layout = block(source, '    std::optional<LayoutObservation> observe_layout(')
hold_result = block(source, '    enum class LayoutHoldResult {') + ';'
apply_hold = block(source, '    LayoutHoldResult apply_exclusive_layout_hold(')
hold_layout = block(source, '    void hold_exclusive_layout(')
repair_gate = block(integration, '  bool run_owned_layout_repair(')

# The monitor loop is too entangled to run here, so its wiring is checked as
# text: only exclusive sessions, only once a finished recovery has settled, and
# on the reading taken by the presence check of the same pass.
monitor = block(source, '    void run_virtual_display_recovery_monitor(')
assert 'if (state.params.exclusive_layout && now >= recovery_cooldown_until) {' in monitor
assert monitor.count('hold_exclusive_layout(state, layout_hold, now, stop_token);') == 1
assert monitor.index('const auto presence = monitor_target_presence(state);') < monitor.index('hold_exclusive_layout(state,')
presence = block(source, '    MonitorTargetPresence monitor_target_presence(')
assert presence.index('state.layout_observation.reset();') < presence.index('enumerate_devices(')
assert 'state.layout_observation = observe_layout(*devices);' in presence

# Both stream paths tell the monitor which sessions are exclusive.
assert 'recovery_params.exclusive_layout =' in (root / 'src/nvhttp.cpp').read_text()
assert 'recovery_params.exclusive_layout = !wants_extended_layout;' in (root / 'src/webrtc_stream.cpp').read_text()

program = r'''
#include "src/platform/windows/virtual_display_recovery_policy.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <vector>
using namespace std::chrono_literals;
using UINT32=std::uint32_t;
using LONG=long;
using HANDLE=void*;
constexpr UINT32 DISPLAYCONFIG_PATH_ACTIVE=0x1;
constexpr UINT32 DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE=1;
constexpr UINT32 DISPLAYCONFIG_MODE_INFO_TYPE_TARGET=2;
constexpr UINT32 SDC_USE_SUPPLIED_DISPLAY_CONFIG=0x20;
constexpr UINT32 SDC_VALIDATE=0x40;
constexpr UINT32 SDC_APPLY=0x80;
constexpr UINT32 SDC_SAVE_TO_DATABASE=0x200;
constexpr UINT32 SDC_VIRTUAL_MODE_AWARE=0x8000;
constexpr LONG ERROR_SUCCESS=0;
constexpr LONG ERROR_ACCESS_DENIED=5;
constexpr LONG ERROR_GEN_FAILURE=31;
constexpr LONG ERROR_NOT_SUPPORTED=50;
constexpr LONG ERROR_INVALID_PARAMETER=87;

struct Path {
 std::string device_id;
 UINT32 flags=0;
 UINT32 source_index=0;
};
struct Mode {
 UINT32 infoType=DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
 struct Source {struct Point {long x=0,y=0;} position;} sourceMode;
};
struct Config {std::vector<Path> m_paths;std::vector<Mode> m_modes;};

// The desktop Windows would report: every connected display, on or off.
std::vector<Path> desktop_paths;
std::vector<Mode> desktop_modes;
std::set<std::string> virtual_ids{"virtual1"};
bool query_ok=true;
LONG validate_result=ERROR_SUCCESS,apply_result=ERROR_SUCCESS;
bool apply_sticks=true;
bool virtual_mode_supported=true;
// Refuse the service account and accept the signed-in user.
bool needs_user=false,as_user=false,user_available=true;
int impersonations=0;
struct Call {UINT32 flags;std::vector<Path> paths;std::vector<Mode> modes;bool as_user;};
std::vector<Call> calls;
std::chrono::milliseconds call_delay{0};

LONG SetDisplayConfig(UINT32 path_count,Path *paths,UINT32 mode_count,Mode *modes,UINT32 flags) {
 calls.push_back({flags,{paths,paths+path_count},{modes,modes+mode_count},as_user});
 std::this_thread::sleep_for(call_delay);
 if(!virtual_mode_supported&&(flags&SDC_VIRTUAL_MODE_AWARE)) return ERROR_INVALID_PARAMETER;
 if(needs_user&&!as_user) return ERROR_ACCESS_DENIED;
 if(flags&SDC_VALIDATE) return validate_result;
 if(apply_result!=ERROR_SUCCESS||!apply_sticks) return apply_result;
 // A display left out of the request keeps its state, as some drivers do.
 for(UINT32 i=0;i<path_count;++i) {
  for(auto &live:desktop_paths) {
   if(live.device_id==paths[i].device_id) live.flags=paths[i].flags;
  }
 }
 desktop_modes.assign(modes,modes+mode_count);
 return ERROR_SUCCESS;
}
int closed_tokens=0;
void CloseHandle(HANDLE) {++closed_tokens;}
namespace platf {
HANDLE retrieve_users_token(bool elevated) {assert(!elevated);static int token;return user_available?&token:nullptr;}
std::error_code impersonate_current_user(HANDLE,std::function<void()> callback) {
 ++impersonations;as_user=true;callback();as_user=false;return {};
}
}

bool equals_ci(const std::string &a,const std::string &b) {
 auto lower=[](std::string s){for(auto &c:s)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));return s;};
 return lower(a)==lower(b);
}

int recovery_guards=0;
namespace display_device {
enum class DisplayRecoveryBehavior {Automatic,Skip};
struct DisplayRecoveryBehaviorGuard {
 explicit DisplayRecoveryBehaviorGuard(DisplayRecoveryBehavior behavior) {assert(behavior==DisplayRecoveryBehavior::Skip);++recovery_guards;}
 ~DisplayRecoveryBehaviorGuard() {--recovery_guards;}
};
enum class QueryType {Active,All};
struct EnumeratedDevice {std::string m_device_id,m_display_name;};
using EnumeratedDeviceList=std::vector<EnumeratedDevice>;
struct WinApiLayer {
 std::optional<Config> queryDisplayConfig(QueryType type) const {
  assert(recovery_guards==1);
  if(!query_ok) return std::nullopt;
  Config config{{},desktop_modes};
  for(const auto &path:desktop_paths) {
   if(type==QueryType::All||(path.flags&DISPLAYCONFIG_PATH_ACTIVE)) config.m_paths.push_back(path);
  }
  return config;
 }
 std::string getDeviceId(const Path &path) const {return path.device_id;}
};
namespace win_utils {
std::optional<UINT32> getSourceIndex(const Path &path,const std::vector<Mode> &modes) {
 return path.source_index<modes.size()?std::optional<UINT32>{path.source_index}:std::nullopt;
}
}
}
bool is_virtual_display_device(const display_device::EnumeratedDevice &device) {
 return std::any_of(virtual_ids.begin(),virtual_ids.end(),[&](const auto &id){return equals_ci(id,device.m_device_id);});
}

struct Log {template<class T> Log &operator<<(const T &) {return *this;}};
#define BOOST_LOG(level) Log{}

struct Lease {std::uint64_t display_id=1,lease_id=2;std::optional<std::string> device_id="Virtual1";};
std::optional<Lease> owned=Lease{};
struct Tracker {std::optional<Lease> get(int) {return owned;}};
Tracker &driver_lease_tracker() {static Tracker tracker;return tracker;}
std::recursive_mutex g_virtual_display_operation_mutex;
''' + layout_observation + r'''
struct RecoveryMonitorState {
 int guid_uuid=1;
 std::optional<std::string> current_device_id="virtual1";
 std::optional<LayoutObservation> layout_observation;
 std::string describe_target() const {return current_device_id.value_or("unknown");}
};
bool monitor_should_abort(const RecoveryMonitorState &,std::stop_token stop) {return stop.stop_requested();}

// What the admission gate in display_helper_integration.cpp consults.
std::mutex apply_execution_mutex;
std::mutex &pending_apply_execution_mutex() {return apply_execution_mutex;}
bool pending_apply=false;
std::function<void()> while_admitting;
bool has_pending_apply() {if(while_admitting) while_admitting();return pending_apply;}
std::atomic<bool> g_restore_expected{false};
bool cancellation_requested(const std::function<bool()> &cancelled) {return cancelled&&cancelled();}
bool mutation_gate_open=true;
int mutation_scopes=0;
namespace display_helper::borrow {
struct native_mutation_scope_t {
 bool held=false;
 static native_mutation_scope_t enter(bool wait) {
  assert(!wait);
  if(mutation_gate_open) ++mutation_scopes;
  return {mutation_gate_open};
 }
 explicit operator bool() const {return held;}
};
}
namespace display_helper_integration {
''' + repair_gate + r'''
}
''' + observe_layout + '\n' + hold_result + '\n' + apply_hold + '\n' + hold_layout + r'''
using hold_t=VDISPLAY::policy::exclusive_layout_hold_t;
const std::set<std::string> monitors{"portrait","wide"};

// Wide monitor asleep and off, portrait monitor off, stream display on.
void exclusive_desktop() {
 desktop_paths={{"virtual1",DISPLAYCONFIG_PATH_ACTIVE,0},{"portrait",0,1},{"wide",0,2}};
 desktop_modes.assign(3,Mode{});
 calls.clear();query_ok=true;validate_result=apply_result=ERROR_SUCCESS;apply_sticks=true;
 virtual_mode_supported=true;needs_user=false;user_available=true;impersonations=0;
}
// What Windows loads when the wide monitor drops: the portrait monitor joins
// on the left and the stream display moves right.
void portrait_joins() {
 desktop_paths={{"virtual1",DISPLAYCONFIG_PATH_ACTIVE,0},{"portrait",DISPLAYCONFIG_PATH_ACTIVE,1}};
 desktop_modes.assign(3,Mode{});
 desktop_modes[0].sourceMode.position={1440,0};
}
bool active(const std::string &id) {
 return std::any_of(desktop_paths.begin(),desktop_paths.end(),[&](const auto &path){
  return path.device_id==id&&(path.flags&DISPLAYCONFIG_PATH_ACTIVE);
 });
}
// The enumeration the monitor takes on each check.
LayoutObservation observed() {
 display_device::EnumeratedDeviceList devices;
 for(const auto &path:desktop_paths) {
  devices.push_back({path.device_id,(path.flags&DISPLAYCONFIG_PATH_ACTIVE)?"DISPLAY":""});
 }
 return *observe_layout(devices);
}

int main() {
 // One enumeration becomes the connected set, the switched-on set, and which
 // of them are Vibeshine's own displays.
 const display_device::EnumeratedDeviceList enumerated_devices{{"Virtual1","DISPLAY20"},{"PORTRAIT",""},{"wide",""}};
 const auto reading=observe_layout(enumerated_devices);
 assert(reading);
 assert((reading->connected==std::set<std::string>{"portrait","virtual1","wide"}));
 assert((reading->active==std::set<std::string>{"virtual1"}));
 assert((reading->virtual_displays==std::set<std::string>{"virtual1"}));
 // A display that cannot be identified could be the stream display.
 const display_device::EnumeratedDeviceList unidentified_devices{{"virtual1","DISPLAY20"},{"",""}};
 const auto unidentified_reading=observe_layout(unidentified_devices);
 assert(!unidentified_reading);

 // The other display is switched off, not left out, the stream display moves
 // back to the origin, and the result is saved for the connected set.
 exclusive_desktop();portrait_joins();
 assert(apply_exclusive_layout_hold("Virtual1",monitors)==LayoutHoldResult::held);
 assert(calls.size()==2);
 const UINT32 aware=SDC_USE_SUPPLIED_DISPLAY_CONFIG|SDC_VIRTUAL_MODE_AWARE;
 assert(calls[0].flags==(aware|SDC_VALIDATE));
 assert(calls[1].flags==(aware|SDC_APPLY|SDC_SAVE_TO_DATABASE));
 assert(calls[1].paths.size()==2);
 assert(calls[1].paths[0].device_id=="virtual1"&&(calls[1].paths[0].flags&DISPLAYCONFIG_PATH_ACTIVE));
 assert(calls[1].paths[1].device_id=="portrait"&&!(calls[1].paths[1].flags&DISPLAYCONFIG_PATH_ACTIVE));
 assert(calls[1].modes[0].sourceMode.position.x==0&&calls[1].modes[0].sourceMode.position.y==0);
 assert(active("virtual1")&&!active("portrait"));
 assert(recovery_guards==0&&impersonations==0);

 // Where Windows does not take virtual-mode data, the same layout is sent without it.
 exclusive_desktop();portrait_joins();virtual_mode_supported=false;
 assert(apply_exclusive_layout_hold("virtual1",monitors)==LayoutHoldResult::held);
 assert(calls.size()==3);
 assert(calls[1].flags==(SDC_USE_SUPPLIED_DISPLAY_CONFIG|SDC_VALIDATE));
 assert(calls[2].flags==(SDC_USE_SUPPLIED_DISPLAY_CONFIG|SDC_APPLY|SDC_SAVE_TO_DATABASE));

 // A refusal of the service account is retried once as the signed-in user.
 exclusive_desktop();portrait_joins();needs_user=true;
 assert(apply_exclusive_layout_hold("virtual1",monitors)==LayoutHoldResult::held);
 assert(impersonations==1&&calls.back().as_user&&(calls.back().flags&SDC_SAVE_TO_DATABASE));
 assert(!active("portrait"));
 exclusive_desktop();portrait_joins();needs_user=true;user_available=false;
 assert(apply_exclusive_layout_hold("virtual1",monitors)==LayoutHoldResult::failed);
 assert(impersonations==0&&active("portrait"));

 // Nothing to do once Windows has already gone back to the stream layout.
 exclusive_desktop();
 assert(apply_exclusive_layout_hold("virtual1",monitors)==LayoutHoldResult::not_needed);
 assert(calls.empty());

 // A stream display that is off belongs to reactivation, not to this repair.
 exclusive_desktop();
 desktop_paths[0].flags&=~DISPLAYCONFIG_PATH_ACTIVE;desktop_paths[1].flags|=DISPLAYCONFIG_PATH_ACTIVE;
 assert(apply_exclusive_layout_hold("virtual1",monitors)==LayoutHoldResult::failed);
 assert(calls.empty()&&active("portrait"));

 // An active display that was not among the monitors in the reading may be
 // another stream's display that appeared since. Nothing is changed.
 exclusive_desktop();portrait_joins();
 desktop_paths.push_back({"virtual2",DISPLAYCONFIG_PATH_ACTIVE,2});
 assert(apply_exclusive_layout_hold("virtual1",monitors)==LayoutHoldResult::failed);
 assert(calls.empty()&&active("portrait")&&active("virtual2"));

 // An unreadable desktop, a rejected layout or a missing source mode changes nothing.
 exclusive_desktop();portrait_joins();query_ok=false;
 assert(apply_exclusive_layout_hold("virtual1",monitors)==LayoutHoldResult::failed);
 assert(calls.empty());
 exclusive_desktop();portrait_joins();validate_result=ERROR_GEN_FAILURE;
 assert(apply_exclusive_layout_hold("virtual1",monitors)==LayoutHoldResult::failed);
 assert(std::none_of(calls.begin(),calls.end(),[](const auto &call){return call.flags&SDC_APPLY;}));
 assert(active("portrait"));
 exclusive_desktop();portrait_joins();desktop_paths[0].source_index=9;
 assert(apply_exclusive_layout_hold("virtual1",monitors)==LayoutHoldResult::failed);
 assert(calls.empty());
 exclusive_desktop();portrait_joins();desktop_modes[0].infoType=DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;
 assert(apply_exclusive_layout_hold("virtual1",monitors)==LayoutHoldResult::failed);
 assert(calls.empty());

 // A refused apply, or one Windows does not keep, is reported as a failure.
 exclusive_desktop();portrait_joins();apply_result=ERROR_GEN_FAILURE;
 assert(apply_exclusive_layout_hold("virtual1",monitors)==LayoutHoldResult::failed);
 exclusive_desktop();portrait_joins();apply_sticks=false;
 assert(apply_exclusive_layout_hold("virtual1",monitors)==LayoutHoldResult::failed);
 assert(active("portrait"));

 // The whole lane: a settled exclusive stream, then the wide monitor drops
 // and Windows loads a stored layout with the portrait monitor on.
 std::stop_source stop;
 hold_t::time_point now{};
 const auto settle=[&](RecoveryMonitorState &state,hold_t &hold) {
  exclusive_desktop();
  state.layout_observation=observed();
  hold_exclusive_layout(state,hold,now,stop.get_token());
  now+=hold_t::settle_time;
  hold_exclusive_layout(state,hold,now,stop.get_token());
  assert(calls.empty()&&hold.repairs()==0);
 };
 const auto check=[&](RecoveryMonitorState &state,hold_t &hold) {
  state.layout_observation=observed();
  hold_exclusive_layout(state,hold,now,stop.get_token());
 };
 {
  RecoveryMonitorState state;hold_t hold;mutation_scopes=0;
  settle(state,hold);
  now+=150ms;portrait_joins();check(state,hold);
  assert(mutation_scopes==1&&hold.repairs()==1);
  assert(calls.size()==2&&(calls[1].flags&SDC_SAVE_TO_DATABASE));
  assert(active("virtual1")&&!active("portrait"));
  // The next check sees the stream layout and asks for nothing more.
  now+=150ms;check(state,hold);
  assert(mutation_scopes==1&&calls.size()==2);
 }
 // A display the user switches on by hand leaves the connected set unchanged,
 // and stays on when the wide monitor later drops.
 {
  RecoveryMonitorState state;hold_t hold;
  settle(state,hold);
  now+=150ms;desktop_paths[1].flags|=DISPLAYCONFIG_PATH_ACTIVE;check(state,hold);
  assert(calls.empty()&&active("portrait"));
  now+=10min;desktop_paths.pop_back();check(state,hold);
  assert(calls.empty()&&active("portrait"));
 }
 // Each admission condition stands the repair down without spending the
 // budget; it runs on a later check once the desktop is free again.
 {
  RecoveryMonitorState state;hold_t hold;
  settle(state,hold);
  now+=150ms;portrait_joins();
  pending_apply=true;check(state,hold);pending_apply=false;
  assert(hold.repairs()==0&&calls.empty());
  g_restore_expected=true;check(state,hold);g_restore_expected=false;
  assert(hold.repairs()==0&&calls.empty());
  mutation_gate_open=false;check(state,hold);mutation_gate_open=true;
  assert(hold.repairs()==0&&calls.empty());
  {
   // An APPLY or a REVERT holds the execution lock for its whole exchange.
   std::unique_lock busy(apply_execution_mutex);
   std::thread monitor([&]{check(state,hold);});
   monitor.join();
  }
  assert(hold.repairs()==0&&calls.empty()&&active("portrait"));
  now+=150ms;check(state,hold);
  assert(hold.repairs()==1&&!active("portrait"));
 }
 // A lease replaced after the reading means the display is no longer ours.
 {
  RecoveryMonitorState state;hold_t hold;
  settle(state,hold);
  now+=150ms;portrait_joins();
  while_admitting=[]{owned->lease_id=99;};check(state,hold);
  assert(hold.repairs()==0&&calls.empty()&&active("portrait"));
  while_admitting={};owned=Lease{};
  // Without a driver lease the display is not this session's to protect.
  owned.reset();check(state,hold);
  assert(hold.repairs()==0&&calls.empty());
  owned=Lease{};
 }
 // The display operation lock held by another thread stands the repair down.
 {
  RecoveryMonitorState state;hold_t hold;
  settle(state,hold);
  now+=150ms;portrait_joins();
  {
   std::unique_lock other(g_virtual_display_operation_mutex);
   std::thread monitor([&]{check(state,hold);});
   monitor.join();
  }
  assert(hold.repairs()==0&&calls.empty()&&active("portrait"));
 }
 // A second Vibeshine display means this one is not meant to be alone: the
 // hold stands aside, and starts over once that display is gone.
 {
  RecoveryMonitorState state;hold_t hold;
  settle(state,hold);
  virtual_ids.insert("virtual2");
  now+=150ms;
  desktop_paths.push_back({"virtual2",DISPLAYCONFIG_PATH_ACTIVE,2});check(state,hold);
  assert(calls.empty()&&active("virtual2"));
  // Connected but switched off, it still suspends the hold.
  desktop_paths.back().flags=0;desktop_paths[1].flags|=DISPLAYCONFIG_PATH_ACTIVE;
  now+=150ms;check(state,hold);
  assert(calls.empty()&&active("portrait"));
  // Gone again: nothing learned before may be acted on until a clean layout settles.
  now+=150ms;portrait_joins();check(state,hold);
  assert(calls.empty()&&active("portrait"));
  virtual_ids.erase("virtual2");
  settle(state,hold);
  now+=150ms;portrait_joins();check(state,hold);
  assert(hold.repairs()==1&&!active("portrait"));
 }
 // A layout Windows keeps refusing is retried at the spacing, not on every
 // check, and stops at the budget.
 {
  RecoveryMonitorState state;hold_t hold;
  now=std::chrono::steady_clock::now()-hold_t::settle_time-1s;
  settle(state,hold);
  portrait_joins();validate_result=ERROR_GEN_FAILURE;
  // A slow attempt: the spacing runs from when it finished, not from when
  // the reading that asked for it was taken.
  call_delay=300ms;now=std::chrono::steady_clock::now();check(state,hold);call_delay=0ms;
  assert(hold.repairs()==1&&active("portrait"));
  const auto calls_after_first=calls.size();
  now+=hold_t::min_repair_interval+50ms;check(state,hold);
  assert(hold.repairs()==1&&calls.size()==calls_after_first);
  for(int i=0;i<40;++i) {
   now=std::chrono::steady_clock::now()+hold_t::min_repair_interval*(i+2);check(state,hold);
  }
  assert(hold.repairs()==hold_t::max_repairs&&hold.exhausted());
  const auto calls_at_budget=calls.size();
  now+=1min;check(state,hold);
  assert(calls.size()==calls_at_budget);
 }
 // No reading or a stopped monitor: nothing happens.
 {
  RecoveryMonitorState state;hold_t hold;
  now=hold_t::time_point{}+1h;
  settle(state,hold);
  now+=150ms;portrait_joins();
  state.layout_observation.reset();
  hold_exclusive_layout(state,hold,now,stop.get_token());
  assert(calls.empty());
  state.layout_observation=observed();
  std::stop_source stopped;stopped.request_stop();
  hold_exclusive_layout(state,hold,now,stopped.get_token());
  assert(hold.repairs()==0&&calls.empty()&&active("portrait"));
 }
}
'''
with tempfile.TemporaryDirectory(prefix='layout-hold-') as d:
    cpp = pathlib.Path(d) / 'test.cpp'
    binary = pathlib.Path(d) / 'test'
    cpp.write_text(program)
    subprocess.run(['g++', '-std=c++23', '-O1', '-pthread', '-Wall', '-Wextra', '-Werror', '-I', str(root), str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=20)
print('Layout hold: stream-only layout restored and saved after a hotplug; user changes, other stream displays, a busy desktop and lost ownership left alone')
