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

empty_enumeration = function('    MonitorTargetPresence monitor_target_presence(')
empty_enumeration = empty_enumeration[:empty_enumeration.index('      const bool has_stable_identity')]
empty_enumeration = empty_enumeration.replace('monitor_target_presence(', 'empty_enumeration_presence(')
empty_enumeration += 'return MonitorTargetPresence::unknown; }'
ccd_presence = function('    MonitorTargetPresence ccd_target_presence(')
driver_presence = function('    MonitorTargetPresence driver_target_presence(')

program = r'''
#include "src/capture_recovery.h"
#include <cassert>
#include <chrono>
#include <future>
#include <optional>
#include <stop_token>
#include <thread>
using namespace std::chrono_literals;
struct Lease { std::uint64_t display_id=1,lease_id=2; std::optional<std::string> device_id="virtual1"; };
std::optional<Lease> owned=Lease{};
struct Tracker { std::optional<Lease> get(int) {return owned;} };
Tracker &driver_lease_tracker() {static Tracker tracker;return tracker;}
struct Transport { bool valid() const {return true;} };
std::shared_ptr<Transport> control_transport_snapshot() {return std::make_shared<Transport>();}
bool responsive=true,provenance=true,query_ok=true,helper_ok=true,active=true;
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
 int guid_uuid=1;
 std::optional<std::wstring> current_display_name=L"DISPLAY15",current_monitor_device_path=L"monitor1";
 std::optional<std::string> current_device_id="virtual1",normalized_monitor_device_path="monitor1";
 video::capture_recovery::confirmation_t capture_confirmation;
 std::uint64_t capture_lease_id=0,capture_display_id=0;
};
bool monitor_should_abort(const RecoveryMonitorState&,std::stop_token stop) {return stop.stop_requested();}
enum class MonitorTargetPresence {unknown,missing,present_inactive,present_active};
std::optional<bool> ccd_result=true;
std::optional<bool> ccd_has_active_paths() {return ccd_result;}
namespace display_device {enum class DeviceEnumerationDetail {Minimal};}
namespace display_device {
enum class QueryType {All};
struct WinApiLayer {
 std::optional<CcdConfig> queryDisplayConfig(QueryType) const {return ccd_query_ok?std::optional<CcdConfig>{CcdConfig{ccd_paths}}:std::nullopt;}
 std::string getDeviceId(const CcdPath &path) const {return path.device_id;}
 std::string getMonitorDevicePath(const CcdPath &path) const {return path.monitor_path;}
};
}
namespace platf::display_helper {
struct Coordinator {
 static Coordinator &instance() {static Coordinator c;return c;}
 std::optional<std::vector<int>> enumerate_devices(display_device::DeviceEnumerationDetail) {return std::vector<int>{};}
};
}
struct Log {template<class T> Log &operator<<(T) {return *this;}};
#define BOOST_LOG(level) Log{}
MonitorTargetPresence monitor_target_presence(RecoveryMonitorState&) {return active?MonitorTargetPresence::present_active:MonitorTargetPresence::present_inactive;}
namespace display_helper_integration {
bool reactivate_owned_virtual_display(const std::string &id,const std::function<bool()> &validate,std::function<bool()> cancelled) {
 assert(id=="virtual1");
 if(cancelled()||!validate()) return false;
 ++helper_applies;return helper_ok;
}
}
''' + ccd_presence + '\n' + driver_presence + '\n' + function('    bool reactivate_owned_virtual_display(') + '\n' + empty_enumeration + r'''
int main() {
 RecoveryMonitorState state;
 ccd_paths={{"virtual1","monitor1",0},{"physical","monitor2",DISPLAYCONFIG_PATH_ACTIVE}};
 // Preserve indeterminate enumeration, exact owned inactive detection, and
 // the existing missing-target classification when the driver has no lease.
 ccd_result.reset();
 assert(empty_enumeration_presence(state)==MonitorTargetPresence::unknown);
 ccd_result=true;
 assert(empty_enumeration_presence(state)==MonitorTargetPresence::present_inactive);
 ccd_result=false;
 assert(empty_enumeration_presence(state)==MonitorTargetPresence::present_inactive);
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
 ccd_result=false;
 owned.reset();
 assert(empty_enumeration_presence(state)==MonitorTargetPresence::missing);
 ccd_result=true;
 assert(empty_enumeration_presence(state)==MonitorTargetPresence::unknown);
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
 assert(reactivate_owned_virtual_display(state,{}));
 assert(resets==1&&state.capture_confirmation.pending());
 assert(state.capture_lease_id==2&&state.capture_display_id==1);
 assert(owned->lease_id==2&&owned->display_id==1);
 assert(state.capture_confirmation.poll(std::chrono::steady_clock::now()+5s)==video::capture_recovery::confirmation_status::retry);
 auto &captures=video::capture_recovery::observations();
 captures.frame("DISPLAY15",captures.begin_capture());
 assert(state.capture_confirmation.poll(std::chrono::steady_clock::now()+6s)==video::capture_recovery::confirmation_status::confirmed);
}
'''
with tempfile.TemporaryDirectory(prefix='live-recovery-') as d:
    cpp = pathlib.Path(d) / 'test.cpp'
    binary = pathlib.Path(d) / 'test'
    cpp.write_text(program)
    subprocess.run(['g++', '-std=c++23', '-O1', '-pthread', '-Wall', '-Wextra', '-Werror', '-I', str(root), str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
print('Live recovery: exact lease, adapter provenance, cancellation, lock contention and capture acknowledgement passed')
