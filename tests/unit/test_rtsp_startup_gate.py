"""Replay the production RTSP startup result, pending retirement and lock-release boundary."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--compiler', default='g++')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
rtsp = (root / 'src/rtsp.cpp').read_text()
start = rtsp.index('        std::shared_ptr<stream::session_t> stream_session;')
gate = rtsp[start:rtsp.index('        server->post([server, socket', start)]
stream = (root / 'src/stream.cpp').read_text()
def function(source, name):
    start = source.index(name)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


body = function(stream, 'int start(session_t &session, const std::string &addr_string)')
retirement = '\n'.join(function(rtsp, signature) for signature in (
    'bool session_raise(',
    'void release_pending_runtime_holds(',
    'bool has_pending_monitor_launch(',
    'bool retain_pending_runtime_holds(',
    'void reconcile_abandoned_monitor_locked(',
    'void retire_failed_monitor_start_locked(',
    'void finish_startup(',
))
assert re.search(r'server->run_startup\(\s*launch_session->virtual_display_guid_bytes,', rtsp)
assert 'finish_startup(virtual_display_guid_bytes);' in rtsp
assert re.findall(r'\breturn\s+([^;]+)\s*;', body) == ['-1', 'START_PACING_REJECTED', '0']
for result in ('-1', 'START_PACING_REJECTED'):
    assert body.index(f'return {result};') < body.index('_sessions->push_back(&session)')
    assert body.index(f'return {result};') < body.index('session.videoThread =')
pacing = re.search(r'constexpr int START_PACING_REJECTED\s*=\s*-?\d+;', (root/'src/stream.h').read_text()).group()
program = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include "src/rtsp_launch_registry.h"
using namespace std::string_view_literals;
namespace pending_policy = rtsp_stream::pending_policy;
namespace host_stream_control {struct activity_guard_t {explicit operator bool() const {return true;}};}
namespace config {
 struct {std::chrono::seconds ping_timeout{30};} stream;
}
namespace asio {void post(int, const auto &callback) {callback();}}
struct TestLog {
 TestLog &operator<<(const auto &) {return *this;}
} log_sink;
#define BOOST_LOG(level) log_sink
struct Launch {
 remote_session::role_e role=remote_session::role_e::monitor;
 unsigned id=1;
 std::string client_uuid="client", unique_id="pending", rtsp_source_address="same-address";
 bool rtsp_cipher=false;
 std::uint64_t role_generation=42;
 bool monitor_stream_published=false, new_monitor_display=false;
 std::shared_ptr<void> audio_context_guard, display_power_guard, exact_target_hold;
 std::array<std::uint8_t,16> virtual_display_guid_bytes{};
};
constexpr int ML_FF_SESSION_ID_V1=2;
bool compatible=true;
int released=0, started=0, reconciled=0;
std::mutex lifecycle, apply;
namespace audio {
 std::mutex launch_audio_guard_mutex;
 void copy_launch_audio_guard(const Launch &source, Launch &destination) {
   destination.audio_context_guard=source.audio_context_guard;
 }
}
namespace nvhttp {
 std::mutex &stream_lifecycle_mutex() {return lifecycle;}
 bool can_start_remote_monitor(const std::string &client,std::uint64_t generation,bool ids) {
   assert(client=="client" && generation==42 && ids);
   return compatible;
 }
 void end_remote_monitor_transport(const std::string &client,std::uint64_t generation,const char *reason=nullptr) {
   assert(client=="client" && generation==42);
   if (reason) ++released;
   else ++reconciled;
 }
 void notify_remote_monitor_started(const std::string &client,std::uint64_t generation,bool ids) {
   assert(client=="client" && generation==42 && ids);
   ++started;
 }
}
bool unlocked_on_destroy=false, partial_registration=false;
int result=0;
bool throws=false;
namespace stream {
 struct session_t {~session_t() {
   const bool a=apply.try_lock(), b=lifecycle.try_lock();
   unlocked_on_destroy=a&&b;
   if(a)apply.unlock(); if(b)lifecycle.unlock();
 }};
 namespace session {
 @PACING@
 struct cleanup_reservation_t {~cleanup_reservation_t() {}};
 struct shared_runtime_finalize_context_t {
   std::optional<std::array<std::uint8_t,16>> virtual_display_guid_bytes;
 };
 std::optional<std::array<std::uint8_t,16>> finalized_guid;
 bool finalize_shared_runtime_if_idle(const char *, const shared_runtime_finalize_context_t &context) {
   finalized_guid=context.virtual_display_guid_bytes;
   return true;
 }
 template<class C> auto alloc(const C&,const Launch&) {return std::make_shared<session_t>();}
 int start(session_t&,const std::string&) {
   if(throws) {partial_registration=true;throw std::runtime_error("late registration failure");}
   return result;
 }
 }
}
struct Server {
 using launch_session_t=Launch;
 using pending_launches_t=rtsp_stream::launch_registry_t<Launch>;
 pending_launches_t _launch_sessions;
 std::mutex _launch_sessions_mutex;
 std::string plaintext_route_warning;
 bool pending_hdr=true;
 int inserted=0, io_context=0, finished=0;
 bool finished_ok=false, finished_before_insert=false;
 void insert(const auto&,const auto&,bool) {++inserted;}
 void arm_launch_timer() {}
 void set_pending_vulkan_hdr_layer_stream(bool active) {pending_hdr=active;}
 // These cases never expire a session. Expiry is covered by the reconnect fixture.
 std::vector<std::array<std::uint8_t,16>> expire_launch_sessions_locked(auto, auto *, auto *) {return {};}
 void finalize_expired_launch_sessions(const auto&, bool, const auto&, const auto&) {}
 // Nothing departs in these cases. The terminal composition test covers that path.
 void retire_departed_launch_sessions_locked(auto &, auto &) {}
 // The launch's startup claim ends with the start result, before publication.
 void finish_launch_startup(std::uint32_t id,const std::string &unique_id,bool ok) {
   assert(id==1&&unique_id=="pending");
   ++finished;finished_ok=ok;finished_before_insert=inserted==0;
 }
 @RETIREMENT@
};
bool activates_vulkan_hdr_layer_for_stream(int) {return false;}
void run(int code,bool late,bool can_start=true,bool new_display=false) {
 compatible=can_start;released=started=reconciled=0;
 result=code;throws=late;partial_registration=false;unlocked_on_destroy=false;
 Server value;auto *server=&value;
 auto launch_session=std::make_shared<Launch>();auto session=launch_session;
 launch_session->new_monitor_display=new_display;
 const auto hold=std::make_shared<int>(1);
 launch_session->audio_context_guard=hold;
 launch_session->display_power_guard=hold;
 launch_session->exact_target_hold=hold;
 launch_session->virtual_display_guid_bytes[0]=42;
 assert(value.session_raise(launch_session));
 const std::string remote_address="fake",client_uuid="client";
 struct {int monitor=0,mlFeatureFlags=ML_FF_SESSION_ID_V1;} config;
 // Stands in for the scope guard that finishes the claim if startup unwinds early.
 struct {bool armed=true; void disable() {armed=false;}} startup_claim_guard;
 {
 std::unique_lock lifecycle_lock{lifecycle};std::unique_lock _hot_apply_gate{apply};
 @GATE@
 assert(!_hot_apply_gate.owns_lock()&&!lifecycle_lock.owns_lock());
 assert(startup_failed==(!compatible||late||code!=0));
 assert(startup_status==(code==stream::session::START_PACING_REJECTED&&!late?400:500));
 assert(startup_error.empty()==(compatible&&!late&&code!=stream::session::START_PACING_REJECTED));
 }
 assert(unlocked_on_destroy==compatible);
 const bool retired=!compatible||(!late&&(code==-1||code==stream::session::START_PACING_REJECTED));
 const bool published=compatible&&!late&&code==0;
 assert(value._launch_sessions.empty()==retired && value.pending_hdr==!retired);
 assert(reconciled==retired);
 assert(hold.use_count()==(retired?1:4));
 assert(value.has_pending_monitor_launch("client",42)==(!retired&&!published));
 auto old_snapshot=std::make_shared<Launch>();
 assert(value.retain_pending_runtime_holds(old_snapshot)==(!retired&&!published));
 auto retry=std::make_shared<Launch>();
 retry->id=2;
 retry->unique_id="retry";
 retry->role_generation=43;
 assert(retry->rtsp_source_address==launch_session->rtsp_source_address);
 if (retired || late) {
   assert(value.session_raise(retry)==retired);
   assert(value.has_pending_monitor_launch("client",43)==retired);
 }
 stream::session::finalized_guid.reset();
 value.finish_startup(launch_session->virtual_display_guid_bytes);
 assert(stream::session::finalized_guid==launch_session->virtual_display_guid_bytes);
 assert(value.inserted==published && started==published);
 assert(launch_session->monitor_stream_published==published);
 assert(released==(!compatible&&new_display));
 assert(partial_registration==late);
 assert(!startup_claim_guard.armed);
 assert(value.finished==1&&value.finished_ok==(compatible&&!late&&code==0)&&value.finished_before_insert);
}
int main(){
 run(-1,false);run(stream::session::START_PACING_REJECTED,false);run(0,false);run(0,true);
 run(0,false,false,false);run(0,false,false,true);
}
'''.replace('@PACING@',pacing).replace('@GATE@',gate).replace('@RETIREMENT@',retirement)
with tempfile.TemporaryDirectory(prefix='rtsp-startup-gate-') as directory:
    cpp=Path(directory)/'gate.cpp'; cpp.write_text(program); binary=cpp.with_suffix('')
    sanitizer_flags = ['-fsanitize=address,undefined'] if os.name != 'nt' else []
    subprocess.run([args.compiler,'-std=c++23','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',*sanitizer_flags,f'-I{root}',str(cpp),str(root/'src/rtsp_pending_policy.cpp'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('PASS: startup rejection retires pending monitor holds, retains its display GUID, allows a new generation, and releases the startup claim')
