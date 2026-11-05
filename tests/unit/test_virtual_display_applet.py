"""Exercise the real desktop policy, NVHTTP catalog adapter and remote projection.

No driver or Windows desktop is required. Pass --include-dir for nlohmann/json.
"""
from pathlib import Path
import argparse
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / 'tests/input'))
from test_delayed_mouse_release import function

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--include-dir', action='append', default=[])
parser.add_argument('--compiler', default='clang++')
args = parser.parse_args()
nvhttp = (root / 'src/nvhttp.cpp').read_text()
a = nvhttp.index('        std::vector<const proc::ctx_t *> visible_apps;', nvhttp.index('void applist('))
z = nvhttp.index('        const auto current_app =', a)
visibility = nvhttp[a:z]
# Keep the actual input-only catalog condition, rather than a test copy.
a = nvhttp.index('        const bool show_terminate_entry =', nvhttp.index('void applist('))
z = nvhttp.index('        auto app_list =', a)
conditions = nvhttp[a:z]
backend = function((root / 'src/platform/windows/virtual_display.cpp').read_text(), 'bool use_sunshine_driver()')
program = r'''
#include "src/session_display_policy.h"
#include "src/remote_session.h"
#include <algorithm>
#include <cassert>
#include <iostream>
namespace config { video_t video {}; input_t input {}; }
namespace util { int from_view(const std::string &s) { return std::stoi(s); } }
namespace proc {
int input_only_app_id = 900, terminate_app_id = 901;
struct ctx_t { std::string id, uuid, name; bool remote_monitor_profile = false; std::vector<std::string> id_aliases; };
}
''' + backend + function(nvhttp, 'std::vector<remote_session::app_t> remote_configured_catalogue(') + r'''
std::vector<remote_session::app_t> catalog(std::vector<proc::ctx_t> app_list, int current_appid) {
''' + conditions + visibility + r'''
return configured_apps;
}
struct Session {
 bool virtual_display = false;
 std::optional<std::string> output_name_override;
 std::optional<config::video_t::virtual_display_mode_e> virtual_display_mode_override;
 std::optional<config::video_t::dd_t::config_option_e> dd_config_option_override;
};
int main() {
 using namespace remote_session;
 using Mode = config::video_t::virtual_display_mode_e;
 using Prep = config::video_t::dd_t::config_option_e;
 const caller_t caller {.uuid="owner",.paired=true,.may_view=true,.may_launch=true,.may_terminate=true};
 const std::vector<proc::ctx_t> apps {{"31",VIRTUAL_DISPLAY_UUID,"Virtual Display"},{"32","game","Game"},{"33","shadow","Virtual Display"}};
 for (bool input_mode : {false,true}) {
  config::input.enable_input_only_mode = input_mode;
  for (int running : {0,31}) {
   const auto configured = catalog(apps,running);
   const game_t game {.running=running>0,.owner_uuid="owner",.app={31,VIRTUAL_DISPLAY_UUID,"Virtual Display",false}};
   const auto result = project(caller,game,{},configured,false);
   const auto count = [&](const std::string &uuid) { return std::count_if(result.catalogue.begin(), result.catalogue.end(), [&](const auto &a) { return a.uuid==uuid; }); };
   // Input-only mode keeps its minimal list while an app runs; the tile still shows.
   assert(count(VIRTUAL_DISPLAY_UUID)==1 && count("game")==((input_mode && running) ? 0 : 1) && count("shadow")==0);
   assert(identify(31,VIRTUAL_DISPLAY_UUID)==control_e::none);
  }
 }
 for (bool bundled : {false,true}) {
  config::video.dd.use_sunshine_virtual_display_driver=bundled;
  assert(use_sunshine_driver()==bundled);
  for (auto mode : {Mode::disabled,Mode::per_client,Mode::shared}) {
   config::video.virtual_display_mode=mode;
   for (auto prep : {Prep::disabled,Prep::verify_only,Prep::ensure_active,Prep::ensure_primary,Prep::ensure_only_display}) {
    config::video.dd.configuration_option=prep;
    Session s; s.output_name_override="physical"; s.virtual_display_mode_override=Mode::disabled;
    proc::display_policy::apply_builtin_virtual_display(s,VIRTUAL_DISPLAY_UUID,config::video);
    assert(s.virtual_display && s.output_name_override=="sunshine:virtual_display");
    assert(s.virtual_display_mode_override==Mode::per_client);
    assert(use_sunshine_driver()==bundled); // The applet never changes provider selection.
    Session shared; shared.virtual_display_mode_override=Mode::shared;
    proc::display_policy::apply_builtin_virtual_display(shared,VIRTUAL_DISPLAY_UUID,config::video);
    assert(shared.virtual_display_mode_override==Mode::shared);
    Session inherited;
    proc::display_policy::apply_builtin_virtual_display(inherited,VIRTUAL_DISPLAY_UUID,config::video);
    assert(inherited.virtual_display_mode_override.value_or(mode)==(mode==Mode::disabled ? Mode::per_client : mode));
    if(prep==Prep::disabled || prep==Prep::verify_only) assert(inherited.dd_config_option_override==Prep::ensure_active);
    Session ordinary; ordinary.output_name_override="physical";
    proc::display_policy::apply_builtin_virtual_display(ordinary,"game",config::video);
    assert(!ordinary.virtual_display && ordinary.output_name_override=="physical" && !ordinary.virtual_display_mode_override);
   }
  }
  const auto error=proc::display_policy::builtin_virtual_display_error(VIRTUAL_DISPLAY_UUID,false,config::video);
  assert(error.find(bundled ? "Vibepollo" : "SudoVDA")!=std::string::npos);
  assert(proc::display_policy::builtin_virtual_display_error(VIRTUAL_DISPLAY_UUID,true,config::video).empty());
  assert(proc::display_policy::builtin_virtual_display_error("game",false,config::video).empty());
 }
 std::cout << "PASS Virtual Display: catalog in idle/running and input-only modes; explicit virtual target; shared/per-client modes; preparation; selected provider; failure messages\n";
}
'''
with tempfile.TemporaryDirectory(prefix='virtual-display-applet-') as tmp:
    cpp = Path(tmp) / 'test.cpp'
    cpp.write_text(program)
    exe = Path(tmp) / 'test'
    # CMake passes INTERFACE_INCLUDE_DIRECTORIES as one ';'-separated argument.
    includes = ['-I' + str(root)] + ['-I' + p for group in args.include_dir for p in group.split(';') if p]
    link = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections']
    subprocess.run([args.compiler, '-std=c++23', '-O0', '-ffunction-sections', '-fdata-sections', *includes,
                    str(root / 'src/remote_session.cpp'), str(cpp), *link, '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
