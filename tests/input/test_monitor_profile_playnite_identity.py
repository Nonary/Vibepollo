#!/usr/bin/env python3
"""Actual portable policy and Windows save/runtime bodies with controlled OS leaves."""
from pathlib import Path
import argparse, subprocess, tempfile
from test_monitor_profile_json_identity import fixture
from test_delayed_mouse_release import function

def playnite_fixture(root):
    text=fixture(root,windows=True)
    policy=(root/'src/platform/windows/playnite_sync_policy.cpp').read_text()
    canonical=function(policy,'std::string canonical_playnite_app_uuid(')
    text=text.replace(canonical,'std::string canonical_playnite_app_uuid(std::string_view);',1)
    text=text.replace('#include "src/remote_monitor_profile_policy.h"','#include "src/platform/windows/playnite_sync_policy.h"\n#include "src/remote_monitor_profile_policy.h"',1)
    text=text.replace('namespace file_handler {std::string read_file','namespace file_handler {void write_file(const char*,const std::string& value) {++fixture_writes;fixture_disk=json::parse(value);}\nstd::string read_file',1)
    runtime=(root/'src/platform/windows/playnite_sync.cpp').read_text()
    bodies='namespace platf::playnite::sync {\n'+function(runtime,'bool ensure_runtime_app_uuid(')+function(runtime,'void write_and_refresh_apps(')+'}\n'
    text=text.replace('int main(int argc,char**argv)','int original_main(int argc,char**argv)',1)
    return text+bodies+r"""
int main(int argc,char**argv) {
 original_main(argc,argv);
 namespace policy=platf::playnite::sync::policy;
 const std::string literal="a1234567-1234-4234-8234-123456789abc";
 for(const json value:{json(""),json(nullptr),json(12),json(false),json("not-a-uuid"),json(literal)}) {
  auto marked=profile_json(literal);marked["uuid"]=value;auto preserved=marked;
  assert(!platf::playnite::sync::ensure_runtime_app_uuid(marked)&&marked==preserved);
 }
 auto normal=json{{"name","Normal"}};assert(platf::playnite::sync::ensure_runtime_app_uuid(normal)&&normal["uuid"].is_string()&&!normal["uuid"].get<std::string>().empty());
 auto profile=profile_json(literal);profile["playnite-id"]="provider";profile["playnite-managed"]="auto";
 for(bool installed:{false,true})for(bool reversed:{false,true}) {
  auto ordinary=profile;ordinary.erase("remote-monitor-profile");ordinary["uuid"]="ordinary";
  auto before=reversed?json::array({ordinary,profile,profile}):json::array({profile,profile,ordinary});
  json root={{"apps",before}};platf::playnite::Game game;game.id="provider";game.name="Custom";game.installed=installed;
  bool changed=false;std::size_t matched=0;policy::autosync_reconcile(root,{game},0,0,1,false,true,{},{},{},{},{},true,changed,matched,true);
  assert(std::count(root["apps"].begin(),root["apps"].end(),profile)==2);
 }
 // Autosync can add an ordinary game with a canonical UUID that collides with
 // a marked identity. Actual runtime publication rejects before disk/cache.
 auto marked=profile_json(literal);fixture_environment=json::object();fixture_disk={{"apps",json::array({marked})}};fixture_writes=0;
 auto root=fixture_disk;platf::playnite::Game game;game.id=literal;game.name="Different game";game.installed=true;
 bool changed=false;std::size_t matched=0;policy::autosync_reconcile(root,{game},0,0,0,false,true,{},{},{},{},{},false,changed,matched,true);
 assert(root["apps"].size()==2);bool rejected=false;
 try {platf::playnite::sync::write_and_refresh_apps(root,"fixture");}catch(const std::invalid_argument& error){rejected=!std::string(error.what()).empty();}
 assert(rejected&&fixture_writes==0&&fixture_disk["apps"]==json::array({marked}));
 // Nonconflicting ordinary sync retains the established write/refresh path.
 root={{"apps",json::array({marked,{{"name","Ordinary"},{"uuid","other"}}})}};
 platf::playnite::sync::write_and_refresh_apps(root,"fixture");assert(fixture_writes==1&&fixture_disk==root);
 std::cout<<"PASS actual Playnite policy/runtime bodies: marked UUID generation/canonicalization/match/dedupe/purge preservation; duplicate ordinary publication rejects before disk/cache; ordinary runtime generation and publication compatibility\n";
}
"""

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--save-cpp',type=Path);parser.add_argument('--save-exe',type=Path);parser.add_argument('--json-include',type=Path);args=parser.parse_args()
    root=Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory() as tmp:
        cpp=Path(tmp)/'playnite.cpp';cpp.write_text(playnite_fixture(root))
        if args.save_cpp:args.save_cpp.write_text(cpp.read_text())
        compat=Path(tmp)/'os-time-leaves.h';compat.write_text('#include <ctime>\ninline std::time_t _mkgmtime(std::tm* value){return timegm(value);}\ninline int gmtime_s(std::tm* out,const std::time_t* value){return gmtime_r(value,out)?0:1;}\n')
        exe=args.save_exe or Path(tmp)/'playnite'
        subprocess.run(['g++','-std=c++23','-O0','-pthread','-I'+str(root),'-I'+str(root/'src'),*(['-I'+str(args.json_include)] if args.json_include else []),'-include',str(compat),str(root/'src/app_catalog_policy.cpp'),str(root/'src/remote_session.cpp'),str(root/'src/platform/windows/playnite_sync_policy.cpp'),str(cpp),'-lboost_process','-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
