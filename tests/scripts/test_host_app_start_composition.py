#!/usr/bin/env python3
"""Compile actual app-start admission, Web handler/adapter and terminal bodies.

Process execution uses the unchanged production entry up to its first application
operation, then a controlled endpoint. The full execute translation unit is a
separate syntax/build check. No process, display, provider or Windows API runs.
"""
import argparse
from pathlib import Path
import subprocess
p=argparse.ArgumentParser()
p.add_argument('--source',type=Path,required=True)
p.add_argument('--build-dir',type=Path,required=True)
p.add_argument('--compiler',default='g++')
p.add_argument('--json-include',default='')
p.add_argument('--boost-include',default='')
a=p.parse_args();root=a.source.resolve();build=a.build_dir.resolve();build.mkdir(parents=True,exist_ok=True)
def body(text,signature):
 start=text.index(signature);end=text.index('\n  }',start)+len('\n  }');return text[start:end]
def statement(text,signature):
 start=text.index(signature);end=text.index('\n    };',start)+len('\n    };');return text[start:end]
web=(root/'src/confighttp.cpp').read_text();nv=(root/'src/nvhttp.cpp').read_text();process=(root/'src/process.cpp').read_text()
whole_execute=body(process,'  int proc_t::execute(')
cut=whole_execute.index('#ifdef _WIN32') if 'const ctx_t &app' in whole_execute else whole_execute.index('    // Ensure starting from a clean slate')
execute_entry=whole_execute[:cut]
is_target='  void launchApp(' in web
rtsp=(root/'src/rtsp.cpp').read_text()
rtsp_start=rtsp.index('    template<class Function>\n    void run_startup(')
rtsp_end=rtsp.index('\n    }',rtsp_start)+len('\n    }')
rtsp_method=rtsp[rtsp_start:rtsp_end]
terminal=body(nv,'  void terminate_all_streams_from_host(')
support=r'''
#include "src/host_stream_control.h"
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <future>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
using namespace std::string_view_literals;
using namespace std::chrono_literals;
#define BOOST_LOG(level) std::cerr
struct Response {bool ok=false;int status=0;};
struct Request {std::stringstream content;bool authenticated=true;std::map<std::string,std::string> parse_query_string(){return {};}};
using resp_https_t=std::shared_ptr<Response>;using req_https_t=std::shared_ptr<Request>;
std::function<void()> authentication, lifecycle_request, execute_endpoint, physical_endpoint, response_endpoint;
bool validateContentType(resp_https_t,req_https_t,const char*){return true;}
bool authenticate(resp_https_t r,req_https_t q){if(authentication)authentication();if(!q->authenticated)r->status=401;return q->authenticated;}
void print_req(req_https_t){}
void bad_request(resp_https_t r,req_https_t,const char*){r->status=400;}
void service_unavailable(resp_https_t r,const char*){r->status=503;}
void send_response(resp_https_t r,const nlohmann::json&){if(response_endpoint)response_endpoint();r->ok=true;r->status=200;}
namespace crypto {enum class PERM{_all};struct named_cert_t{std::string name,uuid;PERM perm;};}
namespace http {const std::string unique_id="host";}
namespace rtsp_stream {struct launch_session_t{};void terminate_sessions(bool){}}
namespace webrtc_stream {void shutdown_all_sessions(){}}
namespace remote_session {void notify_monitor_shutdown(){}}
namespace stream::session {struct cleanup_reservation_t{~cleanup_reservation_t(){}};bool finalize_shared_runtime_if_idle(const char*){return true;}bool has_shared_runtime_owner(){return false;}}
namespace remote_display_topology {struct Topology{bool shutdown_complete(){return true;}};Topology&instance(){static Topology t;return t;}}
std::mutex lifecycle;
std::atomic<int> physical_calls{0},launch_sessions{0};
namespace proc {
 struct ctx_t {std::string uuid="app",name="Application";bool remote_monitor_profile=false;};
 struct proc_t {
  std::vector<ctx_t> apps{ctx_t{}};std::atomic<int> app_id{-1},executions{0};
  const auto &get_apps(){return apps;}
  std::optional<ctx_t> resolve_app(int) const {return apps.empty()?std::nullopt:std::make_optional(apps.front());}
  int running(){return app_id.load();}
  void terminate(){host_stream_control::activity_guard_t guard;if(guard)app_id=-1;}
  int execute(const ctx_t &app,std::shared_ptr<rtsp_stream::launch_session_t> launch_session,bool config_read_gate_held=false);
  int execute(int app_id,std::shared_ptr<rtsp_stream::launch_session_t> launch_session,bool config_read_gate_held=false);
 } proc;
'''
entry_tail=r'''
    (void)app; (void)launch_session; (void)config_read_gate_held;
    if(execute_endpoint)execute_endpoint();
    ++executions;app_id=1;return 0;
  }
}
namespace platf::virtual_display_cleanup {
 struct Result{bool virtual_displays_removed=true,physical_display_recovered=true;};
 Result terminate_all(const char*){++physical_calls;if(physical_endpoint)physical_endpoint();return {};}
}
namespace nvhttp {
 std::mutex launch_request_mutex;
 std::mutex &stream_lifecycle_mutex(){if(lifecycle_request)lifecycle_request();return lifecycle;}
 [[maybe_unused]] std::shared_ptr<rtsp_stream::launch_session_t> make_launch_session(bool,bool,const std::map<std::string,std::string>&,const crypto::named_cert_t*){++launch_sessions;return std::make_shared<rtsp_stream::launch_session_t>();}
'''
if not is_target:entry_tail=entry_tail.replace('(void)app;','(void)app_id;').replace('++executions;app_id=1;','++executions;this->app_id=1;')
common=r'''
}
int failures=0;
void check(bool ok,const char *label){std::cout<<(ok?"PASS ":"FAIL ")<<label<<'\n';if(!ok)++failures;}
bool odd(){auto deadline=std::chrono::steady_clock::now()+2s;while(std::chrono::steady_clock::now()<deadline){if(host_stream_control::request_ticket()&1)return true;std::this_thread::yield();}return false;}
bool counted(){std::lock_guard lock{host_stream_control::state().mutex};return host_stream_control::state().active>0;}
struct Gate {std::promise<void> arrived,release;std::shared_future<void> go=release.get_future();std::future<void> entered=arrived.get_future();void block(){arrived.set_value();go.wait();}bool wait(){return entered.wait_for(2s)==std::future_status::ready;}void open(){release.set_value();}};
std::shared_ptr<rtsp_stream::launch_session_t> session(){return std::make_shared<rtsp_stream::launch_session_t>();}
'''
call='proc::proc.execute(proc::ctx_t{},session())' if is_target else 'proc::proc.execute(1,session())'
process_checks=r'''
void process_during(){
 Gate physical;physical_endpoint=[&]{physical.block();};
 int nested=0;auto terminal=std::async(std::launch::async,[&]{nvhttp::terminate_all_streams_from_host([&]{return true;});});
 check(physical.wait(),"actual terminal reaches final physical endpoint");
 std::unique_lock lock{lifecycle};
 check(CALL==503,"actual execute entry rejects external startup while lifecycle is held during terminal");lock.unlock();
 physical.open();terminal.get();physical_endpoint={};
 host_stream_control::terminate([]{return true;},[&]{nested=CALL;});
 check(nested==503,"actual execute entry rejects startup from terminal-owned cleanup");
 const int before=proc::proc.executions;
 check(CALL==0&&proc::proc.executions==before+1,"actual execute entry permits legitimate new launch after terminal returns");
}
void process_admitted(){
 Gate entered;execute_endpoint=[&]{entered.block();};
 auto launch=std::async(std::launch::async,[]{return CALL;});
 check(entered.wait(),"actual execute entry admits operation before startup endpoint");
 auto terminal=std::async(std::launch::async,[]{nvhttp::terminate_all_streams_from_host([]{return true;});});
 check(odd(),"terminal closes admission while admitted execute endpoint is paused");
 check(counted()&&physical_calls==0&&terminal.wait_for(50ms)==std::future_status::timeout,"execute owner prevents physical recovery through paused startup tail");
 entered.open();check(launch.get()==0,"already-admitted execute may finish");terminal.get();execute_endpoint={};
 check(proc::proc.app_id==-1&&physical_calls==1,"terminal removes application after admitted execute drains");
}
'''.replace('CALL',call)
translation=support+execute_entry+entry_tail+terminal+common
rtsp_support=r'''
#include <optional>
#include <array>
struct Queue {std::vector<std::function<void()>>tasks;template<class T>void push(T t){tasks.emplace_back(std::move(t));}void run(){auto ready=std::move(tasks);tasks.clear();for(auto&t:ready)t();}};
struct RtspAdapter {
 Queue startup_pool;host_stream_control::startup_dispatch_t startup_tasks;std::atomic_bool stopping{false};int finalizers=0;
 void finish_startup(){std::unique_lock lock{lifecycle};++finalizers;}
 void finish_startup(std::optional<std::array<std::uint8_t,16>>){finish_startup();}
'''
rtsp_checks=r'''
};
void pending_start_wait(){
 RtspAdapter server;Gate entered;std::unique_lock lock{lifecycle};int result=-1;
 server.run_startup(ARG[&](bool admitted){if(admitted){entered.arrived.set_value();std::unique_lock pending_lock{lifecycle};result=CALL;}});
 auto worker=std::async(std::launch::async,[&]{server.startup_pool.run();});
 check(entered.wait(),"actual RTSP pending worker admitted before waiting for lifecycle");
 auto terminal=std::async(std::launch::async,[]{nvhttp::terminate_all_streams_from_host([]{return true;});});
 check(odd(),"terminal closes admission while actual RTSP worker awaits lifecycle");
 check(counted()&&physical_calls==0&&terminal.wait_for(50ms)==std::future_status::timeout,"terminal drain does not take lifecycle from counted pending start");
 lock.unlock();worker.get();terminal.get();
 check(result==503&&server.finalizers==1&&physical_calls==1,"pending worker cancels execute and finalizes before terminal recovery without inversion");
}
'''.replace('ARG','std::nullopt,' if is_target else '').replace('CALL',call)
translation+=rtsp_support+rtsp_method+rtsp_checks
if is_target:
 launch=body(web,'  void launchApp(resp_https_t response, req_https_t request')
 adapter=statement(web,'    auto register_blocking_api_route =')
 # Extract the actual currently registered app-start statement, including an
 # older ordinary registration for a falsifying baseline run.
 regline=next(line for line in web.splitlines() if '"^/api/apps/launch$"' in line)
 web_support=r'''
namespace thread_pool_util {struct ThreadPool{std::vector<std::function<void()>>tasks;template<class T>void push(T t){tasks.emplace_back(std::move(t));}void run(){auto ready=std::move(tasks);tasks.clear();for(auto&t:ready)t();}};}
namespace confighttp {
'''
 web_support+=launch+r'''
}
using Handler=std::function<void(resp_https_t,req_https_t)>;
resp_https_t response(){return std::make_shared<Response>();}
req_https_t request(){auto q=std::make_shared<Request>();q->content<<R"({"uuid":"app"})";return q;}
void web_late(bool waiting_on_lock){
 Gate entered,physical;
 std::atomic_bool once{true};lifecycle_request=[&]{if(once.exchange(false)){entered.arrived.set_value();if(!waiting_on_lock)entered.go.wait();}};
 physical_endpoint=[&]{physical.block();};
 std::unique_lock lock{lifecycle,std::defer_lock};if(waiting_on_lock)lock.lock();
 auto r=response();auto launch=std::async(std::launch::async,[&]{confighttp::launchApp(r,request());});
 check(entered.wait(),"actual Web app launch pauses after running before lifecycle acquisition");
 auto terminal=std::async(std::launch::async,[]{nvhttp::terminate_all_streams_from_host([]{return true;});});
 check(odd(),"terminal closes admission while entered Web app launch waits");
 check(counted()&&physical_calls==0&&terminal.wait_for(50ms)==std::future_status::timeout,"whole Web app operation counts lifecycle wait before terminal can recover");
 if(waiting_on_lock)lock.unlock();else entered.open();
 launch.get();check(physical.wait(),"terminal completes lifecycle cleanup after queued Web app operation releases");
 check(proc::proc.executions==0,"execute entry cancels startup first reached after terminal closed");
 physical.open();terminal.get();physical_endpoint={};lifecycle_request={};
 auto fresh=response();confighttp::launchApp(fresh,request());
 check(fresh->ok&&proc::proc.executions==1,"actual Web app launch permits fresh post-terminal request");
}
void web_tail(){
 Gate entered;response_endpoint=[&]{entered.block();};auto r=response();
 auto launch=std::async(std::launch::async,[&]{confighttp::launchApp(r,request());});
 check(entered.wait()&&proc::proc.executions==1,"actual Web app launch reaches response tail after successful execute");
 auto terminal=std::async(std::launch::async,[]{nvhttp::terminate_all_streams_from_host([]{return true;});});
 check(odd(),"terminal closes admission while Web app response tail is paused");
 check(counted()&&physical_calls==0&&terminal.wait_for(50ms)==std::future_status::timeout,"whole Web app operation remains counted after execute through response cleanup tail");
 entered.open();launch.get();terminal.get();response_endpoint={};
 check(r->ok&&proc::proc.app_id==-1&&physical_calls==1,"terminal removes successful Web-started application after full handler tail drains");
}
void web_auth(){
 Gate entered;authentication=[&]{entered.block();};auto r=response();
 auto launch=std::async(std::launch::async,[&]{confighttp::launchApp(r,request());});
 check(entered.wait(),"actual Web app intake precedes delayed authentication");
 nvhttp::terminate_all_streams_from_host([]{return true;});entered.open();launch.get();authentication={};
 check(r->status==503&&launch_sessions==0&&proc::proc.executions==0,"Web app request crossing terminal during authentication is canceled before session construction");
}
void web_during(){
 Gate physical;physical_endpoint=[&]{physical.block();};
 auto terminal=std::async(std::launch::async,[]{nvhttp::terminate_all_streams_from_host([]{return true;});});
 check(physical.wait(),"terminal physical recovery entered for late Web request");
 auto r=response();confighttp::launchApp(r,request());
 check(r->status==503&&launch_sessions==0&&proc::proc.executions==0,"actual Web app launch rejects intake during physical recovery before all startup endpoints");
 physical.open();terminal.get();
}
void web_registered(){
 thread_pool_util::ThreadPool blocking_route_pool;std::map<std::string,Handler>routes;
 auto register_api_route=[&](const char*path,const char*,auto handler,bool stream_start=false){routes[path]=[handler,stream_start](resp_https_t r,req_https_t q){handler(r,q,stream_start?host_stream_control::request_ticket():0);};};
 using confighttp::launchApp;
#define _WIN32
'''
 web_support+=adapter+'\n'+regline+r'''
 (void)register_blocking_api_route;
#undef _WIN32
 auto handler=routes.at("^/api/apps/launch$");
 auto denied=request();denied->authenticated=false;auto d=response();handler(d,denied);
 check(d->status==401&&blocking_route_pool.tasks.empty(),"registered Web app route keeps authentication before queue admission");
 auto old=response();handler(old,request());
 check(!old->ok&&blocking_route_pool.tasks.size()==1,"actual Web app registration queues startup through counted adapter");
 nvhttp::terminate_all_streams_from_host([]{return true;});blocking_route_pool.run();
 check(old->status==503&&proc::proc.executions==0&&launch_sessions==0,"registered Web app route cancels stale queued request across terminal epoch");
 auto during=response();host_stream_control::terminate([]{return true;},[&]{handler(during,request());});blocking_route_pool.run();
 check(during->status==503&&proc::proc.executions==0,"registered Web app route cancels request queued during terminal even when run later");
 auto fresh=response();handler(fresh,request());blocking_route_pool.run();
 check(fresh->ok&&proc::proc.executions==1,"registered Web app route accepts legitimate fresh request after terminal returns");
}
'''
 translation+=web_support

# Execute the complete actual registered outer wrapper, including CSRF.
registered_extra=r'''
struct RegisteredServer {std::map<std::string,std::map<std::string,Handler>>resource;};
std::function<void()> csrf_endpoint;
std::string get_client_id(req_https_t){return "client";}
bool validate_csrf_token(resp_https_t,req_https_t,const std::string&){if(csrf_endpoint)csrf_endpoint();return true;}
std::string normalize_route_pattern(const char*p){return p;}
void record_token_route(const std::string&,const char*){}
resp_https_t registered_response(){return std::make_shared<Response>();}
req_https_t registered_request(){auto q=std::make_shared<Request>();q->content<<R"({"uuid":"app"})";return q;}
int ordinary_calls=0;
void ordinaryHandler(resp_https_t r,req_https_t){++ordinary_calls;r->status=200;}

struct RegisteredRoutes {
 RegisteredServer server;thread_pool_util::ThreadPool pool;
 RegisteredRoutes(){
  auto &blocking_route_pool=pool;
  using confighttp::launchApp;
#define _WIN32
REGISTRAR
ADAPTER
REGISTRATION
  register_blocking_api_route("^/ordinary$","POST",ordinaryHandler);
#undef _WIN32
 }
 void start(resp_https_t r,req_https_t q){server.resource.at("^/api/apps/launch$").at("POST")(r,q);}
 void ordinary(resp_https_t r,req_https_t q){server.resource.at("^/ordinary$").at("POST")(r,q);}
 void run(){pool.run();}
};
void registered_auth(bool closed,bool csrf){
 RegisteredRoutes routes;Gate entered,physical;
 std::atomic_bool once{true};auto pause=[&]{if(once.exchange(false))entered.block();};
 if(csrf)csrf_endpoint=pause;else authentication=pause;
 std::future<void>terminal;
 if(closed){physical_endpoint=[&]{physical.block();};terminal=std::async(std::launch::async,[]{nvhttp::terminate_all_streams_from_host([]{return true;});});check(physical.wait(),"actual terminal holds closed epoch before registered request entry");}
 auto r=registered_response();auto request=std::async(std::launch::async,[&]{routes.start(r,registered_request());});
 check(entered.wait()&&routes.pool.tasks.empty(),csrf?"actual registered outer CSRF pauses before authentication and enqueue":"actual registered outer authentication pauses before enqueue");
 check(!counted(),"authentication has not yet acquired a counted worker owner");
 if(closed){physical.open();terminal.get();physical_endpoint={};}else nvhttp::terminate_all_streams_from_host([]{return true;});
 check(physical_calls==1&&(host_stream_control::request_ticket()&1)==0,"actual full terminal close and reopen finishes before authentication release");
 entered.open();request.get();authentication={};csrf_endpoint={};
 check(routes.pool.tasks.size()==1,"original registered request reaches queue after terminal returns");routes.run();
 check(r->status==503&&proc::proc.executions==0&&launch_sessions==0,"original registered intake ticket rejects stale request before startup mutation");
 auto fresh=registered_response();routes.start(fresh,registered_request());routes.run();
 check(fresh->status==200&&proc::proc.executions==1,"registered route permits fresh startup after stale request rejection");
}
void registered_nonstart(){
 RegisteredRoutes routes;Gate entered,physical;physical_endpoint=[&]{physical.block();};
 auto terminal=std::async(std::launch::async,[]{nvhttp::terminate_all_streams_from_host([]{return true;});});
 check(physical.wait(),"actual terminal reaches physical endpoint for ordinary-route check");
 auto during=registered_response();routes.ordinary(during,registered_request());routes.run();
 check(during->status==200&&ordinary_calls==1&&!counted(),"ordinary blocking route remains unfenced during terminal recovery");
 std::atomic_bool once{true};authentication=[&]{if(once.exchange(false))entered.block();};auto later=registered_response();
 auto request=std::async(std::launch::async,[&]{routes.ordinary(later,registered_request());});
 check(entered.wait(),"ordinary route authentication begins during closed epoch");physical.open();terminal.get();physical_endpoint={};
 entered.open();request.get();authentication={};routes.run();
 check(later->status==200&&ordinary_calls==2,"ordinary route crossing full terminal epoch remains permitted");
}
void registered_denied(){
 RegisteredRoutes routes;auto q=registered_request();q->authenticated=false;auto r=registered_response();routes.start(r,q);
 check(r->status==401&&routes.pool.tasks.empty()&&!counted(),"actual registered start keeps authentication denial before enqueue and counted admission");
}

void registered_inner_auth(){
 RegisteredRoutes routes;std::atomic<int>auth_count{0};Gate entered;
 authentication=[&]{if(++auth_count==2)entered.block();};auto r=registered_response();routes.start(r,registered_request());
 auto worker=std::async(std::launch::async,[&]{routes.run();});check(entered.wait(),"actual inner application authentication pauses after counted worker admission");
 auto terminal=std::async(std::launch::async,[]{nvhttp::terminate_all_streams_from_host([]{return true;});});check(odd(),"terminal closes epoch while inner handler authentication is paused");
 check(counted()&&physical_calls==0&&terminal.wait_for(50ms)==std::future_status::timeout,"exact-ticket worker remains counted through inner authentication");
 entered.open();worker.get();terminal.get();authentication={};
 check(r->status==503&&proc::proc.executions==0&&launch_sessions==0&&physical_calls==1,"inner handler retains original intake ticket and rejects before startup endpoints");
}

'''
registered_extra=registered_extra.replace('REGISTRAR',statement(web,'    auto register_api_route =')).replace('ADAPTER',statement(web,'    auto register_blocking_api_route ='))
registered_extra=registered_extra.replace('REGISTRATION',next(line for line in web.splitlines() if 'register_blocking_api_route(\"^/api/apps/launch$\"' in line))
translation+=registered_extra
translation+=process_checks+r'''
int main(int argc,char**argv){if(argc!=2)return 2;std::string name=argv[1];
 if(name=="registered-auth")registered_auth(false,false);else if(name=="registered-closed-auth")registered_auth(true,false);else if(name=="registered-csrf")registered_auth(false,true);else if(name=="registered-nonstart")registered_nonstart();else if(name=="registered-denied")registered_denied();else if(name=="registered-inner-auth")registered_inner_auth();else if(name=="process-during")process_during();else if(name=="process-admitted")process_admitted();else if(name=="pending-start-wait")pending_start_wait();
'''
if is_target:translation+=' else if(name=="web-late")web_late(false);else if(name=="web-lock-wait")web_late(true);else if(name=="web-tail")web_tail();else if(name=="web-auth")web_auth();else if(name=="web-during")web_during();else if(name=="web-registered")web_registered();\n'
translation+=' else { return 2; }\n return failures?1:0;\n}\n'
cpp=build/'production-app-start-composition.cpp';cpp.write_text(translation)
exe=build/'production-app-start-composition'
cmd=[a.compiler,'-std=c++23','-Wall','-Wextra','-Werror','-pthread','-I',str(root)]
for include in (a.json_include+';'+a.boost_include).split(';'):
 if include:cmd+=['-I',str(Path(include).resolve())]
subprocess.run(cmd+[str(cpp),'-o',str(exe)],check=True)
cases=['registered-auth','registered-closed-auth','registered-csrf','registered-nonstart','registered-denied']+['registered-inner-auth']+['process-during','process-admitted','pending-start-wait']+(['web-late','web-lock-wait','web-tail','web-auth','web-during','web-registered'] if is_target else [])
failed=[]
for case in cases:
 print('CASE',case,flush=True)
 try:
  result=subprocess.run([str(exe),case],timeout=12)
  if result.returncode:failed.append(case)
 except subprocess.TimeoutExpired:failed.append(case+': timeout')
if failed:raise SystemExit('Failed cases: '+', '.join(failed))
