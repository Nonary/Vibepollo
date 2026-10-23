#!/usr/bin/env python3
"""Compile actual allocation/projection and NVHTTP selection/retention prefixes.

Deterministic app/config adapters; no server, TLS, RTSP or physical display.
"""
from pathlib import Path
import argparse, subprocess, tempfile
from test_delayed_mouse_release import function
PREAMBLE = r"""
#include "src/remote_session.h"
#include "src/app_catalog_policy.h"
#include <cassert>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <optional>
using overrides_t=std::unordered_map<std::string,std::string>;
struct settings_t { overrides_t config_overrides; };
std::optional<settings_t> verified_client;
std::optional<settings_t> get_named_cert_by_uuid(const std::string&) { return verified_client; }
std::optional<settings_t> get_client_snapshot_by_uuid(const std::string&) { return verified_client; }
int fixture_current_app = 0;
namespace proc {
int input_only_app_id = -1, terminate_app_id = -2;
struct ctx_t { std::string uuid; bool remote_monitor_profile{}; overrides_t config_overrides; std::string id; std::string name; std::vector<std::string> id_aliases; std::optional<std::size_t> configured_index; std::optional<display_target::descriptor_t> monitor_target; };
struct process_t { std::vector<ctx_t> apps; std::vector<ctx_t> get_apps() const {return apps;} int current_app_id() const {return fixture_current_app;} bool update_active_app_live_rtx_hdr_overrides(const std::string&) {return true;} std::optional<ctx_t> resolve_app(const std::string& id,const std::string& uuid) {
 std::vector<catalog::app_identity_t> identities;
 for(auto& app:apps)identities.push_back({.uuid=app.uuid,.id=app.id,.aliases=app.id_aliases});
 const auto found=catalog::resolve_app(identities,id,uuid);
 return found?std::make_optional(apps[*found]):std::nullopt;
} } proc;
}
namespace util { int from_view(const std::string& s) { return std::stoi(s); } }
int merge_calls=0,tail_reached=0,status=0;
namespace config { void merge_config_overrides(overrides_t& dest,const overrides_t& src) { ++merge_calls; for(auto& [k,v]:src)dest[k]=v; } }
struct tree_t { template<class T> void put(const std::string& key,const T& value) { if constexpr(std::is_integral_v<T>) { if(key=="root.<xmlattr>.status_code") status=value; } } } tree;
struct identity_t { std::string uuid="paired-owner"; } identity,request_client_identity;

remote_session::control_e last_control; remote_session::dispatch_t last_decision;
"""
TESTS = r"""
void retained_regression() {
 const auto builtin=remote_session::synthetic(remote_session::control_e::monitor);
 const std::string witness="00211013-3213-4121-8110-000000000000";
 proc::catalog::app_identity_t app{.name="Remote Monitor",.uuid=witness,.art_version="default"};
 assert(std::get<0>(proc::catalog::calculate_versioned_ids(witness,"default",0))==std::to_string(builtin.id));
 std::set<std::string> occupied,active;std::map<std::string,proc::catalog::alias_state_t> persisted;bool changed=false;
 proc::catalog::assign_compatible_id(app,0,occupied,persisted,active,changed);
 proc::proc.apps.push_back({app.uuid,true,{{"output_name","accepted-custom"}},app.id,app.name});
 proc::proc.apps.push_back({builtin.uuid,false,{{"output_name","builtin-before"}},std::to_string(builtin.id),builtin.title});
 remote_session::caller_t caller{.uuid="paired-owner",.paired=true,.may_view=true,.may_launch=true,.may_terminate=true};
 auto projected=remote_session::project(caller,{}, {},{{std::stoi(app.id),app.uuid,app.name,false,true}},false);
 assert(projected.catalogue.front().uuid==witness);
 const auto tile=projected.catalogue.front();
 verified_client=settings_t{{{"adapter_name","client-current"}}};
 admit_id(std::to_string(tile.id),tile.uuid);
 assert(status==200 && selected && selected->uuid==witness && merged.at("output_name")=="accepted-custom" && merged.at("adapter_name")=="client-current");
 assert(tile.id!=builtin.id && remote_session::identify(tile.id,tile.uuid)==remote_session::control_e::none);
 remember_remote_owner("paired-owner",remote_session::role_e::monitor,41,selected);
 proc::proc.apps[0].config_overrides["output_name"]="edited-custom";
 admit_id(std::to_string(remote_session::resume_id),"");
 assert(status==200 && selected->uuid==witness && merged.at("output_name")=="accepted-custom");
 proc::proc.apps.erase(proc::proc.apps.begin());
 verified_client->config_overrides["adapter_name"]="client-updated";
 admit_id(std::to_string(remote_session::resume_id),"");
 assert(status==200 && selected->uuid==witness && merged.at("output_name")=="accepted-custom" && merged.at("adapter_name")=="client-updated");
 admit_id(std::to_string(builtin.id),builtin.uuid);
 assert(status==409 && merge_calls==0 && tail_reached==0);
 forget_remote_owner("paired-owner",remote_session::role_e::monitor,40);
 assert(retained_monitor_profile("paired-owner")->uuid==witness);
 forget_remote_owner("paired-owner",remote_session::role_e::monitor,41);
 admit_id(std::to_string(builtin.id),builtin.uuid);
 assert(status==200 && selected->uuid==builtin.uuid && merged.at("output_name")=="builtin-before");
 remember_remote_owner("paired-owner",remote_session::role_e::monitor,42,selected);
 proc::proc.apps[0].config_overrides["output_name"]="builtin-after";
 admit_id(std::to_string(remote_session::resume_id),"");
 assert(status==200 && selected->uuid==builtin.uuid && merged.at("output_name")=="builtin-after");
 proc::catalog::app_identity_t ordinary{.name="Ordinary",.uuid="ordinary",.art_version="default"};
 proc::catalog::assign_compatible_id(ordinary,1,occupied,persisted,active,changed);
 proc::proc.apps.push_back({ordinary.uuid,false,{},ordinary.id,ordinary.name});
 admit_id(ordinary.id,ordinary.uuid);assert(status==299 && !selected && merge_calls==0);
 assert(remote_session::identify(remote_session::monitor_id,builtin.uuid)==remote_session::control_e::monitor);
 std::cout<<"PASS actual allocator -> projection -> identify/configured lookup -> production retained-profile launch prefix: witness "<<witness<<" remapped to "<<app.id<<"; edit/deletion Resume; current client merge; builtin conflict; stale generation; builtin live layer; normal app continuation\n";
}

void clear_admission() { remote_role_owners.clear(); proc::proc.apps.clear(); }
void install(const proc::catalog::app_identity_t& app,bool profile=true) {
 proc::proc.apps.push_back({app.uuid,profile,{{"output_name",app.uuid}},app.id,app.name,app.aliases});
}
remote_session::projection_t projection(int current=0) {
 auto configured=projected_configured(current);
 remote_session::game_t game{.running=current>0,.owner_uuid="other-client",.generation=1,.app={}};
 for(const auto& row:configured)if(row.id==current)game.app=row;
 remote_session::caller_t caller{.uuid="paired-owner",.paired=true,.may_view=true,.may_launch=true,.may_terminate=true};
 return remote_session::project(caller,game,{},configured,current>0);
}
void admit_profile(const std::string& id,const std::string& uuid,int current,const std::string& expected) {
 remote_role_owners.clear();admit_id(id,uuid,current);
 assert(last_control==remote_session::control_e::monitor && !last_decision.resume);
 assert(status==200 && selected && selected->uuid==expected && merged.at("output_name")==expected);
 remember_remote_owner("paired-owner",remote_session::role_e::monitor,100,selected);
 admit_id(std::to_string(remote_session::resume_id),"",current);
 assert(status==200 && selected && selected->uuid==expected);
}
int main() {
 retained_regression();
 const std::vector<std::pair<int,std::string>> witnesses{
  {2147483601,"02113232-3321-4330-8110-000000000000"},
  {2147483602,"31332122-3011-4022-8100-000000000000"},
  {2147483603,"20323020-0201-4131-8110-000000000000"},
  {2147483604,"31213120-0033-4220-8010-000000000000"},
  {2147483605,"20202022-3223-4333-8000-000000000000"},
  {2147483606,"13023332-3113-4021-8010-000000000000"}};
 for(const auto& [legacy,uuid]:witnesses)for(bool persisted_current:{false,true}) {
  clear_admission();proc::catalog::app_identity_t app{.name="Profile",.uuid=uuid,.art_version="default"};
  assert(std::get<0>(proc::catalog::calculate_versioned_ids(uuid,"default",0))==std::to_string(legacy));
  std::set<std::string> occupied,active;std::map<std::string,proc::catalog::alias_state_t> state;bool changed=false;
  if(persisted_current)state[uuid]={std::to_string(legacy),"default",{"123",std::to_string(legacy)}};
  proc::catalog::assign_compatible_id(app,0,occupied,state,active,changed);
  std::vector<proc::catalog::app_identity_t> apps{app};proc::catalog::prune_and_filter_aliases(apps,state,active,changed);app=apps[0];
  assert(std::stoi(app.id)!=legacy && remote_session::identify(std::stoi(app.id))==remote_session::control_e::none);
  assert(std::find(app.aliases.begin(),app.aliases.end(),std::to_string(legacy))==app.aliases.end());
  install(app);const auto projected=projection();const auto tile=projected.catalogue.front();assert(tile.uuid==uuid);
  admit_profile(std::to_string(tile.id),tile.uuid,0,uuid);
  admit_profile(std::to_string(tile.id),"",0,uuid);
  if(persisted_current)admit_profile("123","",0,uuid);
  assert(remote_session::identify(legacy)==static_cast<remote_session::control_e>(legacy-2147483600));
 }
 clear_admission();
 proc::catalog::app_identity_t active_app{.name="Game",.uuid="11111111-1111-4111-8111-111111111111",.art_version="default"};
 proc::catalog::app_identity_t profile{.name="Profile",.uuid="00000000-0000-4000-8000-0000198ad951",.art_version="default"};
 std::set<std::string> occupied,active;std::map<std::string,proc::catalog::alias_state_t> state;bool changed=false;
 proc::catalog::assign_compatible_id(active_app,0,occupied,state,active,changed);
 proc::catalog::assign_compatible_id(profile,1,occupied,state,active,changed);
 std::vector<proc::catalog::app_identity_t> apps{active_app,profile};proc::catalog::prune_and_filter_aliases(apps,state,active,changed);active_app=apps[0];profile=apps[1];
 assert(active_app.id=="1779010096" && profile.id=="705268272");
 install(active_app,false);install(profile);auto projected=projection(std::stoi(active_app.id));
 const auto image_ctx=asset_context(profile.id,remote_session::synthetic_uuid(remote_session::control_e::running_game),std::stoi(active_app.id));assert(!image_ctx);
 fixture_current_app=std::stoi(active_app.id);
 assert(!launch_context(profile.id,remote_session::synthetic_uuid(remote_session::control_e::running_game)));
 assert(launch_context(profile.id,profile.uuid)->uuid==profile.uuid);
 std::set<int> ids;for(const auto& row:projected.catalogue)assert(ids.insert(row.id).second);
 assert(projected.catalogue.front().id!=std::stoi(profile.id));
 admit_profile(profile.id,profile.uuid,std::stoi(active_app.id),profile.uuid);
 admit_profile(profile.id,"",std::stoi(active_app.id),profile.uuid);
 // Explicit configured UUIDs retain their meaning even with a cached contextual ID.
 admit_profile(std::to_string(projected.catalogue.front().id),profile.uuid,std::stoi(active_app.id),profile.uuid);
 remote_role_owners.clear();admit_id(std::to_string(projected.catalogue.front().id),"",std::stoi(active_app.id));
 assert(status==298 && last_decision.resume && last_decision.resume_role==remote_session::role_e::game);
 // A current alias, rather than current ID, can occupy the previous running ID.
 clear_admission();profile.aliases.push_back(profile.id);profile.id="77";install(active_app,false);install(profile);
 projected=projection(std::stoi(active_app.id));assert(projected.catalogue.front().id!=705268272);
 admit_profile("705268272","",std::stoi(active_app.id),profile.uuid);
 // Actual target visibility adapter reserves hidden app identities too.
 const auto hidden_catalogue=projected_configured(std::stoi(active_app.id),true);
 const auto hidden_profile=std::find_if(hidden_catalogue.begin(),hidden_catalogue.end(),[&](const auto& row){return row.uuid==profile.uuid;});
 assert(hidden_profile!=hidden_catalogue.end() && !hidden_profile->catalogue_visible);
 remote_session::caller_t secondary{.uuid="secondary",.paired=true,.may_view=true,.may_launch=true};
 remote_session::game_t hidden_game{.running=true,.owner_uuid="other",.app=hidden_catalogue.front()};
 const auto hidden_projection=remote_session::project(secondary,hidden_game,{},hidden_catalogue,true);
 assert(hidden_projection.catalogue.front().id!=705268272);
 // Unambiguous old running IDs remain numerically stable and resume normally.
 clear_admission();install(active_app,false);projected=projection(std::stoi(active_app.id));assert(projected.catalogue.front().id==705268272);
 admit_id("705268272","",std::stoi(active_app.id));assert(status==298 && last_decision.resume);
 std::cout<<"PASS all six legacy fresh/persisted IDs, aliases/cached IDs, actual dynamic current/alias collision, projection uniqueness, UUID/numeric selection, dispatch, retained profile and unambiguous running compatibility\n";
}
"""
def fixture(root):
 s=(root/'src/nvhttp.cpp').read_text()
 a=s.index('      const auto retained_profile = retained_monitor_profile(');z=s.index('      if (!no_active_sessions &&',a);block=s[a:z]
 a=s.index('auto synthetic_control =',s.index('auto appid_str ='));z=s.index('if (selected_monitor_profile)',a);z=s.index(';',z)+1;routing=s[a:z]
 struct=function(s,'struct remote_role_owner_t {')+';'
 funcs='\n'.join(function(s,x) for x in ['std::string remote_role_owner_key(','std::optional<remote_session::monitor_profile_t> retained_monitor_profile(','void remember_remote_owner(','bool forget_remote_owner(','std::vector<remote_session::app_t> remote_configured_catalogue('])
 fn="""
std::optional<remote_session::monitor_profile_t> selected; overrides_t merged;
void admit_id(const std::string& appid_str,const std::string& appuuid_str,int current_appid=0) {
 status=0;merge_calls=0;tail_reached=0;selected.reset();merged.clear();
"""+routing+"""
 last_control=synthetic_control;
 if(synthetic_control==remote_session::control_e::none) { status=299;return; }
 remote_session::caller_t caller{.uuid="paired-owner",.paired=true,.may_view=true,.may_launch=true,.may_terminate=true};
 remote_session::game_t game{.running=current_appid>0};
 const remote_session::owner_t owner{.role=retained_monitor_profile(identity.uuid) ? remote_session::role_e::monitor : remote_session::role_e::none};
 last_decision=remote_session::dispatch(caller,game,owner,synthetic_control);
 if(!last_decision.allowed){status=409;return;}
 if(synthetic_control==remote_session::control_e::running_game){status=298;return;}
"""+block+"""
 selected=monitor_profile;merged=requested_runtime_overrides;++tail_reached;status=200;
}
"""
 a=s.index('const auto synthetic_control =',s.index('void appasset('));z=s.index('    std::string app_image;',a)
 asset=s[a:z]
 a=s.index('const auto launch_identity_control =');z=s.index('launch_session->client_display_warning_requested',a)
 launch=s[a:z]
 adapters='std::optional<proc::ctx_t> asset_context(const std::string& appid,const std::string& appuuid,int current_appid) {'+asset+'return app_ctx;}\n'
 adapters+='std::optional<proc::ctx_t> launch_context(const std::string& launch_appid_arg,const std::string& launch_appuuid_arg) {'+launch+'return launch_app_ctx;}\n'
 if 'std::vector<const proc::ctx_t *> visible_apps;' in s:
  a=s.index('        std::vector<const proc::ctx_t *> visible_apps;',s.index('void applist('));z=s.index('        const auto current_app =',a)
  adapters+='std::vector<remote_session::app_t> projected_configured(int current_appid,bool should_hide_inactive_apps=false) {const auto app_list=proc::proc.get_apps();const bool show_terminate_entry=should_hide_inactive_apps;'+s[a:z]+'return configured_apps;}\n'
 else:
  adapters+='std::vector<remote_session::app_t> projected_configured(int,bool=false) {return remote_configured_catalogue(proc::proc.get_apps());}\n'
 return PREAMBLE+struct+'\nstd::mutex remote_role_owners_mutex;std::unordered_map<std::string,remote_role_owner_t> remote_role_owners;\n'+funcs+adapters+fn+TESTS
if __name__=='__main__':
 parser=argparse.ArgumentParser();parser.add_argument('--save-cpp',type=Path);args=parser.parse_args()
 root=Path(__file__).resolve().parents[2]
 with tempfile.TemporaryDirectory() as tmp:
  cpp=Path(tmp)/'identity.cpp';cpp.write_text(fixture(root))
  if args.save_cpp:args.save_cpp.write_text(cpp.read_text())
  exe=Path(tmp)/'identity'
  subprocess.run(['g++','-std=c++23','-O0','-pthread','-I'+str(root),'-I'+str(root/'src'),str(root/'src/app_catalog_policy.cpp'),str(root/'src/remote_session.cpp'),str(cpp),'-o',str(exe)],check=True)
  subprocess.run([str(exe)],check=True)
