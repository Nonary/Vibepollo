#!/usr/bin/env python3
"""Execute production route-registration fragments with fake socket/executor edges.

This is extraction for compilation, not a source-presence assertion. The emitted
translation unit executes unchanged registration/dispatch code from nvhttp.cpp
and confighttp.cpp. It cannot validate Windows APIs or real stream teardown.
"""
import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[2])
parser.add_argument('--build-dir', type=Path, required=True)
parser.add_argument('--compiler', default='g++')
parser.add_argument('--json-include', default='')
parser.add_argument('--boost-include', default='')
parser.add_argument('--without-rtsp', action='store_true', help='Use only for an older baseline lacking the revised RTSP adapter')
args = parser.parse_args()
root = args.source.resolve()
build = args.build_dir.resolve()
build.mkdir(parents=True, exist_ok=True)
nv = (root / 'src/nvhttp.cpp').read_text()
web = (root / 'src/confighttp.cpp').read_text()
rtsp = (root / 'src/rtsp.cpp').read_text()


def statement(text, start):
    begin = text.index(start)
    end = text.index('\n    };', begin) + len('\n    };')
    return text[begin:end]


def method(text, start):
    begin = text.index(start)
    end = text.index('\n    }', begin) + len('\n    }')
    return text[begin:end]


rtsp_methods = [method(rtsp, '    template<class Function>\n    void post('),
                method(rtsp, '    template<class Function>\n    void run_startup('),
                method(rtsp, '    int startup_count() const'),
                method(rtsp, '    int total_startup_count() const')] if not args.without_rtsp else []

nv_parts = [statement(nv, '    auto run_on_blocking_pool ='),
            statement(nv, '    auto run_blocking_nvhttp ='),
            statement(nv, '    https_server.resource["^/launch$"'),
            statement(nv, '    https_server.resource["^/resume$"'),
            statement(nv, '    https_server.resource["^/cancel$"')]
web_adapter = statement(web, '    auto register_blocking_api_route =')
registration_start = web.index('    register_blocking_api_route("^/api/webrtc/sessions$"')
web_registration = web[registration_start:web.index(';', registration_start) + 1]
service_start = web.index('  void service_unavailable(')
service_end = web.index('\n  }', service_start) + len('\n  }')
service_response = web[service_start:service_end]

support = r'''
#include "src/host_stream_control.h"
#include "src/nvhttp_dispatch.h"
#include <functional>
#include <optional>
#include <array>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
namespace SimpleWeb {
  enum class StatusCode { server_error_service_unavailable = 503 };
  using CaseInsensitiveMultimap = std::multimap<std::string, std::string>;
}
struct Response {
  bool close_connection_after_response = false;
  int status = 0;
  std::string body;
  void write(const std::string &value) { body = value; }
  void write(SimpleWeb::StatusCode code, const std::string &value, const SimpleWeb::CaseInsensitiveMultimap &) {
    status = static_cast<int>(code); body = value;
  }
};
struct Request { bool authenticated = true;std::map<std::string,std::string> parse_query_string(){return {};} };
namespace remote_session {enum class control_e {none,disconnect_monitor,disconnect_input,terminate};control_e identify(int,const std::string &){return control_e::none;}}
namespace util {int from_view(const std::string &){return 0;}}
std::string get_arg(const std::map<std::string,std::string> &,const char *,const char *fallback){return fallback;}
using resp_https_t = std::shared_ptr<Response>;
using req_https_t = std::shared_ptr<Request>;
using Handler = std::function<void(resp_https_t, req_https_t)>;
namespace thread_pool_util {
  struct ThreadPool {
    std::vector<std::function<void()>> tasks;
    template<class Task> void push(Task task) { tasks.emplace_back(std::move(task)); }
    void run() { auto ready = std::move(tasks); tasks.clear(); for (auto &task : ready) task(); }
  };
}
struct Server { std::map<std::string, std::map<std::string, Handler>> resource; };
namespace proc {
  struct Process { void running() {} int current_app_id() { return 0; } } proc;
}
std::mutex lifecycle,launch_request_mutex;
std::unique_lock<std::mutex> acquire_stream_start_lifecycle_lock() { return std::unique_lock{lifecycle}; }
int launches = 0, resumes = 0, web_sessions = 0,cancels = 0;
std::uint64_t last_launch_trace_id = 0, last_resume_trace_id = 0;
void launch(int &, resp_https_t, req_https_t, int, std::uint64_t trace_id = 0) { ++launches; last_launch_trace_id = trace_id; }
void resume(int &, resp_https_t, req_https_t, int, bool = true, bool = false, std::uint64_t trace_id = 0) { ++resumes; last_resume_trace_id = trace_id; }
void cancel(resp_https_t,req_https_t){++cancels;}
void createWebRTCSession(resp_https_t, req_https_t) { ++web_sessions; }
bool authenticate(resp_https_t response, req_https_t request) {
  if (!request->authenticated) response->status = 401;
  return request->authenticated;
}
void bad_request(resp_https_t response, req_https_t, const char *) { response->status = 400; }
void add_cors_headers(SimpleWeb::CaseInsensitiveMultimap &) {}
#define BOOST_LOG(level) std::cerr
#define _WIN32
'''
checks = r'''
int failures = 0;
void check(bool ok, const char *message) {
  std::cout << (ok ? "PASS " : "FAIL ") << message << '\n';
  if (!ok) ++failures;
}
void nv_routes() {
  namespace dispatch = nvhttp::dispatch;
  thread_pool_util::ThreadPool blocking_route_pool;
  Server https_server;
  int host_audio = 0;
  auto mutation_admission = std::make_shared<single_flight::admission_t>();
'''
nv_checks = r'''
  for (const auto *operation : {"launch", "resume", "cancel"}) {
    const int before = launches + resumes + cancels;
    auto &handler = https_server.resource[std::string("^/") + operation + "$"]["GET"];
    auto response = std::make_shared<Response>();
    handler(response, std::make_shared<Request>());
    auto busy = std::make_shared<Response>();
    handler(busy, std::make_shared<Request>());
    check(busy->body.find("503") != std::string::npos, "real NVHTTP route keeps queued single-flight busy rejection");
    check(host_stream_control::terminate([] { return true; }, [] {}), "authorized terminal composition executes");
    blocking_route_pool.run();
    check(launches + resumes + cancels == before, "real launch/resume route rejects pre-terminal accepted queue");
    check(response->body.find("503") != std::string::npos, "real launch/resume cancellation writes 503 XML");
  }
  const int before_fresh = launches;
  auto &fresh = https_server.resource["^/launch$"]["GET"];
  fresh(std::make_shared<Response>(), std::make_shared<Request>());
  blocking_route_pool.run();
  check(launches == before_fresh + 1, "real NVHTTP route accepts fresh post-terminal request");
  check(last_launch_trace_id != 0 && !dispatch::include_request_identity_log_fields(last_launch_trace_id),
        "real launch route supplies a redacted request-detail logging context");
  const int before_fresh_resume = resumes;
  auto &fresh_resume = https_server.resource["^/resume$"]["GET"];
  fresh_resume(std::make_shared<Response>(), std::make_shared<Request>());
  blocking_route_pool.run();
  check(resumes == before_fresh_resume + 1 && last_resume_trace_id != 0 &&
        !dispatch::include_request_identity_log_fields(last_resume_trace_id),
        "real resume route supplies a redacted request-detail logging context");
}
void web_routes() {
  thread_pool_util::ThreadPool blocking_route_pool;
  std::map<std::string, Handler> routes;
  auto register_api_route = [&](const char *pattern, const char *, auto handler, bool stream_start = false) { routes[pattern] = [handler,stream_start](resp_https_t r,req_https_t q) { handler(r,q,stream_start ? host_stream_control::request_ticket() : 0); }; };
'''
web_checks = r'''
  auto &handler = routes["^/api/webrtc/sessions$"];
  auto denied = std::make_shared<Request>(); denied->authenticated = false;
  auto denied_response = std::make_shared<Response>();
  handler(denied_response, denied);
  check(denied_response->status == 401 && blocking_route_pool.tasks.empty(), "real blocking WebRTC adapter retains authentication before queue");
  auto old = std::make_shared<Response>();
  handler(old, std::make_shared<Request>());
  host_stream_control::terminate([] { return true; }, [] {});
  blocking_route_pool.run();
  check(web_sessions == 0, "real WebRTC route rejects pre-terminal accepted queue");
  check(old->status == 503 && old->body.find("canceled") != std::string::npos, "real WebRTC service_unavailable emits cancellation JSON");
  const int before_during = web_sessions;
  auto during = std::make_shared<Response>();
  host_stream_control::terminate([] { return true; }, [&] { handler(during, std::make_shared<Request>()); });
  blocking_route_pool.run();
  check(web_sessions == before_during && during->status == 503, "real WebRTC route rejects intake during final restore");
  const int before_fresh = web_sessions;
  handler(std::make_shared<Response>(), std::make_shared<Request>());
  blocking_route_pool.run();
  check(web_sessions == before_fresh + 1, "real WebRTC route accepts fresh post-terminal request");
}

'''
rtsp_support = r'''
namespace boost::asio {
  template<class Task> void post(thread_pool_util::ThreadPool &queue, Task task) { queue.push(std::move(task)); }
}
struct RtspAdapter {
  thread_pool_util::ThreadPool startup_pool, io_context;
  host_stream_control::startup_dispatch_t startup_tasks;
  std::atomic_bool stopping {false};
  int finalizers = 0;
  void finish_startup(std::optional<std::array<std::uint8_t,16>>) { check(startup_count() == 0, "actual RTSP worker retires runtime count before finalizing"); ++finalizers; }
'''
rtsp_checks = r'''
};
void rtsp_routes() {
  RtspAdapter server;
  int starts = 0, responses = 0;
  server.run_startup(std::nullopt,[&](bool admitted) {
    if (admitted) ++starts;
    server.post([&] { ++responses; });
  });
  check(server.startup_count() == 1, "actual RTSP run_startup publishes queued runtime owner");
  host_stream_control::terminate([] { return true; }, [&] {
    check(server.startup_count() == 0, "actual RTSP owner lookup retires old queued epoch before final restore");
  });
  server.startup_pool.run();
  check(starts == 0 && server.finalizers == 0, "actual RTSP adapter rejects canceled worker without late ordinary finalizer");
  check(server.total_startup_count() == 1, "actual RTSP socket completion preserves shutdown draining");
  server.io_context.run();
  check(responses == 1 && server.total_startup_count() == 0, "actual RTSP canceled socket completion drains without runtime work");
  server.run_startup(std::nullopt,[&](bool admitted) {
    if (admitted) ++starts;
    server.post([&] { ++responses; });
  });
  server.startup_pool.run();
  check(starts == 1 && server.finalizers == 1 && responses == 1, "actual RTSP successful worker finalizes before delayed socket completion");
  host_stream_control::terminate([] { return true; }, [] {});
  server.io_context.run();
  check(server.finalizers == 1 && responses == 2, "actual RTSP delayed socket callback cannot finalize after terminal restore");
}
'''
translation = support + service_response + checks + '\n'.join(nv_parts) + nv_checks + web_adapter + '\n' + web_registration + web_checks
if rtsp_methods:
    translation += rtsp_support + '\n'.join(rtsp_methods) + rtsp_checks
translation += 'int main() { nv_routes(); web_routes(); ' + ('rtsp_routes(); ' if rtsp_methods else '') + 'return failures == 0 ? 0 : 1; }\n'

cpp = build / 'production-route-composition.cpp'
cpp.write_text(translation)
exe = build / 'production-route-composition'
includes = []
for include in (args.json_include + ';' + args.boost_include).split(';'):
    if include:
        includes += ['-I', str(Path(include).resolve())]
subprocess.run([args.compiler, '-std=c++23', '-Wall', '-Wextra', '-Werror', '-pthread', '-I', str(root), *includes, str(cpp), '-o', str(exe)], check=True)
subprocess.run([str(exe)], check=True)
