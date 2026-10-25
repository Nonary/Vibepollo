#!/usr/bin/env python3
"""Compile the production Steam settings forwarding with host-only slot fakes.

The driver forwards Steam's SET_SETTINGS_VALUES / LOAD_DEFAULT_SETTINGS writes as
steam_settings events; raise_steam_settings hands them to a client that advertised
LI_CCAP_STEAM_SETTINGS, whole and in order, as events the mailbox never coalesces.
Set AUDIT_JSON_INCLUDE to nlohmann/json's include directory. --baseline reads HEAD.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[2]
def read(name):
    return subprocess.check_output(['git','show','HEAD:'+name],cwd=ROOT,text=True) if '--baseline' in sys.argv else (ROOT/name).read_text()
def block(text,start):
    pos=text.index('{',start);depth=1;end=pos+1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]
s=read('src/platform/windows/vhf_gamepad.cpp')
settings_body=block(s,s.index('  void vhf_gamepad_t::impl_t::raise_steam_settings(')).replace('vhf_gamepad_t::impl_t::','')
haptic_start=s.index('  void vhf_gamepad_t::impl_t::raise_steam_haptic(')
haptic_body=s[haptic_start:s.index('    const std::uint8_t sides =',haptic_start)].replace('vhf_gamepad_t::impl_t::','')+'  }\n'
common=read('src/platform/common.h')
types=common[common.index('  enum class gamepad_feedback_e'):common.index('  using feedback_queue_t')]
parser=block(common,common.index('inline unsigned steam_haptic_stop_key'))
helpers=block(common,common.index('  inline unsigned gamepad_feedback_state_key'))+'\n'+block(common,common.index('  inline bool raise_gamepad_feedback'))
code=r"""
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <vector>
#include "thread_safe_test.h"
#include "controller_speaker_pcm.h"
using namespace std::literals;
#define BOOST_LOG(level) std::clog
constexpr int LI_CCAP_STEAM_HAPTIC=1;
constexpr int LI_CCAP_STEAM_SETTINGS=2;
namespace config { struct { bool forward_rumble=true; } input; }
@TYPES@
@PARSER@
@HELPERS@
namespace vhf_gamepad {
struct steam_haptic_t {std::uint8_t length;std::array<std::uint8_t,12> report;};
struct steam_settings_t {std::uint8_t command;std::uint8_t count;std::array<std::uint8_t,27> triples;};
}
struct slot_t {bool active=true;unsigned client_capabilities=3;std::uint16_t client_relative_index=0;
 std::shared_ptr<safe::queue_t<gamepad_feedback_msg_t>> feedback_queue;};
std::array<slot_t,16> slots;
@HAPTIC_BODY@
@SETTINGS_BODY@
int main(){int failures=0;
 auto check=[&](bool ok,const char* msg){std::cout<<(ok?"PASS ":"FAIL ")<<msg<<'\n';failures+=!ok;};
 auto queue=std::make_shared<safe::queue_t<gamepad_feedback_msg_t>>(256);
 for(int i=0;i<16;i++){slots[i].client_relative_index=i;slots[i].feedback_queue=queue;}
 auto settings=[&](int id,int command,int count,int first){
   vhf_gamepad::steam_settings_t w {};w.command=command;w.count=count;
   for(int i=0;i<count;i++){w.triples[i*3]=first+i;w.triples[i*3+1]=0x34;w.triples[i*3+2]=0x12;}
   raise_steam_settings(id,w);
 };
 auto haptic=[&](int id,int type,int side,bool stop){
   vhf_gamepad::steam_haptic_t h {12,{}};h.report[0]=type;h.report[1]=side;
   if(!stop){h.report[2]=3;h.report[4]=1;h.report[6]=1;h.report[7]=1;}
   raise_steam_haptic(id,h,std::chrono::steady_clock::now());
 };
 slots[1].client_capabilities=LI_CCAP_STEAM_HAPTIC;settings(1,0x87,1,9);
 check(!queue->pop(0ms),"a client without LI_CCAP_STEAM_SETTINGS gets no settings write");
 slots[2].active=false;settings(2,0x87,1,9);
 check(!queue->pop(0ms),"an inactive slot gets no settings write");
 config::input.forward_rumble=false;settings(0,0x8E,0,0);
 auto m=queue->pop(0ms);config::input.forward_rumble=true;
 check(m && m->type==gamepad_feedback_e::steam_settings && m->id==0 && m->data.steam_settings.command==0x8E && m->data.steam_settings.count==0,
       "load-defaults is forwarded even with forward_rumble off (it is not rumble)");
 // A 20-triple write arrives as three chunks around haptic traffic: all three survive, in order,
 // and a chunk never replaces its sibling or a pending haptic stop.
 haptic(0,0x82,1,true);settings(0,0x87,9,0);haptic(0,0x81,0,false);settings(0,0x87,9,9);settings(0,0x87,2,18);haptic(0,0x82,1,true);
 std::vector<int> order;
 while(auto x=queue->pop(0ms)){
   if(x->type==gamepad_feedback_e::steam_settings)order.push_back(x->data.steam_settings.triples[0]);
   else order.push_back(-x->data.steam_haptic.report[0]);
 }
 bool ordered=order.size()==5 && order[0]==0 && order[1]==-0x81 && order[2]==9 && order[3]==18 && order[4]==-0x82;
 check(ordered,"chunked settings write stays whole and in order; the repeated stop coalesced behind it");
 check(gamepad_feedback_state_key(gamepad_feedback_msg_t::make_steam_settings(0,0x87,1,{}))==0,"settings writes carry no state key");
 settings(0,0x87,1,42);for(int i=0;i<40;i++)haptic(0,0x81,0,false);
 auto first=queue->pop(0ms);
 check(first && first->type==gamepad_feedback_e::steam_settings && first->data.steam_settings.triples[0]==42,
       "a haptic burst cannot evict a queued settings chunk");
 while(queue->pop(0ms)){}
 settings(0,0x87,9,0);haptic(0,0x82,0,true);settings(0,0x87,9,9);
 for(int i=0;i<40;i++)haptic(0,0x81,0,false);
 settings(0,0x87,2,18);order.clear();int pulses=0,stops=0;
 while(auto x=queue->pop(0ms)){
   if(x->type==gamepad_feedback_e::steam_settings)order.push_back(x->data.steam_settings.triples[0]);
   else if(x->data.steam_haptic.report[0]==0x82)++stops;
   else ++pulses;
 }
 check(order==std::vector<int>({0,9,18}) && pulses==32 && stops==1,
       "all settings chunks and the pending stop survive a burst; haptic events keep their own bound");
 for(int i=0;i<1000;i++)settings(3,0x87,1,i&0x7F);
 int total=0,last=-1;while(auto x=queue->pop(0ms)){++total;last=x->data.steam_settings.triples[0];}
 check(total==16 && last==(999&0x7F),"settings floods keep the newest 16 chunks in a separate bounded class");
 // Even total capacity pressure cannot turn an ordinary pulse into a settings eviction.
 safe::queue_t<gamepad_feedback_msg_t> tiny(2);
 auto chunk=gamepad_feedback_msg_t::make_steam_settings(0,0x87,1,{});
 check(raise_gamepad_feedback(tiny,chunk) && raise_gamepad_feedback(tiny,chunk),"a small queue accepts two distinct settings events");
 std::array<std::uint8_t,12> pulse {0x81,0,1,0,1,0,1,0};
 bool admitted=raise_gamepad_feedback(tiny,gamepad_feedback_msg_t::make_steam_haptic(0,8,pulse));
 auto a=tiny.pop(0ms),b=tiny.pop(0ms);
 check(!admitted && a && b && a->type==gamepad_feedback_e::steam_settings && b->type==gamepad_feedback_e::steam_settings,
       "an ordinary pulse is refused when a full queue contains only reserved settings");
 auto w=queue->pop(0ms);
 vhf_gamepad::steam_settings_t full {};full.command=0x87;full.count=9;for(int i=0;i<27;i++)full.triples[i]=i;
 raise_steam_settings(5,full);w=queue->pop(0ms);
 bool verbatim=w && w->id==5 && w->data.steam_settings.count==9;
 for(int i=0;verbatim && i<27;i++)verbatim=w->data.steam_settings.triples[i]==i;
 check(verbatim,"all nine triples reach the client byte for byte");
 // Existing two-argument users can carry move-only snapshots and opt out of eviction.
 safe::queue_t<std::unique_ptr<int>> snapshots(1);
 bool initial=snapshots.raise_latest(std::make_unique<int>(1),[](const auto &){return false;});
 bool overflow=snapshots.raise_latest(std::make_unique<int>(2),[](const auto &){return false;});
 auto snapshot=snapshots.pop(0ms);
 check(initial && !overflow && snapshot && *snapshot==1,"the existing move-only snapshot API still refuses unrelated overflow");
 snapshots.raise_latest(std::make_unique<int>(3),[](const auto &){return false;});
 bool replaced=snapshots.raise_latest(std::make_unique<int>(4),[](const auto &){return true;});
 snapshot=snapshots.pop(0ms);
 check(replaced && snapshot && *snapshot==4 && !snapshots.pop(0ms),"the existing snapshot API still coalesces its matching value");
 return failures!=0;
}
""".replace('@TYPES@',types).replace('@PARSER@',parser).replace('@HELPERS@',helpers).replace('@HAPTIC_BODY@',haptic_body).replace('@SETTINGS_BODY@',settings_body)
with tempfile.TemporaryDirectory(prefix='host-settings-') as directory:
    work=Path(directory);(work/'thread_safe_test.h').write_text(read('src/thread_safe.h'));(work/'test.cpp').write_text(code)
    args=['g++','-std=c++20','-fsanitize=address,undefined','-g','-I'+str(ROOT/'src')]
    if os.getenv('AUDIT_JSON_INCLUDE'):args+=['-I'+os.environ['AUDIT_JSON_INCLUDE']]
    subprocess.run(args+[str(work/'test.cpp'),'-pthread','-o',str(work/'test')],check=True)
    result=subprocess.run([str(work/'test')],capture_output=True,text=True)
    print(result.stdout,end='')
    if result.returncode:print(result.stderr[:4000],file=sys.stderr)
    raise SystemExit(result.returncode)
