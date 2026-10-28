#!/usr/bin/env python3
"""Complete publication handlers with actual identity/publication policy.

Controlled HTTP/auth/file/cache, discovery/artwork and Playnite configuration
leaves. Executes complete DELETE, purge, target reorder, Playnite runtime
publisher/config reconciliation and target runtime/retry migration bodies.
No server, full process parse, native provider/driver or device claim.
"""
from pathlib import Path
import argparse,subprocess,tempfile,sys
sys.dont_write_bytecode=True

def fixture(root):
 sys.path.insert(0,str(root/'tests/input'))
 for n in ['test_monitor_profile_provider_identity','test_monitor_profile_json_identity','test_monitor_profile_catalog_identity','test_delayed_mouse_release']:
  sys.modules.pop(n,None)
 import test_monitor_profile_provider_identity as provider
 from test_delayed_mouse_release import function
 code=provider.fixture(root).replace('int main(int argc,char**argv) {','int original_provider_main(int argc,char**argv) {',1)
 code=code.replace('struct request_t { std::stringstream content; };','struct request_t { std::stringstream content; std::vector<std::string> path_match; std::string method="DELETE"; };')
 http=(root/'src/confighttp.cpp').read_text();process=(root/'src/process.cpp').read_text()
 code+='\n#include <format>\nnamespace confighttp {\n'+function(http,'std::optional<size_t> resolve_app_index_token(')+'\n'
 for name in ['void deleteApp(','void purgeAutoSyncedApps(']+(['void reorderApps('] if 'void reorderApps(' in http else []):code+=function(http,name)+'\n'
 code+='}\nnamespace platf::playnite {void force_sync(bool){}}\n'
 code+='namespace config {playnite_t playnite; playnite_t parse_playnite(std::unordered_map<std::string,std::string>&){return playnite;}\n#define _WIN32\n'+function((root/'src/config_playnite.cpp').read_text(),'void apply_playnite(')+'\n#undef _WIN32\n}\n'
 code+='namespace platf::playnite::sync {\n'+function((root/'src/platform/windows/playnite_sync.cpp').read_text(),'void write_and_refresh_apps(')+'\n}\n'
 if 'void migrate(' in process:code+='namespace proc {\n'+function(process,'void migrate(')+'\n}\n'
 code+=TESTS
 if 'void reorderApps(' in http:code+=TARGET_TESTS
 code+='\nint main(int argc,char**argv){std::string which=argc>1?argv[1]:"all";\n'
 for test in ['purge','deletion','runtime','fullscreen']+(['reorder','migration','repair'] if 'void reorderApps(' in http else []):code+='if(which=="all"||which=="'+test+'")'+test+'_witness();\n'
 code+='}\n';sys.path.pop(0);return code
TESTS=r'''
const std::string profile_id="a1234567-1234-4234-8234-123456789abc";
const std::string ordinary_id="b1234567-1234-4234-8234-123456789abc";
const std::string spare_id="c1234567-1234-4234-8234-123456789abc";
auto deletion_request(const std::string& token){auto req=std::make_shared<request_t>();req->path_match={"",token};return req;}
void purge_witness(){
 auto profile=profile_json(profile_id);profile["playnite-managed"]="auto";
 auto normal=json{{"name","ordinary auto"},{"uuid",ordinary_id},{"playnite-managed","auto"}};
 for(const json marker:{json(false),json("true"),json(1),json(nullptr)}){
  normal["remote-monitor-profile"]=marker;
  auto manual=json{{"name","manual"},{"uuid",spare_id},{"playnite-managed","manual"}};
  const json before={{"apps",json::array({profile,normal,manual})}};reset(before);
  confighttp::purgeAutoSyncedApps(0,std::make_shared<request_t>());
  assert(fixture_http==200&&fixture_writes==1&&fixture_refreshes==1&&fixture_response["removed"]==1);
  assert_retained(fixture_disk,profile);assert_retained(fixture_disk,manual);assert(fixture_disk["apps"].size()==2);
 }
 // Invalid profiles are also retained for repair by provider purge.
 profile["uuid"]="not-a-uuid";reset({{"apps",json::array({profile,normal})}});
 confighttp::purgeAutoSyncedApps(0,std::make_shared<request_t>());
 assert(fixture_http==200&&fixture_response["removed"]==1);assert_retained(fixture_disk,profile);
 std::cout<<"PASS complete provider purge: valid/invalid marked auto profiles retained; only ordinary imports counted; strict marker compatibility\n";
}
void deletion_witness(){
 auto profile=profile_json(profile_id);auto normal=json{{"name","ordinary"},{"uuid",ordinary_id}};
 reset({{"apps",json::array({profile,normal})}});confighttp::deleteApp(0,deletion_request("1"));
 assert(fixture_http==200&&fixture_writes==1&&fixture_refreshes==1);assert(fixture_disk["apps"]==json::array({profile}));
 reset({{"apps",json::array({profile,normal})}});confighttp::deleteApp(0,deletion_request(profile_id));
 assert(fixture_http==200&&fixture_writes==1&&fixture_refreshes==1&&fixture_disk["apps"]==json::array({normal}));
 profile["uuid"]="broken";reset({{"apps",json::array({profile,normal})}});confighttp::deleteApp(0,deletion_request("0"));
 assert(fixture_http==200&&fixture_disk["apps"]==json::array({normal}));
 if(source_uuid_expansion){
  setenv("V7_ORDER_Z",profile_id.c_str(),1);
  const std::string raw=R"v7({"env":{"V7_ORDER_Z":"b1234567-1234-4234-8234-123456789abc","V7_ORDER_A":"$(V7_ORDER_Z)"},"apps":[{"name":"Profile","uuid":"a1234567-1234-4234-8234-123456789abc","remote-monitor-profile":true},{"name":"Ordinary","uuid":"$(V7_ORDER_A)"},{"name":"Unrelated deletion","uuid":"c1234567-1234-4234-8234-123456789abc"}]})v7";
  const auto before=json::parse(raw);reset(before,raw);
  assert(proc::configured_monitor_profile_error_from_file(before["apps"][0],before,raw).empty());
  confighttp::deleteApp(0,deletion_request("2"));
  assert(fixture_http==400&&fixture_writes==0&&fixture_refreshes==0&&fixture_raw==raw&&fixture_disk==before);
  assert(proc::configured_monitor_profile_error_from_file(before["apps"][0],fixture_disk,fixture_raw).empty());unsetenv("V7_ORDER_Z");
 }
 std::cout<<"PASS complete DELETE: individual valid/invalid profile deletion allowed; safe ordinary deletion; source raw-order conflict denies before disk/cache\n";
}
void runtime_witness(){
 auto profile=profile_json(profile_id);const json before={{"apps",json::array({profile})}};reset(before);
 auto collision=before;collision["apps"].push_back({{"name","provider duplicate"},{"uuid",profile_id}});
 bool rejected=false;try{platf::playnite::sync::write_and_refresh_apps(collision,"fixture-apps.json");}catch(const std::invalid_argument&){rejected=true;}
 assert(rejected&&fixture_writes==0&&fixture_refreshes==0&&fixture_disk==before);
 auto safe=before;safe["apps"].push_back({{"name","ordinary"},{"uuid",ordinary_id}});
 platf::playnite::sync::write_and_refresh_apps(safe,"fixture-apps.json");
 assert(fixture_writes==1&&fixture_refreshes==1&&fixture_disk["apps"].size()==2);assert_retained(fixture_disk,profile);
 std::cout<<"PASS complete Playnite runtime publisher: conflicts deny before disk/cache; safe publication writes exactly once\n";
}
void fullscreen_witness(){
 auto profile=profile_json(profile_id);profile["playnite-fullscreen"]=true;profile["name"]="Playnite (Fullscreen)";
 auto normal=profile;normal.erase("remote-monitor-profile");normal["uuid"]=ordinary_id;
 for(bool reverse:{false,true}){
  reset({{"apps",reverse?json::array({normal,profile}):json::array({profile,normal})}});
  config::playnite.enabled=false;config::playnite.auto_sync=false;config::playnite.fullscreen_entry_enabled=false;
  std::unordered_map<std::string,std::string> vars;config::apply_playnite(vars);
  assert(fixture_writes==1&&fixture_refreshes==1&&fixture_disk["apps"]==json::array({profile}));
  reset({{"apps",json::array({profile})}});config::apply_playnite(vars);
  assert(fixture_writes==0&&fixture_refreshes==0&&fixture_disk["apps"]==json::array({profile}));
 }
 std::cout<<"PASS complete Windows config Playnite reconciliation: fullscreen metadata cannot remove marked profiles; ordinary fullscreen cleanup continues\n";
}
'''
TARGET_TESTS=r'''
void reorder_witness(){
 auto profile=profile_json(profile_id);auto normal=json{{"name","ordinary"},{"uuid",ordinary_id}};
 auto duplicate=normal;duplicate["name"]="duplicate";
 reset({{"apps",json::array({profile,normal,duplicate})}});
 auto req=std::make_shared<request_t>();req->content<<json{{"order",json::array({ordinary_id,profile_id})}}.dump();
 confighttp::reorderApps(0,req);
 assert(fixture_http==200&&fixture_writes==1&&fixture_refreshes==1);
 assert(fixture_disk["apps"]==json::array({normal,profile,duplicate}));
 std::cout<<"PASS complete target reorder: selected order and remaining ordinary duplicate order preserved with profile validation\n";
}
void migration_witness(){
 auto profile=profile_json(profile_id);profile["uuid"]="broken";profile["elevated"]="true";profile["exit-timeout"]="10";
 auto normal=json{{"name","legacy ordinary"},{"uuid",ordinary_id},{"elevated","true"}};
 json before={{"version",1},{"apps",json::array({profile,normal})}};reset(before);auto migration=before;
 proc::migrate(migration,"fixture-apps.json");
 assert(fixture_writes==1&&fixture_refreshes==0&&fixture_disk["version"]==2);assert_retained(fixture_disk,profile);
 assert(fixture_disk["apps"][1]["elevated"]==true);
 // Exercise the exact migration publication boundary against a changed candidate:
 // previously-valid profile conflict must be denied before the direct writer.
 profile=profile_json(profile_id);before={{"version",1},{"apps",json::array({profile})}};reset(before);migration=before;
 migration["apps"].push_back({{"name","conflict"},{"uuid",profile_id}});
 bool rejected=false;try{proc::migrate(migration,"fixture-apps.json");}catch(const std::invalid_argument&){rejected=true;}
 assert(rejected&&fixture_writes==0&&fixture_refreshes==0&&fixture_disk==before);
 std::cout<<"PASS complete target runtime/retry migration: unchanged invalid row stays repairable; ordinary normalization persists; conflicting candidate denied before writer\n";
}
void repair_witness(){
 auto invalid=profile_json("broken");invalid["elevated"]="true";
 auto ordinary=json{{"name","ordinary"},{"uuid",ordinary_id}};
 for(const json bad:{json("broken"),json(""),json(nullptr),json(42),json(false)}){
  invalid["uuid"]=bad;reset({{"apps",json::array({invalid,ordinary})}});
  auto input=profile_json(profile_id);input["index"]=0;
  auto req=std::make_shared<request_t>();req->content<<input.dump();confighttp::saveApp(0,req);
  assert(fixture_http==200&&fixture_writes==1&&fixture_refreshes==1&&fixture_disk["apps"].size()==2);
  assert(std::none_of(fixture_disk["apps"].begin(),fixture_disk["apps"].end(),[](const auto& row){return row.value("name",std::string{})=="Custom"&&row["uuid"]!="a1234567-1234-4234-8234-123456789abc";}));
  auto loaded=parse_profile_rows(fixture_disk["apps"]);assert(std::count_if(loaded.begin(),loaded.end(),[](const auto& x){return x.remote_monitor_profile;})==1);
  assert_retained(fixture_disk,ordinary);
 }
 std::cout<<"PASS complete target save: explicit invalid marked index repair replaces one row; no appended duplicate or retained broken row\n";
}
'''
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[2]);p.add_argument('--out',type=Path);p.add_argument('--json-include',type=Path);p.add_argument('--save-exe',type=Path);a=p.parse_args();root=a.root.resolve();code=fixture(root)
 if a.out:a.out.write_text(code)
 else:
  with tempfile.TemporaryDirectory() as temporary:
   cpp=Path(temporary)/'publication.cpp';cpp.write_text(code);exe=a.save_exe or Path(temporary)/'publication'
   subprocess.run(['g++','-std=c++23','-O0','-pthread','-I'+str(root),'-I'+str(root/'src'),*(['-I'+str(a.json_include)] if a.json_include else []),str(root/'src/app_catalog_policy.cpp'),str(root/'src/remote_session.cpp'),str(root/'src/steam_sync_policy.cpp'),str(root/'src/lutris_sync_policy.cpp'),str(cpp),'-lboost_process','-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
