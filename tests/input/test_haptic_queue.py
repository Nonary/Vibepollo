#!/usr/bin/env python3
"""Compile actual raw feedback forwarding and mailbox code with host-only slot fakes.

Set AUDIT_JSON_INCLUDE to nlohmann/json's include directory. --baseline reads HEAD.
The synthesized rumble path is covered by test_vhf_gamepad_policy.cpp separately.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[2]
def read(name):
    return subprocess.check_output(['git','show','HEAD:'+name],cwd=ROOT,text=True) if '--baseline' in sys.argv else (ROOT/name).read_text()
s=read('src/platform/windows/vhf_gamepad.cpp')
start=s.index('  void vhf_gamepad_t::impl_t::raise_steam_haptic(')
end=s.index('    const std::uint8_t sides =',start)
body=s[start:end].replace('vhf_gamepad_t::impl_t::','')+'  }\n'
common=(ROOT/'src/platform/common.h').read_text()
types=common[common.index('  enum class gamepad_feedback_e'):common.index('  using feedback_queue_t')]
policy=common
def block(text,start):
    pos=text.index('{',start);depth=1;end=pos+1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]
parser=block(policy,policy.index('inline unsigned steam_haptic_stop_key'))
helpers=block(common,common.index('  inline unsigned gamepad_feedback_state_key'))+'\n'+block(common,common.index('  inline bool raise_gamepad_feedback'))
if '--baseline' in sys.argv:
    helpers=block(common,common.index('  inline unsigned gamepad_feedback_state_key'))+'''\ninline bool raise_gamepad_feedback(safe::queue_t<gamepad_feedback_msg_t>& q,gamepad_feedback_msg_t m){q.raise(std::move(m));return true;}\n''' 
code=r'''
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <tuple>
#include <set>
#include "thread_safe_test.h"
using namespace std::literals;
#define BOOST_LOG(level) std::clog
constexpr int LI_CCAP_STEAM_HAPTIC=1;
namespace config { struct { bool forward_rumble=true; } input; }
TYPES
PARSER
HELPERS
namespace vhf_gamepad {struct steam_haptic_t {std::uint8_t length;std::array<std::uint8_t,12> report;}; }
struct slot_t {bool active=true;unsigned client_capabilities=1;std::uint16_t client_relative_index=0;
 std::shared_ptr<safe::queue_t<gamepad_feedback_msg_t>> feedback_queue;};
std::array<slot_t,16> slots;
BODY
int main(){int failures=0;
 auto check=[&](bool ok,const char* msg){std::cout<<(ok?"PASS ":"FAIL ")<<msg<<'\n';failures+=!ok;};
 auto queue=std::make_shared<safe::queue_t<gamepad_feedback_msg_t>>(256);
 for(int i=0;i<16;i++){slots[i].client_relative_index=i;slots[i].feedback_queue=queue;}
 auto send=[&](int id,int type,int side,bool stop){
   vhf_gamepad::steam_haptic_t h {12,{}};h.report[0]=type;h.report[1]=side;
   if(!stop){h.report[2]=3;h.report[4]=1;h.report[6]=1;h.report[7]=1;}
   raise_steam_haptic(id,h,std::chrono::steady_clock::now());
 };
 config::input.forward_rumble=false;send(0,0x80,0,false);
 check(!queue->pop(0ms),"forward_rumble off withholds Steam haptics from the client");config::input.forward_rumble=true;
 send(0,0x80,0,false);send(0,0x82,0,false);send(0,0x80,0,true);
 auto a=queue->pop(0ms),b=queue->pop(0ms);
 check(a && b && a->data.steam_haptic.report[0]==0x82 && b->data.steam_haptic.report[0]==0x80,
       "new rumble stop follows the older effect after coalescing");
 queue->reset();
 send(0,0x82,0,true);for(int i=0;i<255;i++)send(1,0x81,0,false);send(0,0x82,1,true);
 std::set<unsigned> stops;
 while(auto m=queue->pop(0ms))if(m->id==0 && m->data.steam_haptic.report[0]==0x82 && m->data.steam_haptic.report[2]==0)stops.insert(m->data.steam_haptic.report[1]);
 check(stops.size()==2,"full queue accepts new side stop and preserves the other side stop");
 for(int i=0;i<32;i++)send(15,0x81,0,false);
 for(int id=0;id<16;id++){
   send(id,0x80,0,true);
   for(int type:{0x81,0x82})for(int side=0;side<3;side++)send(id,type,side,true);
 }
 for(int id=0;id<16;id++) {
   raise_gamepad_feedback(*queue,gamepad_feedback_msg_t::make_rumble(id,0,0));
   raise_gamepad_feedback(*queue,gamepad_feedback_msg_t::make_rumble_triggers(id,0,0));
   raise_gamepad_feedback(*queue,gamepad_feedback_msg_t::make_rgb_led(id,12,34,56));
   for(int motion:{1,2})raise_gamepad_feedback(*queue,gamepad_feedback_msg_t::make_motion_event_state(id,motion,100));
   for(int flags:{4,8,12})raise_gamepad_feedback(*queue,gamepad_feedback_msg_t::make_adaptive_triggers(id,flags,0,0,{},{}));
 }
 for(int i=0;i<1000;i++)send(15,0x81,0,false);
 std::set<std::tuple<int,int,int>> keys;std::set<std::pair<int,unsigned>> states;unsigned events=0,total=0;
 while(auto m=queue->pop(0ms)){
   ++total;auto key=gamepad_feedback_state_key(*m);if(key)states.emplace(m->id,key);else ++events;
   if(m->type!=gamepad_feedback_e::steam_haptic)continue;
   auto &h=m->data.steam_haptic;
   if(steam_haptic_stop_key(h.report.data(),h.length))keys.emplace(m->id,h.report[0],h.report[1]);
 }
 check(keys.size()==112,"all 112 stop keys survive sustained event overload");
 check(states.size()==240 && events<=32 && total<=256,"mixed traffic retains every controller state with a bounded fresh event backlog");
 return failures!=0;
}
'''.replace('TYPES',types).replace('PARSER',parser).replace('HELPERS',helpers).replace('BODY',body)
with tempfile.TemporaryDirectory(prefix='host-haptics-') as directory:
    work=Path(directory);(work/'thread_safe_test.h').write_text(read('src/thread_safe.h'));(work/'test.cpp').write_text(code)
    args=['g++','-std=c++20','-fsanitize=address,undefined','-g','-I'+str(ROOT/'src')]
    if os.getenv('AUDIT_JSON_INCLUDE'):args+=['-I'+os.environ['AUDIT_JSON_INCLUDE']]
    subprocess.run(args+[str(work/'test.cpp'),'-pthread','-o',str(work/'test')],check=True)
    result=subprocess.run([str(work/'test')],capture_output=True,text=True)
    print(result.stdout,end='')
    if result.returncode:print(result.stderr[:4000],file=sys.stderr)
    raise SystemExit(result.returncode)
