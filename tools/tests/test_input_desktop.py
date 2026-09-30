#!/usr/bin/env python3
"""Replay actual Windows desktop/input functions against Win32 contract fakes.

This validates retry bounds and handle ownership, not physical monitor focus.
"""
import os, subprocess, sys, tempfile
from pathlib import Path
root=Path(sys.argv[1]) if len(sys.argv)>1 else Path(__file__).resolve().parents[2]
source=(root/'src/platform/windows/input.cpp').read_text()
misc=(root/'src/platform/windows/misc.cpp').read_text()
def function(text, signature):
 start=text.index(signature); brace=text.index('{',start); depth=1; end=brace+1
 while depth:
  depth += (text[end]=='{')-(text[end]=='}'); end+=1
 return text[start:end]
sync=function(misc, '  HDESK syncThreadDesktop()' if '  HDESK syncThreadDesktop()' in misc else '  bool syncThreadDesktop()')
if '  class input_desktop_binding_t' in misc:
 start=misc.index('  class input_desktop_binding_t'); end=misc.index('  bool syncThreadDesktop()',start)
 sync=misc[start:end]+sync
cache='thread_local HDESK _lastKnownInputDesktop = nullptr;' if '_lastKnownInputDesktop' in source else ''
send=function(source,'  void send_input(INPUT &i)')
pointer=function(source,'  bool inject_synthetic_pointer_input(')
fakes=r'''
#include <cassert>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <unordered_map>
#include <utility>
using namespace std::literals;
using HDESK=void*;
using UINT32=unsigned;
using HSYNTHETICPOINTERDEVICE=std::uintptr_t;
constexpr unsigned DF_ALLOWOTHERACCOUNTHOOK=1, GENERIC_ALL=0xffff;
constexpr bool FALSE=false;
struct INPUT {};
struct POINTER_TYPE_INFO {};
unsigned sends=0, injections=0, opens=0, closes=0, close_failures=0, logs=0;
HDESK initial_desktop=reinterpret_cast<HDESK>(1), current_desktop=initial_desktop;
std::uintptr_t next_handle=100;
unsigned input_desktop_object=2;
std::unordered_map<HDESK,unsigned> desktop_objects {{initial_desktop,1}};
bool fail_open=false, fail_set=false, fail_forever=false;
std::unordered_set<HDESK> open_handles;
unsigned GetCurrentThreadId() { return 1; }
HDESK GetThreadDesktop(unsigned) { return current_desktop; }
HDESK OpenInputDesktop(unsigned,bool,unsigned) {
 ++opens; if(fail_open) return 0;
 auto handle=reinterpret_cast<HDESK>(next_handle++); open_handles.insert(handle); desktop_objects[handle]=input_desktop_object; return handle;
}
bool SetThreadDesktop(HDESK desktop) {
 if(fail_set) return false;
 // An already-selected desktop may keep the thread's existing handle.
 if(desktop_objects[desktop]!=desktop_objects[current_desktop]) current_desktop=desktop;
 return true;
}
bool CloseDesktop(HDESK desktop) {
 if(desktop==current_desktop || desktop==initial_desktop) { ++close_failures; return false; }
 ++closes; desktop_objects.erase(desktop); return open_handles.erase(desktop)!=0;
}
unsigned GetLastError() { return 5; }
struct Log { template<class T> Log& operator<<(const T&) { return *this; } ~Log() { ++logs; } };
#define BOOST_LOG(level) Log{}
namespace util {
 struct Hex { std::string_view to_string_view() { return "5"; } };
 Hex hex(unsigned) { return {}; }
}
unsigned SendInput(unsigned,INPUT*,unsigned) {
 if(++sends>3) throw std::runtime_error("input retry exceeded the two-attempt contract");
 return fail_forever || sends==1 ? 0 : 1;
}
bool inject(HSYNTHETICPOINTERDEVICE,const POINTER_TYPE_INFO*,UINT32) {
 if(++injections>3) throw std::runtime_error("pointer retry exceeded the two-attempt contract");
 return !(fail_forever || injections==1);
}
struct input_raw_t { decltype(&inject) fnInjectSyntheticPointerInput=inject; };
'''
main=r'''
int main(int argc,char**argv) {
 std::string mode=argv[1];
 INPUT i; POINTER_TYPE_INFO p; input_raw_t raw;
 try {
  if(mode=="persistent-send") { fail_forever=true; platf::send_input(i); assert(sends<=2 && opens<=1); }
  else if(mode=="persistent-pointer") { fail_forever=true; assert(!platf::inject_synthetic_pointer_input(&raw,1,&p,1)); assert(injections<=2 && opens<=1); }
  else if(mode=="failed-attachment") { fail_set=true; assert(!platf::syncThreadDesktop()); assert(current_desktop==initial_desktop && open_handles.empty()); }
  else if(mode=="desktop-ownership") { assert(platf::syncThreadDesktop()); ++input_desktop_object; assert(platf::syncThreadDesktop()); ++input_desktop_object; assert(platf::syncThreadDesktop()); assert(open_handles.size()==1 && closes==2 && close_failures==0); }
  else if(mode=="same-desktop") {
   input_desktop_object=1; assert(platf::syncThreadDesktop()); assert(platf::syncThreadDesktop());
   assert(current_desktop==initial_desktop && open_handles.empty() && close_failures==0);
  }
  else if(mode=="failed-attachment-send") { fail_set=true; platf::send_input(i); assert(sends==1 && current_desktop==initial_desktop && open_handles.empty()); }
  else if(mode=="failed-attachment-pointer") { fail_set=true; assert(!platf::inject_synthetic_pointer_input(&raw,1,&p,1)); assert(injections==1 && open_handles.empty()); }
  else if(mode=="thread-exit") {
   std::thread worker([] { assert(platf::syncThreadDesktop()); assert(platf::syncThreadDesktop()); assert(open_handles.size()==1); });
   worker.join(); assert(current_desktop==initial_desktop && open_handles.empty() && close_failures==0);
  }
  else if(mode=="transient-send") { platf::send_input(i); assert(sends==2 && opens==1); }
  else if(mode=="transient-pointer") { assert(platf::inject_synthetic_pointer_input(&raw,1,&p,1)); assert(injections==2 && opens==1); }
  else if(mode=="failed-open") { fail_open=true; platf::send_input(i); assert(sends==1 && opens==1); }
  else return 2;
  fail_set=false;
  std::cout<<mode<<" PASS attempts="<<sends+injections<<" open="<<opens<<" retained="<<open_handles.size()<<" failed_close="<<close_failures<<'\n';
 } catch(const std::exception&e) { std::cerr<<mode<<" FAIL: "<<e.what()<<'\n'; return 1; }
}
'''
# Each replay is isolated so desktop ownership and retry state do not leak
# between scenarios. Win32 fakes enforce the documented CloseDesktop lifetime.
with tempfile.TemporaryDirectory(prefix='415-desktop-') as tmp:
 cpp=Path(tmp)/'replay.cpp'; exe=Path(tmp)/'replay'
 cpp.write_text(fakes+'\nnamespace platf {\n'+cache+'\n'+sync+'\n'+send+'\n'+pointer+'\n}\n'+main)
 subprocess.run([sys.argv[2] if len(sys.argv)>2 else os.environ.get('CXX','c++'),'-std=c++23','-O2','-pthread',str(cpp),'-o',str(exe)],check=True)
 results=[]
 for scenario in ['persistent-send','persistent-pointer','failed-attachment','desktop-ownership','same-desktop','failed-attachment-send','failed-attachment-pointer','thread-exit','transient-send','transient-pointer','failed-open']:
  result=subprocess.run([str(exe),scenario],capture_output=True,text=True,timeout=5)
  print((result.stdout+result.stderr).strip()); results.append(result.returncode)
 sys.exit(any(results))
