#!/usr/bin/env python3
"""Actual complete save→maintenance DELETE and duplicate/no-op policy witnesses.

Deterministic HTTP/auth/file/cache leaves; actual source ordered effective UUID
policy and target literal identity helpers. Not a real HTTP/TLS host, full
process parser, provider discovery or native device test.
"""
from pathlib import Path
import argparse,subprocess,tempfile,sys
sys.dont_write_bytecode=True

def fixture(root):
 sys.path.insert(0,str(root/'tests/input'))
 for name in ['test_monitor_profile_publication','test_monitor_profile_provider_identity','test_monitor_profile_json_identity','test_monitor_profile_catalog_identity','test_delayed_mouse_release']:sys.modules.pop(name,None)
 import test_monitor_profile_publication as publication
 code=publication.fixture(root).replace('int main(int argc,char**argv){','int old_publication_main(int argc,char**argv){',1)
 code=code.replace('bool authenticate(int,req_https_t){return true;}','bool fixture_authenticated=true; bool authenticate(int,req_https_t){return fixture_authenticated;}')
 sys.path.pop(0)
 return code+TESTS
TESTS=r'''
auto maintenance_request(const std::string& uuid,const json& flag=true){
 auto req=deletion_request(uuid);req->content<<json{{"provider-maintenance",flag}}.dump();return req;
}
void assert_no_write(const json& before){assert(fixture_writes==0&&fixture_refreshes==0&&fixture_disk==before);}
void maintenance_race(){
 auto ordinary=json{{"name","Previously ordinary import"},{"uuid",profile_id},{"playnite-id","provider-game"},{"playnite-managed","auto"}};
 reset({{"apps",json::array({ordinary})}});const auto stale_uuid=ordinary["uuid"].get<std::string>();
 auto profile=profile_json(profile_id);profile["playnite-id"]="provider-game";profile["playnite-managed"]="auto";
 auto save=std::make_shared<request_t>();save->content<<profile.dump();confighttp::saveApp(0,save);
 assert(fixture_http==200&&fixture_writes==1&&fixture_refreshes==1&&fixture_disk["apps"].size()==1);
 assert(proc::configured_monitor_profile_error_from_file(fixture_disk["apps"][0],fixture_disk,fixture_raw).empty());
 const auto after_save=fixture_disk;fixture_writes=fixture_refreshes=fixture_http=0;
 confighttp::deleteApp(0,maintenance_request(stale_uuid));
 assert(fixture_http==200&&fixture_response["removed"]==0);assert_no_write(after_save);
 // Deliberate ordinary API deletion still deletes the now-marked profile.
 confighttp::deleteApp(0,deletion_request(stale_uuid));
 assert(fixture_http==200&&fixture_writes==1&&fixture_refreshes==1&&fixture_disk["apps"].empty());
 std::cout<<"PASS actual intervening save then maintenance delete: latest valid marked row retained with removed0/no write; deliberate empty-body delete still allowed\n";
}
void maintenance_duplicates(){
 for(const std::string id:{profile_id,std::string("broken-profile-id")})for(bool marked_first:{false,true}){
  auto profile=profile_json(id);auto ordinary=json{{"name","ordinary import"},{"uuid",id},{"playnite-id","game"},{"playnite-managed","auto"}};
  auto second=ordinary;second["name"]="ordinary peer";
  const json before={{"apps",marked_first?json::array({profile,ordinary,second}):json::array({ordinary,second,profile})}};reset(before);
  assert(!proc::configured_monitor_profile_error_from_file(profile,before,before.dump()).empty());
  confighttp::deleteApp(0,maintenance_request(id));
  assert(fixture_http==200&&fixture_writes==1&&fixture_refreshes==1&&fixture_response["removed"]==2);
  assert(fixture_disk["apps"]==json::array({profile}));
  const auto after=fixture_disk;fixture_writes=fixture_refreshes=0;
  confighttp::deleteApp(0,maintenance_request(id));assert(fixture_http==200&&fixture_response["removed"]==0);assert_no_write(after);
 }
 // Every strict marker is protected, independently of its invalid UUID JSON type.
 for(const json bad:{json(nullptr),json(42),json(false),json(""),json("broken")}){
  auto profile=profile_json(profile_id);profile["uuid"]=bad;
  auto ordinary=json{{"name","ordinary"},{"uuid",ordinary_id}};const json before={{"apps",json::array({profile,ordinary})}};reset(before);
  confighttp::deleteApp(0,maintenance_request(ordinary_id));
  assert(fixture_http==200&&fixture_response["removed"]==1&&fixture_disk["apps"]==json::array({profile}));
 }
 // Marker typing and ordinary duplicates remain ordinary cleanup semantics.
 for(const json marker:{json(false),json("true"),json(1),json(nullptr)}){
  auto row=json{{"name","ordinary typed marker"},{"uuid",ordinary_id},{"remote-monitor-profile",marker}};
  reset({{"apps",json::array({row,row})}});confighttp::deleteApp(0,maintenance_request(ordinary_id));
  assert(fixture_http==200&&fixture_response["removed"]==2&&fixture_disk["apps"].empty());
 }
 std::cout<<"PASS maintenance duplicate orders: all current ordinary sameUUID peers removed/count2; valid/invalid strict-marked peers retained; repeats removed0/no-write; nonboolean markers ordinary\n";
}
void maintenance_controls(){
 auto profile=profile_json(profile_id);const json before={{"apps",json::array({profile})}};
 for(const json bad:{json("true"),json(1),json(nullptr),json::array(),json::object()}){
  reset(before);confighttp::deleteApp(0,maintenance_request(profile_id,bad));assert(fixture_http==400);assert_no_write(before);
 }
 reset(before);confighttp::deleteApp(0,maintenance_request(profile_id,false));
 assert(fixture_http==200&&fixture_writes==1&&fixture_refreshes==1&&fixture_disk["apps"].empty());
 reset(before);confighttp::deleteApp(0,maintenance_request("missing"));assert(fixture_http==200&&fixture_response["removed"]==0);assert_no_write(before);
 // No stale UUID may silently become a numeric-index maintenance deletion.
 auto ordinary=json{{"name","index1 ordinary"},{"uuid",ordinary_id}};
 const json indexed={{"apps",json::array({profile,ordinary})}};reset(indexed);confighttp::deleteApp(0,maintenance_request("1"));
 assert(fixture_http==200&&fixture_response["removed"]==0);assert_no_write(indexed);
 reset(indexed);confighttp::deleteApp(0,deletion_request("1"));assert(fixture_http==200&&fixture_disk["apps"]==json::array({profile}));
 const auto builtin=remote_session::synthetic(remote_session::control_e::monitor);const json controls={{"apps",json::array({{{"name",builtin.title},{"uuid",builtin.uuid}}})}};
 reset(controls);confighttp::deleteApp(0,maintenance_request(builtin.uuid));assert(fixture_http==200&&fixture_response["removed"]==0);assert_no_write(controls);
 reset(before);fixture_authenticated=false;confighttp::deleteApp(0,maintenance_request(profile_id));assert_no_write(before);fixture_authenticated=true;
 // Preserve each product's deliberately existing duplicate-UUID delete selection.
 for(bool marked_first:{false,true}){
  auto ordinary=json{{"name","deliberate ordinary peer"},{"uuid",profile_id}};
  reset({{"apps",marked_first?json::array({profile,ordinary}):json::array({ordinary,profile})}});
  confighttp::deleteApp(0,deletion_request(profile_id));assert(fixture_http==200);
  if(source_uuid_expansion)assert(fixture_disk["apps"]==json::array({marked_first?ordinary:profile}));else assert(fixture_disk["apps"].empty());
 }
 std::cout<<"PASS strict malformed flag denial; false/empty-body deliberate semantics; missing/numeric-literal/builtin/auth controls; existing source first-match/target all-match deliberate deletion\n";
}
void maintenance_raw_order(){
 if(!source_uuid_expansion)return;
 setenv("V8_ORDER_Z",profile_id.c_str(),1);
 const std::string raw=R"v8({"env":{"V8_ORDER_Z":"b1234567-1234-4234-8234-123456789abc","V8_ORDER_A":"$(V8_ORDER_Z)"},"apps":[{"name":"Profile","uuid":"a1234567-1234-4234-8234-123456789abc","remote-monitor-profile":true},{"name":"Ordinary","uuid":"$(V8_ORDER_A)"},{"name":"Unrelated cleanup","uuid":"c1234567-1234-4234-8234-123456789abc"}]})v8";
 const auto before=json::parse(raw);reset(before,raw);assert(proc::configured_monitor_profile_error_from_file(before["apps"][0],before,raw).empty());
 confighttp::deleteApp(0,maintenance_request(spare_id));assert(fixture_http==400);assert_no_write(before);assert(fixture_raw==raw);
 reset(before,raw);confighttp::deleteApp(0,maintenance_request(profile_id));assert(fixture_http==200&&fixture_response["removed"]==0);assert_no_write(before);assert(fixture_raw==raw);
 unsetenv("V8_ORDER_Z");std::cout<<"PASS maintenance raw-source ordering guard denies normalization conflict before write; marked-only no-op preserves original raw bytes\n";
}
int main(int argc,char**argv){std::string which=argc>1?argv[1]:"all";
 if(which=="all"||which=="race")maintenance_race();
 if(which=="all"||which=="duplicates")maintenance_duplicates();
 if(which=="all"||which=="controls")maintenance_controls();
 if(which=="all"||which=="raw")maintenance_raw_order();
}
'''
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[2]);p.add_argument('--out',type=Path);p.add_argument('--json-include',type=Path);p.add_argument('--save-exe',type=Path);a=p.parse_args();root=a.root.resolve();code=fixture(root)
 if a.out:a.out.write_text(code)
 else:
  with tempfile.TemporaryDirectory() as tmp:
   cpp=Path(tmp)/'maintenance.cpp';cpp.write_text(code);exe=a.save_exe or Path(tmp)/'maintenance';subprocess.run(['g++','-std=c++23','-O0','-pthread','-I'+str(root),'-I'+str(root/'src'),*(['-I'+str(a.json_include)] if a.json_include else []),str(root/'src/app_catalog_policy.cpp'),str(root/'src/remote_session.cpp'),str(root/'src/steam_sync_policy.cpp'),str(root/'src/lutris_sync_policy.cpp'),str(cpp),'-lboost_process','-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
