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
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
namespace remote_session { enum class role_e {game,monitor,input}; }
struct Launch {remote_session::role_e role=remote_session::role_e::monitor; bool monitor_stream_published=false;};
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
void run(int code,bool late) {
 result=code;throws=late;partial_registration=false;unlocked_on_destroy=false;
 Server value;auto *server=&value;
 auto launch_session=std::make_shared<Launch>();auto session=launch_session;
 const std::string remote_address="fake",client_uuid="client";
 struct {int monitor=0;} config;
 {
 std::unique_lock lifecycle_lock{lifecycle};std::unique_lock _hot_apply_gate{apply};
 @GATE@
 assert(!_hot_apply_gate.owns_lock()&&!lifecycle_lock.owns_lock());
 assert(startup_failed==(late||code!=0));
 assert(startup_status==(code==stream::session::START_PACING_REJECTED&&!late?400:500));
 assert(startup_error.empty()==(!late&&code!=stream::session::START_PACING_REJECTED));
 }
 assert(unlocked_on_destroy);
 assert(value.retired==(!late&&(code==-1||code==stream::session::START_PACING_REJECTED)));
 assert(value.inserted==(!late&&code==0));
 assert(launch_session->monitor_stream_published==(!late&&code==0));
 assert(partial_registration==late);
}
int main(){run(-1,false);run(stream::session::START_PACING_REJECTED,false);run(0,false);run(0,true);}
'''.replace('@PACING@',pacing).replace('@GATE@',gate)
with tempfile.TemporaryDirectory(prefix='rtsp-startup-gate-') as directory:
    cpp=Path(directory)/'gate.cpp'; cpp.write_text(program); binary=cpp.with_suffix('')
    sanitizer_flags = ['-fsanitize=address,undefined'] if os.name != 'nt' else []
    subprocess.run([args.compiler,'-std=c++23','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',*sanitizer_flags,str(cpp),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('PASS: production early startup errors, late exception, publication and destructor lock release')
