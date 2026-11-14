#!/usr/bin/env python3
"""Exercise the production access toggle against controlled persistence edges."""
from pathlib import Path
import argparse
import subprocess
import tempfile
parser=argparse.ArgumentParser()
parser.add_argument('--compiler',default='c++')
parser.add_argument('--json-include',action='append',default=[])
args=parser.parse_args()
root=Path(__file__).resolve().parents[2]
s=(root/'src/nvhttp.cpp').read_text()
a=s.index('  bool set_client_enabled(')
b=s.index('  bool set_client_sleep_allowed(',a)
method=s[a:b]
code=r'''
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
using namespace std::literals;
#define CHECK(x) do { if (!(x)) {std::cerr << "failed: " #x << '\n';std::exit(1);} } while(false)
std::ostringstream logs;
#define BOOST_LOG(x) logs
namespace config { namespace flag { constexpr int FRESH_STATE=0; } struct {bool flags[1]{};} sunshine; }
namespace host_stream_control { struct activity_guard_t {operator bool()const {return true;}}; }
namespace statefile { std::mutex state; std::mutex &state_mutex(){return state;} void migrate_recent_state_keys(){} }
struct Client { std::string uuid; bool enabled=true; unsigned perm=4; };
struct { std::vector<std::shared_ptr<Client>> named_devices; } client_root;
std::mutex client_mutex;
std::atomic<bool> authorization_state_ready=true;
bool save_ok=true;
std::vector<Client> persisted;
auto save_state_snapshot_locked(const auto &root) {
 if(!save_ok) return false;
 persisted.clear();for(auto &p:root.named_devices)persisted.push_back(*p);return true;
}
std::vector<std::string> revoked;
void revoke_paired_client_access(std::string_view uuid) {
 CHECK(client_mutex.try_lock());client_mutex.unlock();
 CHECK(statefile::state.try_lock());statefile::state.unlock();
 revoked.emplace_back(uuid);
}
METHOD
int main() {
 auto first=std::make_shared<Client>(Client{"first",true,4});
 auto second=std::make_shared<Client>(Client{"second",true,8});
 client_root.named_devices={first,second};
 CHECK(set_client_enabled("first",false));
 CHECK(!first->enabled && first->perm==4 && second->enabled && second->perm==8);
 CHECK(!persisted[0].enabled && persisted[0].perm==4);
 *first=persisted[0]; // simulated reload of the persisted production mutation
 CHECK(set_client_enabled("first",true));
 CHECK(first->enabled && first->perm==4);
 save_ok=false;
 CHECK(!set_client_enabled("first",false));
 CHECK(!first->enabled && first->perm==4 && revoked.back()=="first");
 CHECK(!set_client_enabled("first",true));
 CHECK(!first->enabled && first->perm==4);
 save_ok=true;
 first->perm=0;
 CHECK(set_client_enabled("first",true));
 CHECK(first->enabled && first->perm==0);
 CHECK(!set_client_enabled("missing",false));
 authorization_state_ready=false;
 CHECK(!set_client_enabled("first",true));
 std::cout << "production client access mutation tests passed\n";
}
'''.replace('METHOD',method)
handler_source=(root/'src/confighttp.cpp').read_text()
a=handler_source.index('  void updateClient(')
b=handler_source.index('  void unpair(',a)
handler=handler_source[a:b]
api_edges=r'''
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <unordered_map>
namespace crypto {enum class PERM:unsigned {_no=0,_all=0x07ffffff};}
namespace util {template<class T> T get_non_string_json_value(const nlohmann::json &j,const std::string &key,T fallback) {return j.value(key,fallback);}}
int full_updates=0;
namespace nvhttp {
 using ::set_client_enabled;
 bool set_client_hdr_profile(const std::string&,const std::string&) {return true;}
 bool set_client_sleep_allowed(const std::string&,bool) {return true;}
 std::vector<int> extract_command_entries(const nlohmann::json&,const std::string&) {return {};}
 template<class... T> bool update_device_info(T&&...) {++full_updates;return true;}
}
struct Response {nlohmann::json output;bool bad=false;};
struct Request {std::stringstream content;};
using resp_https_t=Response*;using req_https_t=Request*;
bool validateContentType(Response*,Request*,const char*) {return true;}
bool authenticate(Response*,Request*) {return true;}
void print_req(Request*) {}
void send_response(Response*r,const nlohmann::json &out) {r->output=out;}
void bad_request(Response*r,Request*,const char*) {r->bad=true;}
HANDLER
Response request(const nlohmann::json &body) {
 Response result;Request input;input.content<<body.dump();updateClient(&result,&input);return result;
}
void test_api() {
 auto first=std::make_shared<Client>(Client{"first",true,4});
 auto second=std::make_shared<Client>(Client{"second",true,8});
 client_root.named_devices={first,second};
 auto response=request({{"uuid","first"},{"enabled",false}});
 CHECK(!response.bad && response.output["status"]==true && response.output["enabled_updated"]==true);
 CHECK(!first->enabled && first->perm==4 && second->enabled && full_updates==0);
 response=request({{"uuid","first"},{"enabled",true}});
 CHECK(response.output["status"]==true && first->enabled && first->perm==4 && full_updates==0);
 response=request({{"uuid","first"},{"enabled","false"}});
 CHECK(response.bad && first->enabled && full_updates==0);
 response=request({{"uuid","first"},{"enabled",false},{"hdr_profile","profile"},{"allow_sleep",false}});
 CHECK(response.output["status"]==true && !first->enabled && first->perm==4 && full_updates==0);
 response=request({{"uuid","first"},{"enabled",true},{"name","Edited name"},{"perm",4}});
 CHECK(response.output["status"]==true && response.output["enabled_updated"]==true && first->enabled && full_updates==1);
 response=request({{"uuid","missing"},{"enabled",false}});
 CHECK(response.output["status"]==false);
 std::cout<<"production client access HTTP handler tests passed\n";
}
'''.replace('HANDLER',handler)
api_code=code[:code.index('int main()')]+api_edges+'int main(){test_api();}\n'
with tempfile.TemporaryDirectory(prefix='client-enabled-') as tmp:
 p=Path(tmp);(p/'test.cpp').write_text(code)
 subprocess.run([args.compiler,'-std=c++23','-DNDEBUG','-O2','-Wall','-Wextra','-Werror',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)

with tempfile.TemporaryDirectory(prefix='client-enabled-api-') as tmp:
 p=Path(tmp);(p/'test.cpp').write_text(api_code)
 includes=[flag for group in args.json_include for directory in group.split(';') if directory for flag in ['-I',directory]]
 subprocess.run([args.compiler,'-std=c++23','-DNDEBUG','-O2','-Wall','-Wextra','-Werror',*includes,str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
