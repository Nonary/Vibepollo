"""Bounded, deterministic production-code replay. Never calls native Windows APIs.
Extracts production branch bodies and actual focus helpers; substitutes OS ports,
clock/sleep, metadata/discovery order. No application repository writes/builds.
"""
from pathlib import Path
import argparse
import importlib.util
import subprocess
import hashlib
import json
import tempfile


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo',type=Path,default=Path(__file__).resolve().parents[2])
    parser.add_argument('--compiler',default='g++')
    parser.add_argument('--output-dir',type=Path)
    args=parser.parse_args()
    root=args.repo.resolve()
    temporary=tempfile.TemporaryDirectory(prefix='windows-autofocus-foreground-')
    out=args.output_dir.resolve() if args.output_dir else Path(temporary.name)
    out.mkdir(parents=True,exist_ok=True)
    module=Path(__file__).with_name('test_managed_app_focus_adapters.py')
    spec=importlib.util.spec_from_file_location('adapters',module);adapter=importlib.util.module_from_spec(spec);spec.loader.exec_module(adapter)
    block=adapter.block
    src=(root/'tools/playnite_launcher/launcher.cpp').read_text()
    utils=(root/'tools/playnite_launcher/focus_utils.cpp').read_text()
    worker=(root/'src/platform/windows/managed_app_focus.cpp').read_text()
    # Reuse only the deterministic ports; controller blocks come from --repo.
    base=adapter.windows_adapter_harness(root)
    base=base.split('int main(){')[0]
    base=base.replace('using HWND=uintptr_t;','using HWND=void*;')
    base=base.replace('DWORD GetWindowThreadProcessId(HWND window,DWORD *pid) { if(pid)*pid=window<20?7:8;return 1; }',r'''
    using BOOL=int;using HRESULT=long;using HMODULE=void*;using PVOID=void*;
    #define WINAPI
    constexpr int GW_OWNER=4,SW_RESTORE=9,SWP_NOMOVE=2,SWP_NOSIZE=1,MONITOR_DEFAULTTONULL=0;
    constexpr BOOL TRUE=1,FALSE=0;
    HWND H(uintptr_t n){return reinterpret_cast<HWND>(n);}
    HWND HWND_TOPMOST=H(9998),HWND_NOTOPMOST=H(9999);
    struct RECT{long left,top,right,bottom;};
    struct Window{DWORD pid;bool visible=true,owned=false,minimized=false,cloaked=false,onmonitor=true,validrect=true;};
    std::map<HWND,Window> windows;
    std::vector<std::string> effects;
    DWORD GetWindowThreadProcessId(HWND w,DWORD *pid){if(pid)*pid=windows.at(w).pid;return 1;}
    BOOL IsWindowVisible(HWND w){return windows.at(w).visible;}
    HWND GetWindow(HWND w,int){return windows.at(w).owned?H(9000):nullptr;}
    BOOL IsIconic(HWND w){return windows.at(w).minimized;}
    BOOL GetWindowRect(HWND w,RECT*r){long x=windows.at(w).onmonitor?0:100000;*r={x,0,x+(windows.at(w).validrect?100:0),100};return TRUE;}
    HWND MonitorFromRect(RECT*r,int){return r->left==0?H(1):nullptr;}
    HRESULT dwm(HWND w,DWORD,PVOID p,DWORD){*static_cast<DWORD*>(p)=windows.at(w).cloaked;return 0;}
    HMODULE LoadLibraryW(const wchar_t*){return H(1);}
    void* GetProcAddress(HMODULE,const char*){return reinterpret_cast<void*>(&dwm);}
    #define SUCCEEDED(x) ((x)>=0)
    DWORD GetCurrentThreadId(){return 2;}
    BOOL AttachThreadInput(DWORD,DWORD,BOOL attach){effects.push_back(attach?"attach:on":"attach:off");return TRUE;}
    BOOL ShowWindow(HWND w,int){windows.at(w).minimized=false;effects.push_back("restore:"+std::to_string(reinterpret_cast<uintptr_t>(w)));return TRUE;}
    BOOL SetWindowPos(HWND,HWND target,int,int,int,int,int){effects.push_back(target==HWND_TOPMOST?"topmost:on":"topmost:off");return TRUE;}
    BOOL SetForegroundWindow(HWND w){++focus_calls;effects.push_back("foreground:"+std::to_string(reinterpret_cast<uintptr_t>(w)));foreground=w;return TRUE;}
    template<class T> void fake_sleep(T){}
    ''')
    base=base.replace('#include <iostream>','#include <iostream>\n#define BOOST_LOG(level) std::ostringstream{}\n#include <map>\n#include <filesystem>\n#include <thread>')
    old_focus=block(utils,'bool focus_by_install_dir_extended(')
    markers=['bool is_candidate_window(','bool try_focus_hwnd(','bool confirm_foreground_pid(','std::string window_identity(','bool focus_game_window(']
    for marker in ['static bool is_eligible_game_window(', 'HWND find_foreground_game_window(']:
        if marker in utils: markers.insert(2,marker)
    # Put the eligibility helper before its callers.
    markers.sort(key=lambda marker:utils.index(marker))
    production='\n'.join(block(utils,m) for m in markers)+'\n'+old_focus
    production=production.replace('std::this_thread::sleep_for','fake_sleep').replace('std::chrono::steady_clock::now()','test_now')
    # The old blocking loop requires advancing virtual sleep; new sleeps are only100ms.
    production=production.replace('fake_sleep(1s);','test_now += 1s;')
    focus_namespace=block(base,'namespace focus {')
    newfocus=r'''namespace focus {
    std::vector<HWND> discover_game_windows(const std::wstring &,const std::wstring &={},bool=false){if(during_discovery)during_discovery();return offered;}
    std::vector<DWORD> find_pids_under_install_dir_sorted(const std::wstring&){std::vector<DWORD> p;for(auto w:offered){auto id=windows.at(w).pid;if(std::find(p.begin(),p.end(),id)==p.end())p.push_back(id);}return p;}
    HWND find_main_window_for_pid(DWORD pid){for(auto w:offered)if(windows.at(w).pid==pid)return w;return nullptr;}
    ''' + production + '\n}\nnamespace playnite_launcher { namespace focus = ::focus; }'
    newfocus=newfocus.replace('const std::wstring &={},','const std::wstring &unused=std::wstring{},')
    base=base.replace(focus_namespace,newfocus)
    # Exercise the actual fullscreen notification prefix, including lifecycle dedup.
    notification=block(src,'if (msg.status_name == "gameStarted")')
    notification=notification[:notification.index('          cleanup_spawn_signal.store(true);')]+'}'
    base=base.replace('std::string install_dir="/game",exe_path="/game/game.exe";', 'std::string install_dir="/game",exe_path="/game/game.exe",id_original,id_norm,cleanup_dir;')
    base=base.replace(' void tick(){bool active_game_now', ' std::atomic<bool> playnite_user_exit{false}; bool game_started_once=false;\n std::string resolve_install_dir(const std::string& dir,const std::string&){return dir;}\n void notify(std::string id="game"){struct {std::string status_name="gameStarted",status_game_id="game",status_install_dir="/game",status_exe="/game/game.exe";}msg;msg.status_game_id=id;std::string norm_id=id;'+notification+'}\n void tick(){bool active_game_now')
    loop=block(worker,'while (!cancelled_').replace('budget::clock::now()','test_now')
    worker_scaffold=r'''
    struct Worker {
     managed_app_focus::policy options{3,15,false};std::atomic<bool> cancelled_{false};std::mutex mutex_;
     struct Wake{int waits=0;template<class A,class B,class C>void wait_for(A&,B,C){test_now+=1s;if(++waits>2000)std::abort();}}wake_;
     void run(){struct{std::filesystem::path install_dir="/game";}app;managed_app_focus::budget remaining(options,start);WORKER_LOOP}
    };
    void clean(){reset();windows={{H(11),{7}},{H(12),{7}},{H(21),{8}}};effects.clear();}
    int failures=0;
    void result(const char*name,bool condition){if(!condition)++failures;std::cout<<name<<(condition?" PASS":" FAIL")<<" activations="<<focus_calls<<" final="<<reinterpret_cast<uintptr_t>(foreground)<<" effects=";for(auto&s:effects)std::cout<<s<<",";std::cout<<"\n";}
    int main(){
     // Same PID auxiliary HWND first, render HWND already foreground: regression
     // relative to the old PID-level passive acceptance.
     clean();offered={H(11),H(12)};foreground=H(12);Worker multi;multi.run();result("managed_same_pid_foreground_preserved",focus_calls==0&&foreground==H(12));
     clean();offered={H(11),H(12)};foreground=H(12);focus::focus_by_install_dir_extended(L"/game",3,15,false,{},nullptr);result("old_same_pid_foreground_preserved",focus_calls==0&&foreground==H(12));
     clean();offered={H(12)};foreground=H(12);Worker single;single.run();result("managed_single_window_stops_after_three_confirmations",focus_calls==0&&test_now==start+3s);
     // Actual regular Playnite branch: preserved while foreground allowance is live.
     clean();offered={H(11),H(12)};foreground=H(12);Direct regular;regular.tick();result("regular_initial_foreground_guard",focus_calls==0&&foreground==H(12));
     // A real game remains foreground for 20s; a new auxiliary HWND appears then.
     clean();offered={H(12)};foreground=H(12);Direct late;late.tick();for(int n=1;n<20;++n){test_now=start+std::chrono::seconds(n);late.tick();}test_now=start+20s;offered={H(11),H(12)};late.tick();result("regular_late_auxiliary_preserves_foreground_after_allowance_expires",focus_calls==0&&foreground==H(12));
     clean();offered={H(12)};foreground=H(12);Direct gamefirst;gamefirst.tick();for(int n=1;n<20;++n){test_now=start+std::chrono::seconds(n);gamefirst.tick();}test_now=start+20s;offered={H(12),H(11)};gamefirst.tick();result("regular_late_auxiliary_preserves_foreground_game_first",focus_calls==0&&foreground==H(12));
     clean();offered={H(12)};foreground=H(12);Direct early;early.tick();test_now=start+10s;offered={H(11),H(12)};early.tick();result("regular_early_auxiliary_does_not_steal",focus_calls==0&&foreground==H(12));
     // Actual fullscreen branch has no initial passive-any-game guard.
     clean();offered={H(11),H(12)};foreground=H(12);Fullscreen fs;fs.tick();result("fullscreen_same_pid_foreground_preserved",focus_calls==0&&foreground==H(12));
     // These candidates are only rejected AFTER native activation effects occur.
     clean();windows[H(11)].cloaked=true;offered={H(11),H(12)};foreground=H(12);Fullscreen cloak;cloak.tick();result("cloaked_auxiliary_has_no_effects",focus_calls==0&&effects.empty()&&foreground==H(12));
     clean();windows[H(11)].minimized=true;offered={H(11),H(12)};foreground=H(12);Fullscreen minimized;minimized.tick();result("minimized_auxiliary_has_no_effects",focus_calls==0&&effects.empty()&&windows[H(11)].minimized&&foreground==H(12));
     // Same observed HWND never rearms merely because it is discovered again.
     clean();offered={H(12)};foreground=H(12);Direct stale;stale.tick();test_now=start+20s;foreground=nullptr;stale.tick();result("expired_existing_window_not_reactivated",focus_calls==0&&foreground==nullptr);
     clean();Direct hard;test_now=start+181s;offered={H(11)};hard.tick();result("default_hard_cap",focus_calls==0&&!hard.request_game_focus);
     clean();Direct custom;custom.config.focus_timeout_secs=300;test_now=start+220s;offered={H(11)};custom.tick();result("custom_timeout_allows_after180",focus_calls==1&&foreground==H(11));
     clean();Direct disabled;disabled.config.focus_attempts=0;offered={H(11)};disabled.tick();result("attempts_zero_disables_managed_branch",focus_calls==0&&!disabled.request_game_focus);
     // Original intended improvement: a late FIRST window gets focused.
     clean();Direct slow;slow.tick();test_now=start+25s;offered={H(12)};slow.tick();result("late_first_window_still_supported",focus_calls==1&&foreground==H(12));
     clean();offered={H(12)};foreground=H(12);Fullscreen dup;dup.active_game_flag=false;dup.notify();dup.tick();test_now=start+220s;dup.notify();offered={H(11),H(12)};dup.tick();result("fullscreen_duplicate_start_does_not_rearm",focus_calls==0&&foreground==H(12)&&!dup.game_focus_budget_active);
     clean();offered={H(12)};foreground=H(12);Direct replacement;replacement.tick();test_now=start+25s;offered={H(11)};foreground=nullptr;windows.erase(H(12));replacement.tick();result("genuine_late_replacement_focused",focus_calls==1&&foreground==H(11));
     for(int kind=0;kind<4;++kind){clean();auto&w=windows[H(11)];if(kind==0)w.cloaked=true;if(kind==1)w.minimized=true;if(kind==2)w.onmonitor=false;if(kind==3)w.validrect=false;offered={H(11)};bool accepted=focus::focus_game_window(H(11),{});result("ineligible_target_no_activation",!accepted&&effects.empty()&&focus_calls==0);}
     clean();Direct custom_guard;custom_guard.config.focus_timeout_secs=300;offered={H(12)};foreground=H(12);custom_guard.tick();test_now=start+220s;offered={H(11),H(12)};custom_guard.tick();result("custom_timeout_secondary_does_not_displace_game",focus_calls==0&&foreground==H(12));
     clean();Fullscreen restart;restart.active_game_flag=false;restart.notify();restart.tick();restart.game_stop_signal=true;restart.active_game_flag=false;restart.tick();test_now=start+220s;restart.notify();offered={H(12)};restart.tick();result("genuine_new_lifecycle_can_focus_same_game",focus_calls==1&&foreground==H(12));
     clean();Fullscreen different;different.active_game_flag=false;different.notify();different.tick();test_now=start+220s;different.notify("different");offered={H(21)};different.tick();result("different_game_can_arm_while_previous_was_active",focus_calls==1&&foreground==H(21));
     return failures?1:0;
    }
    '''.replace('WORKER_LOOP',loop)
    cpp=out/'production_replay.cpp';cpp.write_text(base+worker_scaffold)
    meta={'source_sha256':{str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in [root/'src/managed_app_focus.h',root/'src/platform/windows/managed_app_focus.cpp',root/'tools/playnite_launcher/launcher.cpp',root/'tools/playnite_launcher/focus_utils.cpp']},'method':'production blocks extracted verbatim; clock/sleep and native OS/discovery metadata ports stubbed; no native Windows calls','checks':24}
    (out/'manifest.json').write_text(json.dumps(meta,indent=2))
    subprocess.run([args.compiler,'-std=c++23','-O0','-pthread','-I',str(root),str(cpp),'-o',str(out/'production_replay')],check=True)
    with (out/'results.txt').open('w') as f: completed=subprocess.run([str(out/'production_replay')],stdout=f)
    print((out/'results.txt').read_text())
    raise SystemExit(completed.returncode)


if __name__ == "__main__":
    main()
