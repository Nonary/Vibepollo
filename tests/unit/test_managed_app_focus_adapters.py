"""Execute production Playnite branches and KWin script with deterministic OS ports.

The platform effects are stubbed; the timing, branch bodies, and KWin selection
code are read verbatim from production. This is not a Windows/macOS runtime test.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile


def block(source, marker):
    begin = source.index(marker)
    brace = source.index('{', begin)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[begin:end]


def windows_helpers(root, compiler):
    source = (root / 'tools/playnite_launcher/focus_utils.cpp').read_text()
    production = '\n'.join(block(source, marker) + (';' if marker.startswith('struct ') else '') for marker in (
        'bool is_candidate_window(', 'struct window_search', 'BOOL CALLBACK collect_window_proc(',
        'std::wstring normalize_path(', 'std::vector<HWND> discover_game_windows(',
        'std::string window_identity(', 'bool focus_game_window('))
    harness = r'''
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>
using namespace std::chrono_literals;
using DWORD=uint32_t;using HWND=void*;using LPARAM=intptr_t;using BOOL=int;
#define CALLBACK
constexpr BOOL TRUE=1;
constexpr int GW_OWNER=4;
HWND handle(uintptr_t n){return reinterpret_cast<HWND>(n);}
struct Window{DWORD pid;bool visible=true,owned=false;};
std::map<HWND,Window> windows{{handle(11),{7}},{handle(12),{7}},{handle(13),{7,false}},{handle(14),{7,true,true}},{handle(21),{8}}};
std::map<DWORD,std::wstring> images{{7,L"C:/game/game.exe"},{8,L"D:/other/game.exe"}};
HWND foreground=0;int activations=0;std::function<void()> during_activate;
DWORD GetWindowThreadProcessId(HWND w,DWORD *pid){if(pid)*pid=windows.at(w).pid;return 1;}
BOOL IsWindowVisible(HWND w){return windows.at(w).visible;}
HWND GetWindow(HWND w,int){return windows.at(w).owned?handle(100):0;}
HWND GetForegroundWindow(){return foreground;}
void EnumWindows(BOOL (*callback)(HWND,LPARAM),LPARAM p){for(auto &[w,info]:windows)if(!callback(w,p))break;}
namespace platf::dxgi {std::vector<DWORD> find_process_ids_by_name(const wchar_t*){return {7,8};}}
namespace playnite_launcher::focus {
std::vector<DWORD> find_pids_under_install_dir_sorted(const std::wstring&){return {7};}
bool get_process_image_path(DWORD pid,std::wstring &image){image=images.at(pid);return true;}
bool confirm_foreground_pid(DWORD pid){return foreground&&windows.at(foreground).pid==pid;}
bool try_focus_hwnd(HWND w){++activations;foreground=w;if(during_activate)during_activate();return true;}
PRODUCTION
}
int main(){using namespace playnite_launcher::focus;
assert((discover_game_windows(L"C:/game",L"",false)==std::vector<HWND>{handle(11),handle(12)}));
assert((discover_game_windows(L"",L"C:/game/game.exe",true)==std::vector<HWND>{handle(11),handle(12)}));
// Legacy basename-only metadata keeps its fallback, a full path excludes the other install.
assert((discover_game_windows(L"",L"game.exe",true)==std::vector<HWND>{handle(11),handle(12),handle(21)}));
assert(window_identity(handle(11))!=""&&window_identity(handle(11))!=window_identity(handle(12))&&window_identity(handle(12))!=window_identity(handle(21)));
foreground=handle(12);assert(focus_game_window(handle(12),{})&&activations==0);
foreground=0;assert(!focus_game_window(handle(11),[]{return true;})&&activations==0);
assert(!focus_game_window(handle(13),{})&&activations==0);assert(!focus_game_window(handle(14),{})&&activations==0);
bool stopped=false;during_activate=[&]{stopped=true;};assert(!focus_game_window(handle(11),[&]{return stopped;})&&activations==1);
std::cout<<"production Windows discovery/focus helpers with OS ports: passed\n";
}
'''.replace('PRODUCTION', production)
    with tempfile.TemporaryDirectory(prefix='managed-focus-helpers-') as directory:
        directory = Path(directory)
        cpp = directory / 'helpers.cpp'
        cpp.write_text(harness)
        subprocess.run([compiler, '-std=c++23', '-pthread', str(cpp), '-o', str(directory / 'helpers')], check=True)
        subprocess.run([str(directory / 'helpers')], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', default='g++')
    parser.add_argument('--node', default='node')
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    root = args.repo.resolve()
    launcher = (root / 'tools/playnite_launcher/launcher.cpp').read_text()
    direct = block(launcher, 'auto focus_game_after_start = [&]()') + ';\nfocus_game_after_start();'
    fullscreen = '\n'.join(block(launcher, marker) for marker in (
        'if (game_start_signal.exchange(false))', 'if (game_stop_signal.exchange(false))',
        'if (active_game_now && game_focus_budget_active'))
    # Inject the clock at the production effect boundary, not a parallel model.
    direct = direct.replace('std::chrono::steady_clock::now()', 'test_now')
    schedule = block(launcher, 'auto schedule_focus_retry = [&]()').replace('std::chrono::steady_clock::now()', 'test_now') + ';schedule_focus_retry();'
    stop = block(launcher, 'const auto cancel_game_focus = [&]') + ';cancel_game_focus();'
    fullscreen = fullscreen.replace('std::chrono::steady_clock::now()', 'test_now')
    harness = r'''
#include "src/managed_app_focus.h"
#include <atomic>
#include <cassert>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <vector>
using namespace std::chrono_literals;
using DWORD=uint32_t;
using HWND=uintptr_t;
auto start=managed_app_focus::budget::clock::time_point(1h);
auto test_now=start;
HWND foreground=0;
std::vector<HWND> offered;
int focus_calls=0;
bool permit_focus=true;
std::function<void()> during_focus,during_foreground,during_discovery;
HWND GetForegroundWindow() { return foreground; }
DWORD GetWindowThreadProcessId(HWND window,DWORD *pid) { if(pid)*pid=window<20?7:8;return 1; }
namespace platf::dxgi { std::wstring utf8_to_wide(const std::string &s) {return {s.begin(),s.end()};} }
namespace focus {
std::vector<HWND> discover_game_windows(const std::wstring &,const std::wstring &,bool=false){if(during_discovery)during_discovery();return offered;}
std::string window_identity(HWND w) { DWORD pid;GetWindowThreadProcessId(w,&pid);return std::to_string(pid)+":"+std::to_string(w); }
bool focus_game_window(HWND w,std::function<bool()> cancel) {
 if(cancel())return false;
 ++focus_calls;
 if(during_focus)during_focus();
 if(cancel()||!permit_focus)return false;
 foreground=w; return true;
}
}
auto millis_to_steady(int64_t n){return managed_app_focus::budget::clock::time_point(std::chrono::milliseconds(n));}
int64_t steady_to_millis(managed_app_focus::budget::clock::time_point t){return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();}
#define BOOST_LOG(level) std::ostringstream{}
struct Config { int focus_attempts=3; int focus_timeout_secs=15; bool focus_exit_on_first=false; };
struct Direct {
 Config config;
 std::atomic<bool> got_started{true},game_stop_pending{false};
 std::atomic<bool> request_game_focus{true},should_exit{false},game_focus_confirmed{false},had_focus_success{false},lossless_refocus_pending{false},focus_refresh_requested{false};
 std::atomic<int> game_focus_successes_left{3};
 std::atomic<DWORD> last_confirmed_focus_pid{0};
 std::atomic<int64_t> focus_armed_ms{steady_to_millis(start)},next_focus_attempt_ms{std::numeric_limits<int64_t>::min()};
 std::atomic<uint32_t> focus_generation{1};
 std::mutex focus_state_mutex;
 uint32_t tracked_focus_generation=0;
 std::optional<managed_app_focus::budget> game_focus_budget;
 bool focus_exit_on_first_flag=false;
 int lossless_restarts=0;
 auto snapshot_game_info(){return std::pair{std::string("/game"),std::string("/game/game.exe")};}
 bool is_game_foreground(){if(during_foreground)during_foreground();return foreground!=0;}
 DWORD foreground_pid(){DWORD pid;GetWindowThreadProcessId(foreground,&pid);return pid;}
 void restart_lossless_after_refocus(){++lossless_restarts;}
 void tick(){ DIRECT_BODY }
 void schedule(){ SCHEDULE_BODY }
 void stop(){ STOP_BODY }
};
struct Fullscreen {
 Config config;
 std::atomic<bool> game_start_signal{true},game_stop_signal{false},active_game_flag{true};
 bool game_focus_budget_active=false,fullscreen_focus_budget_active=false;
 int game_successes_left=0,fullscreen_successes_left=0;
 auto_type_placeholder
 std::optional<managed_app_focus::budget> game_focus_budget;
 std::mutex game_mutex;
 struct {std::string install_dir="/game",exe_path="/game/game.exe";} game_state;
 void tick(){bool active_game_now=active_game_flag.load(); FULLSCREEN_BODY }
};
void reset(){ test_now=start;foreground=0;offered.clear();focus_calls=0;permit_focus=true;during_focus={};during_foreground={};during_discovery={}; }
int main(){
 reset();Direct d;
 test_now=start+16s;d.tick();assert(d.request_game_focus&&focus_calls==0&&d.game_focus_successes_left==3);
 test_now=start+25s;offered={11};d.tick();assert(focus_calls==1&&d.game_focus_successes_left==2);
 for(int n=26;n<40;++n){test_now=start+std::chrono::seconds(n);d.tick();}
 assert(focus_calls==1&&d.game_focus_successes_left==2); // continuous foreground dedupe
 test_now=start+41s;foreground=0;d.tick();assert(focus_calls==1); // stale window gets no activation
 test_now=start+50s;offered={11,12};d.schedule();d.tick();assert(foreground==12);assert(focus_calls==2&&d.game_focus_successes_left==1&&d.lossless_restarts==1);
 test_now=start+52s;foreground=0;d.tick();assert(focus_calls==3&&d.game_focus_successes_left==0&&!d.request_game_focus);
 reset();Direct hard;test_now=start+179s;hard.tick();assert(hard.request_game_focus);test_now=start+180s;offered={11};hard.tick();assert(!hard.request_game_focus&&focus_calls==0);
 reset();Direct slow_scan;test_now=start+25s;offered={11};during_discovery=[]{test_now+=1s;};slow_scan.tick();assert(slow_scan.game_focus_budget->deadline()==start+41s);
 reset();Direct stopped;test_now=start+25s;offered={11};during_focus=[&]{stopped.request_game_focus=false;++stopped.focus_generation;};stopped.tick();assert(stopped.game_focus_successes_left==3&&foreground==0);
 reset();Direct stopped_at_confirmation;test_now=start+25s;offered={11};foreground=11;during_foreground=[&]{stopped_at_confirmation.stop();};stopped_at_confirmation.tick();assert(!stopped_at_confirmation.request_game_focus&&stopped_at_confirmation.game_focus_successes_left==0&&!stopped_at_confirmation.game_focus_confirmed&&!stopped_at_confirmation.had_focus_success);
 reset();Direct refreshed;refreshed.tick();test_now=start+25s;offered={11};refreshed.tick();test_now=start+35s;refreshed.schedule();refreshed.tick();assert(refreshed.game_focus_budget->can_focus(test_now)&&refreshed.game_focus_successes_left==2);test_now=start+55s;refreshed.schedule();refreshed.tick();assert(!refreshed.game_focus_budget->can_focus(test_now));test_now=start+180s;refreshed.tick();refreshed.schedule();assert(!refreshed.request_game_focus);
 reset();Direct fresh;fresh.request_game_focus=false;fresh.focus_armed_ms=0;fresh.game_focus_successes_left=0;fresh.schedule();assert(fresh.request_game_focus&&fresh.game_focus_successes_left==3);fresh.stop();fresh.game_stop_pending=true;fresh.schedule();assert(!fresh.request_game_focus&&fresh.game_focus_successes_left==0);
 reset();Direct multi;test_now=start+25s;offered={11,12};foreground=12;multi.tick();assert(focus_calls==0&&multi.game_focus_successes_left==2);test_now=start+26s;multi.tick();assert(focus_calls==0&&multi.game_focus_successes_left==2);
 reset();Direct disabled;disabled.config.focus_attempts=0;disabled.tick();assert(!disabled.request_game_focus&&focus_calls==0);
 reset();Direct first;first.focus_exit_on_first_flag=true;test_now=start+25s;offered={11};first.tick();assert(!first.request_game_focus&&focus_calls==1);
 reset();Fullscreen fs;fs.tick();test_now=start+16s;fs.tick();assert(fs.game_focus_budget_active&&focus_calls==0);test_now=start+25s;offered={11};fs.tick();assert(focus_calls==1&&fs.game_successes_left==2);
 test_now=start+41s;fs.tick();assert(focus_calls==1);test_now=start+50s;offered={11,21};fs.tick();assert(foreground==21);assert(focus_calls==2&&fs.game_successes_left==1);
 fs.game_stop_signal=true;fs.active_game_flag=false;test_now=start+51s;fs.tick();assert(!fs.game_focus_budget_active&&!fs.game_focus_budget&&focus_calls==2);
 reset();Fullscreen empty;empty.tick();test_now=start+180s;offered={11};empty.tick();assert(!empty.game_focus_budget_active&&focus_calls==0);
 reset();Fullscreen longwait;longwait.config.focus_timeout_secs=240;longwait.tick();test_now=start+200s;offered={11};longwait.tick();assert(focus_calls==1);test_now=start+240s;longwait.tick();assert(!longwait.game_focus_budget_active);
 reset();Fullscreen zero;zero.config.focus_attempts=0;zero.tick();test_now=start+25s;offered={11};zero.tick();assert(focus_calls==0);
 std::cout<<"production Playnite branches: passed\n";
}
'''
    harness = harness.replace('auto_type_placeholder', '''managed_app_focus::budget::clock::time_point next_game_focus_check=start,next_fullscreen_focus_check=start,fullscreen_focus_deadline=start;''')
    harness = harness.replace('DIRECT_BODY', direct).replace('FULLSCREEN_BODY', fullscreen).replace('SCHEDULE_BODY', schedule).replace('STOP_BODY', stop)
    with tempfile.TemporaryDirectory(prefix='managed-focus-paths-') as directory:
        directory = Path(directory)
        cpp = directory / 'production.cpp'
        cpp.write_text(harness)
        subprocess.run([args.compiler, '-std=c++23', '-pthread', '-I', str(root), str(cpp), '-o', str(directory / 'paths')], check=True)
        subprocess.run([str(directory / 'paths')], check=True)
    windows_helpers(root, args.compiler)
    header = (root / 'packaging/linux/managed_app_focus_kwin.h').read_text()
    script = re.search(r'R"JS\((.*?)\)JS"', header, re.S).group(1)
    script = script[:script.index('callDBus(')]
    js = r'''
const assert=require('assert'),vm=require('vm');
const production=PRODUCTION;
function run(windows,pids,activate,accept=()=>true) {
 const workspace={windowList:()=>windows}; let active=null;
 Object.defineProperty(workspace,'activeWindow',{get:()=>active,set:w=>{if(accept(w))active=w;}});
 const context={workspace,pids,activate};vm.runInNewContext(production,context);
 return {result:JSON.parse(JSON.stringify(context.result)),active};
}
function window(pid,id,extra={}){return {pid,internalId:id,normalWindow:true,skipTaskbar:false,fullScreen:false,minimized:true,...extra};}
let native=window(7,'wayland-game'),xwayland=window(8,'xwayland-game',{fullScreen:true}),splash=window(7,'splash');
let ignored=[window(99,'unrelated'),window(7,'dialog',{normalWindow:false}),window(7,'taskbar',{skipTaskbar:true})];
let scan=run([splash,native,xwayland,...ignored],[7,8],[]);assert.deepStrictEqual(scan.result.windows,['8:xwayland-game','7:splash','7:wayland-game']);assert.strictEqual(scan.active,null);
let apply=run([splash,native,xwayland,...ignored],[7,8],['7:wayland-game']);assert.strictEqual(apply.active,native);assert.strictEqual(native.minimized,false);assert.strictEqual(apply.result.focused,true);
assert.strictEqual(run([native],[7],['7:gone']).active,null);
assert.strictEqual(run([native],[7],['7:wayland-game'],()=>false).result.focused,false);
assert.deepStrictEqual(run([native,xwayland],[8],[]).result.windows,['8:xwayland-game']);
assert.deepStrictEqual(run([native,xwayland],[],[]).result.windows,[]);
assert.deepStrictEqual(run([], [7],[]).result.windows,[]);
let fallback=run([xwayland,native],[7,8],['8:xwayland-game','7:wayland-game'],w=>w===native);assert.strictEqual(fallback.active,native);assert.strictEqual(fallback.result.focused,true);
console.log('production KWin script: passed');
'''.replace('PRODUCTION', json.dumps(script))
    subprocess.run([args.node, '-e', js], check=True)


if __name__ == '__main__':
    main()
