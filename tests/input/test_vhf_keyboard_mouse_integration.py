#!/usr/bin/env python3
"""Execute the actual Windows input/VHF routing at a fake driver boundary.

The event handlers and connection/submit methods are copied from production,
not reimplemented. No Windows driver or input is installed or injected.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
input_source = (ROOT / 'src/platform/windows/input.cpp').read_text(encoding='utf-8')
vhf_source = (ROOT / 'src/platform/windows/vhf_gamepad.cpp').read_text(encoding='utf-8')


def block(text, marker):
    start = text.index(marker)
    brace = text.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


PREFIX = r'''
#include "src/platform/windows/vhf_keyboard_mouse_policy.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>
using namespace std::literals;
using DWORD = unsigned long;
constexpr DWORD ERROR_SUCCESS=0, ERROR_BUSY=170, ERROR_NOT_SUPPORTED=50, ERROR_DEVICE_REMOVED=1617, ERROR_ACCESS_DENIED=5;
constexpr int WHEEL_DELTA=120;
struct Log { template<class T> Log &operator<<(const T &) { return *this; } };
#define BOOST_LOG(level) Log{}
namespace util { struct hex_value { std::string to_string_view() const { return "error"; } }; template<class T> hex_value hex(T) { return {}; } }
namespace config { struct { std::string keyboard_mouse_backend="auto", gamepad="auto"; bool always_send_scancodes=false; } input; }
namespace dualsense_usbip_gamepad { bool enabled(){return false;} }
namespace platf { bool vhf_gamepad_selected(){return false;} }
namespace lvg { namespace km {
  constexpr std::uint32_t keyboard=1, relative_mouse=2, wheel_detents=4, bound_absolute_mouse=8, primary_absolute_mouse=16, consumer=32;
  constexpr std::uint32_t k_relative_capabilities=keyboard|relative_mouse|wheel_detents;
  struct query_response { std::uint32_t capabilities=0, absolute_target_generation=0; };
  struct keyboard_request { std::uint16_t usage=0; std::uint8_t pressed=0; };
  struct consumer_request { std::uint16_t usage=0; std::uint8_t pressed=0; };
  struct button_request { std::uint8_t button=0, pressed=0; };
  struct relative_request { std::int16_t x=0,y=0; std::int8_t wheel=0,pan=0; };
  struct absolute_request { std::uint16_t x=0,y=0; std::uint32_t target_generation=0; };
  template<class T> T request() { return {}; }
}
struct event { int type=0, value=0, pressed=0, x=0, y=0; };
struct fixture {
  DWORD connect_status=0, query_status=0, open_status=0, abort_status=0;
  std::uint32_t capabilities=km::k_relative_capabilities|km::bound_absolute_mouse|16|km::consumer, generation=1;
  int connects=0, closes=0, opens=0, aborts=0;
  std::deque<DWORD> statuses;
  std::vector<event> events;
};
fixture boundary;
struct client {
  bool is_connected=false;
  DWORD connect(){++boundary.connects; is_connected=boundary.connect_status==0; return boundary.connect_status;}
  bool connected() const {return is_connected;}
  void close(){++boundary.closes;is_connected=false;}
  DWORD query_keyboard_mouse(km::query_response *r){r->capabilities=boundary.capabilities;r->absolute_target_generation=boundary.generation;return boundary.query_status;}
  DWORD open_keyboard_mouse(){++boundary.opens;return boundary.open_status;}
  DWORD abort_keyboard_mouse(){++boundary.aborts;return boundary.abort_status;}
  DWORD next(event e){boundary.events.push_back(e);if(boundary.statuses.empty())return 0;auto s=boundary.statuses.front();boundary.statuses.pop_front();return s;}
  DWORD submit_keyboard_mouse(const km::keyboard_request &r){return next({1,r.usage,r.pressed});}
  DWORD submit_keyboard_mouse(const km::consumer_request &r){return next({2,r.usage,r.pressed});}
  DWORD submit_keyboard_mouse(const km::button_request &r){return next({3,r.button,r.pressed});}
  DWORD submit_keyboard_mouse(const km::relative_request &r){return next({4,r.wheel,r.pan,r.x,r.y});}
  DWORD submit_keyboard_mouse(const km::absolute_request &r){return next({5,0,0,r.x,r.y});}
}; }
constexpr int INPUT_MOUSE=1,INPUT_KEYBOARD=2;
constexpr int MOUSEEVENTF_MOVE=1,MOUSEEVENTF_ABSOLUTE=2,MOUSEEVENTF_VIRTUALDESK=4;
constexpr int MOUSEEVENTF_LEFTUP=8,MOUSEEVENTF_LEFTDOWN=16,MOUSEEVENTF_MIDDLEUP=32,MOUSEEVENTF_MIDDLEDOWN=64;
constexpr int MOUSEEVENTF_RIGHTUP=128,MOUSEEVENTF_RIGHTDOWN=256,MOUSEEVENTF_XUP=512,MOUSEEVENTF_XDOWN=1024;
constexpr int MOUSEEVENTF_WHEEL=2048,MOUSEEVENTF_HWHEEL=4096,XBUTTON1=1,XBUTTON2=2;
constexpr int KEYEVENTF_SCANCODE=1,KEYEVENTF_EXTENDEDKEY=2,KEYEVENTF_KEYUP=4;
constexpr int KEYEVENTF_UNICODE=8, CP_UTF8=65001, MB_ERR_INVALID_CHARS=8;
using WCHAR = wchar_t;
int MultiByteToWideChar(int, int, const char *src, int len, WCHAR *dest, int capacity) {
  if (len > capacity) return 0;
  for (int i = 0; i < len; ++i) dest[i] = static_cast<unsigned char>(src[i]);
  return len;
}
constexpr int VK_LWIN=0x5b,VK_RWIN=0x5c,VK_RMENU=0xa5,VK_RCONTROL=0xa3,VK_INSERT=0x2d,VK_DELETE=0x2e;
constexpr int VK_HOME=0x24,VK_END=0x23,VK_PRIOR=0x21,VK_NEXT=0x22,VK_UP=0x26,VK_DOWN=0x28;
constexpr int VK_LEFT=0x25,VK_RIGHT=0x27,VK_DIVIDE=0x6f,VK_APPS=0x5d,VK_PAUSE=0x13;
constexpr int SS_KBE_FLAG_NON_NORMALIZED=1, MAPVK_VK_TO_VSC=0,MAPVK_VK_TO_VSC_EX=1;
constexpr int SM_XVIRTUALSCREEN=1,SM_YVIRTUALSCREEN=2,SM_CXVIRTUALSCREEN=3,SM_CYVIRTUALSCREEN=4,SM_CXSCREEN=5,SM_CYSCREEN=6;
int GetSystemMetrics(int which) { switch(which) {case SM_XVIRTUALSCREEN:return -1920;case SM_YVIRTUALSCREEN:return 0;case SM_CXVIRTUALSCREEN:return 3840;case SM_CYVIRTUALSCREEN:return 1080;case SM_CXSCREEN:return 1920;default:return 1080;} }

constexpr int VK_LBUTTON=1,VK_RBUTTON=2,VK_MBUTTON=4,VK_XBUTTON1=5,VK_XBUTTON2=6;
struct POINT {long x=0,y=0;};
int delayed_button_reads=0, delayed_cursor_reads=0, button_reads=0, cursor_reads=0;
short GetAsyncKeyState(int key) {
  ++button_reads;
  if(delayed_button_reads>0){--delayed_button_reads;return 0;}
  const std::array<int,5> keys{VK_LBUTTON,VK_RBUTTON,VK_MBUTTON,VK_XBUTTON1,VK_XBUTTON2};
  for(auto e=lvg::boundary.events.rbegin();e!=lvg::boundary.events.rend();++e)
    if(e->type==3 && e->value>=1 && e->value<=5 && keys[e->value-1]==key)return e->pressed?short(0x8000):0;
  return 0;
}
bool GetCursorPos(POINT *point) {
  ++cursor_reads;
  if(delayed_cursor_reads>0){--delayed_cursor_reads;*point={-100,-100};return true;}
  for(auto e=lvg::boundary.events.rbegin();e!=lvg::boundary.events.rend();++e)
    if(e->type==5){point->x=long(std::uint64_t(e->x)*1920/65536);point->y=long(std::uint64_t(e->y)*1080/65536);return true;}
  *point={};return true;
}
unsigned MapVirtualKeyW(unsigned key,unsigned) {return key==0x41?0x1e:0;}
unsigned MapVirtualKey(unsigned key,unsigned) {return MapVirtualKeyW(key,0);}
struct INPUT { int type=0; struct {int dwFlags=0,dx=0,dy=0,mouseData=0;} mi; struct {int wScan=0,wVk=0,dwFlags=0;} ki; };
std::array<int,256> VK_TO_SCANCODE_MAP=[] {std::array<int,256> a{};a[0x41]=0x1e;a[0x42]=0x30;return a;}();
std::vector<INPUT> injected;
std::function<void()> before_inject;
void send_input(INPUT &i) {if (before_inject) before_inject(); injected.push_back(i);}
namespace platf {
  struct recovery_t { bool waiting=false; bool pending() const {return waiting;} void request(){waiting=true;} void reset(){waiting=false;} };
  class vhf_gamepad_t {
  public:
    struct impl_t;
    std::unique_ptr<impl_t> impl;
    vhf_gamepad_t(); ~vhf_gamepad_t();
    bool probe(){return true;} bool open_keyboard_mouse(); bool keyboard_mouse_fallback_safe();
    bool keyboard_mouse_fallback(const std::function<void()> &);
    void keyboard_mouse_key(std::uint16_t,bool); bool keyboard_mouse_consumer_supported() const;
    void keyboard_mouse_consumer(std::uint16_t,bool); void keyboard_mouse_button(std::uint8_t,bool);
    void keyboard_mouse_relative(int,int,int,int); bool keyboard_mouse_absolute(std::uint16_t,std::uint16_t);
  };
  struct vhf_gamepad_t::impl_t {
    std::shared_mutex lifetime; lvg::client client; recovery_t recovery; std::atomic<unsigned> active_count{0};
    bool keyboard_mouse_owned=false,keyboard_mouse_faulted=false,keyboard_mouse_teardown_confirmed=false;
    bool keyboard_mouse_recovery_needs_probe=false;
    std::chrono::steady_clock::time_point keyboard_mouse_retry_at{};
    bool absolute_uses_primary=false,keyboard_mouse_consumer=false; std::uint32_t absolute_generation=0;
    void observe_status(DWORD s){if(s==ERROR_DEVICE_REMOVED)recovery.request();}
    vhf_keyboard_mouse::mouse_delivery_t mouse_delivery;
    MOUSE_SETTLE_METHOD
    SUBMIT_METHOD
  };
  vhf_gamepad_t::vhf_gamepad_t():impl(std::make_unique<impl_t>()){} vhf_gamepad_t::~vhf_gamepad_t()=default;
  struct vigem_t {bool vhf_gamepad_available=false;bool init(){return false;}};
  struct usbip_gamepad_t {bool probe(){return false;}};
  void CreateSyntheticPointerDevice(){} void InjectSyntheticPointerInput(){} void DestroySyntheticPointerDevice(){}
  void *GetModuleHandleA(const char*){return nullptr;} void *GetProcAddress(void*,const char*){return nullptr;}
  struct input_raw_t {
    ~input_raw_t(){delete vigem;delete vhf;delete usbip;}
    vigem_t *vigem=nullptr; vhf_gamepad_t *vhf=nullptr; usbip_gamepad_t *usbip=nullptr;
    bool vhf_keyboard_mouse=false; std::uint8_t vhf_held_buttons=0;
    vhf_keyboard_mouse::keyboard_routes vhf_key_routes{};
    decltype(CreateSyntheticPointerDevice) *fnCreateSyntheticPointerDevice=nullptr;
    decltype(InjectSyntheticPointerInput) *fnInjectSyntheticPointerInput=nullptr;
    decltype(DestroySyntheticPointerDevice) *fnDestroySyntheticPointerDevice=nullptr;
  };
  using input_t=std::shared_ptr<input_raw_t>;
  struct touch_port_t {int offset_x,offset_y,width,height;};
  constexpr touch_port_t target_touch_port{0,0,65535,65535};
'''

TEST = r'''
}
int main() {
  using namespace platf;
  using lvg::boundary;
  // Auto prefers a compatible driver and routes through it from the first
  // event. Missing, old, incomplete, and inaccessible drivers stay on Win32.
  boundary={};injected.clear();
  auto automatic=input();
  assert(automatic->vhf_keyboard_mouse && boundary.opens==1);
  keyboard_update(automatic,0x41,false,0);keyboard_update(automatic,0x41,true,0);
  assert(injected.empty() && boundary.events.size()==2);
  for(int failure=0;failure<4;++failure){
    boundary={};injected.clear();
    if(failure==0)boundary.connect_status=ERROR_NOT_SUPPORTED;
    if(failure==1)boundary.query_status=ERROR_NOT_SUPPORTED;
    if(failure==2)boundary.capabilities=lvg::km::keyboard;
    if(failure==3)boundary.open_status=ERROR_ACCESS_DENIED;
    auto fallback=input();
    assert(!fallback->vhf_keyboard_mouse);
    keyboard_update(fallback,0x41,false,0);keyboard_update(fallback,0x41,true,0);
    assert(boundary.events.empty() && injected.size()==2);
    assert(boundary.opens==(failure==3?1:0));
  }

  // Explicit SendInput bypasses the VHF keyboard/mouse endpoint.
  boundary={};injected.clear();config::input.keyboard_mouse_backend="sendinput";
  auto ordinary=input(); assert(boundary.connects==0); keyboard_update(ordinary,0x41,false,0);
  assert(boundary.events.empty() && injected.size()==1 && injected[0].type==INPUT_KEYBOARD);

  // A missing/old driver falls back before accepting an event and closes only
  // a newly opened handle, preserving the existing SendInput path.
  boundary={};injected.clear();boundary.query_status=ERROR_NOT_SUPPORTED;config::input.keyboard_mouse_backend="vhf";
  auto missing=input();assert(boundary.connects==1 && boundary.opens==0 && boundary.aborts==0 && boundary.closes==1);
  button_mouse(missing,1,false);assert(boundary.events.empty() && injected.size()==1 && injected[0].mi.dwFlags==MOUSEEVENTF_LEFTDOWN);
  boundary={};injected.clear();boundary.connect_status=ERROR_ACCESS_DENIED;
  auto denied=input();keyboard_update(denied,0x41,false,0);
  assert(!denied->vhf_keyboard_mouse && boundary.opens==0 && boundary.aborts==0 && injected.size()==1);
  boundary={};injected.clear();boundary.open_status=ERROR_ACCESS_DENIED;
  auto denied_open=input();move_mouse(denied_open,1,-1);
  assert(!denied_open->vhf_keyboard_mouse && boundary.opens==1 && boundary.aborts==1 && boundary.closes==1 && injected.size()==1);

  // An already-open shared gamepad connection remains open after a failed KM
  // query, and likewise if a connected driver advertises incomplete capability.
  boundary={}; vhf_gamepad_t shared; shared.impl->client.is_connected=true;shared.impl->active_count=1;
  boundary.query_status=ERROR_NOT_SUPPORTED;assert(!shared.open_keyboard_mouse());assert(boundary.closes==0);
  boundary.query_status=0;boundary.capabilities=lvg::km::keyboard;assert(!shared.open_keyboard_mouse());assert(boundary.opens==0 && boundary.closes==0);

  // An open error can be a lost response after creation. The child is still
  // neutral: attempt same-owner removal and close the newly opened handle
  // even if abort fails, then use Win32 for the first input event.
  boundary={};injected.clear();boundary.open_status=ERROR_ACCESS_DENIED;boundary.abort_status=ERROR_ACCESS_DENIED;
  auto uncertain_open=input();assert(!uncertain_open->vhf_keyboard_mouse && boundary.aborts==1 && boundary.closes==1);
  button_mouse(uncertain_open,1,false);
  assert(boundary.events.empty() && injected.size()==1 && injected[0].mi.dwFlags==MOUSEEVENTF_LEFTDOWN);

  // A shared pad handle is preserved while attempting best-effort cleanup.
  boundary={}; vhf_gamepad_t shared_open;shared_open.impl->client.is_connected=true;shared_open.impl->active_count=1;
  boundary.open_status=ERROR_ACCESS_DENIED;boundary.abort_status=ERROR_ACCESS_DENIED;
  assert(!shared_open.open_keyboard_mouse() && boundary.aborts==1 && boundary.closes==0);

  // Real extracted input handlers route keyboard, consumer, mouse and scroll
  // to the shared client; duplicate keys do not gain extra ownership.
  boundary={};injected.clear();auto active=input();assert(active->vhf_keyboard_mouse && boundary.opens==1);
  keyboard_update(active,0x41,false,0);keyboard_update(active,0x41,false,0);keyboard_update(active,0x41,true,0);
  keyboard_update(active,0xad,false,0);keyboard_update(active,0xad,true,0);
  button_mouse(active,2,false);button_mouse(active,2,true);move_mouse(active,5,-4);scroll(active,120);hscroll(active,-120);
  assert(injected.empty() && boundary.events.size()==9);
  assert(boundary.events[0].type==1 && boundary.events[0].value==4 && boundary.events[0].pressed==1);
  assert(boundary.events[1].type==1 && boundary.events[1].pressed==0);
  assert(boundary.events[2].type==2 && boundary.events[2].value==0xe2 && boundary.events[2].pressed==1);
  assert(boundary.events[4].type==3 && boundary.events[4].value==3 && boundary.events[4].pressed==1);
  assert(boundary.events[6].type==4 && boundary.events[6].x==5 && boundary.events[6].y==-4);
  assert(boundary.events[7].value==1 && boundary.events[8].pressed==-1);
  touch_port_t virtual_desktop{0,0,3840,1080};
  abs_mouse(active,virtual_desktop,3839,1079);assert(boundary.events.back().type==5);
  auto before=boundary.events.size();abs_mouse(active,virtual_desktop,0,500);
  assert(boundary.events.size()==before && injected.size()==1 && injected.back().mi.dwFlags&MOUSEEVENTF_ABSOLUTE);

  // A failed submission is not replayed. Once abort confirms child removal,
  // releases of keys/buttons owned by that child are suppressed, while fresh
  // input uses SendInput. Another already-fallback Win32 key still gets its up.
  boundary={};injected.clear();auto failure=input();
  keyboard_update(failure,0x43,false,0);button_mouse(failure,1,false);
  keyboard_update(failure,0xba,false,0); // a second VHF key
  boundary.statuses.push_back(ERROR_DEVICE_REMOVED);
  keyboard_update(failure,0x41,false,0);
  assert(boundary.aborts==1 && boundary.events.size()==4 && injected.empty());
  // A held key from the removed device must not trigger host software-repeat
  // modifier pulses while its release is still pending.
  assert(keyboard_uses_native_repeat(failure,0x43));
  keyboard_update(failure,0x41,true,0);keyboard_update(failure,0x43,true,0);button_mouse(failure,1,true);
  assert(injected.empty()); keyboard_update(failure,0x42,false,0); keyboard_update(failure,0x42,true,0);
  assert(injected.size()==2 && injected[0].type==INPUT_KEYBOARD && (injected[1].ki.dwFlags&KEYEVENTF_KEYUP));

  // A key that was already using SendInput before child removal still needs
  // its matching release; an unrelated key may be newly pressed afterward.
  boundary={};injected.clear();boundary.capabilities &= ~lvg::km::consumer;
  auto mixed=input(); keyboard_update(mixed,0xad,false,0);assert(injected.size()==1);
  boundary.statuses.push_back(ERROR_DEVICE_REMOVED); keyboard_update(mixed,0x41,false,0);
  keyboard_update(mixed,0xad,true,0);
  assert(injected.size()==2 && (injected[1].ki.dwFlags&KEYEVENTF_KEYUP) && injected[1].ki.wVk==0xad);

  // Validated BUSY is the only retryable outcome. An immediate second submit
  // remains on the same HID connection, and no Win32 replay is generated.
  boundary={};injected.clear();auto busy=input();boundary.statuses.push_back(ERROR_BUSY);
  keyboard_update(busy,0x41,false,0);
  assert(boundary.events.size()==2 && boundary.aborts==0 && injected.empty());

  // A failed abort does not authorize fallback or replay into a second path.
  boundary={};injected.clear();auto uncertain=input();boundary.abort_status=ERROR_ACCESS_DENIED;
  boundary.statuses.push_back(ERROR_DEVICE_REMOVED);button_mouse(uncertain,1,false);
  button_mouse(uncertain,1,true);keyboard_update(uncertain,0x41,false,0);
  // An off-primary absolute target normally falls back to SendInput, but a
  // failed abort leaves the VHF mouse's held state unknown. It must remain
  // suppressed until teardown is confirmed, just like all relative input.
  abs_mouse(uncertain,virtual_desktop,0,500);
  assert(boundary.aborts==1 && boundary.events.size()==1 && injected.empty());

  // A later same-owner abort can establish removal without recreating input.
  // Only a new event uses Win32; the failed press is never replayed.
  boundary.abort_status=0;
  uncertain->vhf->impl->keyboard_mouse_retry_at={};
  move_mouse(uncertain,3,0);
  assert(boundary.aborts==2 && !uncertain->vhf_keyboard_mouse && injected.size()==1 && injected[0].mi.dx==3);
  keyboard_update(uncertain,0x42,false,0);
  assert(injected.size()==2 && boundary.events.size()==1);

  // Win32-only key presses must also wait while the old HID child's state is
  // uncertain. An already injected fallback key still needs its key-up.
  boundary={};injected.clear();boundary.capabilities &= ~lvg::km::consumer;
  auto uncertain_mixed=input();keyboard_update(uncertain_mixed,0xad,false,0);
  assert(injected.size()==1);
  boundary.abort_status=ERROR_ACCESS_DENIED;boundary.statuses.push_back(ERROR_DEVICE_REMOVED);
  button_mouse(uncertain_mixed,1,false);
  keyboard_update(uncertain_mixed,0xf0,false,0);keyboard_update(uncertain_mixed,0xf0,true,0);
  char text[] = "X"; unicode(uncertain_mixed,text,1);
  assert(injected.size()==1);
  keyboard_update(uncertain_mixed,0xad,true,0);
  assert(injected.size()==2 && (injected.back().ki.dwFlags&KEYEVENTF_KEYUP));
  keyboard_update(uncertain_mixed,0xf1,false,0);
  assert(keyboard_uses_native_repeat(uncertain_mixed,0xf1));
  boundary.abort_status=0;uncertain_mixed->vhf->impl->keyboard_mouse_retry_at={};
  move_mouse(uncertain_mixed,3,0);
  assert(!uncertain_mixed->vhf_keyboard_mouse && injected.size()==3);
  assert(keyboard_uses_native_repeat(uncertain_mixed,0xf1));
  keyboard_update(uncertain_mixed,0xf1,true,0);assert(injected.size()==3);
  unicode(uncertain_mixed,text,1);assert(injected.size()==5);

  // If the old control handle no longer accepts abort, close it and reconnect.
  // A busy PnP open must not authorize fallback, even if the new driver slot
  // would answer an abort as empty. Once open proves the prior child is gone,
  // synchronous abort of the new child makes future fallback safe.
  boundary={};injected.clear();boundary.abort_status=ERROR_DEVICE_REMOVED;
  auto stale=input();boundary.statuses.push_back(ERROR_DEVICE_REMOVED);
  button_mouse(stale,1,false);
  stale->vhf->impl->keyboard_mouse_retry_at={}; boundary.open_status=ERROR_BUSY;
  move_mouse(stale,1,0);
  assert(stale->vhf_keyboard_mouse && injected.empty() && boundary.opens==2 && boundary.closes==2);
  stale->vhf->impl->keyboard_mouse_retry_at={}; boundary.open_status=0;boundary.abort_status=0;
  move_mouse(stale,2,0);
  assert(!stale->vhf_keyboard_mouse && boundary.opens==3 && injected.size()==1 && injected[0].mi.dx==2);
  button_mouse(stale,1,true);assert(injected.size()==1);

  // A reconnected gamepad handle must also pass the PnP open check; do not
  // close it or accept an empty-slot abort as proof of prior child removal.
  boundary={};injected.clear();auto shared_recovery=input();
  shared_recovery->vhf->impl->active_count=1;
  shared_recovery->vhf->impl->keyboard_mouse_faulted=true;
  shared_recovery->vhf->impl->keyboard_mouse_owned=false;
  shared_recovery->vhf->impl->keyboard_mouse_recovery_needs_probe=true;
  boundary.open_status=ERROR_BUSY;
  move_mouse(shared_recovery,1,0);
  assert(shared_recovery->vhf_keyboard_mouse && injected.empty() && boundary.closes==0);
  shared_recovery->vhf->impl->keyboard_mouse_retry_at={}; boundary.open_status=0;
  move_mouse(shared_recovery,2,0);
  assert(!shared_recovery->vhf_keyboard_mouse && injected.size()==1 && boundary.closes==0);

  // Once the PnP probe succeeded, a failed abort leaves the replacement owned
  // by the shared connection. Retry that abort on the same handle, rather than
  // closing it, reopening another child, or switching to Win32 prematurely.
  boundary={};injected.clear();auto probe_abort=input();
  probe_abort->vhf->impl->active_count=1;
  probe_abort->vhf->impl->keyboard_mouse_faulted=true;
  probe_abort->vhf->impl->keyboard_mouse_owned=false;
  probe_abort->vhf->impl->keyboard_mouse_recovery_needs_probe=true;
  boundary.abort_status=ERROR_ACCESS_DENIED;
  move_mouse(probe_abort,1,0);
  assert(probe_abort->vhf_keyboard_mouse && probe_abort->vhf->impl->keyboard_mouse_owned &&
         !probe_abort->vhf->impl->keyboard_mouse_recovery_needs_probe && boundary.closes==0 && injected.empty());
  probe_abort->vhf->impl->keyboard_mouse_retry_at={};boundary.abort_status=0;
  move_mouse(probe_abort,2,0);
  assert(!probe_abort->vhf_keyboard_mouse && injected.size()==1 && boundary.closes==0 && boundary.opens==2);

  // Each mixed-backend injection holds the production lifetime lock. A feedback
  // worker cannot mark recovery pending between its safety check and SendInput.
  boundary={}; injected.clear(); auto locked=input();
  int guarded=0;
  before_inject=[&] {
    bool acquired=false;
    std::thread feedback([&] {
      acquired=locked->vhf->impl->lifetime.try_lock_shared();
      if (acquired) locked->vhf->impl->lifetime.unlock_shared();
    });
    feedback.join(); assert(!acquired); ++guarded;
  };
  keyboard_update(locked,0x15,false,0); // unmapped VK
  abs_mouse(locked,target_touch_port,0,0); // off-primary target
  char locked_text[]="X"; unicode(locked,locked_text,1);
  assert(guarded==4);
  before_inject={};
  // Existing fallback keys still receive releases during recovery. New
  // fallback presses, absolute motion, and Unicode remain suppressed.
  locked->vhf->impl->recovery.request();
  const auto sent=injected.size();
  keyboard_update(locked,0x16,false,0);
  abs_mouse(locked,target_touch_port,0,0);
  unicode(locked,locked_text,1);
  assert(injected.size()==sent);
  keyboard_update(locked,0x15,true,0);
  assert(injected.size()==sent+1 && (injected.back().ki.dwFlags & KEYEVENTF_KEYUP));
  locked->vhf->impl->recovery.reset();
  keyboard_update(locked,0x16,true,0);
  assert(injected.size()==sent+1); // suppressed press has no unmatched release

  // Queue admission is not mouse-class delivery. Delayed button and position
  // observations must be confirmed before crossing the two mouse collections.
  boundary={};injected.clear();
  auto ordered=input();
  button_mouse(ordered,1,false);
  delayed_button_reads=2;button_reads=0;
  assert(ordered->vhf->keyboard_mouse_absolute(50000,32000));
  assert(button_reads>=3 && !ordered->vhf->impl->keyboard_mouse_faulted);
  delayed_cursor_reads=2;cursor_reads=0;
  button_mouse(ordered,1,true);
  assert(cursor_reads>=3 && !ordered->vhf->impl->keyboard_mouse_faulted);
  assert(boundary.events.size()==3 && boundary.events[0].type==3 && boundary.events[0].pressed==1);
  assert(boundary.events[1].type==5 && boundary.events[2].type==3 && boundary.events[2].pressed==0);

  // A desktop that never acknowledges the press must not receive a move or
  // a replay through SendInput; recovery owns removal of uncertain held input.
  boundary={};injected.clear();delayed_cursor_reads=0;delayed_button_reads=0;
  auto unresponsive=input();button_mouse(unresponsive,1,false);
  delayed_button_reads=10000;
  assert(unresponsive->vhf->keyboard_mouse_absolute(50000,32000));
  assert(unresponsive->vhf->impl->keyboard_mouse_faulted && unresponsive->vhf->impl->recovery.pending());
  assert(boundary.events.size()==1 && injected.empty());
  delayed_button_reads=0;
}
'''


def main():
    submit = block(vhf_source, 'template<class Request>\n    bool submit_keyboard_mouse_locked(')
    prefix = PREFIX.replace('SUBMIT_METHOD', submit)
    prefix = prefix.replace('MOUSE_SETTLE_METHOD', block(vhf_source, 'bool settle_keyboard_mouse_locked('))
    vhf_methods = '\n'.join(block(vhf_source, marker) for marker in (
        'bool vhf_gamepad_t::open_keyboard_mouse()',
        'bool vhf_gamepad_t::keyboard_mouse_fallback_safe()',
        'bool vhf_gamepad_t::keyboard_mouse_fallback(',
        'void vhf_gamepad_t::keyboard_mouse_key(',
        'bool vhf_gamepad_t::keyboard_mouse_consumer_supported() const',
        'void vhf_gamepad_t::keyboard_mouse_consumer(',
        'void vhf_gamepad_t::keyboard_mouse_button(',
        'void vhf_gamepad_t::keyboard_mouse_relative(',
        'bool vhf_gamepad_t::keyboard_mouse_absolute(',
    ))
    input_methods = '\n'.join(block(input_source, marker) for marker in (
        'input_t input()',
        'static bool use_vhf_keyboard_mouse(',
        'void abs_mouse(',
        'void move_mouse(',
        'void button_mouse(',
        'void scroll(',
        'void hscroll(',
        'bool keyboard_uses_native_repeat(',
        'void keyboard_update(',
        'void unicode(',
    ))
    with tempfile.TemporaryDirectory(prefix='vhf-host-routing-') as directory:
        source = Path(directory) / 'test.cpp'
        program = Path(directory) / 'test'
        source.write_text(prefix + vhf_methods + input_methods + TEST, encoding='utf-8')
        subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-pthread', '-I', str(ROOT), str(source), '-o', str(program)], check=True)
        subprocess.run([str(program)], check=True)
    print('VHF keyboard/mouse production routing tests passed')


if __name__ == '__main__':
    main()
