"""Run the production live-recovery admission against deterministic driver seams."""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
source = (root / 'src/platform/windows/virtual_display_sunshine.cpp').read_text()

def function(signature):
    start = source.index(signature)
    brace = source.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

ccd_presence = function('    MonitorTargetPresence ccd_target_presence(')
driver_presence = function('    MonitorTargetPresence driver_target_presence(')
target_presence = function('    MonitorTargetPresence monitor_target_presence(')
target_presence = target_presence.replace('monitor_target_presence(', 'actual_monitor_target_presence(')
reactivate_presence = function('    bool reactivate_owned_virtual_display(')
reactivate_presence = reactivate_presence.replace('monitor_target_presence(', 'actual_monitor_target_presence(')
gdi_name_check = function('    bool is_gdi_display_name(')
presence_check = function('    bool is_virtual_display_present(')
presence_check = presence_check.replace('is_virtual_display_present(', 'actual_is_virtual_display_present(')
monitor_path_for_id = function('    std::optional<std::wstring> resolve_monitor_device_path_for_device_id(')

program = r'''
#include "src/capture_recovery.h"
#include "src/platform/windows/virtual_display_recovery_policy.h"
#include <cassert>
#include <chrono>
#include <future>
#include <optional>
#include <stop_token>
#include <thread>
using namespace std::chrono_literals;
struct Lease { std::uint64_t display_id=1,lease_id=2; std::optional<std::string> device_id="virtual1"; };
std::optional<Lease> owned=Lease{};
int tracker_gets=0;
bool mutate_device_id_after_snapshot=false;
struct Tracker {
 std::optional<Lease> get(int) {
  auto snapshot=owned;
  if(mutate_device_id_after_snapshot&&tracker_gets++==0&&owned) owned->device_id="replacement-virtual";
  return snapshot;
 }
};
Tracker &driver_lease_tracker() {static Tracker tracker;return tracker;}
struct Transport { bool valid() const {return true;} };
std::shared_ptr<Transport> control_transport_snapshot() {return std::make_shared<Transport>();}
bool responsive=true,provenance=true,query_ok=true,helper_ok=true;
Lease entry;
int helper_applies=0,resets=0;
struct CcdTargetInfo {bool targetAvailable=true;};
struct CcdPath {
 std::string device_id,monitor_path;
 unsigned flags=0;
 CcdTargetInfo targetInfo;
 CcdPath(std::string device,std::string monitor,unsigned path_flags,CcdTargetInfo target_info={}):
  device_id(std::move(device)),monitor_path(std::move(monitor)),flags(path_flags),targetInfo(target_info) {}
};
struct CcdConfig {std::vector<CcdPath> m_paths;};
std::vector<CcdPath> ccd_paths;
bool ccd_query_ok=true;
constexpr unsigned DISPLAYCONFIG_PATH_ACTIVE=1;
bool equals_ci(const std::string &a,const std::string &b) {
 auto lower=[](std::string s){for(auto &c:s)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));return s;};
 return lower(a)==lower(b);
}
std::string normalize_display_name(const std::string &s) {
 auto result=s;for(auto &c:result)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));return result;
}
bool driver_transport_responsive(Transport*) {return responsive;}
bool render_adapter_request_provenance_matches(int,const char*) {return provenance;}
std::recursive_mutex g_virtual_display_operation_mutex;
namespace sunshine_driver {
struct ControlClient {
 explicit ControlClient(Transport&) {}
 struct Result {
  struct Value {unsigned entry_count=1;Lease entries[1];} value;
  bool ok() const {return query_ok;}
 };
 Result query_display_state() {Result r;r.value.entries[0]=entry;return r;}
};
}
namespace platf {
std::string to_utf8(const std::wstring &s) {return {s.begin(),s.end()};}
std::wstring from_utf8(const std::string &s) {return {s.begin(),s.end()};}
}
namespace mail {
constexpr char switch_display[]="switch";
struct Manager {
 template<class T> Manager *event(const char*) {return this;}
 void raise(int value) {assert(value==-1);++resets;}
};
Manager manager;Manager *man=&manager;
}
struct RecoveryMonitorState {
 struct Params {std::string client_name;};
 Params params;
 int guid_uuid=1;
 std::optional<std::wstring> current_display_name=L"DISPLAY15",current_monitor_device_path=L"monitor1";
 std::optional<std::string> current_device_id="virtual1",normalized_monitor_device_path="monitor1";
 std::optional<std::string> normalized_display_name="display15";
 video::capture_recovery::confirmation_t capture_confirmation;
 std::uint64_t capture_lease_id=0,capture_display_id=0;
 std::optional<std::string> capture_device_id;
 void update_identifiers(const std::optional<std::wstring> &name,const std::optional<std::string> &id,
                         const std::optional<std::wstring> &path) {
  current_display_name=name;current_device_id=id;current_monitor_device_path=path;
  normalized_display_name=name?std::optional<std::string>{normalize_display_name(std::string(name->begin(),name->end()))}:std::nullopt;
  normalized_monitor_device_path=path?std::optional<std::string>{normalize_display_name(std::string(path->begin(),path->end()))}:std::nullopt;
 }
 std::string describe_target() const {return current_device_id.value_or("unknown");}
};
bool monitor_should_abort(const RecoveryMonitorState&,std::stop_token stop) {return stop.stop_requested();}
enum class MonitorTargetPresence {unknown,missing,no_devices,present_inactive,present_active};
std::optional<bool> ccd_result=true;
std::optional<bool> ccd_has_active_paths() {return ccd_result;}
namespace display_device {
enum class DeviceEnumerationDetail {Minimal};
enum class QueryType {All};
struct EnumeratedDevice {
 std::string m_device_id,m_monitor_device_path,m_display_name,m_friendly_name;
 bool virtual_display=true;
};
struct WinApiLayer {
 std::optional<CcdConfig> queryDisplayConfig(QueryType) const {return ccd_query_ok?std::optional<CcdConfig>{CcdConfig{ccd_paths}}:std::nullopt;}
 std::string getDeviceId(const CcdPath &path) const {return path.device_id;}
 std::string getMonitorDevicePath(const CcdPath &path) const {return path.monitor_path;}
};
}
std::vector<display_device::EnumeratedDevice> enumerated_devices;
bool enumeration_ok=true;
int enumeration_queries=0;
void observe_frame(const std::string &output,std::uint64_t instance,
                   std::chrono::steady_clock::time_point now=std::chrono::steady_clock::now()) {
 video::capture_recovery::observe_frame_from_current_device_list(
  video::capture_recovery::observations(),output,instance,
  [] {
   ++enumeration_queries;
   if(!enumeration_ok) return std::optional<std::vector<display_device::EnumeratedDevice>>{};
   return std::optional{enumerated_devices};
  },
  [](const auto &device)->const std::string& {return device.m_display_name;},
  [](const auto &device)->const std::string& {return device.m_device_id;},now
 );
}
bool is_virtual_display_device(const display_device::EnumeratedDevice &device) {return device.virtual_display;}
namespace platf::display_helper {
struct Coordinator {
 static Coordinator &instance() {static Coordinator c;return c;}
 std::optional<std::vector<display_device::EnumeratedDevice>> enumerate_devices(display_device::DeviceEnumerationDetail) {return enumerated_devices;}
};
}
struct Log {template<class T> Log &operator<<(T) {return *this;}};
#define BOOST_LOG(level) Log{}
namespace display_helper_integration {
bool reactivate_owned_virtual_display(const std::string &id,const std::function<bool()> &validate,std::function<bool()> cancelled) {
 assert(id=="virtual1");
 if(cancelled()||!validate()) return false;
 ++helper_applies;return helper_ok;
}
}
''' + function('    std::optional<std::wstring> resolve_virtual_display_name_for_device_id(') + '\n' + monitor_path_for_id + '\n' + gdi_name_check + '\n' + presence_check + '\n' + ccd_presence + '\n' + driver_presence + '\n' + target_presence + '\n' + reactivate_presence + r'''
int main() {
 assert(VDISPLAY::policy::readiness_has_dynamic_identity_hint(true,false,false,false,false,false));
 assert(VDISPLAY::policy::readiness_has_strong_identity_hint(true,false,false,false));
 assert(VDISPLAY::policy::readiness_rejects_weak_identity_candidate(true,true));
 assert(!VDISPLAY::policy::readiness_rejects_weak_identity_candidate(false,true));
 assert(VDISPLAY::policy::readiness_rejects_weak_identity_candidate(false,false));
 // A mutable GDI alias can match the wrong peer's device ID during creation.
 // With a requested stable identity, only the matching EDID may prove the
 // exact target; without one, the existing strong-hint behavior still applies.
 assert(!VDISPLAY::policy::readiness_candidate_is_exact_target(true,true,false));
 assert(VDISPLAY::policy::readiness_candidate_is_exact_target(true,false,true));
 assert(VDISPLAY::policy::readiness_candidate_is_exact_target(false,true,false));
 assert(VDISPLAY::policy::readiness_candidate_is_exact_target(false,false,true));
 assert(VDISPLAY::policy::defer_unidentified_readiness_candidate(false,false));
 assert(!VDISPLAY::policy::defer_unidentified_readiness_candidate(false,true));
 assert(!VDISPLAY::policy::defer_unidentified_readiness_candidate(true,false));
 enumerated_devices={{"peer-id","peer-path","\\\\.\\DISPLAY17","client",true}};
 assert(!actual_is_virtual_display_present(std::nullopt,std::nullopt));
 assert(!actual_is_virtual_display_present(std::optional<std::wstring>{L"\\\\.\\DISPLAY17"},std::optional<std::string>{"target-id"}));
 assert(actual_is_virtual_display_present(std::nullopt,std::optional<std::string>{"peer-id"}));
 assert(actual_is_virtual_display_present(std::optional<std::wstring>{L"client"},std::nullopt));
 assert(!VDISPLAY::policy::readiness_has_dynamic_identity_hint(false,false,false,false,false,false));
 assert(!VDISPLAY::policy::readiness_has_strong_identity_hint(false,false,false,false));
 assert(video::capture_recovery::lease_identity_matches(
  2,1,std::string("virtual1"),2,1,std::string("virtual1")));
 assert(!video::capture_recovery::lease_identity_matches(
  2,1,std::string("replacement-virtual"),2,1,std::string("virtual1")));
 assert(!video::capture_recovery::lease_identity_matches(
  2,1,std::string("virtual1"),3,1,std::string("virtual1")));
 assert(!video::capture_recovery::lease_identity_matches(
  2,1,std::nullopt,2,1,std::string("virtual1")));
 auto reclaimed_id=VDISPLAY::policy::select_reuse_device_id(true,std::string("virtual1"),std::string("replacement-virtual"));
 assert(reclaimed_id&&*reclaimed_id=="virtual1");
 assert(!VDISPLAY::policy::select_reuse_device_id(true,std::nullopt,std::string("replacement-virtual")));
 auto unowned_candidate=VDISPLAY::policy::select_reuse_device_id(false,std::nullopt,std::string("replacement-virtual"));
 assert(unowned_candidate&&*unowned_candidate=="replacement-virtual");
 enumerated_devices={{"replacement-virtual","monitor3","DISPLAY17","client",true},
                     {"virtual1","monitor1","DISPLAY18","client",true}};
 assert(resolve_virtual_display_name_for_device_id("virtual1")==L"DISPLAY18");
 assert(!resolve_virtual_display_name_for_device_id("missing-virtual"));
 int client_name_lookups=0, any_name_lookups=0;
 auto reclaimed_name=VDISPLAY::policy::resolve_reuse_display_name(
  true,std::optional<std::string>{"virtual1"},true,
  [](const std::string &id){return resolve_virtual_display_name_for_device_id(id);},
  [&]{++client_name_lookups;return std::optional<std::wstring>{L"DISPLAY17"};},
  [&]{++any_name_lookups;return std::optional<std::wstring>{L"DISPLAY99"};});
 assert(reclaimed_name&&*reclaimed_name==L"DISPLAY18");
 assert(client_name_lookups==0&&any_name_lookups==0);
 auto unresolved_reclaimed_name=VDISPLAY::policy::resolve_reuse_display_name(
  true,std::optional<std::string>{"missing-virtual"},true,
  [](const std::string &id){return resolve_virtual_display_name_for_device_id(id);},
  [&]{++client_name_lookups;return std::optional<std::wstring>{L"DISPLAY17"};},
  [&]{++any_name_lookups;return std::optional<std::wstring>{L"DISPLAY99"};});
 assert(!unresolved_reclaimed_name);
 assert(client_name_lookups==0&&any_name_lookups==0);
 auto ordinary_reuse_name=VDISPLAY::policy::resolve_reuse_display_name(
  false,std::nullopt,false,
  [](const std::string &){return std::optional<std::wstring>{};},
  [&]{++client_name_lookups;return std::optional<std::wstring>{L"DISPLAY17"};},
  [&]{++any_name_lookups;return std::optional<std::wstring>{L"DISPLAY99"};});
 assert(ordinary_reuse_name&&*ordinary_reuse_name==L"DISPLAY17");
 assert(client_name_lookups==1&&any_name_lookups==0);
 enumerated_devices={{"replacement-virtual","monitor3","\\\\.\\DISPLAY18","client",true}};
 int name_path_lookups=0;
 auto stale_gdi_alias_path=VDISPLAY::policy::resolve_monitor_device_path(
  std::optional<std::string>{"virtual1"},
  [](const std::string &id){return resolve_monitor_device_path_for_device_id(id);},
  [&]{++name_path_lookups;return std::optional<std::wstring>{L"monitor3"};});
 assert(!stale_gdi_alias_path&&name_path_lookups==0);
 auto name_only_path=VDISPLAY::policy::resolve_monitor_device_path(
  std::nullopt,
  [](const std::string &){return std::optional<std::wstring>{};},
  [&]{++name_path_lookups;return std::optional<std::wstring>{L"monitor3"};});
 assert(name_only_path&&*name_only_path==L"monitor3"&&name_path_lookups==1);
 enumerated_devices={{"replacement-virtual","monitor3","\\\\.\\DISPLAY17","client",true},
                     {"virtual1","monitor1","","client",true}};
 auto reclaimed_name_without_gdi=VDISPLAY::policy::resolve_reuse_display_name(
  true,std::optional<std::string>{"virtual1"},true,
  [](const std::string &id){return resolve_virtual_display_name_for_device_id(id);},
  [&]{++client_name_lookups;return std::optional<std::wstring>{L"\\\\.\\DISPLAY17"};},
  [&]{++any_name_lookups;return std::optional<std::wstring>{L"\\\\.\\DISPLAY17"};});
 assert(reclaimed_name_without_gdi&&*reclaimed_name_without_gdi==L"virtual1");
 assert(!is_gdi_display_name(*reclaimed_name_without_gdi));
 assert(client_name_lookups==1&&any_name_lookups==0);
 enumerated_devices.clear();
 RecoveryMonitorState state;
 ccd_paths={{"virtual1","monitor1",0},{"physical","monitor2",DISPLAYCONFIG_PATH_ACTIVE}};
 // Exercise the full production empty-enumeration path. Unknown CCD results
 // stay unknown, and an exact available inactive lease remains recoverable.
 ccd_result.reset();
 assert(actual_monitor_target_presence(state)==MonitorTargetPresence::unknown);
 ccd_result=true;
 assert(actual_monitor_target_presence(state)==MonitorTargetPresence::present_inactive);
 ccd_paths={{"virtual1","monitor1",0}};
 ccd_result=false;
 assert(actual_monitor_target_presence(state)==MonitorTargetPresence::present_inactive);
 // A retained driver lease alone must not turn a missing target into a 12s
 // inactive grace. Other enumerated CCD outputs do not prove this target exists.
 ccd_result=true;
 ccd_paths={{"physical","monitor2",DISPLAYCONFIG_PATH_ACTIVE}};
 assert(driver_target_presence(state)==MonitorTargetPresence::missing);
 ccd_paths={{"virtual1","monitor1",DISPLAYCONFIG_PATH_ACTIVE}};
 assert(driver_target_presence(state)==MonitorTargetPresence::present_active);
 ccd_paths={{"virtual1","monitor1",0,CcdTargetInfo{false}}};
 assert(driver_target_presence(state)==MonitorTargetPresence::missing);
 ccd_paths={{"","",0}};
 assert(driver_target_presence(state)==MonitorTargetPresence::unknown);
 ccd_paths={{"virtual1","monitor1",0}};
 ccd_query_ok=false;
 assert(driver_target_presence(state)==MonitorTargetPresence::unknown);
 ccd_query_ok=true;

 // A stale GDI display name cannot override mismatching stable target IDs.
 RecoveryMonitorState identity_state;
 enumerated_devices={{"other-virtual","monitor2","DISPLAY15","",true}};
 ccd_paths={{"physical","monitor2",DISPLAYCONFIG_PATH_ACTIVE}};
 assert(actual_monitor_target_presence(identity_state)==MonitorTargetPresence::missing);
 assert(identity_state.current_device_id=="virtual1");
 // A fresh display name is trusted only after an exact device-ID/path match.
 enumerated_devices={{"virtual1","monitor1","DISPLAY16","",true}};
 assert(actual_monitor_target_presence(identity_state)==MonitorTargetPresence::present_active);
 assert(identity_state.current_display_name==L"DISPLAY16");
 // A friendly-name candidate with a different ID cannot establish presence
 // for a monitor that owns an exact driver lease.
 identity_state.current_device_id.reset();
 identity_state.current_monitor_device_path.reset();
 identity_state.normalized_monitor_device_path.reset();
 identity_state.current_display_name.reset();
 identity_state.normalized_display_name.reset();
 identity_state.params.client_name="client";
 enumerated_devices={{"replacement-virtual","monitor3","DISPLAY17","client",true}};
 ccd_paths={{"physical","monitor2",DISPLAYCONFIG_PATH_ACTIVE}};
 assert(actual_monitor_target_presence(identity_state)==MonitorTargetPresence::missing);
 assert(!identity_state.current_device_id);
 assert(!identity_state.current_display_name);
 // The same friendly name can establish presence when the candidate ID is
 // exactly the one retained by the driver lease.
 enumerated_devices={{"virtual1","monitor1","DISPLAY18","client",true}};
 ccd_paths={{"virtual1","monitor1",DISPLAYCONFIG_PATH_ACTIVE}};
 assert(actual_monitor_target_presence(identity_state)==MonitorTargetPresence::present_active);
 assert(identity_state.current_device_id=="virtual1");
 assert(identity_state.current_display_name==L"DISPLAY18");
 enumerated_devices.clear();
 ccd_paths={{"virtual1","monitor1",0}};

 ccd_result=false;
 owned.reset();
 assert(actual_monitor_target_presence(state)==MonitorTargetPresence::no_devices);
 ccd_result=true;
 assert(actual_monitor_target_presence(state)==MonitorTargetPresence::unknown);
 owned=Lease{};
 assert(driver_target_presence(state)==MonitorTargetPresence::present_inactive);
 query_ok=false;
 assert(driver_target_presence(state)==MonitorTargetPresence::unknown);
 assert(!reactivate_owned_virtual_display(state,{}));query_ok=true;
 entry.lease_id=3;
 assert(driver_target_presence(state)==MonitorTargetPresence::missing);
 assert(!reactivate_owned_virtual_display(state,{}));entry.lease_id=2;
 provenance=false;assert(!reactivate_owned_virtual_display(state,{}));provenance=true;
 responsive=false;assert(!reactivate_owned_virtual_display(state,{}));responsive=true;
 owned.reset();assert(!reactivate_owned_virtual_display(state,{}));owned=Lease{};
 std::stop_source stopped;stopped.request_stop();
 assert(!reactivate_owned_virtual_display(state,stopped.get_token()));
 // A generic recovery can own the operation lock while entering helper
 // admission. Live repair must decline immediately, never wait in reverse order.
 std::promise<void> locked,release;
 auto released=release.get_future();
 std::thread owner([&] {std::lock_guard lock(g_virtual_display_operation_mutex);locked.set_value();released.wait();});
 locked.get_future().wait();
 const auto started=std::chrono::steady_clock::now();
 assert(!reactivate_owned_virtual_display(state,{}));
 assert(std::chrono::steady_clock::now()-started<500ms);
 release.set_value();owner.join();
 assert(helper_applies==0&&resets==0);
 helper_ok=false;assert(!reactivate_owned_virtual_display(state,{}));assert(resets==0);helper_ok=true;
 // A tracker identity refresh between the initial lease snapshot and helper
 // admission invalidates the captured ID and must not reactivate a stale target.
 tracker_gets=0;mutate_device_id_after_snapshot=true;
 const auto stale_identity_helper_calls=helper_applies;
 assert(!reactivate_owned_virtual_display(state,{}));
 assert(helper_applies==stale_identity_helper_calls&&resets==0);
 mutate_device_id_after_snapshot=false;tracker_gets=0;owned=Lease{};
 // The exact active device supplies a display name, but capture confirmation
 // resolves the current name-to-ID mapping and accepts only the lease ID.
 enumerated_devices={{"virtual1","monitor1","DISPLAY15","",true}};
 ccd_paths={{"virtual1","monitor1",DISPLAYCONFIG_PATH_ACTIVE}};
 assert(reactivate_owned_virtual_display(state,{}));
 assert(resets==1&&state.capture_confirmation.pending());
 assert(state.capture_lease_id==2&&state.capture_display_id==1);
 assert(state.capture_device_id=="virtual1");
 // If the tracked device identity changes after watch installation, the
 // confirmation gate rejects polling the old captured-device observer.
 owned->device_id="replacement-virtual";
 assert(!video::capture_recovery::lease_identity_matches(
  owned->lease_id,owned->display_id,owned->device_id,
  state.capture_lease_id,state.capture_display_id,state.capture_device_id));
 owned=Lease{};
 assert(owned->lease_id==2&&owned->display_id==1);
 assert(state.capture_confirmation.poll(std::chrono::steady_clock::now()+5s)==video::capture_recovery::confirmation_status::retry);
 auto &captures=video::capture_recovery::observations();
 observe_frame("DISPLAY15",captures.begin_capture());
 assert(state.capture_confirmation.poll(std::chrono::steady_clock::now()+6s)==video::capture_recovery::confirmation_status::confirmed);
 // Reassigning the same GDI name to another device before the fresh frame
 // cannot acknowledge the exact leased target.
 enumerated_devices.clear();
 assert(reactivate_owned_virtual_display(state,{}));
 enumerated_devices={{"replacement-virtual","monitor3","DISPLAY15","",true}};
 const auto reassigned_instance=captures.begin_capture();
 observe_frame("DISPLAY15",reassigned_instance);
 assert(state.capture_confirmation.poll(std::chrono::steady_clock::now()+6s)==video::capture_recovery::confirmation_status::retry);
 // A later capture instance whose current name mapping identifies the lease
 // is accepted, even if its GDI name was not trusted before reactivation.
 enumerated_devices={{"virtual1","monitor1","DISPLAY15","",true}};
 observe_frame("DISPLAY15",captures.begin_capture());
 assert(state.capture_confirmation.poll(std::chrono::steady_clock::now()+7s)==video::capture_recovery::confirmation_status::confirmed);

 // A valid but mismatched name-to-ID result is retried on the same capture
 // instance, because Windows may reassign that output name after the first frame.
 enumerated_devices={{"replacement-virtual","monitor3","DISPLAY19","",true}};
 assert(reactivate_owned_virtual_display(state,{}));
 const auto remapped_instance=captures.begin_capture();
 const auto remapped_time=std::chrono::steady_clock::now();
 const auto remap_queries_before=enumeration_queries;
 observe_frame("DISPLAY19",remapped_instance,remapped_time);
 assert(enumeration_queries==remap_queries_before+1);
 assert(state.capture_confirmation.poll(remapped_time+6s)==video::capture_recovery::confirmation_status::retry);
 enumerated_devices={{"virtual1","monitor1","DISPLAY19","",true}};
 observe_frame("DISPLAY19",remapped_instance,remapped_time+499ms);
 assert(enumeration_queries==remap_queries_before+1);
 observe_frame("DISPLAY19",remapped_instance,remapped_time+500ms);
 assert(enumeration_queries==remap_queries_before+2);
 assert(state.capture_confirmation.poll(remapped_time+7s)==video::capture_recovery::confirmation_status::confirmed);

 // If Windows enumeration temporarily fails on the first frame, retry the
 // same capture instance after the throttle instead of leaving it unobserved.
 enumerated_devices.clear();
 assert(reactivate_owned_virtual_display(state,{}));
 const auto transient_instance=captures.begin_capture();
 const auto lookup_time=std::chrono::steady_clock::now();
 enumeration_ok=false;
 const auto queries_before=enumeration_queries;
 observe_frame("DISPLAY15",transient_instance,lookup_time);
 assert(enumeration_queries==queries_before+1);
 enumeration_ok=true;
 enumerated_devices={{"virtual1","monitor1","DISPLAY15","",true}};
 observe_frame("DISPLAY15",transient_instance,lookup_time+499ms);
 assert(enumeration_queries==queries_before+1);
 assert(state.capture_confirmation.poll(lookup_time+6s)==video::capture_recovery::confirmation_status::retry);
 observe_frame("DISPLAY15",transient_instance,lookup_time+500ms);
 assert(enumeration_queries==queries_before+2);
 assert(state.capture_confirmation.poll(lookup_time+7s)==video::capture_recovery::confirmation_status::confirmed);

 // A mismatched friendly-name candidate cannot establish presence or seed the
 // capture lookup. Exact driver state can still prove the leased target active.
 RecoveryMonitorState weak_state;
 weak_state.params.client_name="client";
 weak_state.current_device_id.reset();
 weak_state.current_display_name.reset();
 weak_state.current_monitor_device_path.reset();
 weak_state.normalized_monitor_device_path.reset();
 weak_state.normalized_display_name.reset();
 enumerated_devices={{"replacement-virtual","monitor3","DISPLAY17","client",true}};
 ccd_paths={{"physical","monitor2",DISPLAYCONFIG_PATH_ACTIVE}};
 assert(actual_monitor_target_presence(weak_state)==MonitorTargetPresence::missing);
 assert(!weak_state.current_device_id&&!weak_state.current_display_name);
 ccd_paths={{"virtual1","monitor1",DISPLAYCONFIG_PATH_ACTIVE}};
 assert(actual_monitor_target_presence(weak_state)==MonitorTargetPresence::present_active);
 assert(!weak_state.current_device_id&&!weak_state.current_display_name);
 ccd_paths={{"virtual1","monitor1",DISPLAYCONFIG_PATH_ACTIVE}};
 assert(reactivate_owned_virtual_display(weak_state,{}));
 const auto weak_candidate_instance=captures.begin_capture();
 observe_frame("DISPLAY17",weak_candidate_instance);
 assert(weak_state.capture_confirmation.poll(std::chrono::steady_clock::now()+6s)==video::capture_recovery::confirmation_status::retry);
 enumerated_devices={{"virtual1","monitor1","DISPLAY17","client",true}};
 observe_frame("DISPLAY17",captures.begin_capture());
 assert(weak_state.capture_confirmation.poll(std::chrono::steady_clock::now()+7s)==video::capture_recovery::confirmation_status::confirmed);
}
'''
with tempfile.TemporaryDirectory(prefix='live-recovery-') as d:
    cpp = pathlib.Path(d) / 'test.cpp'
    binary = pathlib.Path(d) / 'test'
    cpp.write_text(program)
    subprocess.run(['g++', '-std=c++23', '-O1', '-pthread', '-Wall', '-Wextra', '-Werror', '-I', str(root), str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
print('Live recovery: exact lease, identity provenance, no-devices lifecycle, cancellation, lock contention and capture acknowledgement passed')
