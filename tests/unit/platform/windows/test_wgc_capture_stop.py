#!/usr/bin/env python3
"""Compile production WGC waits/stop forwarding and the video join watchdog.

Clock and Windows IPC/process operations are boundaries, accelerated 100x.
Production budgets remain 15s (handle wait) and 10s (watchdog). All cancellation,
ownership, connection/config checkpoints and handle-wait decisions are extracted
unchanged. Usage: script repository [compiler] [original-source directory].
"""
from pathlib import Path
import re, subprocess, sys, tempfile
root=Path(sys.argv[1]).resolve();compiler=sys.argv[2] if len(sys.argv)>2 else 'c++'
before=Path(sys.argv[3]) if len(sys.argv)>3 else None
def read(path):
    candidate=before/Path(path).name if before else root/path
    return (candidate if candidate.exists() else root/path).read_text()
ipc=read('src/platform/windows/ipc/ipc_session.cpp');h=read('src/platform/windows/ipc/ipc_session.h')
start=ipc.index('  void ipc_session_t::initialize_if_needed() {')
pre=ipc[start+len('  void ipc_session_t::initialize_if_needed() {'):ipc.index('    // Check if properly initialized',start)]
connect_start=ipc.index('    control_pipe->wait_for_client_connection(5000);')
stop_block='    if (stop_requested()) {\n      return;\n    }\n'
if ipc[:connect_start].endswith(stop_block+'\n'):connect_start-=len(stop_block)+1
connect=ipc[connect_start:ipc.index('    // Send config data to helper process')]
start=ipc.index('    if (!control_pipe->send(config_span, 5000)) {')
if ipc[:start].endswith(stop_block):start-=len(stop_block)
send=ipc[start:ipc.index('    const auto handle_wait_start',start)]
start=ipc.index('    const auto handle_wait_start')
wait=ipc[start:ipc.index('    auto cleanup_on_failure',start)].replace('std::chrono::steady_clock','TestClock')
methods=[]
for name in ('request_stop','stop_requested'):
    m=re.search(r'^    (?:void|bool) '+name+r'\([^\n]*\) (?:const )?noexcept \{[\s\S]*?^    }',h,re.M)
    methods.append(m.group() if m else ('void request_stop() noexcept {}' if name=='request_stop' else 'bool stop_requested() const noexcept {return false;}'))
video=read('src/video.cpp');start=video.index('  void end_capture_async(capture_thread_async_ctx_t &capture_thread_ctx) {')
end=video[start:video.index('\n  int start_capture_sync',start)]
display=read('src/platform/windows/display_wgc.cpp');forward=[];snapshots=[]
for cls in ('display_wgc_ipc_vram_t','display_wgc_ipc_ram_t'):
    m=re.search(r'^  void '+cls+r'::request_capture_stop\(\) noexcept \{[\s\S]*?^  }',display,re.M)
    forward.append(m.group() if m else 'void '+cls+'::request_capture_stop() noexcept {}')
    start=display.index('  capture_e '+cls+'::snapshot(')
    body=display.index('{',start)
    boundary='    timeout = effective_wgc_timeout' if 'vram' in cls else '    winrt::com_ptr<ID3D11Texture2D> gpu_tex'
    snapshots.append('capture_e '+cls+'::before_frame() '+display[body:display.index(boundary,body)]+'    return capture_e::ok;\n  }')
stream=read('src/stream.cpp');start=stream.index('    class join_deadline_t {')
watch=stream[start:stream.index('\n  }  // namespace',start)].replace('std::chrono::seconds(10)','std::chrono::milliseconds(100)')
start=stream.index('        auto hung_stage = std::make_shared<std::atomic<const char *>>("video thread");',stream.index('    void join(session_t &session'))
join='      {\n'+stream[start:stream.index('      // Watchdog coverage ends',start)]
code=r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <sstream>
#include <thread>
#include "src/thread_safe.h"
#include "src/sync.h"
using namespace std::literals;
#define BOOST_LOG(level) std::ostringstream{}
std::atomic_int traps{0};namespace lifetime {void debug_trap(){++traps;}}
using HANDLE=void*;using DWORD=unsigned;constexpr DWORD WAIT_OBJECT_0=0,WAIT_TIMEOUT=258;
struct Helper {bool live=true;HANDLE get_process_handle(){return this;}void terminate(){}};
DWORD WaitForSingleObject(HANDLE h,int){return static_cast<Helper*>(h)->live?WAIT_TIMEOUT:WAIT_OBJECT_0;}
bool GetExitCodeProcess(HANDLE,DWORD* code){*code=0;return true;}
struct TestClock {
 using time_point=std::chrono::steady_clock::time_point;
 static inline auto origin=std::chrono::steady_clock::now();
 static time_point now(){return time_point((std::chrono::steady_clock::now()-origin)*100);}
};
// BUDGETS
struct shared_handle_data_t {std::uint64_t handles[3];};
enum class PipeResult {Success,Timeout,BrokenPipe,Error};
struct Pipe {
 int phase=2;std::promise<void> entered;std::shared_future<void> resume;bool first=true;int receives=0;
 void delay(int here,int timeout){if(phase!=here)return;if(first){first=false;entered.set_value();resume.wait();}if(here!=2)std::this_thread::sleep_for(std::chrono::microseconds(timeout*10));}
 void wait_for_client_connection(int timeout){delay(0,timeout);}
 bool is_connected(){return true;}
 bool send(std::span<const uint8_t>,int timeout){delay(1,timeout);return true;}
 PipeResult receive(std::span<uint8_t>,size_t& bytes,int timeout){
  ++receives;if(phase==3){bytes=sizeof(shared_handle_data_t);return PipeResult::Success;}
  delay(2,timeout);std::this_thread::sleep_for(std::chrono::microseconds(timeout*10));bytes=0;return PipeResult::Timeout;
 }
};
struct IPC {
 std::unique_ptr<Helper> _process_helper=std::make_unique<Helper>();
 std::shared_ptr<Pipe> control_pipe=std::make_shared<Pipe>();
 std::atomic_bool _stop_requested{false},_initializing{false},_initialized{false};int setups=0;
// METHODS
 bool setup_shared_resources_from_shared_handles(const shared_handle_data_t&){++setups;return true;}
 bool should_swap_to_dxgi(){return false;}bool should_reinit(){return false;}bool is_initialized(){return _initialized;}
 void handle_desktop_switch_message(std::span<const uint8_t>){}
 void initialize_if_needed(){
// PRE
// CONNECT
 std::array<uint8_t,1> config_span{};
// SEND
// WAIT
 _initialized=true;
 }
};
namespace platf {enum class capture_e{ok,error,reinit,interrupted};struct display_t{virtual ~display_t()=default;};namespace dxgi {
struct display_base_t:display_t{virtual void request_capture_stop() noexcept {}};
struct display_wgc_ipc_vram_t:display_base_t{std::shared_ptr<IPC> _ipc_session;void request_capture_stop() noexcept override;capture_e before_frame();};
struct display_wgc_ipc_ram_t:display_base_t{std::shared_ptr<IPC> _ipc_session;void request_capture_stop() noexcept override;capture_e before_frame();};
// FORWARD
// SNAPSHOTS
}}
struct capture_thread_async_ctx_t {
 std::shared_ptr<safe::queue_t<int>> capture_ctx_queue=std::make_shared<safe::queue_t<int>>();
 sync_util::sync_t<std::weak_ptr<platf::display_t>> display_wp;std::thread capture_thread;
};
// WATCH
#define _WIN32
// END
#undef _WIN32
struct session_t{std::thread videoThread,audioThread;safe::signal_t controlEnd;};
void join_session(session_t& session){
// JOIN
}
int failures=0;void check(bool ok,const char* s){if(!ok){++failures;std::cerr<<"FAIL: "<<s<<"\n";}}
template<class Display> void launch(capture_thread_async_ctx_t& c,std::shared_ptr<IPC> ipc){
 auto d=std::make_shared<Display>();d->_ipc_session=ipc;c.display_wp=d;
 c.capture_thread=std::thread([d=std::move(d)]{d->_ipc_session->initialize_if_needed();while(d.use_count()!=1)std::this_thread::yield();});
}
template<class Display> void cancel(int phase){
 traps=0;auto ipc=std::make_shared<IPC>();ipc->control_pipe->phase=phase;
 std::promise<void> resume;ipc->control_pipe->resume=resume.get_future().share();auto entered=ipc->control_pipe->entered.get_future();
 capture_thread_async_ctx_t c;launch<Display>(c,ipc);check(entered.wait_for(1s)==std::future_status::ready,"entered pending helper phase");
 safe::signal_t shutdown;session_t s;s.videoThread=std::thread([&]{shutdown.view();end_capture_async(c);});s.audioThread=std::thread([]{});s.controlEnd.raise(true);shutdown.raise(true);
 auto limit=std::chrono::steady_clock::now()+1s;while(c.capture_ctx_queue->running()&&std::chrono::steady_clock::now()<limit)std::this_thread::yield();
 check(!c.capture_ctx_queue->running(),"capture queue stopped");
 std::thread reaper([&]{join_session(s);});resume.set_value();reaper.join();
 check(traps==0,"WGC cancellation completes before video watchdog");check(ipc->stop_requested(),"stop targets this WGC generation");check(ipc->setups==0,"cancelled helper imports no handles");
}
int main(){
 for(int p=0;p<3;++p){cancel<platf::dxgi::display_wgc_ipc_vram_t>(p);cancel<platf::dxgi::display_wgc_ipc_ram_t>(p);}
 auto old=std::make_shared<IPC>();old->control_pipe->phase=3;old->request_stop();old->initialize_if_needed();check(old->control_pipe->receives==0&&!old->_initialized,"retired generation cannot reinitialize");
 auto resumed=std::make_shared<IPC>();resumed->control_pipe->phase=3;resumed->initialize_if_needed();check(resumed->_initialized&&resumed->setups==1&&!resumed->stop_requested(),"fresh resume generation initializes");
 auto peer=std::make_shared<IPC>();peer->control_pipe->phase=3;capture_thread_async_ctx_t other;launch<platf::dxgi::display_wgc_ipc_vram_t>(other,peer);other.capture_thread.join();check(peer->_initialized&&!peer->stop_requested(),"other session remains independent");
 auto exited=std::make_shared<IPC>();exited->_process_helper->live=false;exited->initialize_if_needed();check(exited->setups==0&&!exited->_initialized,"exited helper still fails initialization");
 platf::dxgi::display_wgc_ipc_vram_t vram;vram._ipc_session=old;check(vram.before_frame()==platf::capture_e::interrupted,"VRAM cancellation is interrupted, avoiding reinit teardown");
 platf::dxgi::display_wgc_ipc_ram_t ram;ram._ipc_session=old;check(ram.before_frame()==platf::capture_e::interrupted,"RAM cancellation is interrupted, avoiding reinit teardown");
 auto initializing=std::make_shared<IPC>();initializing->_initializing=true;
 std::promise<void> completed;auto done=completed.get_future();std::thread waiter([&]{initializing->initialize_if_needed();completed.set_value();});
 initializing->request_stop();check(done.wait_for(50ms)==std::future_status::ready,"initialization contender responds to stop");initializing->_initializing=false;waiter.join();
 std::cout<<"13 production WGC stop/handshake/ownership/isolation scenarios, failures="<<failures<<"\n";return failures?1:0;
}
'''
budgets='\n'.join(re.findall(r'^    constexpr auto kHelperHandle(?:WaitTimeout|ProgressInterval) = [^\n]+',ipc,re.M))
if len(budgets.splitlines())!=2:raise RuntimeError('Missing production WGC helper wait budgets')
for name,value in [('METHODS','\n'.join(methods)),('PRE',pre),('CONNECT',connect),('SEND',send),('WAIT',wait),('FORWARD','\n'.join(forward)),('SNAPSHOTS','\n'.join(snapshots)),('WATCH',watch),('END',end),('JOIN',join),('BUDGETS',budgets)]:code=code.replace('// '+name+'\n',value+'\n')
with tempfile.TemporaryDirectory(prefix='wgc-stop-replay-') as temp:
    cpp=Path(temp)/'test.cpp';exe=Path(temp)/'test';cpp.write_text(code)
    subprocess.run([compiler,'-std=c++23','-O2','-pthread','-Wall','-Wextra','-I',str(root),str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=8)
