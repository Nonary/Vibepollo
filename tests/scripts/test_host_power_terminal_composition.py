#!/usr/bin/env python3
"""Execute the actual sleep route/dispatch, service and HTTP framing with terminal.

TLS authority, sockets, native sleep and physical recovery are controlled endpoints.
No native power, stream, display or device operation is performed.
"""
import argparse
from pathlib import Path
import subprocess
p=argparse.ArgumentParser();p.add_argument('--source',type=Path,required=True);p.add_argument('--build-dir',type=Path,required=True);p.add_argument('--compiler',default='g++');p.add_argument('--json-include',default='');p.add_argument('--boost-include',default='');a=p.parse_args();root=a.source.resolve();out=a.build_dir.resolve();out.mkdir(parents=True,exist_ok=True)
nv=(root/'src/nvhttp.cpp').read_text()
def body(signature):
 s=nv.index(signature);return nv[s:nv.index('\n  }',s)+len('\n  }')]
def statement(signature):
 s=nv.index(signature);return nv[s:nv.index('\n    };',s)+len('\n    };')]
terminal=body('  void terminate_all_streams_from_host(');post=body('  void post_sleep(')
adapter=statement('    auto run_on_blocking_pool =')+'\n'+statement('    auto run_blocking_nvhttp =')
route=statement('    https_server.resource["^/power-actions/v1/sleep$"]')
support=r'''
#include "src/host_stream_control.h"
#include "src/nvhttp_dispatch.h"
#include "src/power_action_http.h"
#include <atomic>
#include <chrono>
#include <future>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
using namespace std::chrono_literals;
#define BOOST_LOG(level) std::cerr
std::function<void()> flush_endpoint,native_endpoint,physical_endpoint;
std::atomic<int> native_calls{0},physical_calls{0};
struct Response {
 bool close_connection_after_response=false;int status=0;std::string body;
 void write(const std::string&value){body=value;if(value.find("503")!=std::string::npos)status=503;}
 void write(SimpleWeb::StatusCode code,const std::string&value,const SimpleWeb::CaseInsensitiveMultimap&){status=static_cast<int>(code);body=value;}
 template<class Callback>void send(Callback complete){if(flush_endpoint)flush_endpoint();complete(SimpleWeb::error_code{});}
};
struct Request {std::string method="POST",content;std::multimap<std::string,std::string>header;std::string token;auto parse_query_string()const{return std::multimap<std::string,std::string>{{"token",token}};}};
using resp_https_t=std::shared_ptr<Response>;using req_https_t=std::shared_ptr<Request>;
using Handler=std::function<void(resp_https_t,req_https_t)>;
namespace thread_pool_util {struct ThreadPool{std::vector<std::function<void()>>tasks;template<class T>void push(T t){tasks.emplace_back(std::move(t));}void run(){auto ready=std::move(tasks);tasks.clear();for(auto&t:ready)t();}};}
struct Server {std::map<std::string,std::map<std::string,Handler>>resource;};
namespace rtsp_stream {void terminate_sessions(bool){}}
namespace webrtc_stream {void shutdown_all_sessions(){}}
namespace remote_session {void notify_monitor_shutdown(){}}
namespace proc {struct Proc{void terminate(){}}proc;}
namespace stream::session {struct cleanup_reservation_t{~cleanup_reservation_t(){}};bool finalize_shared_runtime_if_idle(const char*){return true;}bool has_shared_runtime_owner(){return false;}}
namespace remote_display_topology {struct Topology{bool shutdown_complete(){return true;}};Topology&instance(){static Topology t;return t;}}
namespace platf::virtual_display_cleanup {struct Result{bool virtual_displays_removed=true,physical_display_recovered=true;};Result terminate_all(const char*){++physical_calls;if(physical_endpoint)physical_endpoint();return {};}}
namespace nvhttp {
 std::mutex launch_request_mutex,stream_lifecycle_gate;
 std::mutex &stream_lifecycle_mutex(){return stream_lifecycle_gate;}
 power_action::service_t paired_power_actions{stream_lifecycle_gate};
 power_action::hooks_t power_action_hooks(resp_https_t response,req_https_t){return{
  .authority=[] {return power_action::authority_t{"paired-client",true};},
  .supported=[] {return true;},.activity=[] {return power_action::activity_t{};},
  .suspend_if_authorized=[] {++native_calls;if(native_endpoint)native_endpoint();return true;},
  .respond=[response](const auto&value){return power_action::write_response(response,value);}
 };}
'''
common=r'''
}
int failures=0;void check(bool ok,const char*label){std::cout<<(ok?"PASS ":"FAIL ")<<label<<'\n';if(!ok)++failures;}
struct Gate {std::promise<void>arrived,release;std::shared_future<void>go=release.get_future();std::future<void>entered=arrived.get_future();void block(){arrived.set_value();go.wait();}bool wait(){return entered.wait_for(2s)==std::future_status::ready;}void open(){release.set_value();}};
bool odd(){auto limit=std::chrono::steady_clock::now()+2s;while(std::chrono::steady_clock::now()<limit){if(host_stream_control::request_ticket()&1)return true;std::this_thread::yield();}return false;}
bool counted(){std::lock_guard lock{host_stream_control::state().mutex};return host_stream_control::state().active>0;}
req_https_t request(){auto q=std::make_shared<Request>();auto hooks=nvhttp::power_action_hooks(std::make_shared<Response>(),q);q->token=nvhttp::paired_power_actions.capabilities(hooks).token;return q;}
struct Routes {thread_pool_util::ThreadPool pool;Server https_server;
 Routes(){auto &blocking_route_pool=pool;namespace dispatch=nvhttp::dispatch;auto mutation_admission=std::make_shared<single_flight::admission_t>();using nvhttp::post_sleep;
#define _WIN32
'''
checks=r'''
#undef _WIN32
 }
 void submit(resp_https_t r,req_https_t q){https_server.resource.at("^/power-actions/v1/sleep$").at("POST")(r,q);}
 void run(){pool.run();}
};
void queued(){
 Routes routes;auto before=std::make_shared<Response>();routes.submit(before,request());
 nvhttp::terminate_all_streams_from_host([]{return true;});routes.run();
 check(before->status==503&&native_calls==0,"actual registered sleep POST rejects pre-terminal queued acceptance");
 auto during=std::make_shared<Response>();auto q=request();host_stream_control::terminate([]{return true;},[&]{routes.submit(during,q);});routes.run();
 check(during->status==503&&native_calls==0,"actual registered sleep POST rejects intake queued during terminal after reopen");
 auto fresh=std::make_shared<Response>();routes.submit(fresh,request());routes.run();
 check(fresh->status==202&&native_calls==1,"actual power service permits legitimate fresh post-terminal acceptance");
}
void admitted(bool flush){
 Routes routes;Gate entered;auto q=request();auto response=std::make_shared<Response>();
 if(flush)flush_endpoint=[&]{entered.block();};else native_endpoint=[&]{entered.block();};
 routes.submit(response,q);auto worker=std::async(std::launch::async,[&]{routes.run();});
 check(entered.wait(),flush?"actual power response flush pauses with service lifecycle held":"actual power native endpoint pauses after accepted response");
 auto terminal=std::async(std::launch::async,[]{nvhttp::terminate_all_streams_from_host([]{return true;});});
 check(odd(),"terminal closes admission while registered power callback is admitted");
 check(counted()&&physical_calls==0&&terminal.wait_for(50ms)==std::future_status::timeout,"power callback remains counted across acceptance and native completion tail");
 entered.open();worker.get();terminal.get();
 check(response->status==202&&native_calls==1&&physical_calls==1,"terminal recovery follows admitted power service lifecycle release");
}
void late(){
 Routes routes;auto q=request();Gate physical;physical_endpoint=[&]{physical.block();};
 auto terminal=std::async(std::launch::async,[]{nvhttp::terminate_all_streams_from_host([]{return true;});});
 check(physical.wait(),"actual terminal is paused in physical recovery after lifecycle release");
 auto response=std::make_shared<Response>();routes.submit(response,q);routes.run();
 check(response->status==503&&native_calls==0,"actual sleep route cannot accept power action during terminal physical recovery");
 physical.open();terminal.get();
}
int main(int argc,char**argv){if(argc!=2)return 2;std::string name=argv[1];if(name=="queued")queued();else if(name=="flush")admitted(true);else if(name=="native")admitted(false);else if(name=="late")late();else return 2;return failures?1:0;}
'''
cpp=out/'production-power-terminal-composition.cpp';cpp.write_text(support+terminal+post+common+adapter+'\n'+route+checks)
exe=out/'production-power-terminal-composition';cmd=[a.compiler,'-std=c++23','-Wall','-Wextra','-Werror','-pthread','-I',str(root),'-I',str(root/'third-party')]
for include in (a.json_include+';'+a.boost_include).split(';'):
 if include:cmd+=['-I',str(Path(include).resolve())]
power_object=out/'power_action.o'
# The existing service uses defaulted aggregate members. Compile its complete
# body with normal warning flags; keep the extracted new fixture under Werror.
subprocess.run([arg for arg in cmd if arg!='-Werror']+['-c',str(root/'src/power_action.cpp'),'-o',str(power_object)],check=True)
subprocess.run(cmd+[str(cpp),str(power_object),'-lcrypto','-o',str(exe)],check=True)
failed=[]
for case in ['queued','flush','native','late']:
 print('CASE',case,flush=True)
 try:
  result=subprocess.run([str(exe),case],timeout=12)
  if result.returncode:failed.append(case)
 except subprocess.TimeoutExpired:failed.append(case+': timeout')
if failed:raise SystemExit('Failed cases: '+', '.join(failed))
