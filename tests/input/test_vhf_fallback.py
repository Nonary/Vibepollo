#!/usr/bin/env python3
"""Execute production Windows controller allocation with fake driver boundaries.

The full allocation/selection functions are extracted unchanged; fake VHF and
ViGEm objects expose configurable availability and profile-allocation results.
--baseline reads input.cpp from HEAD. No kernel driver or controller is required.
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PATH = 'src/platform/windows/input.cpp'
source = (subprocess.check_output(['git', 'show', 'HEAD:' + PATH], cwd=ROOT, text=True)
          if '--baseline' in sys.argv else (ROOT / PATH).read_text())

def block(text, marker):
    start = text.index(marker)
    end = text.index('{', start)
    depth = 1
    while depth:
        end += 1
        depth += (text[end] == '{') - (text[end] == '}')
    return text[start:end + 1]

policy = (ROOT/'src/platform/windows/vhf_gamepad_policy.h').read_text()
# vhf_profile_e moved into the policy header upstream; keep both texts searchable.
header = policy + '\n' + (ROOT/'src/platform/windows/vhf_gamepad.h').read_text()
prefix = r'''
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
using namespace std::literals;
struct Log {template<class T> Log& operator<<(const T&){return *this;}};
#define BOOST_LOG(level) Log{}
constexpr int MAX_GAMEPADS=16;
constexpr int LI_CTYPE_STEAM=4,LI_CTYPE_PS=2,LI_CTYPE_NINTENDO=3,LI_CTYPE_XBOX=1;
constexpr int LI_CCAP_ACCEL=1,LI_CCAP_GYRO=2,LI_CCAP_TOUCHPAD=4,LI_CCAP_RGB_LED=8;
constexpr int LI_CCAP_GRIP_SENSE=0x200,LI_CCAP_STEAM_HAPTIC=0x800,LI_CCAP_SC26_BUTTONS=0x1000;
namespace config {struct {std::string gamepad="auto";bool motion_as_ds4=false,touchpad_as_ds4=false,native_pen_touch=false;} input;}
void* GetModuleHandleA(const char*){return nullptr;}
void* GetProcAddress(void*,const char*){return nullptr;}
namespace platf {
namespace platform_caps {typedef std::uint32_t caps_t;constexpr caps_t pen_touch=0x01,controller_touch=0x02;}
PROFILE_ENUM;
namespace vhf_gamepad {BACKEND_ENUM; BACKEND_SELECT
constexpr std::uint16_t STEAM_CONTROLLER_CLIENT_CAPS=LI_CCAP_GRIP_SENSE|LI_CCAP_STEAM_HAPTIC|LI_CCAP_SC26_BUTTONS;
IS_STEAM_CLIENT
SELECT_DESIRED
SUPPORTS_TOUCH}
namespace dualsense_usbip_gamepad {bool enabled(){return false;} bool select_composite_dualsense(std::string_view,vhf_profile_e,bool,bool){return false;}}
struct gamepad_id_t {int globalIndex,clientRelativeIndex;};
struct gamepad_arrival_t {int type;std::uint16_t capabilities=0;};
using feedback_queue_t=std::shared_ptr<int>;
enum VIGEM_TARGET_TYPE {Xbox360Wired,DualShock4Wired};
enum class gamepad_backend_e {none,vhf,vigem,usbip};
struct FakeVhf {
 bool live=true,requested_success=false,automatic_success=true;
 std::vector<vhf_profile_e> calls;
 bool available(){return live;}
 bool probe(){return live;}
 int alloc(const gamepad_id_t&,feedback_queue_t& feedback,vhf_profile_e profile,std::uint16_t){
  calls.push_back(profile);
  bool ok=profile==vhf_profile_e::automatic?automatic_success:requested_success;
  if(ok)feedback.reset();return ok?0:-1;
 }
};
struct FakeVigem {
 bool live=false;int calls=0;
 bool available(){return live;}
 int alloc_gamepad_internal(const gamepad_id_t&,feedback_queue_t&,VIGEM_TARGET_TYPE){++calls;return live?0:-1;}
};
struct FakeUsbip {bool available(){return false;} int alloc(const gamepad_id_t&,const gamepad_arrival_t&,feedback_queue_t&){return -1;}};
struct input_raw_t {
 std::unique_ptr<FakeVhf> vhf=std::make_unique<FakeVhf>();
 std::unique_ptr<FakeUsbip> usbip;
 std::unique_ptr<FakeVigem> vigem=std::make_unique<FakeVigem>();
 gamepad_backend_e gamepad_backend[MAX_GAMEPADS]{};
};
using input_t=std::shared_ptr<input_raw_t>;
'''.replace('PROFILE_ENUM',block(header,'enum class vhf_profile_e'))
prefix=prefix.replace('BACKEND_ENUM',block(policy,'enum class backend_e'))
prefix=prefix.replace('BACKEND_SELECT',block(policy,'[[nodiscard]] constexpr backend_e select_automatic_backend('))
# The production profile-selection policy (upstream moved it out of input.cpp).
policy_cpp=(ROOT/'src/platform/windows/vhf_gamepad_policy.cpp').read_text()
prefix=prefix.replace('IS_STEAM_CLIENT',block(policy_cpp,'bool is_steam_controller_client('))
prefix=prefix.replace('SELECT_DESIRED',block(policy_cpp,'vhf_profile_e select_desired_profile('))
# get_capabilities() asks the policy whether the selection can yield a touchpad (upstream
# moved that decision out of input.cpp too); use the production function.
prefix=prefix.replace('SUPPORTS_TOUCH',block(policy_cpp,'bool supports_controller_touch('))
suffix = r'''
}
int main(){
 using namespace platf;
 int errors=0,checks=0;
 auto check=[&](bool ok,const char* name){++checks;errors+=!ok;std::cout<<(ok?"PASS ":"FAIL ")<<name<<'\n';};
 for(int type:{LI_CTYPE_STEAM,LI_CTYPE_PS,LI_CTYPE_NINTENDO}){
  config::input.gamepad="auto";auto input=std::make_shared<input_raw_t>();
  int rc=alloc_gamepad(input,{0,2},{type,(std::uint16_t)(type==LI_CTYPE_STEAM?LI_CCAP_SC26_BUTTONS:0)},std::make_shared<int>(1));
  check(rc==0&&input->vhf->calls.size()==2&&input->vhf->calls.back()==vhf_profile_e::automatic&&
        input->gamepad_backend[0]==gamepad_backend_e::vhf&&input->vigem->calls==0,
        "auto without ViGEm retries an unsupported client-derived profile");
 }
 const gamepad_arrival_t STEAM{LI_CTYPE_STEAM,LI_CCAP_SC26_BUTTONS};
 for(const char* setting:{"vhf_steam","vhf_ds5","vhf_switch"}){
  config::input.gamepad=setting;auto input=std::make_shared<input_raw_t>();input->vigem->live=true;
  check(alloc_gamepad(input,{0,0},STEAM,std::make_shared<int>(1))==-1&&
        input->vhf->calls.size()==1&&input->vigem->calls==0,"explicit profile is not substituted");
 }
 config::input.gamepad="vhf";auto input=std::make_shared<input_raw_t>();
 check(alloc_gamepad(input,{0,0},STEAM,std::make_shared<int>(1))==0&&
       input->vhf->calls.size()==2,"plain vhf still retries automatic profile");
 config::input.gamepad="auto";input=std::make_shared<input_raw_t>();input->vhf->requested_success=true;
 check(alloc_gamepad(input,{0,0},STEAM,std::make_shared<int>(1))==0&&
       input->vhf->calls.size()==1,"available client profile is kept without a retry");
 input=std::make_shared<input_raw_t>();input->vhf->automatic_success=false;
 check(alloc_gamepad(input,{0,0},STEAM,std::make_shared<int>(1))==-1&&
       input->vhf->calls.size()==2&&input->vigem->calls==0,"both VHF allocations fail cleanly without ViGEm");
 input=std::make_shared<input_raw_t>();input->vigem->live=true;input->vhf->automatic_success=false;
 check(alloc_gamepad(input,{0,0},STEAM,std::make_shared<int>(1))==0&&
       input->vigem->calls==1&&input->gamepad_backend[0]==gamepad_backend_e::vigem,
       "Steam auto still falls back to available ViGEm after both VHF failures");
 input=std::make_shared<input_raw_t>();
 check(alloc_gamepad(input,{-1,0},STEAM,std::make_shared<int>(1))==-1&&
       input->vhf->calls.empty(),"invalid slot performs no allocation");
 // The Steam Controller profile needs a Steam Controller client controller: the Steam type with
 // the Steam extensions. Other controllers the client forwards (a keyboard's joystick interface,
 // a Steam Deck's SDL controls) get the automatic choice even under a forced vhf_steam.
 config::input.gamepad="vhf_steam";
 check(vhf_desired_profile(STEAM)==vhf_profile_e::steam_controller&&
       vhf_desired_profile({LI_CTYPE_XBOX,LI_CCAP_ACCEL|LI_CCAP_GYRO})==vhf_profile_e::automatic&&
       vhf_desired_profile({LI_CTYPE_STEAM,LI_CCAP_ACCEL|LI_CCAP_GYRO})==vhf_profile_e::automatic&&
       vhf_desired_profile({0,0})==vhf_profile_e::automatic,
       "vhf_steam is kept for Steam Controller client controllers; the rest get the automatic choice");
 input=std::make_shared<input_raw_t>();
 check(alloc_gamepad(input,{1,1},{LI_CTYPE_XBOX,0},std::make_shared<int>(1))==0&&
       input->vhf->calls.size()==1&&input->vhf->calls.back()==vhf_profile_e::automatic,
       "a second, non-Steam controller under vhf_steam is allocated as the automatic profile");
 config::input.gamepad="auto";input=std::make_shared<input_raw_t>();
 check(alloc_gamepad(input,{0,0},{LI_CTYPE_STEAM,LI_CCAP_ACCEL|LI_CCAP_GYRO},std::make_shared<int>(1))==0&&
       input->vhf->calls.size()==1&&input->vhf->calls.back()==vhf_profile_e::automatic,
       "auto: the Steam type without the Steam extensions does not reach for the Steam Controller profile");
 // The advertised controller-touch capability must agree with the profile the selection can
 // yield: plain vhf gives a Steam/PlayStation client a pad-bearing profile whatever the
 // motion/touchpad preferences say, so the flag stays on; only Xbox/Switch selections drop it.
 config::input.motion_as_ds4=false;config::input.touchpad_as_ds4=false;
 for(const char* setting:{"vhf","auto","vhf_ds4","vhf_ds5","vhf_steam","ds4","ds5"}){
  config::input.gamepad=setting;
  check((get_capabilities()&platform_caps::controller_touch)!=0,"controller touch advertised for a selection that can yield a touchpad");
 }
 config::input.gamepad="vhf";
 check(vhf_desired_profile(STEAM)==vhf_profile_e::steam_controller&&vhf_desired_profile({LI_CTYPE_PS})==vhf_profile_e::dualsense,
       "plain vhf without the ds4 preferences still selects the client's pad-bearing profile");
 for(const char* setting:{"vhf_xbox","vhf_xbox_one","vhf_switch","x360"}){
  config::input.gamepad=setting;
  check((get_capabilities()&platform_caps::controller_touch)==0,"controller touch not advertised for a selection without a touchpad");
 }
 std::cout<<checks<<" checks; "<<errors<<" failures\n";return errors?1:0;
}
'''
code=prefix+'\n'.join(block(source,m) for m in [
    'static bool vhf_gamepad_selected(', 'static vhf_profile_e vhf_desired_profile(',
    'int alloc_gamepad(input_t &', 'platform_caps::caps_t get_capabilities('])+suffix
with tempfile.TemporaryDirectory(prefix='vhf-fallback-') as directory:
    work=Path(directory)
    (work/'test.cpp').write_text(code)
    subprocess.run(['g++','-std=c++20','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',
                    str(work/'test.cpp'),'-o',str(work/'test')],check=True)
    raise SystemExit(subprocess.run([str(work/'test')],env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0')).returncode)
