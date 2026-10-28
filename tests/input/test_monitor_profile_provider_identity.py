#!/usr/bin/env python3
"""Actual Steam/Lutris policy and publication-boundary component fixture.

Runs actual force-sync API/config functions, actual autosync file transaction
blocks, normalized publication helper, configured identity/environment helpers,
and parser admission fields. Discovery/artwork, HTTP/file/cache and settings
parsing are controlled leaves. No provider discovery, full parser/server, native
host or client/device qualification is claimed.
"""
from pathlib import Path
import sys, argparse, subprocess
sys.dont_write_bytecode=True

def fixture(root):
 sys.path.insert(0,str(root/'tests/input'))
 for name in ['test_monitor_profile_json_identity','test_monitor_profile_catalog_identity','test_delayed_mouse_release']:
  sys.modules.pop(name,None)
 import test_monitor_profile_json_identity as api
 from test_delayed_mouse_release import function
 code=api.fixture(root).replace('int main(int argc,char**argv)','int original_json_main(int argc,char**argv)',1)
 code=code.replace('json fixture_disk; int fixture_writes=0;', 'std::string fixture_raw; int fixture_refreshes=0; json fixture_disk; int fixture_writes=0;')
 code=code.replace('namespace file_handler {std::string read_file(const char*) {return fixture_disk.dump();}}',r'''namespace file_handler {std::string read_file(const char*) {return fixture_raw.empty()?fixture_disk.dump():fixture_raw;}
 void write_file(const char*,const std::string& value){++fixture_writes;fixture_raw=value;fixture_disk=json::parse(value);}}
 namespace proc {void refresh(const std::string&,bool=false){++fixture_refreshes;}}
 ''')
 http=(root/'src/confighttp.cpp').read_text()
 refresh=function(http,'bool refresh_client_apps_cache(')
 predecl='bool refresh_client_apps_cache(json&,bool=true);\n' if 'bool sort_by_name' in refresh else ''
 code=code.replace('bool refresh_client_apps_cache(json& value,bool=true){++fixture_writes;fixture_disk=value;return true;}',predecl+function(http,'static void sort_apps_by_name(')+'\n'+refresh)
 providers=r'''
#include "src/steam_sync_policy.h"
#include "src/lutris_sync_policy.h"
#include "src/config_steam.h"
#include "src/config_lutris.h"
#include "src/steam_auto_sync.h"
#include "src/lutris_auto_sync.h"
namespace platf {std::filesystem::path appdata(){return "/fixture";}}
std::vector<platf::steam::game_t> steam_found;
std::vector<platf::lutris::game_t> lutris_found;
namespace platf::steam {
 std::string launch_command(std::uint32_t id){return "steam://"+std::to_string(id);}
 std::string launch_command(const game_t& game){return launch_command(game.app_id);}
 bool available(){return true;}
 std::vector<game_t> discover_catalog(const std::vector<std::filesystem::path>&){return steam_found;}
 namespace artwork {void prepare(std::vector<game_t>&,const std::filesystem::path&) {}}
 namespace autosync {void configure(settings_t){}}
}
namespace platf::lutris {
 std::string launch_command(std::int64_t id){return "lutris://"+std::to_string(id);}
 bool game_t::steam_backed()const{return service=="steam";}
 bool database_available(){return true;} bool executable_available(){return true;}
 bool discovery_ready(){return true;}
 std::vector<game_t> discover(const std::filesystem::path&){return lutris_found;}
 namespace artwork {void prepare(std::vector<game_t>&,const std::filesystem::path&) {}}
 namespace autosync {void configure(settings_t){}}
}
namespace config {
 steam_t steam;lutris_t lutris;
 steam_t parse_steam(std::unordered_map<std::string,std::string>&){return steam;}
 lutris_t parse_lutris(std::unordered_map<std::string,std::string>&){return lutris;}
}
'''
 providers+='namespace confighttp {\n'+function((root/'src/confighttp_steam.cpp').read_text(),'void postSteamForceSync(')+'\n'+function((root/'src/confighttp_lutris.cpp').read_text(),'void postLutrisForceSync(')+'\n}\n'
 providers+='namespace config {\n'+function((root/'src/config_steam.cpp').read_text(),'void apply_steam(')+'\n'+function((root/'src/config_lutris.cpp').read_text(),'void apply_lutris(')+'\n}\n'
 for name,raw,settings in [('steam','const auto text = file_handler::read_file(config::stream.file_apps.c_str());','platf::steam::autosync::settings_t'),('lutris','const auto contents = file_handler::read_file(config::stream.file_apps.c_str());','platf::lutris::autosync::settings_t')]:
  s=(root/f'src/{name}_auto_sync.cpp').read_text();start=s.index(raw);a=s.index('if (!'+('text' if name=='steam' else 'contents')+'.empty())',start)
  block=function(s,s[a:s.index('{',a)+1])
  if name=='lutris': block=block.replace('sync::policy::reconcile','platf::lutris::sync::policy::reconcile')
  providers+=f'void {name}_autosync_transaction() {{ {settings} settings;settings.remove_uninstalled=false;settings.'+('include_tools=true;settings.sync_all_installed=true;' if name=='steam' else 'include_steam=true;')+f'auto games={name}_found;std::uint64_t fingerprint=9,previous_fingerprint=0;bool have_fingerprint=false;auto next_artwork_retry=std::chrono::steady_clock::time_point{{}};\n{raw}\n{block}\n}}\n'
 sys.path.pop(0)
 return code+providers+TESTS

TESTS=r'''
void reset(const json& root,const std::string& raw={}) {
 fixture_disk=root;fixture_raw=raw;fixture_writes=fixture_refreshes=0;fixture_http=0;fixture_response={};profile_log.str("");
 fixture_environment=root.value("env",json::object());
 config::steam.enabled=config::steam.auto_sync=config::steam.sync_all_installed=true;config::steam.include_tools=true;
 config::steam.autosync_remove_uninstalled=false;config::steam.exclude_games_meta.clear();
 config::lutris.enabled=config::lutris.auto_sync=config::lutris.include_steam=true;config::lutris.autosync_remove_uninstalled=false;config::lutris.exclude_games_meta.clear();
 steam_found.clear();lutris_found.clear();
}
void assert_retained(const json& root,const json& marked) {
 assert(std::count(root["apps"].begin(),root["apps"].end(),marked)==1);
}
void steam_preservation() {
 const auto canonical=platf::steam::sync::policy::canonical_steam_app_uuid(42);
 for(bool metadata:{false,true})for(bool legacy:{false,true})for(bool remove:{false,true})for(bool excluded:{false,true})for(bool available:{false,true}) {
  auto profile=profile_json(canonical);if(metadata)profile["steam-id"]="42";if(!legacy)profile["steam-managed"]="auto";
  json root={{"apps",json::array({profile})}};assert(proc::configured_monitor_profile_error(profile,root).empty());
  platf::steam::game_t game;game.app_id=42;game.name="Steam ordinary";game.installed=true;
  std::vector<config::id_name_t> exclusions;if(excluded)exclusions.push_back({"42",{}});
  platf::steam::sync::policy::reconcile(root,available?std::vector{game}:std::vector<platf::steam::game_t>{},remove,exclusions,true,"installed");
  assert(root["apps"]==json::array({profile}));
 }
 // Ordinary historical adoption, updates, stale removal, duplicates and strict marker typing stay active.
 for(const json marker:{json(false),json("true"),json(1),json(nullptr)}) {
  json ordinary={{"name","Legacy"},{"uuid",canonical},{"remote-monitor-profile",marker}};
  json root={{"apps",json::array({ordinary})}};assert(platf::steam::sync::policy::reconcile(root,{},true,{},true));assert(root["apps"].empty());
 }
 std::cout<<"PASS Steam marked canonical/metadata preservation through removal/exclusion/update; ordinary legacy and marker compatibility\n";
}
void lutris_preservation() {
 const auto canonical=platf::lutris::sync::policy::canonical_lutris_app_uuid(42);
 for(bool remove:{false,true})for(bool excluded:{false,true})for(bool available:{false,true})for(bool nonimportable:{false,true}) {
  auto profile=profile_json(canonical);profile["lutris-id"]="42";profile["lutris-managed"]="auto";
  json root={{"apps",json::array({profile})}};assert(proc::configured_monitor_profile_error(profile,root).empty());
  platf::lutris::game_t game;game.id=42;game.name="Lutris ordinary";if(nonimportable)game.service="steam";
  std::vector<config::id_name_t> exclusions;if(excluded)exclusions.push_back({"42",{}});
  platf::lutris::sync::policy::reconcile(root,available?std::vector{game}:std::vector<platf::lutris::game_t>{},remove,exclusions,!nonimportable);
  assert(root["apps"]==json::array({profile}));
 }
 for(const json marker:{json(false),json("true"),json(1),json(nullptr)}) {
  json ordinary={{"name","Managed"},{"uuid",canonical},{"lutris-id","42"},{"lutris-managed","auto"},{"remote-monitor-profile",marker}};
  json root={{"apps",json::array({ordinary})}};assert(platf::lutris::sync::policy::reconcile(root,{},true,{},true));assert(root["apps"].empty());
 }
 std::cout<<"PASS Lutris marked metadata preservation through removal/exclusion/nonimportable/update; ordinary removal and marker compatibility\n";
}
void invoke(int caller) {
 auto request=std::make_shared<request_t>();std::unordered_map<std::string,std::string> vars;
 if(caller==0)confighttp::postLutrisForceSync(0,request);
 if(caller==1)config::apply_lutris(vars);
 if(caller==2)lutris_autosync_transaction();
 if(caller==3)confighttp::postSteamForceSync(0,request);
 if(caller==4)config::apply_steam(vars);
 if(caller==5)steam_autosync_transaction();
}
void publication() {
 const auto canonical=platf::lutris::sync::policy::canonical_lutris_app_uuid(42);
 const auto good=profile_json(canonical);
 // Actual API and config functions; actual autosync read/reconcile/refresh transaction blocks.
 for(int caller=0;caller<6;++caller)for(bool reverse:{false,true}) {
  auto profile=good;bool steam=caller>=3;
  // Steam's canonical namespace already matches manually, so exercise its publication
  // guard with a real ordinary provider update to a previously different canonical UUID.
  if(steam)profile["uuid"]=platf::steam::sync::policy::canonical_steam_app_uuid(42);
  json old={{"name","Old ordinary"},{"uuid","b1234567-1234-4234-8234-123456789abc"}};
  if(steam){old["steam-id"]="42";old["steam-managed"]="auto";}
  json apps=steam?(reverse?json::array({old,profile}):json::array({profile,old})):json::array({profile});
  json before={{"env",json::object()},{"apps",apps}};reset(before);
  assert(proc::configured_monitor_profile_error(profile,before).empty());
  if(steam){platf::steam::game_t game;game.app_id=42;game.name="Ordinary";game.installed=true;steam_found={game};}
  else {platf::lutris::game_t game;game.id=42;game.name="Ordinary";lutris_found={game};}
  bool threw=false;try{invoke(caller);}catch(const std::invalid_argument&){threw=true;}
  assert(fixture_writes==0&&fixture_refreshes==0&&fixture_disk==before);
  if(caller==0||caller==3){assert(fixture_http==400);assert(!fixture_response["error"].get<std::string>().empty());}
  if(caller==2||caller==5)assert(threw);
  assert(!profile_log.str().empty());
  auto loaded=parse_profile_rows(fixture_disk["apps"]);assert(std::count_if(loaded.begin(),loaded.end(),[](const auto& row){return row.remote_monitor_profile;})==1);
 }
 // Valid, unrelated provider additions still publish and refresh exactly once.
 for(int caller=0;caller<6;++caller) {
  auto profile=profile_json("a1234567-1234-4234-8234-123456789abc");json before={{"env",json::object()},{"apps",json::array({profile})}};reset(before);
  if(caller>=3){platf::steam::game_t game;game.app_id=42;game.name="Ordinary";game.installed=true;steam_found={game};}
  else {platf::lutris::game_t game;game.id=42;game.name="Ordinary";lutris_found={game};}
  invoke(caller);assert(fixture_writes==1&&fixture_refreshes==1);assert_retained(fixture_disk,profile);assert(fixture_disk["apps"].size()==2);
 }
 // Retain the documented repair exception: unchanged already-invalid duplicate
 // profiles remain omitted and may coexist with another added duplicate.
 auto ordinary=good;ordinary.erase("remote-monitor-profile");ordinary["name"]="Existing duplicate";
 json before={{"apps",json::array({good,ordinary})}};reset(before);
 assert(!proc::configured_monitor_profile_error(good,before).empty());
 platf::lutris::game_t game;game.id=42;game.name="Additional duplicate";lutris_found={game};invoke(0);
 assert(fixture_http==200&&fixture_writes==1&&fixture_refreshes==1);assert_retained(fixture_disk,good);
 assert(fixture_disk["apps"].size()==3);auto loaded=parse_profile_rows(fixture_disk["apps"]);
 assert(std::none_of(loaded.begin(),loaded.end(),[](const auto& row){return row.remote_monitor_profile;}));
 // Legitimate repair path: removing the ordinary duplicate restores admission.
 json repaired={{"apps",json::array({good})}};assert(confighttp::refresh_client_apps_cache(repaired));
 assert(proc::configured_monitor_profile_error(good,fixture_disk).empty());
 std::cout<<"PASS all six actual provider publication paths: valid-before conflicts deny before disk/cache; safe additions publish; explicit already-invalid duplicate exception and repair\n";
}
void environment_boundary() {
 if(!source_uuid_expansion)return;
 const std::string id="a1234567-1234-4234-8234-123456789abc";setenv("V6_ORDER_Z",id.c_str(),1);
 const std::string raw=R"v6({"env":{"V6_ORDER_Z":"b1234567-1234-4234-8234-123456789abc","V6_ORDER_A":"$(V6_ORDER_Z)"},"apps":[{"name":"Profile","uuid":"a1234567-1234-4234-8234-123456789abc","remote-monitor-profile":true},{"name":"Ordinary","uuid":"$(V6_ORDER_A)"}]})v6";
 const auto before=json::parse(raw);reset(before,raw);
 assert(proc::configured_monitor_profile_error_from_file(before["apps"][0],before,raw).empty());
 auto after=before;after["apps"].push_back({{"name","New ordinary"},{"uuid","c1234567-1234-4234-8234-123456789abc"}});
 bool threw=false;try{confighttp::refresh_client_apps_cache(after);}catch(const std::invalid_argument&){threw=true;}
 assert(threw&&fixture_writes==0&&fixture_refreshes==0&&fixture_disk==before);unsetenv("V6_ORDER_Z");
 std::cout<<"PASS actual source ordered JSON/OS effective identity boundary rejects normalization conflict\n";
}
int main(int argc,char**argv) {
 const std::string which=argc>1?argv[1]:"all";
 if(which=="all"||which=="steam")steam_preservation();
 if(which=="all"||which=="lutris")lutris_preservation();
 if(which=="all"||which=="publication")publication();
 if(which=="all"||which=="environment")environment_boundary();
 return 0;
}
'''
if __name__=='__main__':
 import tempfile
 p=argparse.ArgumentParser();p.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[2]);p.add_argument('--out',type=Path);p.add_argument('--json-include',type=Path);p.add_argument('--save-exe',type=Path);a=p.parse_args();root=a.root.resolve()
 code=fixture(root)
 if a.out:a.out.write_text(code)
 else:
  with tempfile.TemporaryDirectory() as temporary:
   cpp=Path(temporary)/'provider.cpp';cpp.write_text(code);exe=a.save_exe or Path(temporary)/'provider'
   command=['g++','-std=c++23','-O0','-pthread','-I'+str(root),'-I'+str(root/'src'),*(['-I'+str(a.json_include)] if a.json_include else []),str(root/'src/app_catalog_policy.cpp'),str(root/'src/remote_session.cpp'),str(root/'src/steam_sync_policy.cpp'),str(root/'src/lutris_sync_policy.cpp'),str(cpp),'-lboost_process','-o',str(exe)]
   subprocess.run(command,check=True);subprocess.run([str(exe)],check=True)
