"""Run production live-recovery helpers against deterministic driver and SetupAPI seams."""
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
product_code_from_id = function('    std::uint16_t virtual_display_product_code_from_display_id(')
product_code_string_from_id = function('    std::string virtual_display_product_code_string_from_display_id(')
serial_number_from_id = function('    std::uint32_t virtual_display_serial_number_from_display_id(')
edid_match = function('    bool matches_virtual_display_id_edid(')
container_lookup = function('    std::optional<GUID> monitor_container_id_from_device_path(')
stable_identity_match = function('    bool matches_virtual_display_id_identity(')

program = r'''
#include "src/capture_recovery.h"
#include "src/platform/windows/virtual_display_policy.h"
#include "src/platform/windows/virtual_display_recovery_policy.h"
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <cstring>
#include <future>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
using namespace std::chrono_literals;
struct GUID {std::uint32_t Data1{};std::uint16_t Data2{},Data3{};std::uint8_t Data4[8]{};};
using DWORD=std::uint32_t;
using BOOL=int;
using DEVPROPTYPE=std::uint32_t;
using PBYTE=std::uint8_t*;
struct DEVPROPKEY {};
constexpr DEVPROPKEY DEVPKEY_Device_ContainerId{};
constexpr DEVPROPTYPE DEVPROP_TYPE_GUID=13;
constexpr DWORD ERROR_INSUFFICIENT_BUFFER=122;
struct DeviceInfoSet {};
using HDEVINFO=DeviceInfoSet*;
HDEVINFO const INVALID_HANDLE_VALUE=nullptr;
struct SP_DEVICE_INTERFACE_DATA {DWORD cbSize{};std::wstring path;};
struct SP_DEVINFO_DATA {DWORD cbSize{};std::wstring path;};
struct SP_DEVICE_INTERFACE_DETAIL_DATA_W {DWORD cbSize{};wchar_t DevicePath[1]{};};
DeviceInfoSet test_device_info_set;
std::unordered_map<std::wstring,GUID> monitor_container_ids;
DWORD setupapi_last_error{};
DWORD GetLastError() {return setupapi_last_error;}
HDEVINFO SetupDiCreateDeviceInfoList(const GUID*,void*) {return &test_device_info_set;}
BOOL SetupDiDestroyDeviceInfoList(HDEVINFO) {return 1;}
BOOL SetupDiOpenDeviceInterfaceW(HDEVINFO,const wchar_t *path,DWORD,SP_DEVICE_INTERFACE_DATA *data) {
 if(!monitor_container_ids.contains(path)) return 0;
 data->path=path;return 1;
}
BOOL SetupDiGetDeviceInterfaceDetailW(HDEVINFO,const SP_DEVICE_INTERFACE_DATA *interface_data,
                                      SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail,DWORD detail_size,
                                      DWORD *required_size,SP_DEVINFO_DATA *device_data) {
 if(!detail) {
  *required_size=static_cast<DWORD>(offsetof(SP_DEVICE_INTERFACE_DETAIL_DATA_W,DevicePath)+
                                    (interface_data->path.size()+1)*sizeof(wchar_t));
  setupapi_last_error=ERROR_INSUFFICIENT_BUFFER;return 0;
 }
 const auto minimum_size=static_cast<DWORD>(offsetof(SP_DEVICE_INTERFACE_DETAIL_DATA_W,DevicePath)+
                                             (interface_data->path.size()+1)*sizeof(wchar_t));
 if(detail_size<minimum_size) return 0;
 std::wmemcpy(detail->DevicePath,interface_data->path.c_str(),interface_data->path.size()+1);
 if(device_data) device_data->path=interface_data->path;
 return 1;
}
BOOL SetupDiGetDevicePropertyW(HDEVINFO,const SP_DEVINFO_DATA *device_data,const DEVPROPKEY *,
                               DEVPROPTYPE *property_type,PBYTE property_buffer,DWORD property_buffer_size,
                               DWORD *required_size,DWORD) {
 const auto found=monitor_container_ids.find(device_data->path);
 if(found==monitor_container_ids.end()||property_buffer_size<sizeof(GUID)) return 0;
 std::memcpy(property_buffer,&found->second,sizeof(GUID));
 *property_type=DEVPROP_TYPE_GUID;*required_size=sizeof(GUID);return 1;
}
BOOL IsEqualGUID(const GUID &left,const GUID &right) {return std::memcmp(&left,&right,sizeof(GUID))==0;}
#ifndef _WIN32
int _wcsicmp(const wchar_t *left,const wchar_t *right) {
 while(*left&&*right) {
  const auto a=std::towlower(*left++),b=std::towlower(*right++);
  if(a!=b) return a<b?-1:1;
 }
 return *left?1:(*right?-1:0);
}
#endif
namespace sunshine_driver {
struct DriverGuid {std::uint32_t data1;std::uint16_t data2,data3;std::uint8_t data4[8];};
DriverGuid container_guid_from_display_id(std::uint64_t id) {
 return {0x9161d0d5u,static_cast<std::uint16_t>((id>>48)&0xffffu),static_cast<std::uint16_t>((id>>32)&0xffffu),
         {0x93,0x7d,static_cast<std::uint8_t>(id>>24),static_cast<std::uint8_t>(id>>16),
          static_cast<std::uint8_t>(id>>8),static_cast<std::uint8_t>(id),0x53,0x44}};
}
GUID to_windows_guid(const DriverGuid &guid) {
 return {guid.data1,guid.data2,guid.data3,{guid.data4[0],guid.data4[1],guid.data4[2],guid.data4[3],
                                           guid.data4[4],guid.data4[5],guid.data4[6],guid.data4[7]}};
}
}
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
 struct Edid {std::string m_manufacturer_id,m_product_code;std::uint32_t m_serial_number{};};
 std::optional<Edid> m_edid;
 EnumeratedDevice(std::string device_id={},std::string monitor_path={},std::string display_name={},
                  std::string friendly_name={},bool is_virtual=true,std::optional<Edid> edid={})
  :m_device_id(device_id),m_monitor_device_path(monitor_path),m_display_name(display_name),
   m_friendly_name(friendly_name),virtual_display(is_virtual),m_edid(edid) {}
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
''' + product_code_from_id + '\n' + product_code_string_from_id + '\n' + serial_number_from_id + '\n' + edid_match + '\n' + container_lookup + '\n' + stable_identity_match + r'''
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
 // Distinct 64-bit IDs can collide after the driver's EDID projection. The
 // production EDID matcher recognizes both, so exact readiness must also
 // match the driver-assigned PnP ContainerID for the full display ID.
 constexpr std::uint64_t colliding_display_id=0x1234000000000001ull;
 constexpr std::uint64_t colliding_peer_id=colliding_display_id+0x0000100000001000ull;
 assert(colliding_display_id!=colliding_peer_id);
 assert(virtual_display_product_code_from_display_id(colliding_display_id)==
        virtual_display_product_code_from_display_id(colliding_peer_id));
 assert(virtual_display_serial_number_from_display_id(colliding_display_id)==
        virtual_display_serial_number_from_display_id(colliding_peer_id));
 const display_device::EnumeratedDevice::Edid colliding_edid{
  "SDD",virtual_display_product_code_string_from_display_id(colliding_display_id),
  virtual_display_serial_number_from_display_id(colliding_display_id)};
 const auto target_container_id=sunshine_driver::to_windows_guid(
  sunshine_driver::container_guid_from_display_id(colliding_display_id));
 const auto peer_container_id=sunshine_driver::to_windows_guid(
  sunshine_driver::container_guid_from_display_id(colliding_peer_id));
 assert(!IsEqualGUID(target_container_id,peer_container_id));
 monitor_container_ids.clear();
 monitor_container_ids[L"path-target"]=target_container_id;
 monitor_container_ids[L"path-peer"]=peer_container_id;
 display_device::EnumeratedDevice colliding_target{"target-id","path-target","","",true,colliding_edid};
 display_device::EnumeratedDevice colliding_peer{"peer-id","path-peer","","",true,colliding_edid};
 assert(matches_virtual_display_id_edid(colliding_target,colliding_display_id));
 assert(matches_virtual_display_id_edid(colliding_peer,colliding_display_id));
 assert(matches_virtual_display_id_identity(colliding_target,colliding_display_id));
 assert(!matches_virtual_display_id_identity(colliding_peer,colliding_display_id));
 // A singleton wrong peer is the key negative control: EDID-only uniqueness
 // would accept it, but its full container identity must reject it.
 const std::vector<display_device::EnumeratedDevice> singleton_peer{colliding_peer};
 const auto is_virtual=[](const auto &candidate){return is_virtual_display_device(candidate);};
 const auto matches_colliding_id=[&](const auto &candidate){return matches_virtual_display_id_identity(candidate,colliding_display_id);};
 const auto singleton_peer_count=VDISPLAY::policy::count_stable_identity_matches(singleton_peer,is_virtual,matches_colliding_id);
 assert(singleton_peer_count==0);
 assert(!VDISPLAY::policy::stable_identity_candidate_is_unique(matches_virtual_display_id_identity(colliding_peer,colliding_display_id),singleton_peer_count));
 assert(!VDISPLAY::policy::readiness_candidate_is_exact_target(true,false,
  VDISPLAY::policy::stable_identity_candidate_is_unique(matches_virtual_display_id_identity(colliding_peer,colliding_display_id),singleton_peer_count)));
 const std::vector<display_device::EnumeratedDevice> unique_devices{colliding_target};
 const auto unique_match_count=VDISPLAY::policy::count_stable_identity_matches(unique_devices,is_virtual,matches_colliding_id);
 assert(unique_match_count==1);
 assert(VDISPLAY::policy::stable_identity_candidate_is_unique(matches_virtual_display_id_identity(colliding_target,colliding_display_id),unique_match_count));
 assert(VDISPLAY::policy::readiness_candidate_is_exact_target(true,false,
  VDISPLAY::policy::stable_identity_candidate_is_unique(matches_virtual_display_id_identity(colliding_target,colliding_display_id),unique_match_count)));
 // Duplicate exact paths/records still fail closed even when each record has
 // the requested full identity.
 const std::vector<display_device::EnumeratedDevice> ambiguous_devices{colliding_target,colliding_target};
 const auto ambiguous_match_count=VDISPLAY::policy::count_stable_identity_matches(ambiguous_devices,is_virtual,matches_colliding_id);
 assert(ambiguous_match_count==2);
 assert(!VDISPLAY::policy::stable_identity_candidate_is_unique(matches_virtual_display_id_identity(colliding_target,colliding_display_id),ambiguous_match_count));
 assert(!VDISPLAY::policy::readiness_candidate_is_exact_target(true,false,
  VDISPLAY::policy::stable_identity_candidate_is_unique(matches_virtual_display_id_identity(colliding_target,colliding_display_id),ambiguous_match_count)));
 display_device::EnumeratedDevice unavailable_container{"unavailable-id","missing-path","","",true,colliding_edid};
 assert(!matches_virtual_display_id_identity(unavailable_container,colliding_display_id));
 auto wrong_edid=colliding_edid;wrong_edid.m_manufacturer_id="XYZ";
 display_device::EnumeratedDevice wrong_candidate{"wrong-id","path-target","","",true,wrong_edid};
 const std::vector<display_device::EnumeratedDevice> wrong_devices{wrong_candidate};
 const auto wrong_match_count=VDISPLAY::policy::count_stable_identity_matches(wrong_devices,is_virtual,matches_colliding_id);
 assert(wrong_match_count==0);
 assert(!matches_virtual_display_id_edid(wrong_candidate,colliding_display_id));
 assert(!VDISPLAY::policy::stable_identity_candidate_is_unique(matches_virtual_display_id_identity(wrong_candidate,colliding_display_id),wrong_match_count));
 assert(VDISPLAY::policy::defer_unidentified_readiness_candidate(false,false));
 assert(!VDISPLAY::policy::defer_unidentified_readiness_candidate(false,true));
 assert(!VDISPLAY::policy::defer_unidentified_readiness_candidate(true,false));
 enumerated_devices={{"peer-id","peer-path","\\\\.\\DISPLAY17","client",true}};
 assert(!actual_is_virtual_display_present(std::nullopt,std::nullopt));
 assert(!actual_is_virtual_display_present(std::optional<std::wstring>{L"\\\\.\\DISPLAY17"},std::optional<std::string>{"target-id"}));
 assert(actual_is_virtual_display_present(std::nullopt,std::optional<std::string>{"peer-id"}));
 assert(actual_is_virtual_display_present(std::optional<std::wstring>{L"client"},std::nullopt));
 // A peer's pre-readiness GDI alias must be discarded after stable EDID
 // selects the target. Rebuild it only from that selected target's device ID.
 auto rebound_reuse_name=VDISPLAY::policy::bind_reuse_display_name_to_device_id(
  true,std::optional<std::string>{"target-id"},std::optional<std::wstring>{L"\\\\.\\DISPLAY17"},
  [](const std::string &id){assert(id=="target-id");return std::optional<std::wstring>{L"\\\\.\\DISPLAY18"};},
  [](const std::wstring &name){return name.rfind(L"\\\\.\\DISPLAY",0)==0;});
 assert(rebound_reuse_name&&*rebound_reuse_name==L"\\\\.\\DISPLAY18");
 auto unresolved_reuse_name=VDISPLAY::policy::bind_reuse_display_name_to_device_id(
  true,std::nullopt,std::optional<std::wstring>{L"\\\\.\\DISPLAY17"},
  [](const std::string &){assert(false);return std::optional<std::wstring>{};},
  [](const std::wstring &){return true;});
 assert(!unresolved_reuse_name);
 auto legacy_reuse_name=VDISPLAY::policy::bind_reuse_display_name_to_device_id(
  false,std::nullopt,std::optional<std::wstring>{L"\\\\.\\DISPLAY17"},
  [](const std::string &){assert(false);return std::optional<std::wstring>{};},
  [](const std::wstring &){return true;});
 assert(legacy_reuse_name&&*legacy_reuse_name==L"\\\\.\\DISPLAY17");
 enumerated_devices={{"peer-id","peer-path","\\\\.\\DISPLAY17","client",true}};
 assert(!actual_is_virtual_display_present(rebound_reuse_name,std::optional<std::string>{"target-id"}));
 assert(!actual_is_virtual_display_present(unresolved_reuse_name,std::nullopt));
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
 auto reclaimed_id=VDISPLAY::policy::select_reuse_device_id(true,true,std::string("virtual1"),std::string("replacement-virtual"));
 assert(reclaimed_id&&*reclaimed_id=="virtual1");
 assert(!VDISPLAY::policy::select_reuse_device_id(true,true,std::nullopt,std::string("replacement-virtual")));
 assert(!VDISPLAY::policy::select_reuse_device_id(false,true,std::nullopt,std::string("replacement-virtual")));
 auto unowned_candidate=VDISPLAY::policy::select_reuse_device_id(false,false,std::nullopt,std::string("replacement-virtual"));
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
print('Live recovery: exact lease, EDID-collision container identity, no-devices lifecycle, cancellation, lock contention and capture acknowledgement passed')
