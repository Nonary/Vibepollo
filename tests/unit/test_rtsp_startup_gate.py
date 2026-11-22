"""Replay the production RTSP startup result and lock-release boundary."""
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
start = stream.index('int start(session_t &session, const std::string &addr_string)')
opening = stream.index('{', start)
depth = 1
end = opening + 1
while depth:
    depth += (stream[end] == '{') - (stream[end] == '}')
    end += 1
body = stream[start:end]
assert re.findall(r'\breturn\s+([^;]+)\s*;', body) == ['-1', 'START_PACING_REJECTED', '0']
for result in ('-1', 'START_PACING_REJECTED'):
    assert body.index(f'return {result};') < body.index('_sessions->push_back(&session)')
    assert body.index(f'return {result};') < body.index('session.videoThread =')
pacing = re.search(r'constexpr int START_PACING_REJECTED\s*=\s*-?\d+;', (root/'src/stream.h').read_text()).group()
program = r'''
#include <cassert>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
namespace remote_session { enum class role_e {game,monitor,input}; }
struct Launch {
 remote_session::role_e role=remote_session::role_e::monitor;
 std::string client_uuid="client";
 std::uint64_t role_generation=42;
 bool monitor_stream_published=false, new_monitor_display=false;
};
constexpr int ML_FF_SESSION_ID_V1=2;
bool compatible=true;
int released=0, started=0;
namespace nvhttp {
 bool can_start_remote_monitor(const std::string &client,std::uint64_t generation,bool ids) {
   assert(client=="client" && generation==42 && ids);
   return compatible;
 }
 void end_remote_monitor_transport(const std::string &client,std::uint64_t generation,const char *) {
   assert(client=="client" && generation==42);
   ++released;
 }
 void notify_remote_monitor_started(const std::string &client,std::uint64_t generation,bool ids) {
   assert(client=="client" && generation==42 && ids);
   ++started;
 }
}
std::mutex lifecycle, apply;
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
 template<class C> auto alloc(const C&,const Launch&) {return std::make_shared<session_t>();}
 int start(session_t&,const std::string&) {
   if(throws) {partial_registration=true;throw std::runtime_error("late registration failure");}
   return result;
 }
 }
}
struct Server {
 int retired=0, inserted=0;
 void insert(const auto&,const auto&,bool) {++inserted;}
 void retire_failed_monitor_start_locked(const auto&) {++retired;}
};
bool activates_vulkan_hdr_layer_for_stream(int) {return false;}
void run(int code,bool late,bool can_start=true,bool new_display=false) {
 compatible=can_start;released=started=0;
 result=code;throws=late;partial_registration=false;unlocked_on_destroy=false;
 Server value;auto *server=&value;
 auto launch_session=std::make_shared<Launch>();auto session=launch_session;
 launch_session->new_monitor_display=new_display;
 const std::string remote_address="fake",client_uuid="client";
 struct {int monitor=0,mlFeatureFlags=ML_FF_SESSION_ID_V1;} config;
 {
 std::unique_lock lifecycle_lock{lifecycle};std::unique_lock _hot_apply_gate{apply};
 @GATE@
 assert(!_hot_apply_gate.owns_lock()&&!lifecycle_lock.owns_lock());
 assert(startup_failed==(!compatible||late||code!=0));
 assert(startup_status==(code==stream::session::START_PACING_REJECTED&&!late?400:500));
 assert(startup_error.empty()==(compatible&&!late&&code!=stream::session::START_PACING_REJECTED));
 }
 assert(unlocked_on_destroy==compatible);
 assert(value.retired==(!compatible||(!late&&(code==-1||code==stream::session::START_PACING_REJECTED))));
 const bool published=compatible&&!late&&code==0;
 assert(value.inserted==published && started==published);
 assert(launch_session->monitor_stream_published==published);
 assert(released==(!compatible&&new_display));
 assert(partial_registration==late);
}
int main(){
 run(-1,false);run(stream::session::START_PACING_REJECTED,false);run(0,false);run(0,true);
 run(0,false,false,false);run(0,false,false,true);
}
'''.replace('@PACING@',pacing).replace('@GATE@',gate)
with tempfile.TemporaryDirectory(prefix='rtsp-startup-gate-') as directory:
    cpp=Path(directory)/'gate.cpp'; cpp.write_text(program); binary=cpp.with_suffix('')
    sanitizer_flags = ['-fsanitize=address,undefined'] if os.name != 'nt' else []
    subprocess.run([args.compiler,'-std=c++23','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',*sanitizer_flags,str(cpp),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('PASS: production early startup errors, late exception, publication, destructor lock release and incompatible monitor admission')
