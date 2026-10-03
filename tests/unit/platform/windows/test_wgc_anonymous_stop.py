#!/usr/bin/env python3
"""Compile the complete production anonymous server handshake with native waits mocked.

The clock/waits are accelerated 100x. Checks preserve healthy handoff and framed
fallback, and cancel between every nested native operation without I/O retries.
Native CancelIoEx completion and D3D driver behavior are outside this fixture.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

root = Path(sys.argv[1]).resolve()
compiler = sys.argv[2] if len(sys.argv) > 2 else 'c++'
before = Path(sys.argv[3]) if len(sys.argv) > 3 else None

def read(relative):
    original = before / Path(relative).name if before else root / relative
    return (original if original.exists() else root / relative).read_text()

source = read('src/platform/windows/ipc/pipes.cpp')
header = read('src/platform/windows/ipc/pipes.h')
interface = header[header.index('  class INamedPipe {'):header.index('  class FramedPipe:')]
message = re.search(r'  struct AnonConnectMsg \{[\s\S]*?^  };', header, re.M).group()
helpers = source[source.index('    constexpr uint32_t kMaxHandshakeFrameLen'):source.index('  std::unique_ptr<INamedPipe> AnonymousPipeFactory::create_server')]
factory = source[source.index('  std::unique_ptr<INamedPipe> AnonymousPipeFactory::create_server'):source.index('  std::unique_ptr<INamedPipe> AnonymousPipeFactory::create_client')]
classes = source[source.index('  class PrefetchedPipe:'):source.index('  std::unique_ptr<INamedPipe> AnonymousPipeFactory::handshake_server')]
classes = classes.replace('std::chrono::steady_clock', 'TestClock')
factory_decl = '\n'.join(re.findall(r'^    std::unique_ptr<INamedPipe> create_server\([^\n]+', header[header.index('  class AnonymousPipeFactory:'):], re.M)).replace(' override;', ';')
create = 'factory.create_server("control", [&] { return stop.load(); })' if 'stop_requested' in factory else 'factory.create_server("control")'
code = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
using namespace std::literals;
#define BOOST_LOG(level) std::ostringstream{}
#ifndef _WIN32
constexpr int _TRUNCATE=0;
template<size_t N> void wcsncpy_s(wchar_t (&out)[N], const wchar_t* in, int){std::wcsncpy(out,in,N-1);out[N-1]=0;}
#endif
struct TestClock {
 using time_point=std::chrono::steady_clock::time_point;
 static inline auto origin=std::chrono::steady_clock::now();
 static time_point now(){return time_point((std::chrono::steady_clock::now()-origin)*100);}
};
namespace platf::dxgi {
constexpr uint8_t ACK_MSG=2;
enum class PipeResult{Success,Timeout,Disconnected,BrokenPipe,Error};
// INTERFACE
// MESSAGE
std::wstring utf8_to_wide(const std::string& s){return std::wstring(s.begin(),s.end());}
std::string generate_guid(){return "data";}
struct State {
 int phase=-1;bool entered_once=false,framed=false,send_fail=false;std::atomic_int calls{0};
 int connections=0,sends=0,acks=0,peeks=0,data_connections=0,disconnects=0;
 std::promise<void> entered;std::shared_future<void> resume;
 void operation(int here,int timeout){
  ++calls;
  if(here==phase&&!entered_once){entered_once=true;entered.set_value();resume.wait();}
  if(timeout>0)std::this_thread::sleep_for(std::chrono::microseconds(timeout*10));
 }
};
class NativePipe:public INamedPipe {
 std::shared_ptr<State> state;bool data;
 public:
 NativePipe(std::shared_ptr<State> s,bool d):state(std::move(s)),data(d){}
 bool send(std::span<const uint8_t>,int timeout) override {
  ++state->sends;state->operation(data?6:1,timeout);return !state->send_fail;
 }
 PipeResult receive(std::span<uint8_t> dst,size_t& bytes,int timeout) override {
  if(timeout==10){++state->peeks;state->operation(3,timeout);bytes=0;return PipeResult::Timeout;}
  ++state->acks;state->operation(2,timeout);
  if(state->framed){uint32_t length=4096;std::memcpy(dst.data(),&length,4);bytes=4;return PipeResult::Success;}
  dst[0]=ACK_MSG;bytes=1;return PipeResult::Success;
 }
 PipeResult receive_latest(std::span<uint8_t> dst,size_t& bytes,int timeout) override {return receive(dst,bytes,timeout);}
 void wait_for_client_connection(int timeout) override {
  if(data){++state->data_connections;state->operation(4,timeout>0?timeout:5000);}
  else{++state->connections;state->operation(0,timeout);}
 }
 void disconnect() override {++state->disconnects;}
 bool is_connected() override {return true;}
};
class NamedPipeFactory {
 public:std::shared_ptr<State> state;
 std::unique_ptr<INamedPipe> create_server(const std::string& name){return std::make_unique<NativePipe>(state,name=="data");}
};
class AnonymousPipeFactory {
 public:
 enum class HandshakeAckResult{Acked,Fallback,Failed};
 NamedPipeFactory _pipe_factory;
// FACTORY_DECL
};
namespace {
// HELPERS
// FACTORY
// CLASSES
}
using namespace platf::dxgi;
int failures=0;
void check(bool ok,const char* name){if(!ok){++failures;std::cerr<<"FAIL: "<<name<<"\n";}}
std::unique_ptr<INamedPipe> make_pipe(std::shared_ptr<State> state,std::atomic_bool& stop){
 AnonymousPipeFactory factory;factory._pipe_factory.state=std::move(state);
 return CREATE;
}
void cancel(int phase){
 auto state=std::make_shared<State>();state->phase=phase;std::atomic_bool stop{false};
 std::promise<void> resume;state->resume=resume.get_future().share();auto entered=state->entered.get_future();
 auto pipe=make_pipe(state,stop);
 auto worker=std::async(std::launch::async,[&]{pipe->wait_for_client_connection(5000);});
 check(entered.wait_for(1s)==std::future_status::ready,"entered actual nested handshake stage");
 stop=true;int at_stop=state->calls;resume.set_value();
 check(worker.wait_for(90ms)==std::future_status::ready,"cancelled composite handshake returns before nominal video watchdog");
 worker.get();check(state->calls==at_stop,"stop prevents another nested native wait");
 check(state->disconnects==0,"stop prevents data handoff of cancelled generation");
 int completed=state->calls;std::array<uint8_t,8> buffer{};size_t bytes=9;
 check(!pipe->send(buffer,5000),"cancelled config send is rejected");
 check(pipe->receive(buffer,bytes,250)==PipeResult::Disconnected&&bytes==0,"cancelled receive is rejected");
 check(pipe->receive_latest(buffer,bytes,250)==PipeResult::Disconnected&&bytes==0,"cancelled latest receive is rejected");
 check(completed==state->calls,"cancelled generation issues no native I/O");
}
int main(){
 for(int phase=0;phase<5;++phase)cancel(phase);
 std::atomic_bool stopped{true};auto retired=std::make_shared<State>();auto old=make_pipe(retired,stopped);
 old->wait_for_client_connection(5000);check(retired->calls==0,"early cancellation prevents handshake entirely");
 std::atomic_bool live{false};auto healthy=std::make_shared<State>();auto pipe=make_pipe(healthy,live);
 pipe->wait_for_client_connection(5000);std::array<uint8_t,4> config{};
 check(pipe->send(config,5000),"healthy generation sends config after data handoff");
 check(healthy->connections==1&&healthy->sends==2&&healthy->acks==1&&healthy->peeks==1&&healthy->data_connections==1&&healthy->disconnects==1,"healthy protocol has one handshake/config write and correct handoff");
 check(!live&&retired->calls==0,"new generation and independent session remain isolated");
 auto fallback=std::make_shared<State>();fallback->framed=true;auto framed=make_pipe(fallback,live);
 framed->wait_for_client_connection(5000);size_t bytes=0;std::array<uint8_t,8> buffer{};
 check(framed->receive(buffer,bytes,250)==PipeResult::Success&&bytes==4,"framed fallback preserves prefetched bytes");
 check(fallback->data_connections==0&&fallback->disconnects==0,"framed fallback keeps control connection");
 std::cout<<"9 complete production anonymous handshake cancellation/handoff/fallback/isolation scenarios, failures="<<failures<<"\n";
 return failures?1:0;
}
'''
for name,value in [('INTERFACE',interface),('MESSAGE',message),('HELPERS',helpers),('FACTORY',factory),('FACTORY_DECL',factory_decl),('CLASSES',classes)]:
    code = code.replace('// '+name+'\n',value+'\n')
code=code.replace('CREATE',create)
with tempfile.TemporaryDirectory(prefix='wgc-anonymous-replay-') as temp:
    cpp=Path(temp)/'test.cpp';exe=Path(temp)/'test';cpp.write_text(code)
    subprocess.run([compiler,'-std=c++23','-O2','-pthread','-Wall','-Wextra',str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=8)
