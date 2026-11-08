#!/usr/bin/env python3
"""Run the registered Linux NVHTTP routes with a controlled executor/peer."""
import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--source', type=Path, required=True)
parser.add_argument('--build-dir', type=Path, required=True)
parser.add_argument('--compiler', required=True)
parser.add_argument('--json-include', default='')
parser.add_argument('--boost-include', default='')
args = parser.parse_args()
root = args.source.resolve()
output = args.build_dir.resolve()
output.mkdir(parents=True, exist_ok=True)
source = (root / 'src/nvhttp.cpp').read_text()

def statement(start):
    begin = source.index(start)
    return source[begin:source.index('\n    };', begin) + len('\n    };')]

parts = [statement('    auto run_on_blocking_pool ='),
         statement('    auto run_blocking_nvhttp ='),
         statement('    auto run_linux_start_nvhttp ='),
         statement('    https_server.resource["^/launch$"'),
         statement('    https_server.resource["^/resume$"'),
         statement('    https_server.resource["^/cancel$"')]

remote_source = (root / 'src/remote_session.cpp').read_text()
def function(text, signature):
    begin = text.index(signature)
    brace = text.index('{', begin)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[begin:end]
identity_table = remote_source[remote_source.index('    struct control_identity_t'):remote_source.index('    bool configured_identity(')]
identity = function(remote_source, '  control_e identify(const std::int32_t id, const std::string_view uuid)')

support = r'''
#include "src/nvhttp_dispatch.h"
#include "src/remote_session.h"
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
using namespace nvhttp;
struct Peer: pending_peer_t {
 std::function<void(pending_peer_end_e)> end;
 void start(std::chrono::steady_clock::time_point, std::function<void(pending_peer_end_e)> f) override {end=std::move(f);}
 void prepare(std::function<void(bool)> f) override {f(true);}
 bool connected() override {return true;}
 void stop() override {}
};
struct Response {bool close_connection_after_response=false;std::string body;void write(const std::string &s){body=s;}};
using args_t=std::map<std::string,std::string>;
struct Request {std::shared_ptr<Peer> peer=std::make_shared<Peer>();args_t args;args_t parse_query_string(){return args;}};
std::string get_arg(const args_t &args,const char *key,const char *fallback){auto found=args.find(key);return found==args.end()?std::string(fallback):found->second;}
namespace util {int from_view(const std::string &value){return std::stoi(value);}}

using resp_https_t=std::shared_ptr<Response>;
using req_https_t=std::shared_ptr<Request>;
using Handler=std::function<void(resp_https_t,req_https_t)>;
struct Identities {pending_peer_factory_t pending_peer(req_https_t r){return [r]{return r->peer;};}} tls_client_identities;
namespace thread_pool_util {
 struct ThreadPool {
  std::vector<std::function<void()>> tasks;
  template<class T> void push(T task){tasks.emplace_back(std::move(task));}
  void run_one(){auto task=std::move(tasks.front());tasks.erase(tasks.begin());task();}
 };
}
struct Server {std::map<std::string,std::map<std::string,Handler>> resource;};
namespace proc {struct Process {void running(){} int current_app_id(){return 0;}} proc;}
int lifecycle_acquisitions=0;
std::mutex lifecycle,launch_request_mutex;
std::unique_lock<std::mutex> acquire_stream_start_lifecycle_lock(){++lifecycle_acquisitions;return std::unique_lock{lifecycle};}
int launches=0,resumes=0,cancels=0;
void launch(int&,resp_https_t,req_https_t,int,std::uint64_t){++launches;}
void resume(int&,resp_https_t,req_https_t,int,bool,bool,std::uint64_t){++resumes;}
void cancel(resp_https_t,req_https_t){++cancels;}
#define BOOST_LOG(level) std::cerr
int failures=0;
void check(bool value,const char *label){std::cout<<(value?"PASS ":"FAIL ")<<label<<'\n';if(!value)++failures;}
int main(){
 thread_pool_util::ThreadPool blocking_route_pool;
 Server https_server;
 int host_audio=0;
 auto mutation_admission=std::make_shared<single_flight::admission_t>();
 auto linux_handoff=std::make_shared<handoff::admission_t>();
'''
checks = r'''
 auto &cancel_route=https_server.resource["^/cancel$"]["GET"];
 auto &launch_route=https_server.resource["^/launch$"]["GET"];
 auto &resume_route=https_server.resource["^/resume$"]["GET"];
 for(bool is_resume:{false,true}) {
  auto request=std::make_shared<Request>();auto response=std::make_shared<Response>();
  cancel_route(std::make_shared<Response>(),std::make_shared<Request>());
  (is_resume?resume_route:launch_route)(response,request);
  check(response->body.empty()&&blocking_route_pool.tasks.size()==1,"start waits behind the registered cancel route");
  auto duplicate=std::make_shared<Response>();
  launch_route(duplicate,std::make_shared<Request>());
  check(duplicate->body.find("503")!=std::string::npos,"only one registered start can wait");
  blocking_route_pool.run_one();
  check(blocking_route_pool.tasks.size()==1,"cancel completion schedules the waiting start");
  blocking_route_pool.run_one();
 }
 check(cancels==2&&launches==1&&resumes==1,"registered launch and resume run once after cancel");
 auto request=std::make_shared<Request>();auto response=std::make_shared<Response>();
 cancel_route(std::make_shared<Response>(),std::make_shared<Request>());
 launch_route(response,request);
 request->peer->end(pending_peer_end_e::disconnected);
 blocking_route_pool.run_one();
 check(launches==1&&blocking_route_pool.tasks.empty(),"disconnected registered request never starts later");
 const auto acquired=lifecycle_acquisitions;
 auto teardown=std::make_shared<Request>();teardown->args["appid"]=std::to_string(remote_session::disconnect_monitor_id);
 launch_route(std::make_shared<Response>(),teardown);
 blocking_route_pool.run_one();
 check(lifecycle_acquisitions==acquired,"downstream teardown controls keep their lifecycle-aware launch path");
 return failures?1:0;
}
'''
cpp = output / 'linux-nvhttp-handoff-route-composition.cpp'
cpp.write_text('#include \"src/remote_session.h\"\n#include <array>\nnamespace remote_session {\n' + identity_table + identity + '\n}\n' + support + '\n'.join(parts) + checks)
exe = output / 'linux-nvhttp-handoff-route-composition'
includes = []
for include in (args.json_include + ';' + args.boost_include).split(';'):
    if include:
        includes += ['-I', str(Path(include).resolve())]
subprocess.run([args.compiler, '-std=c++23', '-Wall', '-Wextra', '-Werror', '-pthread', '-I', str(root), *includes, str(cpp), '-o', str(exe)], check=True)
subprocess.run([str(exe)], check=True)
