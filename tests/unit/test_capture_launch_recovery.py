#!/usr/bin/env python3
"""Execute production capture classification and launch recovery at inert OS seams."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--compiler', default='g++')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


common = r'''
#include <algorithm>
#include <cassert>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
#define BOOST_LOG(level) std::ostringstream()
namespace video {
enum class capture_probe_result_e { ready, display_unavailable, capture_failed, encoder_unavailable };
enum class capture_source_e { active_output, exact_output };
struct config_t { int width{}, height{}, framerate{}, framerateX100{}, dynamicRange{};
 capture_source_e capture_source = capture_source_e::active_output; std::optional<std::string> capture_output; };
using encoder_probe_adapter_hint_lease_t = unsigned;
}
'''
classification = common + r'''
#include "src/platform/windows/capture_initialization.h"
namespace display_device {
enum class DisplayRecoveryBehavior { Skip };
struct DisplayRecoveryBehaviorGuard { explicit DisplayRecoveryBehaviorGuard(DisplayRecoveryBehavior) {} };
auto map_output_name(const std::string &output) { return output; }
}
namespace platf {
struct display_t { static inline int live = 0; display_t() { ++live; } ~display_t() { --live; } };
bool capture_opens = false, missing = true;
int attempts = 0;
std::string attempted_output;
auto display(int, const std::string &output, const video::config_t &) {
  ++attempts;
  attempted_output = output;
  dxgi::capture_initialization_display_unavailable = missing || output == "missing";
  return capture_opens && output != "missing" ? std::make_shared<display_t>() : std::shared_ptr<display_t>{};
}
}
namespace video {
std::mutex encoder_probe_mutex;
struct formats_t { int dev_type = 1; } formats;
struct encoder_t { formats_t *platform_formats = &formats; } encoder;
encoder_t *chosen_encoder = &encoder;
int enumerations = 0;
void refresh_displays(int, std::vector<std::string> &outputs, int &selected) {
  ++enumerations;
  // Normal capture's refresh supplies an automatic open when no names exist.
  outputs = {""}; selected = 0;
}
''' + function((root / 'src/video.cpp').read_text(), '  capture_probe_result_e probe_capture(') + r'''
}
int main() {
  using enum video::capture_probe_result_e;
  assert(video::probe_capture({}) == display_unavailable);
  assert(platf::attempts == 1); // Recovery classification follows a real open attempt.
  platf::missing = false;
  assert(video::probe_capture({}) == capture_failed); // Backend, adapter and access errors never force a monitor.
  platf::capture_opens = true;
  assert(video::probe_capture({}) == ready);
  assert(platf::display_t::live == 0); // Preflight must release the duplication/IPC session before real capture.
  const auto attempts = platf::attempts;
  video::config_t exact;
  exact.capture_source = video::capture_source_e::exact_output;
  exact.capture_output = "missing";
  assert(video::probe_capture(exact) == display_unavailable);
  assert(platf::attempts == attempts + 1 && platf::attempted_output == "missing"); // Never open a physical fallback.
  exact.capture_output = "virtual";
  assert(video::probe_capture(exact) == ready);
  const auto enumerations = video::enumerations;
  platf::capture_opens = false;
  assert(video::probe_capture(exact) == capture_failed); // Locked target absent from DDX enumeration is opened directly.
  assert(video::enumerations == enumerations && platf::attempted_output == "virtual");
  video::chosen_encoder = nullptr;
  assert(video::probe_capture({}) == encoder_unavailable);
}
'''

duplication = common + r'''
#include <cstdint>
#include "src/platform/windows/capture_initialization.h"
using namespace std::literals;
using D3D_FEATURE_LEVEL = int;
using IUnknown = void;
constexpr int D3D_FEATURE_LEVEL_11_1 = 1, D3D_FEATURE_LEVEL_11_0 = 1,
 D3D_FEATURE_LEVEL_10_1 = 1, D3D_FEATURE_LEVEL_10_0 = 1,
 D3D_FEATURE_LEVEL_9_3 = 1, D3D_FEATURE_LEVEL_9_2 = 1, D3D_FEATURE_LEVEL_9_1 = 1,
 D3D_DRIVER_TYPE_UNKNOWN = 0, D3D11_CREATE_DEVICE_FLAGS = 0, D3D11_SDK_VERSION = 0,
 IID_IDXGIOutput1 = 0;
constexpr int DXGI_ERROR_UNSUPPORTED = -10, DXGI_ERROR_NOT_CURRENTLY_AVAILABLE = -11,
 E_ACCESSDENIED = -12, E_DEVICE_FAILED = -13;
bool FAILED(int value) { return value < 0; }
bool SUCCEEDED(int value) { return value >= 0; }
namespace util { struct hex_t { auto to_string_view() const { return "error"; } }; hex_t hex(int) { return {}; } }
namespace std::this_thread { void sleep_for(std::chrono::milliseconds) {} }
void syncThreadDesktop() {}
int device_result = 0, query_result = 0, duplication_result = 0;
struct adapter_t { void *get() { return this; } };
struct device_t { void *get() { return this; } };
struct dup_t {};
struct native_output_t {
 int QueryInterface(int, void **) { return query_result; }
 int DuplicateOutput(void *, dup_t *) { return duplication_result; }
} native_output;
struct output_t { auto operator->() { return &native_output; } };
using output1_t = output_t;
template<class... Args> int D3D11CreateDevice(Args...) { return device_result; }
namespace platf::dxgi {
''' + function((root / 'src/platform/windows/display_base.cpp').read_text(), '  bool test_dxgi_duplication(') + r'''
}
int main() {
 adapter_t adapter; output_t output;
 for (bool enumeration : {false, true}) {
  for (int status : {0, DXGI_ERROR_UNSUPPORTED, DXGI_ERROR_NOT_CURRENTLY_AVAILABLE, E_ACCESSDENIED, E_DEVICE_FAILED}) {
   duplication_result = status;
   platf::dxgi::capture_initialization_display_unavailable = true; // Every open clears stale reasons.
   assert(platf::dxgi::test_dxgi_duplication(adapter, output, enumeration) == (status == 0));
   assert(!platf::dxgi::capture_initialization_display_unavailable);
  }
 }
 duplication_result = DXGI_ERROR_UNSUPPORTED;
 device_result = E_DEVICE_FAILED;
 assert(!platf::dxgi::test_dxgi_duplication(adapter, output, false));
 assert(!platf::dxgi::capture_initialization_display_unavailable);
 device_result = 0; query_result = E_DEVICE_FAILED;
 assert(!platf::dxgi::test_dxgi_duplication(adapter, output, false));
 assert(!platf::dxgi::capture_initialization_display_unavailable);
}
'''

recovery = common + r'''
#include <array>
#include <cstring>
#include "src/framegen_policy.h"
#include "src/platform/windows/virtual_display_policy.h"
struct GUID { unsigned char bytes[16]; };
namespace VDISPLAY {
void cancel_virtual_display_recovery_monitor(GUID) {}
bool is_virtual_display_selection(const std::string &s) { return s == "virtual-selection"; }
}
namespace config {
struct video_t {
 enum class virtual_display_mode_e { disabled, per_client };
 virtual_display_mode_e virtual_display_mode = virtual_display_mode_e::disabled;
 std::string output_name;
 enum class virtual_display_layout_e { extended, exclusive };
 struct dd_t { enum class config_option_e { verify_only, ensure_only_display }; };
} video;
std::optional<std::string> output = "physical";
auto runtime_output_name_override() { return output; }
void set_runtime_output_name_override(std::optional<std::string> v) { output = v; }
}
namespace rtsp_stream {
struct launch_session_t {
 bool input_only = false, secondary_game_client = false;
 int width = 1920, height = 1080, fps = 60000;
 std::array<unsigned char, 16> virtual_display_guid_bytes {};
 bool capture_display_recovery = false, normal_vdd_identity_newly_reserved = false;
 std::string client_uuid = "client"; unsigned normal_vdd_identity_token = 0;
 std::string virtual_display_device_id;
 bool virtual_display = false, client_requests_virtual_display = false;
 std::optional<bool> client_virtual_display_override, display_preparation_client_virtual_override;
 std::optional<config::video_t::virtual_display_mode_e> virtual_display_mode_override;
 std::optional<config::video_t::virtual_display_layout_e> virtual_display_layout_override;
 std::optional<config::video_t::dd_t::config_option_e> dd_config_option_override;
 std::optional<std::string> output_name_override = "physical";
 bool virtual_display_preparation_override = false, display_config_preapplied = true;
 bool virtual_display_failed = false, normal_vdd_capacity_rejected = false;
 std::string virtual_display_catalog_error;
};
bool hdr_capture_required(const launch_session_t &, const config::video_t &) { return false; }
}
namespace audio { struct display_change_hold_t {}; }
namespace mail {
constexpr int shutdown = 0;
struct event_t { bool peek() { return false; } };
struct man_t { template<class T> event_t *event(int) { static event_t e; return &e; } };
man_t *man = nullptr;
}
bool active = false, protected_peer = false;
bool has_stream_session_activity() { return active; }
struct remote_display_topology {
 static remote_display_topology &instance() { static remote_display_topology t; return t; }
 void rollback_normal_game_identity(const std::string &, unsigned) {}
 auto protected_remote_monitor_client_ids() { return protected_peer ? std::vector<int>{1} : std::vector<int>{}; }
};
std::vector<std::string> calls;
bool creation_ok = true, apply_ok = true, cache_ok = true, encoder_ok = true;
namespace video {
std::vector<capture_probe_result_e> results;
std::optional<std::string> first_probed_output;
capture_probe_result_e probe_capture(const config_t &config) {
 assert(config.framerate == 60 && config.framerateX100 == 6000);
 calls.push_back("capture"); assert(!results.empty());
 if (std::count(calls.begin(), calls.end(), "capture") > 1) {
  assert(config.capture_source == capture_source_e::exact_output && config.capture_output == "virtual");
 } else {
  first_probed_output = config.capture_source == capture_source_e::exact_output ? config.capture_output : std::nullopt;
 }
 auto result = results.front(); results.erase(results.begin()); return result;
}
bool clear_pending_virtual_display_adapter_hint(unsigned) { calls.push_back("clear_hint"); return true; }
bool has_successful_encoder_probe() { return cache_ok; }
int probe_encoders() { calls.push_back("encoder"); return !encoder_ok; }
}
namespace display_helper_integration {
constexpr auto kStreamStartApplyVerificationTimeout = std::chrono::seconds(6);
enum class ApplyRetryPolicy { StreamStart };
namespace helpers {
std::optional<int> build_request_from_session(const config::video_t &, const rtsp_stream::launch_session_t &s, bool) {
 assert(s.virtual_display_layout_override == config::video_t::virtual_display_layout_e::exclusive);
 assert(s.dd_config_option_override == config::video_t::dd_t::config_option_e::ensure_only_display);
 calls.push_back("request"); return 1;
}
}
bool apply(int, void *, const std::function<bool()> &, ApplyRetryPolicy, std::chrono::steady_clock::time_point) {
 calls.push_back("apply"); return apply_ok;
}
}
auto make_stream_start_restore_disarm(std::function<bool()> c, std::chrono::steady_clock::time_point) {
 return [c] { assert(!c()); calls.push_back("disarm"); };
}
void prepare_virtual_display_for_session(
 const std::shared_ptr<rtsp_stream::launch_session_t> &s, audio::display_change_hold_t &,
 bool idle, bool changes, bool input_only, std::optional<std::string> &output,
 std::optional<unsigned> &, void *, const std::function<void()> &disarm,
 const std::function<bool()> &cancelled, std::chrono::steady_clock::time_point,
 bool preserve, bool, bool skip_snapshot, bool reserve_normal_identity) {
 assert(skip_snapshot);
 assert(idle && changes && !input_only && !preserve && !cancelled());
 assert(!s->output_name_override && !config::output && !output);
 assert(s->virtual_display && s->client_requests_virtual_display && s->client_virtual_display_override == true);
 assert(s->virtual_display_mode_override == config::video_t::virtual_display_mode_e::per_client);
 disarm(); calls.push_back("create");
 s->normal_vdd_identity_token = reserve_normal_identity ? 7 : 0;
 s->virtual_display_failed = !creation_ok;
 if (creation_ok) { s->virtual_display_device_id = "virtual"; output = "virtual"; config::output = "virtual"; }
}
''' + function((root / 'src/nvhttp.cpp').read_text(), '    bool recover_failed_capture_for_session(') + r'''
namespace util {
template<class F> struct guard_t { F callback; ~guard_t() { callback(); } };
template<class F> auto fail_guard(F callback) { return guard_t<F>{callback}; }
}
namespace platf::virtual_display_cleanup {
int cleanups = 0;
bool run(const char *, bool restore) { assert(restore); ++cleanups; return true; }
}
''' + function((root / 'src/nvhttp.cpp').read_text(), '  bool recover_capture_display_for_webrtc(') + '\n' + function((root / 'src/nvhttp.cpp').read_text(), '  void rollback_capture_display_recovery(') + r'''
int main() {
 using enum video::capture_probe_result_e;
 audio::display_change_hold_t audio;
 bool last_preapplied = false;
 unsigned last_identity_token = 0;
 auto run = [&](std::vector<video::capture_probe_result_e> results, bool may_change = true, bool input = false, bool secondary = false, bool use_pin = true) {
  calls.clear(); video::results = std::move(results);
  auto session = std::make_shared<rtsp_stream::launch_session_t>();
  session->input_only = input; session->secondary_game_client = secondary;
  std::optional<std::string> output = use_pin ? std::optional<std::string>{"physical"} : std::nullopt; config::output = output;
  std::optional<unsigned> hint = 7;
  bool revert = false;
  VDISPLAY::policy::retained_resume_recovery_rearm_t rearm;
  rearm.stage(VDISPLAY::policy::retained_resume_reuse_path_e::prepared_existing_display, [] {});
  const auto result = recover_failed_capture_for_session(session, audio, output, hint, may_change, false, revert, &rearm);
  if (session->capture_display_recovery) { assert(revert && !rearm.pending()); }
  last_preapplied = session->display_config_preapplied;
  last_identity_token = session->normal_vdd_identity_token;
  return result;
 };
 assert(run({ready})); assert((calls == std::vector<std::string>{"capture"}));
 assert(run({capture_failed})); assert((calls == std::vector<std::string>{"capture"}));
 config::video.output_name = "missing-configured-display";
 assert(run({display_unavailable, ready}, true, false, false, false));
 assert(video::first_probed_output == "missing-configured-display" && last_identity_token == 7);
 assert(run({ready}));
 assert(!video::first_probed_output); // Session pin retains normal fallback semantics.
 config::video.output_name = "virtual-selection";
 assert(run({ready}, true, false, false, false));
 assert(!video::first_probed_output);
 config::video.output_name.clear();
 assert(run({display_unavailable, ready}));
 assert((calls == std::vector<std::string>{"capture", "clear_hint", "disarm", "create", "request", "apply", "capture"}));
 assert(!run({display_unavailable, display_unavailable}));
 assert(std::count(calls.begin(), calls.end(), "create") == 1);
 assert(run({}, false)); assert(calls.empty());
 protected_peer = true; assert(!run({display_unavailable})); assert(calls.size() == 1); protected_peer = false;
 assert(run({}, true, false, true)); assert(calls.empty());
 assert(run({}, true, true)); assert(calls.empty());
 active = true; assert(run({})); assert(calls.empty()); active = false;
 creation_ok = false; assert(!run({display_unavailable})); assert(calls.back() == "create"); creation_ok = true;
 apply_ok = false; assert(run({display_unavailable, ready}));
 assert(calls.back() == "capture" && !last_preapplied);
 assert(run({display_unavailable, capture_failed})); // A secure desktop keeps its ordinary runtime retries.
 assert(calls.back() == "capture" && !last_preapplied);
 assert(!run({display_unavailable, display_unavailable}));
 assert(calls.back() == "capture" && !last_preapplied); apply_ok = true;
 cache_ok = false; encoder_ok = false; assert(!run({display_unavailable})); assert(calls.back() == "encoder");
 encoder_ok = true; assert(run({display_unavailable, ready}));
 assert(std::count(calls.begin(), calls.end(), "encoder") == 1);
 // A disabled normal VDD mode must never veto actual display-failure recovery.
 assert(config::video.virtual_display_mode == config::video_t::virtual_display_mode_e::disabled);
 auto browser = std::make_shared<rtsp_stream::launch_session_t>();
 calls.clear(); config::output.reset(); video::results = {display_unavailable, ready};
 assert(recover_capture_display_for_webrtc(browser, audio));
 assert(browser->capture_display_recovery && browser->virtual_display && browser->normal_vdd_identity_token == 0);
 rollback_capture_display_recovery(browser);
 assert(platf::virtual_display_cleanup::cleanups == 1);
 auto failed_browser = std::make_shared<rtsp_stream::launch_session_t>();
 calls.clear(); config::output.reset(); video::results = {display_unavailable, display_unavailable};
 assert(!recover_capture_display_for_webrtc(failed_browser, audio));
 assert(!config::output);
 rollback_capture_display_recovery(failed_browser);
 assert(platf::virtual_display_cleanup::cleanups == 2);
 auto normal = std::make_shared<rtsp_stream::launch_session_t>();
 rollback_capture_display_recovery(normal);
 assert(platf::virtual_display_cleanup::cleanups == 2);

}
'''

preservation = common + r'''
namespace display_helper_integration {
int snapshots = 0;
bool snapshot_current_display_state(const std::function<bool()> &, std::chrono::steady_clock::time_point) { ++snapshots; return true; }
}
struct session_t { bool capture_display_recovery = false; std::optional<std::string> output_name_override; };
struct app_t { std::optional<std::string> output_name_override = "physical"; } _app;
void prepare_snapshot(bool no_active_sessions, bool capture_display_recovery) {
 std::function<bool()> display_startup_cancelled = [] { return false; };
 auto display_startup_deadline = std::chrono::steady_clock::now();
''' + function((root / 'src/nvhttp.cpp').read_text(), '      if (no_active_sessions && !capture_display_recovery) {') + r'''
}
int main() {
 prepare_snapshot(true, false);
 assert(display_helper_integration::snapshots == 1);
 prepare_snapshot(true, true);
 assert(display_helper_integration::snapshots == 1); // Retry keeps the original restore baseline.

}
'''

# Execute each backend's actual early guards. The driver mutation boundary is
# represented by a return value after those guards, so unknown topology is
# exercised for both backends without a Windows desktop or installed driver.
startup = r'''
#include <cassert>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
#include "src/platform/windows/virtual_display_policy.h"
#define BOOST_LOG(level) std::ostringstream()
struct LUID {};
struct snapshot_t { bool enumeration_available = false; std::vector<std::string> display_names; } physical;
auto active_physical_displays() { return physical; }
std::mutex g_ensure_display_acquire_mutex;
namespace platf {
enum class adapter_output_match_e { no_match };
auto adapter_drives_any_output(LUID, const std::vector<std::string> &) { return adapter_output_match_e::no_match; }
}
enum class ensure_display_readiness_e { existing_display };
struct ensure_display_result { ensure_display_readiness_e readiness{}; bool create = false; };
'''
for backend in ('SUNSHINE', 'SUDOVDA'):
    source = (root / f'src/platform/windows/virtual_display_{backend.lower()}.cpp').read_text()
    signature = f'VDISPLAY_{backend}::ensure_display_result VDISPLAY_{backend}::ensure_display('
    body = function(source, signature)
    body = body[:body.index('  if (required_adapter_luid) {')]
    body = body.replace(signature, f'ensure_display_result probe_{backend}(')
    startup += body + '  result.create = true; return result;\n}\n'
startup += r'''
int main() {
 for (auto probe : {&probe_SUNSHINE, &probe_SUDOVDA}) {
  physical = {false, {}};
  assert(probe(std::nullopt, true).create);
  assert(!probe(std::nullopt, false).create);
  physical = {true, {}};
  assert(probe(std::nullopt, true).create);
  assert(!probe(std::nullopt, false).create);
  physical = {true, {"physical"}};
  assert(!probe(std::nullopt, true).create);
 }
}
'''

http_source = (root / 'src/nvhttp.cpp').read_text()
call_marker = '      if (!launch_session->input_only && !recover_failed_capture_for_session('
resume_argument = http_source.index('!joining_existing_game_output, dispatch::include_request_identity_log_fields(trace_id), revert_display_configuration, &retained_resume_recovery_rearm')
resume_start = http_source.rfind(call_marker, 0, resume_argument)
assert resume_start >= 0
resume_statement = function(http_source[resume_start:], call_marker)
base_source = (root / 'src/platform/windows/display_base.cpp').read_text()
classification_start = base_source.index('      if (enumeration_failed) {')
classification_end = base_source.index('      BOOST_LOG(error) << "Failed to locate an output device"', classification_start)
classification_body = base_source[classification_start:classification_end]

call_sites = common + r'''
namespace audio { struct display_change_hold_t {}; }
namespace rtsp_stream { struct launch_session_t { bool input_only=false, normal_vdd_capacity_rejected=false; }; }
namespace dispatch { bool include_request_identity_log_fields(int) { return false; } }
struct Tree { int status=0; template<class T> void put(const char *name, T value) { if(std::string(name)=="root.<xmlattr>.status_code") status=static_cast<int>(value); } void put(const char *, const char *) {} };
int probes=0; bool may_change_seen=true;
bool recover_failed_capture_for_session(const std::shared_ptr<rtsp_stream::launch_session_t> &, audio::display_change_hold_t &, std::optional<std::string> &, std::optional<unsigned> &, bool may_change, bool, bool &, int * = nullptr) { ++probes;may_change_seen=may_change;return true; }
void launch_call(bool input, bool joining=false) {
 auto launch_session=std::make_shared<rtsp_stream::launch_session_t>();launch_session->input_only=input;
 audio::display_change_hold_t request_audio_hold;
 std::optional<std::string> pending_output_override;std::optional<unsigned> pending_adapter_hint;
 bool revert_display_configuration=false;int trace_id=0;Tree tree;(void)joining;
''' + function((root/'src/nvhttp.cpp').read_text(), '      if (!launch_session->input_only && !recover_failed_capture_for_session(') + r'''
}
void resume_call(bool input, bool joining_existing_game_output) {
 auto launch_session=std::make_shared<rtsp_stream::launch_session_t>();launch_session->input_only=input;
 audio::display_change_hold_t request_audio_hold;
 std::optional<std::string> pending_output_override;std::optional<unsigned> pending_adapter_hint;
 bool revert_display_configuration=false;int trace_id=0,retained_resume_recovery_rearm=0;Tree tree;
''' + resume_statement + r'''
}
int main() {
 launch_call(true);resume_call(true,false);assert(probes==0);
 launch_call(false);assert(probes==1 && may_change_seen);
 resume_call(false,true);assert(probes==2 && !may_change_seen);
 resume_call(false,false);assert(probes==3 && may_change_seen);
}
'''

classification_init = common + r'''
#include "src/platform/windows/capture_initialization.h"
void classify(bool enumeration_failed,bool matched_attached_output,bool configured_adapter_present) {
 using platf::dxgi::capture_initialization_display_unavailable;
''' + classification_body + r'''
}
int main() {
 for(bool failed : {false,true}) for(bool attached : {false,true}) for(bool present : {false,true}) {
  platf::dxgi::capture_initialization_display_unavailable=false;
  classify(failed,attached,present);
  assert(platf::dxgi::capture_initialization_display_unavailable==(!failed && !attached && present));
 }
}
'''

filter_start = base_source.index('        if (selected_adapter_luid && !luid_equal(adapter_desc.AdapterLuid, *selected_adapter_luid))')
filter_end = base_source.index('        dxgi::output_t::pointer output_p;', filter_start)
adapter_filters = common + r'''
bool adapter_present(std::optional<int> selected_adapter_luid, bool use_legacy_adapter_name, std::wstring adapter_name, bool devices=true) {
 bool configured_adapter_present=false;
 struct Desc {int AdapterLuid;std::wstring Description;};
 auto luid_equal=[](int a,int b){return a==b;};
 std::vector<Desc> adapters;
 if(devices) adapters.push_back({7,L"GPU"});
 for(const auto &adapter_desc:adapters) {
''' + base_source[filter_start:filter_end] + r'''
 }
 return configured_adapter_present;
}
int main() {
 assert(adapter_present(std::nullopt,false,L"")); // Default adapter, even with zero outputs.
 assert(!adapter_present(std::nullopt,false,L"",false));
 assert(adapter_present(7,false,L""));assert(!adapter_present(8,false,L""));
 assert(adapter_present(std::nullopt,true,L"GPU"));assert(!adapter_present(std::nullopt,true,L"missing"));
}
'''

browser = (root/'src/webrtc_stream.cpp').read_text()
browser = browser[browser.index('    std::optional<std::string> start_webrtc_capture('):]
def guard_statement(marker):
    begin = browser.index(marker)
    return begin, browser[begin:browser.index('});', begin) + 3]
recovery_guard = guard_statement('auto recovery_display_guard = util::fail_guard(')
output_guard = guard_statement('auto output_override_guard = util::fail_guard(')
lease_start = browser.index('std::optional<config::runtime_output_override_lease_t> pending_output_override_lease;')
lease_statement = (lease_start, browser[lease_start:browser.index(';', lease_start) + 1])
guard_order = common + r'''
std::vector<std::string> events;
namespace config {using runtime_output_override_lease_t=int;bool clear_runtime_output_name_override_if_lease(int){events.push_back("clear");return true;}}
namespace nvhttp {void rollback_capture_display_recovery(const auto &){assert(events==std::vector<std::string>{"clear"});events.push_back("rollback");}}
namespace util {
template<class F> struct guard_t {F callback;~guard_t(){callback();}};
template<class F> auto fail_guard(F callback){return guard_t<F>{callback};}
}
void fail_browser_start() {
 auto launch_session=std::make_shared<int>(1);
''' + '\n'.join(body for _,body in sorted([recovery_guard,lease_statement,output_guard])) + r'''
 pending_output_override_lease=7;
}
int main(){fail_browser_start();assert(events==std::vector<std::string>({"clear","rollback"}));}
'''

http = function((root / 'src/nvhttp.cpp').read_text(), '    http_encoder_capabilities_t advertised_encoder_capabilities_for_http()')
assert 'VDISPLAY::ensure_display(' not in http
assert (root / 'src/main.cpp').read_text().count('VDISPLAY::ensure_display(required_adapter_luid, true)') == 1

with tempfile.TemporaryDirectory(prefix='capture-launch-recovery-') as directory:
    directory = Path(directory)
    for name, program in [('classification', classification), ('duplication', duplication), ('recovery', recovery), ('preservation', preservation), ('startup', startup), ('call-sites', call_sites), ('native-classification', classification_init), ('default-adapter', adapter_filters), ('browser-rollback-order', guard_order)]:
        source = directory / f'{name}.cpp'
        binary = directory / name
        source.write_text(program)
        subprocess.run([args.compiler, '-std=c++23', '-Wall', '-Wextra', '-Werror', '-I', str(root), str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
        print(f'{name}: production behavior passed')
