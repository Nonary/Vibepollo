"""Run actual RTSP display-field assembly and Windows preparation construction on Linux.

Windows and Linux preparation fragments run with client/app lookup and platform APIs replaced. --source-dir supports a
red/green comparison against an untouched source snapshot.
"""
import argparse
import pathlib
import subprocess
import tempfile
import sys

root = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / 'tests/input'))
from test_delayed_mouse_release import function as production_function
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source-dir', type=pathlib.Path, default=root / 'src')
parser.add_argument('--compiler', default='g++')
parser.add_argument('--include-dir', action='append', default=[],
                    help='Dependency include path, repeatable; accepts CMake semicolon lists')
args = parser.parse_args()
dependency_includes = [flag for group in args.include_dir for path in group.split(';')
                       if path for flag in ('-I', path)]
source = args.source_dir
launch = (source / 'nvhttp.cpp').read_text()
helpers = (source / 'platform/windows/display_helper_request_helpers.cpp').read_text()
webrtc = (source / 'webrtc_stream.cpp').read_text()
linux = (source / 'platform/linux/private_display.cpp').read_text()
start = linux.index('    auto effective_video = config::video;')
linux_fields = linux[start:linux.index('    const auto parsed = display_device::parse_configuration', start)]

client_start = launch.index('      if (!verified_client->virtual_display_mode_override.empty())')
client_end = launch.index('      launch_session->unique_id =', client_start)
output_start = launch.index('      if (!verified_client->output_name_override.empty())', client_end)
output_end = launch.index('      const auto original_client_name', output_start)
client_fields = launch[client_start:client_end] + launch[output_start:output_end]
app_start = launch.index('          launch_session->virtual_display = app_ctx->virtual_screen;')
app_end = launch.index('          launch_session->app_metadata = std::move(metadata);', app_start)
app_fields = launch[app_start:app_end] + '          launch_session->app_metadata = std::move(metadata);\n'
web_start = webrtc.index('            launch_session->virtual_display = app_ctx->virtual_screen;')
web_end = webrtc.index('            launch_session->app_metadata = std::move(metadata);', web_start)
web_fields = webrtc[web_start:web_end]
web_finalizer = ''
web_marker = '      // Resolve display preparation after client and application fields.'
if web_marker in webrtc:
    start = webrtc.index(web_marker)
    web_finalizer = webrtc[start:webrtc.index('      auto key = crypto::rand(16);', start)]
# The assembly finalizer belongs after both representations have been merged.
finalizer = ''
marker = '    // Resolve display preparation after client and application fields.'
if marker in launch:
    start = launch.index(marker)
    finalizer = launch[start:launch.index('    launch_session->framegen_refresh_rate.reset();', start)]
start = helpers.index('  SessionDisplayConfigurationHelper::SessionDisplayConfigurationHelper(')
constructor = helpers[start:helpers.index('  std::optional<display_device::Resolution>', start)]
finalize_function = ''
resume_finalize = ''
if '    void finalize_display_preparation(' in launch:
    start = launch.index('    void finalize_display_preparation(')
    finalize_function = production_function(launch, 'void finalize_display_preparation(')
    resume_finalize = ' finalize_display_preparation(*launch_session, verified_client);'
start = launch.index('    // A resume request carries no appid or appuuid')
start = launch.index('          // launch_session->virtual_display is deliberately left alone:', start)
end = launch.index('          launch_session->app_metadata = std::move(metadata);', start)
resume_fields = launch[start:end] + '          launch_session->app_metadata = std::move(metadata);\n'
policy_include = '#include "src/session_display_policy.h"' if (source / 'session_display_policy.h').exists() else ''
program = r"""
#include "src/config.h"
#include "src/platform/windows/display_helper_request_policy.h"
#include <boost/algorithm/string.hpp>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
""" + policy_include + r"""
namespace config {
video_t video {};
std::optional<std::string> runtime_output_name_override() { return std::nullopt; }
}
namespace rtsp_stream {
struct launch_session_t {
 struct app_metadata_t { bool virtual_screen = false; };
 std::optional<app_metadata_t> app_metadata;
 bool virtual_display = false, client_requests_virtual_display = false;
 bool requested_resolution_only = false;
 bool virtual_display_preparation_override = false;
 std::optional<bool> display_preparation_client_virtual_override;
 std::string virtual_display_device_id;
 std::optional<bool> client_virtual_display_override;
 std::optional<config::video_t::virtual_display_mode_e> virtual_display_mode_override;
 std::optional<config::video_t::virtual_display_layout_e> virtual_display_layout_override;
 std::optional<config::video_t::dd_t::config_option_e> dd_config_option_override;
 std::optional<std::string> output_name_override, hdr_profile;
};
}
struct App {
 bool virtual_screen = false, virtual_display = false;
 std::optional<config::video_t::virtual_display_mode_e> virtual_display_mode_override;
 std::optional<config::video_t::virtual_display_layout_e> virtual_display_layout_override;
 std::optional<config::video_t::dd_t::config_option_e> dd_config_option_override;
 std::optional<std::string> output_name_override;
 std::unordered_map<std::string, std::string> config_overrides;
};
struct Client {
 bool always_use_virtual_display = false;
 bool requested_resolution_only = false;
 std::string output_name_override, virtual_display_mode_override, virtual_display_layout_override, hdr_profile;
 std::unordered_map<std::string, std::string> config_overrides;
};
auto parse_virtual_display_mode_override(const std::string &v) -> std::optional<config::video_t::virtual_display_mode_e> {
 using E = config::video_t::virtual_display_mode_e;
 if (v == "per_client") return E::per_client;
 if (v == "shared") return E::shared;
 if (v == "disabled") return E::disabled;
 return std::nullopt;
}
auto parse_virtual_display_layout_override(const std::string &v) -> std::optional<config::video_t::virtual_display_layout_e> {
 using E = config::video_t::virtual_display_layout_e;
 if (v == "extended") return E::extended;
 if (v == "extended_primary") return E::extended_primary;
 return std::nullopt;
}
namespace nvhttp {
std::unordered_map<std::string, std::string> client_overrides;
auto get_client_config_overrides(const std::string &) { return client_overrides; }
}
struct SessionOptions { std::optional<std::string> client_uuid; };
namespace util { int from_view(const std::string &v) { return std::stoi(v); } }
namespace proc { namespace display_policy {} }
using verified_client_t = std::optional<Client>;
""" + finalize_function + r"""
auto assemble(Client client, App app, const std::unordered_map<std::string, std::string> &args) {
 auto launch_session = std::make_shared<rtsp_stream::launch_session_t>();
 verified_client_t verified_client {client};
 (void) args;
 rtsp_stream::launch_session_t::app_metadata_t metadata {app.virtual_screen || app.virtual_display};
 auto app_ctx = std::make_shared<App>(app);
""" + client_fields + app_fields + finalizer + r"""
 return launch_session;
}
auto assemble_resume(Client client, App app, const std::unordered_map<std::string, std::string> &args) {
 auto launch_session = assemble(client, {}, args);
 auto app_ctx = std::make_shared<App>(app);
 verified_client_t verified_client {client};
 rtsp_stream::launch_session_t::app_metadata_t metadata {app.virtual_screen || app.virtual_display};
 (void) verified_client;
""" + resume_fields + resume_finalize + r"""
 return launch_session;
}
auto assemble_webrtc(Client client, App app) {
 auto launch_session = std::make_shared<rtsp_stream::launch_session_t>();
 auto app_ctx = std::make_shared<App>(app);
 SessionOptions options {"paired-client"};
 nvhttp::client_overrides = client.config_overrides;
 (void) options;
""" + web_fields + web_finalizer + r"""
 return launch_session;
}
auto linux_preparation(const rtsp_stream::launch_session_t &session) {
""" + linux_fields + r"""
 return effective_video.dd.configuration_option;
}
namespace display_helper_integration::helpers {
class SessionDisplayConfigurationHelper {
public:
 config::video_t video_config_, effective_video_config_;
 const rtsp_stream::launch_session_t &session_;
 bool include_request_detail_log_fields_;
 SessionDisplayConfigurationHelper(const config::video_t &, const rtsp_stream::launch_session_t &, bool = false, bool = true);
};
""" + constructor + r"""
}
int main() {
 using Prep = config::video_t::dd_t::config_option_e;
 using Mode = config::video_t::virtual_display_mode_e;
 using Layout = config::video_t::virtual_display_layout_e;
 config::video.dd.configuration_option = Prep::verify_only;
 config::video.virtual_display_layout = Layout::extended;
 int failed = 0, count = 0;
 auto check = [&](const char *name, Client client, App app, Prep expected, std::unordered_map<std::string, std::string> args = {}, bool virtual_target = true) {
   auto session = assemble(client, app, args);
   session->virtual_display = virtual_target;
   display_helper_integration::helpers::SessionDisplayConfigurationHelper helper(config::video, *session);
   ++count;
   if (helper.effective_video_config_.dd.configuration_option != expected) { std::cerr << "FAIL RTSP " << name << "\n"; ++failed; }
   ++count;
   if (linux_preparation(*session) != expected) { std::cerr << "FAIL Linux RTSP " << name << "\n"; ++failed; }
   auto resumed = assemble_resume(client, app, args);
   resumed->virtual_display = virtual_target;
   display_helper_integration::helpers::SessionDisplayConfigurationHelper resume_helper(config::video, *resumed);
   count += 2;
   if (resume_helper.effective_video_config_.dd.configuration_option != expected) { std::cerr << "FAIL resume " << name << "\n"; ++failed; }
   if (linux_preparation(*resumed) != expected) { std::cerr << "FAIL Linux resume " << name << "\n"; ++failed; }
   if (client.output_name_override.empty() && client.virtual_display_mode_override.empty() &&
       client.virtual_display_layout_override.empty() && !client.always_use_virtual_display && !app.virtual_display && args.empty()) {
     auto web_session = assemble_webrtc(client, app);
     web_session->virtual_display = virtual_target;
     display_helper_integration::helpers::SessionDisplayConfigurationHelper web_helper(config::video, *web_session);
     ++count;
     if (web_helper.effective_video_config_.dd.configuration_option != expected) { std::cerr << "FAIL WebRTC " << name << "\n"; ++failed; }
     ++count;
     if (linux_preparation(*web_session) != expected) { std::cerr << "FAIL Linux WebRTC " << name << "\n"; ++failed; }
   }
 };
 check("true inherited VerifyOnly", {}, {}, Prep::verify_only);
 ++count;
 if (assemble({}, {}, {})->requested_resolution_only) { std::cerr << "FAIL RTSP requested-resolution-only defaults false\n"; ++failed; }
 Client requested_resolution_client; requested_resolution_client.requested_resolution_only = true;
 ++count;
 if (!assemble(requested_resolution_client, {}, {})->requested_resolution_only) { std::cerr << "FAIL RTSP requested-resolution-only propagates true\n"; ++failed; }
 App app; app.virtual_display_mode_override = Mode::per_client;
 check("app mode beats global VerifyOnly", {}, app, Prep::ensure_active);
 app = {}; app.virtual_display_layout_override = Layout::extended_primary;
 check("app layout beats global VerifyOnly", {}, app, Prep::ensure_active);
 app = {}; app.output_name_override = "sunshine:virtual_display";
 check("app VIRTUAL-DISPLAY beats global VerifyOnly", {}, app, Prep::ensure_active);
 app = {}; app.virtual_screen = true;
 check("legacy app virtual-screen beats global VerifyOnly", {}, app, Prep::ensure_active);
 app = {}; app.virtual_display = true;
 check("Vibepollo legacy app virtual-display beats global VerifyOnly", {}, app, Prep::ensure_active);
 Client client; client.virtual_display_mode_override = "shared";
 check("client mode beats global VerifyOnly", client, {}, Prep::ensure_active);
 client = {}; client.virtual_display_layout_override = "extended";
 check("client layout beats global VerifyOnly", client, {}, Prep::ensure_active);
 client = {}; client.output_name_override = "sunshine:virtual_display";
 check("client VIRTUAL-DISPLAY beats global VerifyOnly", client, {}, Prep::ensure_active);
 client = {}; client.always_use_virtual_display = true;
 check("client always virtual beats global VerifyOnly", client, {}, Prep::ensure_active);
 check("explicit virtualDisplay=1 beats global VerifyOnly", {}, {}, Prep::ensure_active, {{"virtualDisplay", "1"}});
 app = {}; app.dd_config_option_override = Prep::ensure_active;
 client = {}; client.config_overrides["dd_configuration_option"] = "verify_only";
 check("client generic beats dedicated app", client, app, Prep::verify_only);
 app = {}; app.virtual_screen = true; app.dd_config_option_override = Prep::verify_only;
 check("explicit app VerifyOnly", {}, app, Prep::verify_only);
 client = {}; client.output_name_override = "physical";
 check("physical target keeps VerifyOnly", client, {}, Prep::verify_only);
 client = {}; client.virtual_display_mode_override = "disabled";
 check("disabled mode keeps VerifyOnly", client, {}, Prep::verify_only);
 check("explicit client disable keeps VerifyOnly", {}, {}, Prep::verify_only, {{"virtualDisplay", "0"}});
 app = {}; app.config_overrides["dd_configuration_option"] = "ensure_active";
 client = {}; client.config_overrides["dd_configuration_option"] = "verify_only";
 check("client generic beats app generic", client, app, Prep::verify_only);
 app = {}; app.dd_config_option_override = Prep::verify_only;
 client.config_overrides["dd_configuration_option"] = "ensure_primary";
 check("client generic activating policy beats dedicated app VerifyOnly", client, app, Prep::ensure_primary);
 app = {}; app.virtual_screen = true; app.config_overrides["dd_configuration_option"] = "verify_only";
 check("app generic VerifyOnly remains explicit", {}, app, Prep::verify_only);
 client = {}; client.virtual_display_mode_override = "per_client";
 check("client virtual choice beats lower app generic VerifyOnly", client, app, Prep::ensure_active);
 client.config_overrides["dd_configuration_option"] = "verify_only";
 check("equal-tier client VerifyOnly remains explicit", client, app, Prep::verify_only);
 app = {}; app.output_name_override = "sunshine:virtual_display";
 check("virtual failure retains physical VerifyOnly", {}, app, Prep::verify_only, {}, false);
 client = {}; client.virtual_display_mode_override = "disabled";
 check("client disabled beats lower app virtual target", client, app, Prep::verify_only);
 client = {}; client.output_name_override = "physical";
 app = {}; app.virtual_display_layout_override = Layout::extended_primary;
 check("client physical beats lower app virtual layout", client, app, Prep::verify_only, {}, false);
 app = {}; app.dd_config_option_override = Prep::ensure_primary;
 app.config_overrides["dd_configuration_option"] = "verify_only";
 check("dedicated app prep wins same-app generic", {}, app, Prep::ensure_primary);
 client = {}; client.output_name_override = "physical";
 app = {}; app.output_name_override = "sunshine:virtual_display";
 ++count;
 if (assemble(client, app, {})->output_name_override != client.output_name_override) {
   std::cerr << "FAIL client physical output must beat app virtual output\n"; ++failed;
 }
 client.output_name_override = "sunshine:virtual_display";
 app.output_name_override = "physical";
 ++count;
 if (assemble(client, app, {})->output_name_override != client.output_name_override) {
   std::cerr << "FAIL client virtual output must beat app physical output\n"; ++failed;
 }
 client = {}; client.output_name_override = "physical";
 app = {}; app.output_name_override = "sunshine:virtual_display";
 ++count;
 if (assemble_resume(client, app, {})->output_name_override != client.output_name_override) {
   std::cerr << "FAIL resumed client physical output must beat app virtual output\n"; ++failed;
 }
 client.output_name_override = "sunshine:virtual_display";
 app.output_name_override = "physical";
 ++count;
 if (assemble_resume(client, app, {})->output_name_override != client.output_name_override) {
   std::cerr << "FAIL resumed client virtual output must beat app physical output\n"; ++failed;
 }
 namespace Policy = display_helper_integration::request_policy;
 for (const auto layout : {Layout::extended, Layout::extended_primary, Layout::extended_isolated, Layout::extended_primary_isolated}) {
   app = {}; app.virtual_display_layout_override = layout;
   auto session = assemble({}, app, {});
   session->virtual_display = true;
   display_helper_integration::helpers::SessionDisplayConfigurationHelper helper(config::video, *session);
   const auto prep = helper.effective_video_config_.dd.configuration_option;
   const auto result = Policy::evaluate({
     .configuration_option = prep == Prep::verify_only ? Policy::ConfigurationOption::VerifyOnly : Policy::ConfigurationOption::EnsureActive,
     .layout = static_cast<Policy::VirtualDisplayLayout>(layout),
     .virtual_display = true,
     .target_device_id = "virtual",
     .topology_snapshot = {{"physical"}},
   });
   const auto expected = layout == Layout::extended_primary || layout == Layout::extended_primary_isolated ? Policy::DevicePreparation::EnsurePrimary : Policy::DevicePreparation::EnsureActive;
   ++count;
   if (result.device_preparation != expected || result.topology != (std::vector<std::vector<std::string>> {{"physical"}, {"virtual"}})) {
     std::cerr << "FAIL downstream layout preparation\n"; ++failed;
   }
 }
 if (config::video.dd.configuration_option != Prep::verify_only) return 2;
 std::cout << count << " display precedence cases, " << failed << " failures\n";
 return failed ? 1 : 0;
}
"""
with tempfile.TemporaryDirectory(prefix='session-display-precedence-') as temporary:
    directory = pathlib.Path(temporary)
    cpp = directory / 'test.cpp'
    cpp.write_text(program)
    binary = directory / 'test'
    subprocess.run([args.compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers', '-I', str(source.parent), *dependency_includes, str(cpp), str(source / 'platform/windows/display_helper_request_policy.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
