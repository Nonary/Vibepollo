#!/usr/bin/env python3
"""Replay issue 736 through production allocation, routing and feedback methods.

Driver/Windows boundaries, profile selection, slot storage and unrelated
feedback codecs use fixtures. Allocation, routing, recovery, worker and teardown
methods are production bodies. --baseline [REV] defaults to the pre-fix Canary.
"""
import importlib.util
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("routing", Path(__file__).with_name("test_vhf_keyboard_mouse_integration.py"))
routing = importlib.util.module_from_spec(spec)
spec.loader.exec_module(routing)
parser = argparse.ArgumentParser()
parser.add_argument('--baseline', nargs='?', const='e2b7fc539382d52987037bb59ba399b8db4cad6f')
parser.add_argument('--compiler', default='g++')
args = parser.parse_args()
source = (subprocess.check_output(["git", "show", args.baseline + ":src/platform/windows/vhf_gamepad.cpp"], cwd=ROOT, text=True)
          if args.baseline else (ROOT / "src/platform/windows/vhf_gamepad.cpp").read_text())
block = routing.block

EXTRA = r'''
#include <condition_variable>
#include <map>
#include <optional>
#include "src/platform/windows/vhf_gamepad_recovery.h"
constexpr DWORD ERROR_NO_MORE_ITEMS=259, ERROR_REVISION_MISMATCH=1306;
constexpr int MAX_GAMEPADS=4, LI_MOTION_TYPE_ACCEL=1, LI_MOTION_TYPE_GYRO=2;
constexpr auto k_feedback_poll_interval=1ms;
constexpr int k_feedback_drain_per_poll=2;
std::atomic<unsigned> polls{0};
std::atomic<unsigned> fresh_polls{0};
namespace lvg { enum class profile {xbox_series,dualsense}; struct feedback_event {}; }
'''
TYPES = r'''
  struct gamepad_id_t {int globalIndex=0,clientRelativeIndex=0;};
  enum class vhf_profile_e {automatic,xbox_series,dualsense};
  enum class gamepad_feedback_e {haptics_pcm};
  struct gamepad_feedback_msg_t {
    int id=0; gamepad_feedback_e type{};
    struct {struct {int sequence=0,samples=0;} haptics;} data;
    static gamepad_feedback_msg_t make_motion_event_state(int,int,int){return {};}
  };
  struct queue_t {
    void try_raise(gamepad_feedback_msg_t){}
    template<class P> void discard_if(P){}
  };
  using feedback_queue_t=std::shared_ptr<queue_t>;
  void raise_gamepad_feedback(queue_t &,gamepad_feedback_msg_t){}
  namespace dualsense_audio {
    struct packet_t {int sequence=0,samples=0;};
    void set_slot(int,bool){} std::vector<packet_t> drain(int){return {};}
  }
  namespace vhf_gamepad {
    struct steam_haptic_t {};struct steam_settings_t {};struct rumble_rgb_t {};
    bool decode_steam_haptic(lvg::feedback_event &,steam_haptic_t &){return false;}
    bool decode_steam_settings(lvg::feedback_event &,steam_settings_t &){return false;}
    bool decode_rumble_rgb(lvg::feedback_event &,rumble_rgb_t &){return false;}
  }
  struct slot_t {
    bool active=false,have_feedback=false;
    lvg::profile profile{};int client_relative_index=0,client_capabilities=0,free_contacts=0;
    feedback_queue_t feedback_queue;
    std::map<int,int> contact_of_pointer;
    std::atomic<bool> submit_failed{false};
    int last_feedback=0,rumble_poll_summary=0;
    std::optional<int> steam_attributes,last_input,last_battery,battery_level_seed;
    void reset(){active=false;feedback_queue.reset();last_input.reset();last_battery.reset();battery_level_seed.reset();}
  };
  bool has_motion(lvg::profile){return false;}
  bool is_steam_controller(lvg::profile){return false;}
  const char *describe_profile(lvg::profile){return "Xbox Series";}
  bool select_profile(lvg::client &,vhf_profile_e desired,lvg::profile &p){p=desired==vhf_profile_e::dualsense?lvg::profile::dualsense:lvg::profile::xbox_series;return true;}
  void log_feedback_counters(lvg::client &,int,lvg::profile){}
  void log_rumble_poll_error(int,unsigned,DWORD){}
'''
FIELDS = r'''
    std::atomic<bool> stopping{false};
    std::mutex wake_mutex;std::condition_variable wake;std::thread feedback_thread;
    std::array<slot_t,MAX_GAMEPADS> slots;
    bool recovery_failure_logged=false;
    void feedback_loop();
    template<class... T> void raise_steam_haptic(T&&...){}
    template<class... T> void raise_steam_settings(T&&...){}
    template<class... T> void raise_feedback(T&&...){}
    template<class... T> void expire_synthesized_rumble(T&&...){}
'''
CLIENT = r'''
  unsigned generation=0;
  DWORD close_keyboard_mouse(){return abort_keyboard_mouse();}
  DWORD create_controller(unsigned nr,profile p){
    ++boundary.creates;
    if(boundary.create_error)return boundary.create_error;
    if(!is_connected || generation<=boundary.stale_through)return ERROR_DEVICE_REMOVED;
    boundary.live[nr]=p;return 0;
  }
  DWORD destroy_controller(unsigned nr){boundary.live.erase(nr);return 0;}
  DWORD poll_feedback(unsigned,feedback_event *){
    ++polls;
    if(!is_connected || generation<=boundary.stale_through)return ERROR_DEVICE_REMOVED;
    ++fresh_polls;return ERROR_NO_MORE_ITEMS;
  }
  DWORD set_steam_controller_attributes(unsigned,int){return 0;}
  DWORD submit_input_state(int){return 0;}
  DWORD submit_battery_state(int){return 0;}
'''
TEST = r'''
}
int main(){
  using namespace platf;
  auto wait_for=[](auto ready){
    const auto deadline=std::chrono::steady_clock::now()+2s;
    while(!ready()){assert(std::chrono::steady_clock::now()<deadline);std::this_thread::yield();}
  };
  auto allocate=[](vhf_gamepad_t &v,int nr=0,vhf_profile_e profile=vhf_profile_e::xbox_series){feedback_queue_t q=std::make_shared<queue_t>();return v.alloc({nr,nr},q,profile,0);};
  auto lose=[](input_t &in){
    lvg::boundary.stale_through=lvg::boundary.connects;
    lvg::boundary.statuses.push_back(ERROR_DEVICE_REMOVED);
    move_mouse(in,1,1);
    assert(in->vhf->impl->recovery.pending());
  };
  // Exact reporter sequence, both with and without another mouse packet after
  // removal. The same first arrival must connect a new handle and create pad 0.
  for(auto profile:{vhf_profile_e::xbox_series,vhf_profile_e::dualsense})for(bool next_mouse:{false,true}){
    lvg::boundary={};injected.clear();polls=0;
    auto in=input();lose(in);
    assert(!in->vhf->impl->feedback_thread.joinable());
    if(next_mouse){move_mouse(in,-1,-1);assert(injected.size()==1);}
    assert(allocate(*in->vhf,0,profile)==0);
    assert(lvg::boundary.closes==1 && lvg::boundary.connects==2);
    assert(lvg::boundary.live.at(0)==(profile==vhf_profile_e::dualsense?lvg::profile::dualsense:lvg::profile::xbox_series));
    assert(!in->vhf->impl->recovery.pending());
    wait_for([]{return polls.load()>0;});
  }
  // An existing idle feedback thread is not needed to perform the recovery.
  {
    lvg::boundary={};injected.clear();polls=0;
    auto in=input();assert(allocate(*in->vhf)==0);
    wait_for([]{return polls.load()>0;});
    in->vhf->free(0);
    assert(in->vhf->impl->feedback_thread.joinable());
    lose(in);assert(allocate(*in->vhf)==0);
    auto before=fresh_polls.load();wait_for([&]{return fresh_polls.load()>before;});
    assert(lvg::boundary.connects==2 && lvg::boundary.closes==1);
    in->vhf->free(0); // Destructor must also wake and join the idle worker.
  }
  // A failed abort must not authorize Win32 replay. Allocation can retire the
  // old handle while retaining the PnP probe requirement on the shared handle.
  {
    lvg::boundary={};injected.clear();
    auto in=input();lvg::boundary.abort_status=ERROR_ACCESS_DENIED;lose(in);
    assert(allocate(*in->vhf)==0);
    assert(in->vhf->impl->keyboard_mouse_recovery_needs_probe);
    assert(!in->vhf->keyboard_mouse_fallback([]{assert(false);}));
    lvg::boundary.open_status=ERROR_BUSY;
    lvg::boundary.abort_status=0; // An empty new-owner slot is not removal proof.
    in->vhf->impl->keyboard_mouse_retry_at={};
    keyboard_update(in,0x41,false,0);
    assert(injected.empty() && in->vhf_keyboard_mouse);
    assert(lvg::boundary.closes==1 && lvg::boundary.creates==1);
    lvg::boundary.open_status=0;lvg::boundary.abort_status=0;
    in->vhf->impl->keyboard_mouse_retry_at={};move_mouse(in,2,0);
    assert(!in->vhf_keyboard_mouse && injected.size()==1);
    keyboard_update(in,0x41,true,0);assert(injected.size()==1);
    assert(lvg::boundary.closes==1 && lvg::boundary.creates==1);
  }
  // Unacknowledged mouse delivery requests recovery while still owning the
  // child. Allocation must synchronously abort that child before reconnecting.
  {
    lvg::boundary={};injected.clear();
    auto in=input();button_mouse(in,1,false);delayed_button_reads=10000;
    assert(in->vhf->keyboard_mouse_absolute(50000,32000));delayed_button_reads=0;
    assert(in->vhf->impl->recovery.pending() && lvg::boundary.aborts==0);
    assert(allocate(*in->vhf)==0);
    assert(lvg::boundary.aborts==1 && lvg::boundary.closes==1 && injected.empty());
  }
  // Active owners remain deferred and their shared connection is preserved.
  // No worker is running in this boundary fixture, making the guard deterministic.
  {
    lvg::boundary={};vhf_gamepad_t v;
    v.impl->client.connect();v.impl->slots[0].active=true;v.impl->active_count=1;
    v.impl->recovery.request();assert(allocate(v,1)==-1);
    assert(lvg::boundary.closes==0 && lvg::boundary.connects==1);
  }
  // A removed-device create while another slot is active must request worker
  // recovery without retiring that slot's shared connection or retrying here.
  {
    lvg::boundary={};vhf_gamepad_t v;v.impl->client.connect();
    v.impl->slots[0].active=true;v.impl->active_count=1;
    lvg::boundary.stale_through=lvg::boundary.connects;
    assert(allocate(v,1)==-1);
    assert(lvg::boundary.closes==0 && lvg::boundary.connects==1 && lvg::boundary.creates==1);
    assert(v.impl->recovery.pending());
  }
  // The real feedback loop still restores an active controller's exact profile.
  {
    lvg::boundary={};polls=0;auto in=input();assert(allocate(*in->vhf,0,vhf_profile_e::dualsense)==0);
    {std::unique_lock lock{in->vhf->impl->lifetime};in->vhf->impl->recovery.request();}
    wait_for([&]{std::shared_lock lock{in->vhf->impl->lifetime};return !in->vhf->impl->recovery.pending();});
    {std::shared_lock lock{in->vhf->impl->lifetime};assert(lvg::boundary.live.at(0)==lvg::profile::dualsense);assert(lvg::boundary.creates==2);}
  }
  // A genuinely absent driver can reject the arrival, but must not leave the
  // flag stuck: another arrival succeeds after the driver becomes available.
  {
    lvg::boundary={};auto in=input();lose(in);lvg::boundary.connect_status=ERROR_NOT_SUPPORTED;
    assert(allocate(*in->vhf)==-1 && !in->vhf->impl->recovery.pending());
    lvg::boundary.connect_status=0;assert(allocate(*in->vhf)==0);
  }
  // A controller arrival can beat the mouse nudge. Retry a removed-device
  // create once on a new handle, including when abort cannot prove KM removal.
  for(DWORD abort_status:{ERROR_SUCCESS,ERROR_ACCESS_DENIED}){
    lvg::boundary={};auto in=input();lvg::boundary.stale_through=lvg::boundary.connects;
    lvg::boundary.abort_status=abort_status;
    assert(!in->vhf->impl->recovery.pending());
    assert(allocate(*in->vhf)==0);
    assert(lvg::boundary.connects==2 && lvg::boundary.closes==1 && lvg::boundary.creates==2);
    if(abort_status!=ERROR_SUCCESS)assert(in->vhf->impl->keyboard_mouse_recovery_needs_probe);
  }
  // Permanent removal must not create an unbounded reconnect loop.
  {
    lvg::boundary={};auto in=input();lvg::boundary.create_error=ERROR_DEVICE_REMOVED;
    assert(allocate(*in->vhf)==-1);
    assert(lvg::boundary.connects==2 && lvg::boundary.creates==2 && lvg::boundary.live.empty());
    assert(lvg::boundary.closes==2 && !in->vhf->impl->recovery.pending());
    lvg::boundary.create_error=0;assert(allocate(*in->vhf)==0);
  }
}
'''

prefix = routing.PREFIX
def replace_once(text, old, new):
    assert text.count(old) == 1, 'Fixture changed: ' + old
    return text.replace(old, new, 1)

# Fail loudly when the shared boundary fixture changes, rather than silently
# dropping instrumentation that enforces stale-handle and teardown behavior.
class FixtureText(str):
    def replace(self, old, new):
        return FixtureText(replace_once(str(self), old, new))

prefix = FixtureText(prefix)
prefix = prefix.replace('namespace config {', EXTRA + '\nnamespace config {')
prefix = prefix.replace('int connects=0, closes=0, opens=0, aborts=0;', 'int connects=0, closes=0, opens=0, aborts=0, creates=0; unsigned stale_through=0; DWORD create_error=0; std::map<unsigned,profile> live;')
prefix = prefix.replace('struct client {', 'struct client {' + CLIENT)
prefix = prefix.replace('++boundary.connects; is_connected=', '++boundary.connects; generation=boundary.connects; is_connected=')
prefix = prefix.replace('++boundary.closes;is_connected=false;', '++boundary.closes;is_connected=false;boundary.live.clear();')
prefix = prefix.replace('struct recovery_t { bool waiting=false; bool pending() const {return waiting;} void request(){waiting=true;} void reset(){waiting=false;} };', 'using recovery_t=vhf_gamepad::connection_recovery_t;' + TYPES)
prefix = prefix.replace('bool probe(){return true;}', 'int alloc(const gamepad_id_t &, feedback_queue_t &, vhf_profile_e, std::uint16_t); void free(int); bool probe(){return true;}')
prefix = prefix.replace('void observe_status(DWORD s){if(s==ERROR_DEVICE_REMOVED)recovery.request();}', block(source, 'void observe_status(') + FIELDS + (block(source, 'void retire_idle_connection_locked(') if 'void retire_idle_connection_locked(' in source else ''))
prefix = prefix.replace('MOUSE_SETTLE_METHOD', block(source, 'bool settle_keyboard_mouse_locked(')).replace('SUBMIT_METHOD', block(source, 'template<class Request>\n    bool submit_keyboard_mouse_locked('))
prefix = prefix.replace('vhf_gamepad_t::~vhf_gamepad_t()=default;', '')
methods = '\n'.join(block(source, marker) for marker in (
    'vhf_gamepad_t::~vhf_gamepad_t()', 'void vhf_gamepad_t::impl_t::feedback_loop()',
    'int vhf_gamepad_t::alloc(', 'void vhf_gamepad_t::free(',
    'bool vhf_gamepad_t::open_keyboard_mouse()', 'bool vhf_gamepad_t::keyboard_mouse_fallback_safe()',
    'bool vhf_gamepad_t::keyboard_mouse_fallback(', 'void vhf_gamepad_t::keyboard_mouse_key(',
    'bool vhf_gamepad_t::keyboard_mouse_consumer_supported() const', 'void vhf_gamepad_t::keyboard_mouse_consumer(',
    'void vhf_gamepad_t::keyboard_mouse_button(', 'void vhf_gamepad_t::keyboard_mouse_relative(',
    'bool vhf_gamepad_t::keyboard_mouse_absolute('))
inputs = '\n'.join(block(routing.input_source, marker) for marker in (
    'input_t input()', 'static bool use_vhf_keyboard_mouse(', 'void move_mouse(', 'void button_mouse(', 'void keyboard_update('))
with tempfile.TemporaryDirectory(prefix='vhf-idle-recovery-') as directory:
    cpp=Path(directory)/'test.cpp';exe=Path(directory)/'test'
    cpp.write_text(prefix+methods+inputs+TEST)
    subprocess.run([args.compiler,'-std=c++20','-Wall','-Wextra','-pthread','-I',str(ROOT),str(cpp),'-o',str(exe)],check=True,timeout=30)
    subprocess.run([str(exe)],check=True,timeout=15)
print('VHF idle recovery production regression passed')
