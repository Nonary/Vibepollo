#!/usr/bin/env python3
"""Execute complete production GET /api/apps with controlled HTTP/file/cache leaves.

Extracts the complete getApps body, its actual route registration, builtin helpers,
and the product process environment/identity validation bodies. It does not start
TLS/the HTTP server or link the full process parser. Runtime snapshots and refresh
are controlled leaves. The same fixture can point at an older source tree.
"""
from pathlib import Path
import argparse
import subprocess
import sys
import tempfile

PRELUDE = r'''
#include "src/boost_process_shim.h"
#include "src/remote_monitor_profile_policy.h"
#include "src/utility.h"
#include "src/uuid.h"
#include <boost/property_tree/json_parser.hpp>
#include <boost/algorithm/string.hpp>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
namespace bp = boost_process_shim;
namespace pt = boost::property_tree;
namespace fs = std::filesystem;
using namespace std::literals;
using json = nlohmann::json;
std::ostringstream fixture_log;
#define BOOST_LOG(level) fixture_log
#define CHECK(expression) do { if (!(expression)) throw std::runtime_error(std::string("failed: ")+ #expression + " line " + std::to_string(__LINE__)); } while(false)
std::string fixture_disk_text;
json fixture_response;
std::vector<json> fixture_writes;
std::vector<std::string> fixture_events;
int fixture_http=0,fixture_refreshes=0;
std::string fixture_cache_control;
bool fixture_authorized=true;
struct request_t {std::stringstream content;};
using resp_https_t=int;
using req_https_t=std::shared_ptr<request_t>;
namespace config {struct stream_t {std::string file_apps="controlled-apps.json";} stream;
struct nvhttp_t {std::string sunshine_name="controlled-host";} nvhttp;}
namespace http {const std::string unique_id="controlled-host-uuid";}
namespace file_handler {
std::string read_file(const char*) { fixture_events.push_back("read"); return fixture_disk_text; }
void write_file(const char*,const std::string& text) { fixture_events.push_back("write"); fixture_writes.push_back(json::parse(text)); fixture_disk_text=text; }
}
namespace proc {
struct ctx_t {std::optional<size_t> configured_index; bool remote_monitor_profile{};std::string uuid,id;};
std::vector<ctx_t> fixture_after_refresh;
struct process_t {
 std::vector<ctx_t> apps;
 auto get_apps() const {fixture_events.push_back("snapshot");return apps;}
 std::string get_running_app_uuid() const {return "controlled-running-app";}
} proc;
void refresh(const std::string&,bool=false) {fixture_events.push_back("refresh");++fixture_refreshes;proc.apps=fixture_after_refresh;}
}
namespace confighttp {
std::recursive_mutex& apps_file_mutex(){static std::recursive_mutex lock;return lock;}
bool check_content_type(resp_https_t,req_https_t,const char*){return true;}
bool validateContentType(resp_https_t,req_https_t,const char*){return true;}
void normalize_adapter_config_pair(json&){}
bool refresh_client_apps_cache(json& value,bool=true){file_handler::write_file(config::stream.file_apps.c_str(),value.dump(4));proc::refresh(config::stream.file_apps);return true;}
bool authenticate(resp_https_t,req_https_t){if(!fixture_authorized)fixture_http=401;return fixture_authorized;}
void print_req(req_https_t){}
void bad_request(resp_https_t,req_https_t,const std::string& error){fixture_events.push_back("error");fixture_http=400;fixture_response={{"status",false},{"error",error}};}
void send_response(resp_https_t,const json& response,std::string_view cache_control={}){fixture_events.push_back("response");fixture_http=200;fixture_response=response;fixture_cache_control=cache_control;}
// Artwork I/O is outside the identity/publication boundary under test.
void attach_configured_cover_versions(json& apps){for(auto& app:apps)app["cover-version"]="controlled-cover-version";}
}
'''
TESTS = r'''
const std::string profile_uuid="a1234567-1234-4234-8234-123456789abc";
json good_profile(){return {{"name","Profile"},{"uuid",profile_uuid},{"remote-monitor-profile",true}};}
void reset(const std::string& raw){
 fixture_disk_text=raw;fixture_response={};fixture_writes.clear();fixture_events.clear();fixture_http=0;fixture_refreshes=0;fixture_cache_control.clear();fixture_authorized=true;proc::proc.apps.clear();proc::fixture_after_refresh.clear();
}
void get(){confighttp::fixture_routes.at({"^/api/apps$","GET"})(0,std::make_shared<request_t>());}
void no_transient_fields(const json& disk){
 CHECK(!disk.contains("current_app"));CHECK(!disk.contains("host_uuid"));CHECK(!disk.contains("host_name"));
 for(const auto& row:disk.at("apps")){
  CHECK(!row.contains("index"));CHECK(!row.contains("id"));CHECK(!row.contains("cover-version"));CHECK(!row.contains("playnite-icon-version"));CHECK(!row.contains("remote-session"));
 }
}
void successful_list(){
 CHECK(fixture_http==200);CHECK(fixture_response.at("apps").is_array());CHECK(fixture_cache_control=="no-store");
 if(!source_uuid_expansion){CHECK(fixture_response["current_app"]=="controlled-running-app");CHECK(fixture_response["host_uuid"]==http::unique_id);CHECK(fixture_response["host_name"]==config::nvhttp.sunshine_name);}
}
void invalid_missing_builtins(){
 const std::vector<json> invalids{
  {{"name","Missing UUID"},{"remote-monitor-profile",true}},
  {{"name","Null UUID"},{"remote-monitor-profile",true},{"uuid",nullptr}},
  {{"name","Malformed UUID"},{"remote-monitor-profile",true},{"uuid","not-a-uuid"}},
  {{"name","Malformed expression"},{"remote-monitor-profile",true},{"uuid","$(BROKEN"}},
  {{"name","Reserved UUID"},{"remote-monitor-profile",true},{"uuid",remote_session::synthetic_uuid(remote_session::control_e::monitor)}}
 };
 for(const auto& invalid:invalids){
  const json normal={{"name","Normal"},{"uuid","normal"}};
  const json before={{"apps",json::array({invalid,normal,normal,good_profile()})}};
  reset(before.dump());
  proc::proc.apps={{0,false,"stale","old0"},{1,false,"normal","old1"},{2,false,"normal","old2"},{3,true,profile_uuid,"old3"}};
  proc::fixture_after_refresh={{0,false,"stale","bad-id"},{1,false,"normal","101"},{2,false,"normal","102"},{3,true,profile_uuid,"103"}};
  get();successful_list();CHECK(fixture_writes.size()==1);CHECK(fixture_refreshes==1);
  const auto disk=json::parse(fixture_disk_text);
  CHECK(disk["apps"].size()==6);CHECK(disk["apps"][0]==invalid);no_transient_fields(disk);
  CHECK(fixture_response["apps"][0]["index"]==0);CHECK(!fixture_response["apps"][0].contains("id"));
  CHECK(fixture_response["apps"][1]["id"]=="101");CHECK(fixture_response["apps"][2]["id"]=="102");CHECK(fixture_response["apps"][3]["id"]=="103");
  CHECK(fixture_events==std::vector<std::string>({"read","write","refresh","snapshot","response"}));
  get();successful_list();CHECK(fixture_writes.size()==1);CHECK(json::parse(fixture_disk_text)==disk);CHECK(!fixture_response["apps"][0].contains("id"));
 }
}
void invalid_ordinary_normalization(){
 json invalid={{"name","Invalid marked"},{"uuid","not-a-uuid"},{"remote-monitor-profile",true}};
 json tree={{"apps",json::array({invalid,{{"name","Normal"},{"uuid","ordinary"},{"auto-detach","true"},{"exit-timeout","5"}}})}};
 confighttp::ensure_remote_session_apps(tree);
 reset(tree.dump());proc::fixture_after_refresh={{1,false,"ordinary","104"}};
 get();successful_list();CHECK(fixture_writes.size()==1);CHECK(fixture_refreshes==1);
 const auto disk=json::parse(fixture_disk_text);CHECK(disk["apps"][0]==invalid);CHECK(disk["apps"][1]["auto-detach"]==true);CHECK(disk["apps"][1]["exit-timeout"]==5);no_transient_fields(disk);
 CHECK(fixture_response["apps"][0]["index"]==0);CHECK(!fixture_response["apps"][0].contains("id"));CHECK(fixture_response["apps"][1]["id"]=="104");
}

json without_response_metadata(json app){app.erase("index");app.erase("id");app.erase("cover-version");app.erase("playnite-icon-version");app.erase("remote-session");return app;}
void invalid_own_legacy_fields(){
 const json legacy={{"auto-detach","true"},{"elevated","false"},{"exit-timeout","5"},
  {"prep-cmd",json::array({{{"do","prepare"},{"elevated","true"}}})},
  {"lossless-scaling-recommended",{{"performance-mode","1"},{"flow-scale","2"},{"scaling-type","LS1"},{"anime4k-size","l"}}},
  {"lossless-scaling-custom",{{"performance-mode","0"},{"flow-scale","3"},{"scaling-type","LS1"},{"anime4k-size","l"}}}};
 std::vector<json> invalids;
 for(const json uuid:{json(nullptr),json(false),json(12),json::array(),json::object(),json(""),json("not-a-uuid"),json("$(BROKEN"),json("00000000-0000-0000-0000-000000000000"),json(remote_session::synthetic_uuid(remote_session::control_e::monitor))}){
  auto invalid=legacy;invalid["name"]="Invalid own legacy fields";invalid["uuid"]=uuid;invalid["remote-monitor-profile"]=true;invalids.push_back(invalid);
 }
 auto missing=legacy;missing["name"]="Missing UUID";missing["remote-monitor-profile"]=true;invalids.push_back(missing);
 for(auto& invalid:invalids)if(!source_uuid_expansion){invalid["state-cmd"]=json::array({{{"do","state"},{"elevated","true"}}});invalid["virtual-display"]="true";invalid["scale-factor"]="150";}
 for(bool normalize_other:{false,true})for(const auto& invalid:invalids){
  json ordinary={{"name","Ordinary"},{"uuid","ordinary"}};if(normalize_other){ordinary["auto-detach"]="true";ordinary["exit-timeout"]="7";}
  json tree={{"apps",json::array({invalid,ordinary,good_profile()})}};confighttp::ensure_remote_session_apps(tree);
  const auto raw=tree.dump();reset(raw);
  proc::proc.apps={{0,true,"stale","incorrect-invalid-id"},{1,false,"ordinary","501"},{2,true,profile_uuid,"502"}};
  proc::fixture_after_refresh=proc::proc.apps;
  get();successful_list();CHECK(fixture_writes.size()==(normalize_other?1:0));CHECK(fixture_refreshes==(normalize_other?1:0));
  auto disk=json::parse(fixture_disk_text);CHECK(disk["apps"][0]==invalid);no_transient_fields(disk);
  if(!normalize_other)CHECK(fixture_disk_text==raw);
  CHECK(fixture_response["apps"][0]["index"]==0);CHECK(!fixture_response["apps"][0].contains("id"));
  CHECK(without_response_metadata(fixture_response["apps"][0])==invalid);
  CHECK(fixture_response["apps"][1]["id"]=="501");CHECK(fixture_response["apps"][2]["id"]=="502");
  if(normalize_other){CHECK(disk["apps"][1]["auto-detach"]==true);CHECK(disk["apps"][1]["exit-timeout"]==7);}
  get();successful_list();CHECK(fixture_writes.size()==(normalize_other?1:0));CHECK(json::parse(fixture_disk_text)==disk);
 }
 // Duplicate marked identities also preserve their own fields, including an
 // integer string that would throw if GET attempted the legacy conversion.
 auto duplicate=legacy;duplicate["name"]="Duplicate";duplicate["uuid"]=profile_uuid;duplicate["remote-monitor-profile"]=true;duplicate["exit-timeout"]="invalid integer";
 json tree={{"apps",json::array({duplicate,good_profile()})}};confighttp::ensure_remote_session_apps(tree);
 const auto raw=tree.dump();reset(raw);get();successful_list();CHECK(fixture_writes.empty());CHECK(fixture_refreshes==0);CHECK(fixture_disk_text==raw);
 CHECK(without_response_metadata(fixture_response["apps"][0])==duplicate);CHECK(!fixture_response["apps"][0].contains("id"));CHECK(!fixture_response["apps"][1].contains("id"));
}
void valid_profile_legacy_normalization(){
 auto valid=good_profile();valid["auto-detach"]="true";valid["exit-timeout"]="6";valid["prep-cmd"]=json::array({{{"do","prepare"},{"elevated","false"}}});
 if(!source_uuid_expansion){valid["state-cmd"]=json::array({{{"do","state"},{"elevated","true"}}});valid["virtual-display"]="true";valid["scale-factor"]="150";}
 json tree={{"apps",json::array({valid})}};confighttp::ensure_remote_session_apps(tree);reset(tree.dump());proc::fixture_after_refresh={{0,true,profile_uuid,"503"}};
 get();successful_list();CHECK(fixture_writes.size()==1);CHECK(fixture_refreshes==1);const auto disk=json::parse(fixture_disk_text);no_transient_fields(disk);
 CHECK(disk["apps"][0]["uuid"]==profile_uuid);CHECK(disk["apps"][0]["remote-monitor-profile"]==true);CHECK(disk["apps"][0]["auto-detach"]==true);CHECK(disk["apps"][0]["exit-timeout"]==6);CHECK(disk["apps"][0]["prep-cmd"][0]["elevated"]==false);CHECK(fixture_response["apps"][0]["id"]=="503");
 if(!source_uuid_expansion){CHECK(disk["apps"][0]["state-cmd"][0]["elevated"]==true);CHECK(disk["apps"][0]["virtual-display"]==true);CHECK(disk["apps"][0]["scale-factor"]==150);}
}
void target_save_repair_probe(){
 json invalid={{"name","Invalid marked"},{"uuid","not-a-uuid"},{"remote-monitor-profile",true},{"auto-detach","true"},{"exit-timeout","5"},{"prep-cmd",json::array({{{"do","prepare"},{"elevated","true"}}})}};
 json tree={{"apps",json::array({invalid})}};confighttp::ensure_remote_session_apps(tree);reset(tree.dump());
 get();successful_list();auto repair=fixture_response["apps"][0];repair.erase("cover-version");repair["uuid"]="c1234567-1234-4234-8234-123456789abc";
 auto request=std::make_shared<request_t>();request->content<<repair.dump();confighttp::saveApp(0,request);
 std::cout<<"TARGET_REPAIR_PROBE "<<json{{"http",fixture_http},{"response",fixture_response},{"disk",json::parse(fixture_disk_text)},{"writes",fixture_writes.size()},{"refreshes",fixture_refreshes}}.dump()<<'\n';
 CHECK(fixture_http==200);CHECK(json::parse(fixture_disk_text)["apps"][0]["uuid"]==repair["uuid"]);
 CHECK(json::parse(fixture_disk_text)["apps"].size()==3);no_transient_fields(json::parse(fixture_disk_text));
}

void ordinary_normalization(){
 json ordinary={{"name","Normal"},{"uuid","ordinary"},{"auto-detach","true"},{"elevated","false"},{"exit-timeout","8"},{"prep-cmd",json::array({{{"do","true"},{"elevated","true"}}})},{"lossless-scaling-custom",{{"performance-mode","1"},{"flow-scale","2"},{"scaling-type","LS1"},{"anime4k-size","l"}}}};
 json tree={{"apps",json::array({good_profile(),ordinary,{{"name","Needs UUID"}}})}};
 confighttp::ensure_remote_session_apps(tree);
 reset(tree.dump());proc::fixture_after_refresh={{0,true,profile_uuid,"201"},{1,false,"ordinary","202"}};
 get();successful_list();CHECK(fixture_writes.size()==1);CHECK(fixture_refreshes==1);
 const auto disk=json::parse(fixture_disk_text);no_transient_fields(disk);CHECK(disk["apps"][0]==good_profile());
 CHECK(disk["apps"][1]["auto-detach"]==true);CHECK(disk["apps"][1]["elevated"]==false);CHECK(disk["apps"][1]["exit-timeout"]==8);CHECK(disk["apps"][1]["prep-cmd"][0]["elevated"]==true);
 CHECK(disk["apps"][1]["lossless-scaling-custom"]["performance-mode"]==true);CHECK(disk["apps"][1]["lossless-scaling-custom"]["flow-scale"]==2);CHECK(disk["apps"][1]["lossless-scaling-custom"]["scaling-type"]=="ls1");CHECK(disk["apps"][1]["lossless-scaling-custom"]["anime4k-size"]=="L");
 CHECK(!disk["apps"][2]["uuid"].get<std::string>().empty());CHECK(fixture_response["apps"][0]["id"]=="201");CHECK(fixture_response["apps"][1]["id"]=="202");CHECK(fixture_response["apps"][2]["index"]==2);
}
void illegal_environment_order(){
 if(!source_uuid_expansion)return;
 setenv("V6_ORDER_Z",profile_uuid.c_str(),1);
 const std::string raw=R"v6({"env":{"V6_ORDER_Z":"b1234567-1234-4234-8234-123456789abc","V6_ORDER_A":"$(V6_ORDER_Z)"},"apps":[{"name":"Profile","uuid":"a1234567-1234-4234-8234-123456789abc","remote-monitor-profile":true},{"name":"Ordinary","uuid":"$(V6_ORDER_A)"}]})v6";
 const auto before=json::parse(raw);
 CHECK(proc::configured_monitor_profile_error_from_file(before["apps"][0],before,raw).empty());
 reset(raw);get();CHECK(fixture_http==400);CHECK(fixture_writes.empty());CHECK(fixture_refreshes==0);CHECK(fixture_disk_text==raw);CHECK(!fixture_response["error"].get<std::string>().empty());CHECK(fixture_events==std::vector<std::string>({"read","error"}));
 unsetenv("V6_ORDER_Z");
}
void invalid_original_environment_order(){
 if(!source_uuid_expansion)return;
 setenv("V7_ORDER_Z","b1234567-1234-4234-8234-123456789abc",1);
 auto invalid=good_profile();invalid["auto-detach"]="true";invalid["exit-timeout"]="5";
 json tree={{"apps",json::array({invalid,{{"name","Ordinary"},{"uuid","$(V7_ORDER_A)"}}})}};confighttp::ensure_remote_session_apps(tree);
 const auto raw=std::string{R"v7({"env":{"V7_ORDER_Z":"a1234567-1234-4234-8234-123456789abc","V7_ORDER_A":"$(V7_ORDER_Z)"},"apps":)v7"}+tree["apps"].dump()+"}";
 const auto before=json::parse(raw);
 CHECK(!proc::configured_monitor_profile_error_from_file(before["apps"][0],before,raw).empty());
 CHECK(proc::configured_monitor_profile_error(before["apps"][0],before).empty());
 reset(raw);proc::proc.apps={{0,true,profile_uuid,"wrong-invalid-id"}};
 get();successful_list();CHECK(fixture_writes.empty());CHECK(fixture_refreshes==0);CHECK(fixture_disk_text==raw);CHECK(!fixture_response["apps"][0].contains("id"));
 CHECK(without_response_metadata(fixture_response["apps"][0])==invalid);
 unsetenv("V7_ORDER_Z");
}
void no_normalization_and_stale_snapshot(){
 json tree={{"apps",json::array({good_profile(),{{"name","Duplicate"},{"uuid","ordinary"}},{{"name","Duplicate"},{"uuid","ordinary"}}})}};
 confighttp::ensure_remote_session_apps(tree);
 const auto raw=tree.dump();reset(raw);proc::proc.apps={{0,false,profile_uuid,"wrong-profile"},{1,false,"ordinary","301"},{2,false,"ordinary","302"}};
 get();successful_list();CHECK(fixture_writes.empty());CHECK(fixture_refreshes==0);CHECK(fixture_disk_text==raw);CHECK(!fixture_response["apps"][0].contains("id"));CHECK(fixture_response["apps"][1]["id"]=="301");CHECK(fixture_response["apps"][2]["id"]=="302");no_transient_fields(json::parse(fixture_disk_text));
 proc::proc.apps[0]={0,true,profile_uuid,"303"};get();CHECK(fixture_response["apps"][0]["id"]=="303");CHECK(fixture_writes.empty());
 proc::proc.apps[0].uuid="b1234567-1234-4234-8234-123456789abc";get();CHECK(!fixture_response["apps"][0].contains("id"));
}
void get_then_repair(){
 if(!source_uuid_expansion)return;
 json invalid={{"name","Invalid marked"},{"uuid","not-a-uuid"},{"remote-monitor-profile",true}};
 reset(json{{"apps",json::array({invalid})}}.dump());
 get();successful_list();CHECK(fixture_writes.size()==1);
 auto repaired=fixture_response["apps"][0];repaired.erase("cover-version");CHECK(repaired["index"]==0);CHECK(!repaired.contains("id"));
 const std::string repaired_uuid="c1234567-1234-4234-8234-123456789abc";repaired["uuid"]=repaired_uuid;
 auto request=std::make_shared<request_t>();request->content<<repaired.dump();
 proc::fixture_after_refresh={{0,true,repaired_uuid,"401"}};
 confighttp::saveApp(0,request);CHECK(fixture_http==200);CHECK(fixture_response["status"]==true);CHECK(fixture_writes.size()==2);
 const auto disk=json::parse(fixture_disk_text);CHECK(disk["apps"].size()==3);CHECK(disk["apps"][0]["uuid"]==repaired_uuid);CHECK(disk["apps"][0]["remote-monitor-profile"]==true);no_transient_fields(disk);
 get();successful_list();CHECK(fixture_writes.size()==2);CHECK(fixture_response["apps"][0]["id"]=="401");
}
void legacy_get_then_repair(){
 if(!source_uuid_expansion)return;
 json invalid={{"name","Invalid marked"},{"uuid","not-a-uuid"},{"remote-monitor-profile",true},{"auto-detach","true"},{"exit-timeout","5"},{"prep-cmd",json::array({{{"do","prepare"},{"elevated","true"}}})}};
 reset(json{{"apps",json::array({invalid})}}.dump());
 get();successful_list();CHECK(fixture_writes.size()==1);
 auto repaired=fixture_response["apps"][0];repaired.erase("cover-version");CHECK(repaired["index"]==0);CHECK(!repaired.contains("id"));
 const std::string repaired_uuid="c1234567-1234-4234-8234-123456789abc";repaired["uuid"]=repaired_uuid;
 auto request=std::make_shared<request_t>();request->content<<repaired.dump();
 proc::fixture_after_refresh={{0,true,repaired_uuid,"401"}};
 confighttp::saveApp(0,request);CHECK(fixture_http==200);CHECK(fixture_response["status"]==true);CHECK(fixture_writes.size()==2);
 const auto disk=json::parse(fixture_disk_text);CHECK(disk["apps"].size()==3);CHECK(disk["apps"][0]["uuid"]==repaired_uuid);CHECK(disk["apps"][0]["remote-monitor-profile"]==true);no_transient_fields(disk);
 get();successful_list();CHECK(fixture_writes.size()==3);CHECK(fixture_refreshes==3);CHECK(fixture_response["apps"][0]["id"]=="401");
 const auto normalized=json::parse(fixture_disk_text);CHECK(normalized["apps"][0]["auto-detach"]==true);CHECK(normalized["apps"][0]["exit-timeout"]==5);CHECK(normalized["apps"][0]["prep-cmd"][0]["elevated"]==true);no_transient_fields(normalized);
}
void authentication_boundary(){reset("not JSON");fixture_authorized=false;get();CHECK(fixture_http==401);CHECK(fixture_events.empty());CHECK(fixture_writes.empty());}
int main(int argc,char**argv){
 confighttp::install_fixture_route();
 if(argc>1 && std::string(argv[1])=="target-save-repair-probe"){try{target_save_repair_probe();return 0;}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
 std::vector<std::pair<std::string,std::function<void()>>> cases{
  {"invalid-own-legacy-fields",invalid_own_legacy_fields},{"valid-profile-legacy-normalization",valid_profile_legacy_normalization},{"invalid-missing-builtins",invalid_missing_builtins},{"invalid-ordinary-normalization",invalid_ordinary_normalization},{"ordinary-normalization",ordinary_normalization},{"illegal-environment-order",illegal_environment_order},{"invalid-original-environment-order",invalid_original_environment_order},{"no-normalization-stale-snapshot",no_normalization_and_stale_snapshot},{"authentication-boundary",authentication_boundary}
 };
 if(source_uuid_expansion){cases.push_back({"get-then-repair",get_then_repair});cases.push_back({"legacy-get-then-repair",legacy_get_then_repair});}
 try{for(const auto& [name,test]:cases){if(argc>1 && name!=argv[1])continue;test();std::cout<<"PASS "<<name<<'\n';}}
 catch(const std::exception& error){std::cerr<<"FAIL HTTP="<<fixture_http<<" writes="<<fixture_writes.size()<<" refreshes="<<fixture_refreshes<<": "<<error.what()<<'\n';return 1;}
 return 0;
}
'''


def fixture(root, http_source=None):
    sys.path.insert(0,str(root/'tests/input'))
    from test_delayed_mouse_release import function
    source=(http_source or root/'src/confighttp.cpp').read_text()
    process=(root/'src/process.cpp').read_text()
    signatures=[
        'bool is_valid_env_key(', 'std::string_view::iterator find_match(',
        'std::string parse_env_val(', 'bp::native_environment configured_app_environment(',
        'std::string configured_app_uuid(',
        'std::vector<std::string> configured_app_uuids(bp::native_environment &env,',
        'std::vector<std::string> configured_app_uuids_from_text(',
        'std::vector<std::string> configured_app_uuids(const nlohmann::json &file_tree)',
        'std::string_view configured_monitor_profile_error_from_file(',
        'std::string_view configured_monitor_profile_error(',
        'std::string_view monitor_profile_changes_error(',
    ]
    source_uuid_expansion='configured_app_environment(' in process
    if not source_uuid_expansion:
        signatures=['std::vector<std::string> configured_app_uuids(const nlohmann::json &file_tree)',
                    'std::string_view configured_monitor_profile_error_from_file(',
                    'std::string_view configured_monitor_profile_error(',
                    'std::string_view monitor_profile_changes_error(',
                    'void migrate_apps(']
    production='constexpr bool source_uuid_expansion='+str(source_uuid_expansion).lower()+';\n'
    production+='namespace proc {\n'+'\n'.join(function(process,s) for s in signatures)+'\n}\n'
    production+='namespace confighttp {\n'+'\n'.join(function(source,s) for s in [
        'std::optional<size_t> find_app_index_by_uuid(',
        'remote_session::control_e configurable_remote_session(',
        'bool ensure_remote_session_apps(', 'void getApps(', 'void saveApp(',
    ])+'\n'
    route=next(line.strip() for line in source.splitlines() if 'register_api_route("^/api/apps$", "GET", getApps);' in line)
    production+='std::map<std::pair<std::string,std::string>,std::function<void(resp_https_t,req_https_t)>> fixture_routes;\n'
    production+='void register_api_route(const std::string& path,const std::string& method,std::function<void(resp_https_t,req_https_t)> callback){fixture_routes[{path,method}]=callback;}\n'
    production+='void install_fixture_route(){'+route+'}\n}\n'
    return PRELUDE+production+TESTS


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[2])
    parser.add_argument('--save-cpp',type=Path)
    parser.add_argument('--http-source',type=Path)
    parser.add_argument('--save-exe',type=Path)
    parser.add_argument('--json-include',type=Path)
    parser.add_argument('--generate-only',action='store_true')
    args=parser.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        cpp=args.save_cpp or Path(tmp)/'get.cpp'
        cpp.write_text(fixture(args.root,args.http_source))
        if args.generate_only:return
        exe=args.save_exe or Path(tmp)/'get'
        subprocess.run(['g++','-std=c++23','-O0','-pthread','-I'+str(args.root),'-I'+str(args.root/'src'),*(['-I'+str(args.json_include)] if args.json_include else []),str(args.root/'src/remote_session.cpp'),str(cpp),'-lboost_process','-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)

if __name__=='__main__':main()
