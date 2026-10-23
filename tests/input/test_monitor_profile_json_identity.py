#!/usr/bin/env python3
"""Actual JSON -> saveApp -> parser admission -> catalog -> launch prefix.

The extracted production saveApp function uses deterministic authenticated HTTP,
file/cache and config adapters. The parser's actual admission and typed marker
blocks and source environment/UUID decoding execute actual production helpers;
name/config fields and external HTTP/file/cache operations use narrow adapters. This is not
a TLS handler, full process parser, native host or device integration test.
"""
from pathlib import Path
import argparse, subprocess, tempfile
from test_monitor_profile_catalog_identity import fixture as catalog_fixture
from test_delayed_mouse_release import function

ADAPTERS=r"""
#include "src/boost_process_shim.h"
#include <boost/property_tree/json_parser.hpp>
#include <boost/algorithm/string.hpp>
namespace bp = boost_process_shim;
namespace pt = boost::property_tree;
#include "src/remote_monitor_profile_policy.h"
#include "src/utility.h"
#include "src/uuid.h"
#include <sstream>
#include <memory>
using namespace std::literals;
using json=nlohmann::json;
std::ostringstream profile_log;
#define BOOST_LOG(level) profile_log
namespace config {struct stream_t {std::string file_apps="fixture-apps.json";} stream;}
json fixture_environment=json::object();
json fixture_disk; int fixture_writes=0; json fixture_response; int fixture_http=0;
namespace file_handler {std::string read_file(const char*) {return fixture_disk.dump();}}
struct request_t { std::stringstream content; };
using resp_https_t=int; using req_https_t=std::shared_ptr<request_t>;
bool check_content_type(int,req_https_t,const char*){return true;}
bool validateContentType(int,req_https_t,const char*){return true;}
bool authenticate(int,req_https_t){return true;}
void print_req(req_https_t){}
void bad_request(int,req_https_t,const std::string& error){fixture_http=400;fixture_response={{"status",false},{"error",error}};}
void send_response(int,const json& response){fixture_http=200;fixture_response=response;}
void normalize_adapter_config_pair(json&){}
namespace confighttp {
std::recursive_mutex& apps_file_mutex(){static std::recursive_mutex value;return value;}
bool refresh_client_apps_cache(json& value,bool=true){++fixture_writes;fixture_disk=value;return true;}
"""
TESTS=r"""
json save_json(json input,json apps) {
 fixture_disk={{"env",fixture_environment},{"apps",apps}};fixture_writes=0;fixture_response={};fixture_http=0;
 auto request=std::make_shared<request_t>();request->content<<input.dump();
 confighttp::saveApp(0,request);
 return {{"http",fixture_http},{"response",fixture_response},{"writes",fixture_writes},{"disk",fixture_disk}};
}
json profile_json(const std::string& uuid) {return {{"name","Custom"},{"uuid",uuid},{"remote-monitor-profile",true},{"config-overrides",{{"output_name",uuid}}}};}
void assert_denied(const json& input,const json& apps) {
 const auto saved=save_json(input,apps);assert(saved["http"]==400);assert(saved["writes"]==0);
 assert(saved["disk"]["apps"]==apps);assert(saved["response"]["status"]==false);assert(!saved["response"]["error"].get<std::string>().empty());
}
void effective_identity_regressions() {
 const std::string identity="a1234567-1234-4234-8234-123456789abc";
 const std::string other="b1234567-1234-4234-8234-123456789abc";
 const auto good=profile_json(identity);
 for(bool upper:{false,true})for(bool reverse:{false,true})for(bool from_os:{false,true}) {
  auto resolved=identity;if(upper)std::transform(resolved.begin(),resolved.end(),resolved.begin(),[](unsigned char c){return std::toupper(c);});
  fixture_environment=json::object();
  if(from_os)setenv("V5_PROFILE_ALIAS",resolved.c_str(),1);
  else fixture_environment={{"A_PROFILE_ALIAS",resolved},{"V5_PROFILE_ALIAS","$(A_PROFILE_ALIAS)"},{"bad=key","ignored"}};
  const auto ordinary=json{{"name","Ordinary"},{"uuid","$(V5_PROFILE_ALIAS)"}};
  auto apps=reverse?json::array({ordinary,good}):json::array({good,ordinary});
  auto loaded=parse_profile_rows(apps);
  assert(loaded.size()==(source_uuid_expansion?1:2));
  if(source_uuid_expansion)assert(!loaded[0].remote_monitor_profile&&loaded[0].uuid==resolved);
  else assert(std::any_of(loaded.begin(),loaded.end(),[&](const auto& app){return app.remote_monitor_profile&&app.uuid==identity;}));
  clear_admission();proc::proc.apps=loaded;auto list=projection();
  assert(std::any_of(list.catalogue.begin(),list.catalogue.end(),[&](const auto& app){return app.monitor_profile&&app.uuid==identity;})==!source_uuid_expansion);
  if(source_uuid_expansion)assert_denied(good,apps);
  else {const auto saved=save_json(good,apps);assert(saved["http"]==200&&saved["disk"]["env"]==fixture_environment);}
  // A new ordinary row or an ordinary index update cannot invalidate a valid profile.
  auto incoming=ordinary;incoming["index"]=-1;
  auto saved=save_json(incoming,json::array({good}));assert(saved["http"]==(source_uuid_expansion?400:200));
  if(source_uuid_expansion)assert(saved["writes"]==0);
  auto old=ordinary;old["uuid"]=other;incoming["index"]=1;
  saved=save_json(incoming,json::array({good,old}));assert(saved["http"]==(source_uuid_expansion?400:200));
  if(source_uuid_expansion)assert(saved["writes"]==0);
  // An existing invalid marked row stays marked while an unrelated ordinary edit succeeds.
  auto invalid=profile_json("$(BROKEN");auto harmless=json{{"name","Harmless"},{"uuid",other}};
  saved=save_json(harmless,json::array({invalid}));assert(saved["http"]==200&&saved["disk"]["apps"][0]==invalid);
  assert(parse_profile_rows(json::array({invalid,harmless})).size()==1);
  // Distinct effective identities remain selectable and retain normal allocation order.
  if(from_os)setenv("V5_PROFILE_ALIAS",other.c_str(),1);else fixture_environment["A_PROFILE_ALIAS"]=other;
  loaded=parse_profile_rows(apps);assert(loaded.size()==2);
  clear_admission();proc::proc.apps=loaded;list=projection();
  const auto tile=std::find_if(list.catalogue.begin(),list.catalogue.end(),[&](const auto& app){return app.monitor_profile&&app.uuid==identity;});assert(tile!=list.catalogue.end());
  admit_profile(std::to_string(tile->id),tile->uuid,0,identity);admit_profile(std::to_string(tile->id),"",0,identity);
  auto ordinary_duplicates=parse_profile_rows(json::array({ordinary,ordinary}));assert(ordinary_duplicates.size()==2&&ordinary_duplicates[0].id!=ordinary_duplicates[1].id);
  unsetenv("V5_PROFILE_ALIAS");
 }

 setenv("V5_Mixed_Key",identity.c_str(),1);fixture_environment=json::object();
 const auto case_rows=parse_profile_rows(json::array({good,{{"name","Case OS"},{"uuid","$(v5_mixed_key)"}}}));
 assert(case_rows.size()==(source_uuid_expansion&&windows_save_path?1:2));unsetenv("V5_Mixed_Key");
 if(source_uuid_expansion) {
  // Property-tree source order is retained, even though save JSON will sort it.
  setenv("V5_ORDER_Z",identity.c_str(),1);
  const std::string raw=R"v5({"env":{"V5_ORDER_Z":"b1234567-1234-4234-8234-123456789abc","V5_ORDER_A":"$(V5_ORDER_Z)"},"apps":[{"name":"Custom","uuid":"a1234567-1234-4234-8234-123456789abc","remote-monitor-profile":true},{"name":"Ordinary","uuid":"$(V5_ORDER_A)"}]})v5";
  const auto before=json::parse(raw);
  assert(!proc::monitor_profile_changes_error(before,before,raw).empty());
  get_normalization_guard(raw,before);
  assert(fixture_http==400&&fixture_writes==0&&!fixture_response["error"].get<std::string>().empty());
  unsetenv("V5_ORDER_Z");
 }
 fixture_environment=json::object();
 auto legacy_expression=json{{"name","Legacy expression"},{"uuid","$(BROKEN"}};
 assert(save_json(legacy_expression,json::array())["http"]==200);
 // Windows saveApp executes its real conditional body; provider APIs are controlled leaves.
 for(const std::string provider_id:{other,identity,std::string("legacy-provider-token"),std::string("")}) {
  auto marked=good;marked["playnite-id"]=provider_id;
  auto saved=save_json(marked,json::array());assert(saved["http"]==200&&saved["writes"]==1);
  assert(saved["disk"]["apps"][0]["uuid"]==identity&&saved["disk"]["apps"][0]["remote-monitor-profile"]==true);
  auto edited=marked;edited["name"]="Edited provider profile";
  saved=save_json(edited,saved["disk"]["apps"]);assert(saved["http"]==200&&saved["disk"]["apps"].size()==1&&saved["disk"]["apps"][0]["uuid"]==identity);
  auto loaded=parse_profile_rows(saved["disk"]["apps"]);assert(loaded.size()==1&&loaded[0].uuid==identity&&loaded[0].remote_monitor_profile);
  if(windows_save_path&&!provider_id.empty()) {
   auto ordinary=marked;ordinary.erase("remote-monitor-profile");saved=save_json(ordinary,json::array());
   assert(saved["http"]==200);auto canonical=provider_id;std::transform(canonical.begin(),canonical.end(),canonical.begin(),[](unsigned char c){return std::toupper(c);});
   assert(saved["disk"]["apps"].back()["uuid"]==canonical);
  }
 }
 if(windows_save_path) {
  auto collision=json{{"name","Incoming ordinary provider"},{"uuid",other},{"playnite-id",identity}};
  assert_denied(collision,json::array({good}));
 }
 std::cout<<"PASS actual effective configured identities: JSON/OS environments, sequential expansion, UUID case, both row orders, ordinary inserts/updates/duplicates, malformed marked preservation, literal target contrast, marked Playnite identity and ordinary canonical compatibility\n";
}
int json_main() {
 get_apps_regressions();
 migration_regressions();
 native_identity_regressions();
 effective_identity_regressions();
 const std::string fresh="a1234567-1234-4234-8234-123456789abc";
 const auto good=profile_json(fresh);
 auto saved=save_json(good,json::array());assert(saved["http"]==200 && saved["writes"]==1);
 auto rows=parse_profile_rows(saved["disk"]["apps"]);assert(rows.size()==1 && rows[0].remote_monitor_profile && rows[0].uuid==fresh);
 clear_admission();proc::proc.apps=rows;
 auto projected=projection();const auto tile=projected.catalogue.front();assert(tile.uuid==fresh);
 admit_profile(std::to_string(tile.id),tile.uuid,0,fresh);
 admit_profile(std::to_string(tile.id),"",0,fresh);
 auto edited=good;edited["name"]="Edited";assert(save_json(edited,json::array({good}))["http"]==200);
 // All seven fixed UUIDs and their case variants reject before a file/cache call.
 for(int i=1;i<=7;++i) {
  auto uuid=remote_session::synthetic_uuid(static_cast<remote_session::control_e>(i));auto bad=profile_json(uuid);
  assert_denied(bad,json::array());
  auto disk=json::array({bad,good});auto loaded=parse_profile_rows(disk);assert(loaded.size()==1 && loaded[0].uuid==fresh);
  clear_admission();proc::proc.apps=loaded;auto list=projection();
  assert(std::none_of(list.catalogue.begin(),list.catalogue.end(),[&](const auto& row){return row.uuid==uuid && !row.synthetic;}));
  remote_session::caller_t caller{.paired=true,.may_view=true,.may_launch=true};
  auto direct=remote_session::project(caller,{}, {},{{99,uuid,"Bad",false,true}},false);
  assert(std::none_of(direct.catalogue.begin(),direct.catalogue.end(),[](const auto& row){return row.id==99;}));
  std::transform(uuid.begin(),uuid.end(),uuid.begin(),[](unsigned char c){return std::toupper(c);});assert_denied(profile_json(uuid),json::array());
 }
 for(const auto& uuid:std::vector<std::string>{"","not-a-uuid","00000000-0000-0000-0000-000000000000","a1234567_1234-4234-8234-123456789abc"}) {
  auto bad=profile_json(uuid);assert_denied(bad,json::array());assert(parse_profile_rows(json::array({bad,good})).size()==1);
 }
 for(const json value:{json(nullptr),json(12),json(false),json::array(),json::object()}){auto bad=good;bad["uuid"]=value;assert_denied(bad,json::array());}
 auto missing=good;missing.erase("uuid");assert_denied(missing,json::array());
 // Duplicate identity against marked and ordinary rows, both orders and casing.
 for(bool ordinary:{false,true})for(bool reversed:{false,true})for(bool upper:{false,true}){
  auto other=good;other["name"]="Other";if(ordinary)other.erase("remote-monitor-profile");
  if(upper){auto u=fresh;std::transform(u.begin(),u.end(),u.begin(),[](unsigned char c){return std::toupper(c);});other["uuid"]=u;}
  auto duplicate=reversed?json::array({other,good}):json::array({good,other});
  assert_denied(good,duplicate);auto loaded=parse_profile_rows(duplicate);assert(loaded.size()==(ordinary?1:0));
  for(const auto& row:loaded)assert(!row.remote_monitor_profile);
 }
 // Defensive projection excludes all conflicting marked rows, preserving normal rows.
 remote_session::caller_t caller{.paired=true,.may_view=true,.may_launch=true};
 auto duplicate_projection=remote_session::project(caller,{}, {},{{91,fresh,"A",false,true},{92,fresh,"B",false,true},{93,"normal","Normal",false,false}},false);
 for(const auto& row:duplicate_projection.catalogue)assert(row.id!=91 && row.id!=92);
 // Legacy builtin entries remain unmarked and use their established routing.
 for(const auto control:{remote_session::control_e::monitor,remote_session::control_e::input}) {
  const auto builtin=remote_session::synthetic(control);json legacy={{"uuid",builtin.uuid},{"name",builtin.title}};
  auto bad=profile_json(builtin.uuid);auto disk=json::array({bad,legacy,good});
  auto loaded=parse_profile_rows(disk);assert(loaded.size()==2);assert(!loaded[0].remote_monitor_profile && loaded[0].uuid==builtin.uuid);
  auto edited=legacy;edited["output"]="builtin.log";auto result=save_json(edited,disk);assert(result["http"]==200);
  assert(result["disk"]["apps"][0]["remote-monitor-profile"]==true);assert(result["disk"]["apps"][1]["output"]=="builtin.log");
  assert(remote_session::identify(0,builtin.uuid)==control);
 }
 // Strict marker typing retains the prior false/string/number policy.
 for(const json marker:{json(false),json("true"),json(1),json(nullptr)}) {auto normal=good;normal["remote-monitor-profile"]=marker;auto loaded=parse_profile_rows(json::array({normal}));assert(loaded.size()==1&&!loaded[0].remote_monitor_profile);}
 auto normal=json{{"name","Normal"},{"uuid","legacy-non-uuid"}};assert(save_json(normal,json::array())["http"]==200);
 std::cout<<"PASS actual JSON/saveApp/parser guard/allocator/project/identify/launch: fresh profiles, all seven fixed UUIDs, case variants, invalid/non-nil UUIDs, duplicates against marked/normal rows, legacy builtin save/routing, strict marker and normal compatibility\n";
 return 0;
}
int main(int argc,char**argv) {
 if(argc>1 && std::string(argv[1])=="--api") {
  json request;std::cin>>request;auto result=save_json(request.at("input"),request.at("apps"));
  if(result["http"]==200){auto parsed=parse_profile_rows(result["disk"]["apps"]);result["loaded"]=json::array();for(const auto& row:parsed)result["loaded"].push_back({{"uuid",row.uuid},{"profile",row.remote_monitor_profile}});}
  std::cout<<result.dump()<<'\n';return 0;
 }
 old_catalog_main();return json_main();
}
"""
def fixture(root, windows=False):
 s=(root/'src/confighttp.cpp').read_text();p=(root/'src/process.cpp').read_text()
 base=catalog_fixture(root).replace('int main() {','int old_catalog_main() {').replace('\n}\n', '\n}\n',1)
 # old catalog main needs a return now that it is an ordinary function.
 end=base.rfind('\n}');base=base[:end]+'\n return 0;'+base[end:]
 helper='\n'.join(function(s,x) for x in ['std::optional<size_t> find_app_index_by_uuid(','remote_session::control_e configurable_remote_session(','bool ensure_remote_session_apps(','void saveApp('])
 migration='namespace proc {'+function(p,'void migrate_apps(')+function(p,'void migration_v2(')+'}\n' if 'void migrate_apps(' in p else ''
 # Run the production environment and UUID decoder, not a literal adapter.
 signatures=['std::string_view::iterator find_match(', 'std::string parse_env_val(']
 if 'configured_app_environment(' in p:
  signatures=['bool is_valid_env_key(']+signatures+['bp::native_environment configured_app_environment(',
   'std::string configured_app_uuid(',
   'std::vector<std::string> configured_app_uuids(bp::native_environment &env,',
   'std::vector<std::string> configured_app_uuids_from_text(',
   'std::vector<std::string> configured_app_uuids(const nlohmann::json &file_tree)']
 else:
  signatures+=['std::vector<std::string> configured_app_uuids(const nlohmann::json &file_tree)']
 signatures+=['std::string_view configured_monitor_profile_error_from_file(', 'std::string_view configured_monitor_profile_error(', 'std::string_view monitor_profile_changes_error(']
 bodies=[]
 for signature in signatures:
  body=function(p,signature)
  if windows and signature=='std::string parse_env_val(': body='#define _WIN32\n'+body+'\n#undef _WIN32\n'
  bodies.append(body)
 production='namespace proc {\n'+'\n'.join(bodies)+'\n}\n'
 production+='constexpr bool source_uuid_expansion='+str('configured_app_environment(' in p).lower()+';\nconstexpr bool windows_save_path='+str(windows).lower()+';\n'

 if 'configured_app_environment(' in p:
  a=p.index('        const int json_index = i++;');z=p.index('        proc::ctx_t ctx;',a);guard=p[a:z]
  a=p.index('ctx.remote_monitor_profile =');z=p.index(';',a)+1;marker=p[a:z]
  setup=r"""
 const json json_tree={{"env",fixture_environment},{"apps",rows}};
 pt::ptree tree;std::stringstream input(json_tree.dump());pt::read_json(input,tree);
 auto &apps_node=tree.get_child("apps");auto &env_vars=tree.get_child("env");
 auto this_env=proc::configured_app_environment(env_vars);
 const auto configured_uuids=proc::configured_app_uuids(this_env,apps_node,json_tree["apps"]);
 std::vector<std::string_view> configured_uuid_views;for(const auto& uuid:configured_uuids)configured_uuid_views.push_back(uuid);
 for(auto& [_,app_node]:apps_node) {
"""
  uuid_decode='ctx.uuid = proc::configured_app_uuid(this_env, app_node);'
 else:
  a=p.index('          const int json_index = i++;');z=p.index('          proc::ctx_t ctx {};',a);guard=p[a:z]
  a=p.index('ctx.remote_monitor_profile =');z=p.index(';',a)+1;marker=p[a:z]
  setup=r"""
 const json tree={{"env",fixture_environment},{"apps",rows}};const auto& json_tree=tree;
"""
  # Exact target environment seed and configured override statements.
  a=p.index('    auto this_env = bp::this_process::env();',p.index('std::optional<proc::proc_t> parse('));z=p.index('    std::set<std::string> ids;',a)
  seed=p[a:z]
  a=p.index('        if (tree.contains("env")',p.index('std::optional<proc::proc_t> parse('));z=p.index('\n\n        // Ensure',a)
  setup+=seed+p[a:z]+'\n for(auto& app_node:tree["apps"]) {\n'
  production=production.replace('namespace proc {\n','namespace proc {\n'+function(p,'bool is_valid_env_key(')+'\n',1)
  uuid_decode='ctx.uuid = app_node.at("uuid");'
  guard=guard.replace('configured_monitor_profile_error(app_node, tree)','proc::configured_monitor_profile_error(app_node, tree)')
  setup=setup.replace('is_valid_env_key(', 'proc::is_valid_env_key(').replace(' = parse_env_val(', ' = proc::parse_env_val(')
 parser=r"""
std::vector<proc::ctx_t> parse_profile_rows(const json& rows) {
 int i=0;std::vector<proc::ctx_t> result;
 std::set<std::string> ids,active;std::map<std::string,proc::catalog::alias_state_t> state;bool changed=false;
"""+setup+guard+r"""
  const auto& japp=json_tree["apps"][json_index];proc::ctx_t ctx;ctx.configured_index=static_cast<std::size_t>(json_index);
"""+marker+'\n'+uuid_decode+r"""
  ctx.name=japp.value("name",std::string{});
  if(japp.contains("config-overrides"))for(const auto& [key,value]:japp["config-overrides"].items())ctx.config_overrides[key]=value.is_string()?value.get<std::string>():value.dump();
  proc::catalog::app_identity_t app{.name=ctx.name,.uuid=ctx.uuid,.art_version="default"};
  proc::catalog::assign_compatible_id(app,json_index,ids,state,active,changed);ctx.id=app.id;ctx.id_aliases=app.aliases;
  result.push_back(ctx);
 }
 return result;
}
"""

 a=s.index('        if (const auto error = proc::monitor_profile_changes_error(original_file_tree, file_tree, content);',s.index('  void getApps('))
 guard_body=function(s,s[a:s.index('{',a)+1])
 publication='void get_normalization_guard(const std::string& content,json file_tree) {const auto original_file_tree=json::parse(content);fixture_http=0;fixture_writes=0;fixture_response={};int response=0;auto request=std::make_shared<request_t>();\n'+guard_body+'\n++fixture_writes;}\n'
 # Extract the actual GET UUID normalization and snapshot ID annotation blocks.
 a=s.index('// Ensure each app has a UUID');a=s.index('if (',a)
 get_normalization=function(s,s[a:s.index('{',a)+1])
 a=s.index('          const auto apps_snapshot = proc::proc.get_apps();',s.index('// Add computed app ids'))
 z=s.index('        } catch (...) {',a);get_ids=s[a:z]
 get_tests=r"""
void get_apps_regressions() {
 const std::string fresh="a1234567-1234-4234-8234-123456789abc";
 json bad={{"name","Invalid marked"},{"remote-monitor-profile",true}};
 json normal={{"name","Normal"},{"uuid","normal"}};json file_tree={{"apps",json::array({bad,normal,normal})}};
 auto original_file_tree=file_tree;std::string content=file_tree.dump();
 bool mutated=false;for(auto &app:file_tree["apps"]) {
"""+get_normalization+r"""
 }
 assert(!file_tree["apps"][0].contains("uuid"));assert(!mutated);
 clear_admission();proc::proc.apps.push_back({"normal",false,{},"123","Normal"});proc::proc.apps.push_back({"normal",false,{},"456","Normal duplicate"});
 proc::proc.apps[0].configured_index=1;proc::proc.apps[1].configured_index=2;
"""+"{\n"+get_ids+"}\n"+r"""
 assert(!file_tree["apps"][0].contains("id"));assert(file_tree["apps"][0]["index"]==0);
 assert(file_tree["apps"][1]["id"]=="123" && file_tree["apps"][1]["index"]==1);
 assert(file_tree["apps"][2]["id"]=="456" && file_tree["apps"][2]["index"]==2);
 fixture_environment={{"V5_GET_ALIAS",fresh}};
 json ordinary_env={{"name","Env"},{"uuid","$(V5_GET_ALIAS)"}};
 json profile={{"name","Profile"},{"uuid",fresh},{"remote-monitor-profile",true}};
 file_tree={{"env",fixture_environment},{"apps",json::array({profile,ordinary_env,ordinary_env})}};
 original_file_tree=file_tree;content=file_tree.dump();
 proc::proc.apps=parse_profile_rows(file_tree["apps"]);
"""+"{\n"+get_ids+"}\n"+r"""
 assert(file_tree["apps"][0].contains("id")==!source_uuid_expansion);
 assert(file_tree["apps"][1]["id"]!=file_tree["apps"][2]["id"]);
 fixture_environment=json::object();
 file_tree={{"env",fixture_environment},{"apps",json::array({{{"name","Reserved marked"},{"uuid",remote_session::synthetic_uuid(remote_session::control_e::resume)},{"remote-monitor-profile",true}},profile})}};
 original_file_tree=file_tree;content=file_tree.dump();
 clear_admission();proc::proc.apps.push_back({"stale",false,{},"123","Old ordinary"});proc::proc.apps[0].configured_index=0;
 proc::proc.apps.push_back({fresh,false,{},"456","Old ordinary same uuid"});proc::proc.apps[1].configured_index=1;
"""+"{\n"+get_ids+"}\n"+r"""
 assert(!file_tree["apps"][0].contains("id")&&!file_tree["apps"][1].contains("id"));
 fixture_environment=json::object();
 const auto builtin=remote_session::synthetic(remote_session::control_e::monitor);
 auto marked=json{{"name","Invalid custom"},{"uuid",builtin.uuid},{"remote-monitor-profile",true}};
 file_tree={{"apps",json::array({marked})}};confighttp::ensure_remote_session_apps(file_tree);
 assert(file_tree["apps"][0]==marked);
 assert(std::count_if(file_tree["apps"].begin(),file_tree["apps"].end(),[&](const auto& row){return row.value("uuid",std::string{})==builtin.uuid&&!remote_session::marked_monitor_profile(row);})==1);
}
"""
 migration_tests='void migration_regressions() {}\n'
 if migration:
  migration_tests=r"""
void migration_regressions() {
 for(const json value:{json(""),json(nullptr),json(12),json(false),json("not-a-uuid"),json("9a1c5a25-58fe-40e0-b9aa-7d3f00000005")}) {
  json bad={{"name","Invalid"},{"remote-monitor-profile",true},{"uuid",value}};
  json tree={{"version",1},{"apps",json::array({bad})}};proc::migration_v2(tree);
  assert(tree["apps"][0]==bad);assert(tree["version"]==2);
 }
 json missing={{"name","Missing"},{"remote-monitor-profile",true}};
 json tree={{"apps",json::array({missing})}};proc::migration_v2(tree);assert(tree["apps"][0]==missing);
}
"""
 native_tests='void native_identity_regressions() {}\n'
 if (root/'src/legacy_generated_app_identities.h').exists():
  generated=[]
  for token in ['FALLBACK_DESKTOP_UUID','REMOTE_INPUT_UUID','TERMINATE_APP_UUID']:
   a=p.index('ctx.uuid = '+token+';');generated.append(p[a:p.index(';',a)+1])
  virtual=(root/'src/builtin_virtual_display.h').read_text();a=virtual.index('ctx.uuid = VIRTUAL_DISPLAY_UUID;');generated.append(virtual[a:virtual.index(';',a)+1])
  native_tests='#include "src/legacy_generated_app_identities.h"\nvoid native_identity_regressions() {std::vector<proc::ctx_t> generated;\n'
  native_tests+='\n'.join('{proc::ctx_t ctx;'+assignment+'generated.push_back(ctx);}' for assignment in generated)
  native_tests+=r"""
  for(const auto& native:generated) {
   for(bool lower:{false,true}) {
    auto uuid=native.uuid;if(lower)std::transform(uuid.begin(),uuid.end(),uuid.begin(),[](unsigned char c){return std::tolower(c);});
    auto marked=profile_json(uuid);assert_denied(marked,json::array());
    assert(parse_profile_rows(json::array({marked})).empty());
    auto normal=marked;normal.erase("remote-monitor-profile");auto rows=parse_profile_rows(json::array({normal}));assert(rows.size()==1&&!rows[0].remote_monitor_profile&&rows[0].uuid==uuid);
    remote_session::caller_t caller{.paired=true,.may_view=true,.may_launch=true};
    auto projected=remote_session::project(caller,{}, {},{{93,uuid,"Invalid native profile",false,true},{94,native.uuid,"Legacy generated app",false,false}},false);
    assert(std::none_of(projected.catalogue.begin(),projected.catalogue.end(),[](const auto& app){return app.monitor_profile;}));
    assert(remote_session::identify(0,uuid)==remote_session::control_e::none);
   }
  }
  std::cout<<"PASS actual target generated UUID assignments: Virtual Display/fallback Desktop/legacy Input/Terminate, both cases; marked API/load/projection rejection and unchanged ordinary identity routing\n";
 }
"""
 windows_prelude=''
 if windows:
  playnite=(root/'src/platform/windows/playnite_sync_policy.cpp').read_text()
  windows_prelude='namespace platf::playnite::sync::policy {\n'+function(playnite,'std::string canonical_playnite_app_uuid(')+'\n}\n'
  windows_prelude+='namespace confighttp {void enhance_app_with_playnite_cover(json&){} void enhance_app_with_playnite_icon(json&){} }\n'
  helper='#define _WIN32\n'+helper+'\n#undef _WIN32\n'
 return base+ADAPTERS+'}\n'+production+migration+windows_prelude+'namespace confighttp {\n'+helper+'}\n'+parser+get_tests+migration_tests+publication+TESTS.replace('int json_main() {',native_tests+'int json_main() {')



if __name__=='__main__':
 parser=argparse.ArgumentParser();parser.add_argument('--save-cpp',type=Path);parser.add_argument('--save-exe',type=Path);parser.add_argument('--json-include',type=Path);parser.add_argument('--windows',action='store_true');args=parser.parse_args()
 root=Path(__file__).resolve().parents[2]
 with tempfile.TemporaryDirectory() as tmp:
  cpp=Path(tmp)/'json.cpp';cpp.write_text(fixture(root,args.windows))
  if args.save_cpp:args.save_cpp.write_text(cpp.read_text())
  exe=args.save_exe or Path(tmp)/'json'
  subprocess.run(['g++','-std=c++23','-O0','-pthread','-I'+str(root),'-I'+str(root/'src'),*(['-I'+str(args.json_include)] if args.json_include else []),str(root/'src/app_catalog_policy.cpp'),str(root/'src/remote_session.cpp'),str(cpp),'-lboost_process','-o',str(exe)],check=True)
  subprocess.run([str(exe)],check=True)
