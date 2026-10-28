"""Exercise the production display-warning observer and Linux target decision.

Only process/client lookup and display-driver work are replaced. No display,
service or session is started. --source-dir enables an untouched negative control.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source-dir', type=Path, default=root / 'src')
parser.add_argument('--xml-output', type=Path)
args = parser.parse_args()
source = args.source_dir
nvhttp = (source / 'nvhttp.cpp').read_text()

def function(signature):
    start = nvhttp.index(signature)
    begin = nvhttp.index('{', start)
    depth = 1
    end = begin + 1
    while depth:
        depth += (nvhttp[end] == '{') - (nvhttp[end] == '}')
        end += 1
    return nvhttp[start:end]

if 'void add_client_display_warning(' not in nvhttp:
    raise SystemExit('NEGATIVE CONTROL: successful responses have no warning observer')
legacy_observer = 'bool client_display_warning_requested(' not in nvhttp
helper = ('template<class Client>\n' if legacy_observer else '') + function('void add_client_display_warning(')
capture = '' if legacy_observer else 'template<class Client, class App>\n' + function('bool client_display_warning_requested(')
observe_call = 'add_client_display_warning(tree, args, session, client, joining);' if legacy_observer else 'add_client_display_warning(tree, session, joining);'
capture_call = '(void) args; (void) session; (void) client;' if legacy_observer else 'session.client_display_warning_requested = client_display_warning_requested(args, client, proc::proc.resolve_app(session.appid));'
mode_parser = function('  std::optional<config::video_t::virtual_display_mode_e> parse_virtual_display_mode_override(')
layout_parser = function('  std::optional<config::video_t::virtual_display_layout_e> parse_virtual_display_layout_override(')
linux = (source / 'platform/linux/private_display.cpp').read_text()
start = linux.index('    const auto mode = session.virtual_display_mode_override.value_or(config::video.virtual_display_mode);', linux.index('  prepare_result_t prepare_session('))
selection = linux[start:linux.index('\n    session.virtual_display = false;', start)]
program = r'''
#include "src/session_display_policy.h"
#include "src/app_display_policy.h"
#include <boost/algorithm/string.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <cassert>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>
namespace pt = boost::property_tree;
namespace config { video_t video {}; }
namespace remote_session { enum class role_e { game, input, monitor }; }
namespace rtsp_stream {
struct launch_session_t {
 bool virtual_display = false, client_requests_virtual_display = false, virtual_display_failed = false, client_display_warning_requested = false, input_only = false;
 remote_session::role_e role = remote_session::role_e::game;
 int appid = 42;
 std::optional<bool> client_virtual_display_override;
 std::optional<std::string> output_name_override;
 std::optional<config::video_t::virtual_display_mode_e> virtual_display_mode_override;
 std::optional<config::video_t::virtual_display_layout_e> virtual_display_layout_override;
};
}
namespace platf::linux_display {
bool independent = true;
struct caps { bool independent_outputs; };
struct backend_t { caps capabilities() const { return {independent}; } };
const backend_t &backend() { static backend_t b; return b; }
}
struct Client {
 bool always_use_virtual_display = false;
 std::string output_name_override, virtual_display_mode_override, virtual_display_layout_override;
};
struct App {
 bool virtual_screen = false, virtual_display = false;
 std::optional<std::string> output_name_override;
 std::optional<config::video_t::virtual_display_mode_e> virtual_display_mode_override;
 std::optional<config::video_t::virtual_display_layout_e> virtual_display_layout_override;
};
namespace proc {
std::optional<App> application = App {};
struct process { std::optional<App> resolve_app(int appid) { return appid == 42 ? application : std::nullopt; } } proc;
}
using args_t = std::unordered_map<std::string, std::string>;
std::string get_arg(const args_t &args, const std::string &key, const std::string &fallback) {
 auto it = args.find(key); return it == args.end() ? fallback : it->second;
}
''' + mode_parser + '\n' + layout_parser + '\n' + capture + '\n' + helper + r'''
void capture_intent(const args_t &args, rtsp_stream::launch_session_t &session, const std::optional<Client> &client) {
''' + capture_call + r'''
}
void observe(pt::ptree &tree, const args_t &args, const rtsp_stream::launch_session_t &session, const std::optional<Client> &client, bool joining) {
 (void) args; (void) client;
''' + observe_call + r'''
}
bool linux_requests_virtual(const rtsp_stream::launch_session_t &session) {
 struct { bool requested = false; } result;
''' + selection + r'''
 return result.requested;
}
int main(int argc, char **argv) {
 using Mode = config::video_t::virtual_display_mode_e;
 using Prep = config::video_t::dd_t::config_option_e;
 config::video.virtual_display_mode = Mode::per_client;
 config::video.dd.configuration_option = Prep::verify_only;
 auto client = std::optional<Client> {Client {}};
 int checks = 0;
 auto check = [&](std::string wire, bool expected, rtsp_stream::launch_session_t session = {}, bool joining = false, int status = 200, std::string verb = "launch") {
   args_t args;
   if (wire != "absent") args["virtualDisplay"] = wire;
   pt::ptree tree;
   tree.put("root.<xmlattr>.status_code", status);
   tree.put(verb == "launch" ? "root.gamesession" : "root.resume", status == 200 ? 1 : 0);
   tree.put("root.sessionUrl0", "rtsp://127.0.0.1:48010");
   auto before = tree;
   capture_intent(args, session, client);
   observe(tree, args, session, client, joining);
   const auto code = tree.get<std::string>("root.clientWarning", "");
   assert(!code.empty() == expected);
   assert(code.empty() || code == "virtual_display_disabled_by_client");
   tree.get_child("root").erase("clientWarning");
   assert(tree == before); // Observer changes neither status, URL nor success flag.
   assert(config::video.dd.configuration_option == Prep::verify_only);
   ++checks;
 };
 rtsp_stream::launch_session_t physical;
 physical.client_virtual_display_override = false;
 physical.virtual_display_mode_override = Mode::disabled;
 assert(!linux_requests_virtual(physical));
 check("0", true, physical);
 check("0", true, physical, false, 200, "resume");
 check("absent", false, physical);
 check("1", false, physical);
 check("00", false, physical);
 check("garbage", false, physical);
 check(std::string(40000, '0'), false, physical);
 for (int status : {400, 401, 403, 409, 503}) check("0", false, physical, false, status);
 check("0", false, physical, true);
 auto actual = physical;
 actual.virtual_display = true;
 check("0", false, actual); // Successful virtual output must never be called physical.
 actual = physical; actual.virtual_display_failed = true;
 check("0", false, actual); // Creation fallback isn't an explicit-disable warning.
 actual = physical; actual.role = remote_session::role_e::monitor;
 check("0", false, actual);
 actual.role = remote_session::role_e::input; check("0", false, actual);
 actual = physical; actual.input_only = true; check("0", false, actual);
 proc::application->virtual_display_mode_override = Mode::per_client;
 check("0", true, physical); // App-mode request is overridden by explicit wire 0.
 proc::application->virtual_screen = true;
 actual = physical; actual.virtual_display = true;
 assert(linux_requests_virtual(actual));
 check("0", false, actual); // Legacy app flag still wins in this production path.
 proc::application = App {};
 proc::application->output_name_override = "DP-1";
 physical.output_name_override = "DP-1";
 assert(!linux_requests_virtual(physical));
 check("0", false, physical); // Explicit app physical beats global virtual.
 proc::application = App {}; physical.output_name_override.reset();
 client->virtual_display_mode_override = "disabled";
 check("0", false, physical);
 client->virtual_display_mode_override.clear();
 client->output_name_override = "DP-1"; check("0", false, physical);
 client->output_name_override.clear();
 config::video.virtual_display_mode = Mode::disabled; check("0", false, physical);
 proc::application->virtual_screen = true;
 check("0", false, physical, false, 200, "resume"); // Legacy flag alone may be ignored independently of query 0.
 proc::application = App {};
 proc::application->virtual_display_layout_override = config::video_t::virtual_display_layout_e::extended;
 check("0", false, physical); // Layout alone is not a creation request.
 proc::application = App {};
 config::video.virtual_display_mode = Mode::per_client;
 platf::linux_display::independent = false; check("0", false, physical);
 platf::linux_display::independent = true;
 physical.appid = 0; proc::application->output_name_override = "DP-1";
 check("0", true, physical, false, 200, "resume"); // Unresolved app isn't imported just for warning.
 // A settings edit between preparation and admission must not rewrite the
 // intent captured from the app/client snapshots that actually built the session.
 args_t zero {{"virtualDisplay", "0"}};
 physical.appid = 42;
 proc::application = App {};
 config::video.virtual_display_mode = Mode::disabled;
 auto race = [&](bool original_virtual) {
   proc::application->virtual_display_mode_override = original_virtual ? Mode::per_client : Mode::disabled;
   capture_intent(zero, physical, client);
   proc::application->virtual_display_mode_override = original_virtual ? Mode::disabled : Mode::per_client;
   pt::ptree response;
   response.put("root.<xmlattr>.status_code", 200); response.put("root.resume", 1);
   observe(response, zero, physical, client, false);
   assert(!response.get<std::string>("root.clientWarning", "").empty() == original_virtual);
   ++checks;
 };
 race(false); race(true);
 proc::application = App {};
 config::video.virtual_display_mode = Mode::per_client;
 client->virtual_display_mode_override = "disabled";
 capture_intent(zero, physical, client);
 client->virtual_display_mode_override.clear();
 pt::ptree changed_client; changed_client.put("root.<xmlattr>.status_code", 200);
 observe(changed_client, zero, physical, client, false);
 assert(changed_client.get<std::string>("root.clientWarning", "").empty());
 ++checks;
 if (argc == 2) {
   pt::ptree tree; tree.put("root.<xmlattr>.status_code", 200); tree.put("root.resume", 1);
   tree.put("root.sessionUrl0", "rtsp://127.0.0.1:48010");
   args_t args {{"virtualDisplay", "0"}};
   capture_intent(args, physical, client);
   observe(tree, args, physical, client, false);
   std::ofstream output(argv[1]); pt::write_xml(output, tree);
 }
 std::cout << checks << " production warning cases passed\n";
}
'''
with tempfile.TemporaryDirectory(prefix='client-display-warning-') as temporary:
    directory = Path(temporary)
    cpp = directory / 'test.cpp'
    cpp.write_text(program)
    binary = directory / 'test'
    subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-I', str(source.parent), str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)] + ([str(args.xml_output)] if args.xml_output else []), check=True)
