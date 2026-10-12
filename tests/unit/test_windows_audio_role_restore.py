#!/usr/bin/env python3
"""Compile production Windows role recovery against deterministic MMDevice/policy fakes."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('source', type=Path)
parser.add_argument('--compiler', default='/usr/bin/g++-15')
parser.add_argument('--sanitize', action='store_true')
args = parser.parse_args()
source = args.source.read_text()

def function(signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

methods = '\n'.join(function(s) for s in (
    '    static pending_role_restores_t normalize_pending_role_restores(',
    '    role_restore_result_e try_restore_pending_role(',
    '    reset_result_e try_reset_pending_roles_from_steam(',
    '    void run_pending_role_restore_task(',
))
cpp = r'''
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
using namespace std::literals;
using HRESULT = long;
using HANDLE = void *;
using DWORD = unsigned long;
constexpr HRESULT S_OK = 0, E_FAIL = -1;
#define SUCCEEDED(x) ((x) >= 0)
#define FAILED(x) ((x) < 0)
#define BOOST_LOG(x) std::cerr
constexpr bool TRUE = true, FALSE = false;
HANDLE CreateEventW(void*, bool, bool, void*) { return reinterpret_cast<HANDLE>(1); }
void CloseHandle(HANDLE) {}
void SetEvent(HANDLE) {}
namespace util {
struct hex { HRESULT x; explicit hex(HRESULT v): x(v) {} std::string to_string_view() const { return std::to_string(x); } };
template<class F> struct guard { F f; ~guard() { f(); } };
template<class F> guard<F> fail_guard(F f) { return {f}; }
}
enum ERole { eConsole, eMultimedia, eCommunications, ERole_enum_count };
using role_device_ids_t = std::array<std::wstring, ERole_enum_count>;
struct pending_role_restore_t { ERole role; std::wstring preferred_id, expected_current_id; bool fallback_transition = false; };
using pending_role_restores_t = std::vector<pending_role_restore_t>;
using pending_restore_token_t = std::shared_ptr<std::atomic_bool>;
constexpr std::size_t role_index(ERole r) { return static_cast<std::size_t>(r); }
enum class role_restore_result_e { restored, no_longer_owned, inactive, no_device, failed };
enum class reset_result_e { success, inactive, no_device, fatal };
struct fixture_t {
 role_device_ids_t defaults {L"steam", L"steam", L"steam"};
 bool preferred_present = false, hide_fails = false, show_fails = false, marker_fails = false, shutdown = false;
 bool newer_owner = false, fail_restore_once = false;
 int hides = 0, shows = 0, writes = 0, waits = 0;
 pending_role_restores_t published;
 std::function<void()> on_wait;
 std::function<void()> on_show;
};
fixture_t *fixture;
namespace audio {
 struct wstring_t { std::wstring value; const wchar_t *get() const { return value.c_str(); } explicit operator bool() const { return !value.empty(); } };
}
struct device_t { std::wstring id; HRESULT GetId(audio::wstring_t *out) { out->value = id; return S_OK; } };
struct enumerator_t {
 template<class T> HRESULT RegisterEndpointNotificationCallback(T*) { return S_OK; }
 template<class T> HRESULT UnregisterEndpointNotificationCallback(T*) { return S_OK; }
};
using device_enum_t = std::shared_ptr<enumerator_t>;
std::shared_ptr<device_t> default_device(device_enum_t&, ERole role = eConsole) {
 const auto& id = fixture->defaults[role_index(role)];
 return id.empty() ? nullptr : std::make_shared<device_t>(device_t{id});
}
struct device_arrival_notification_t {
 explicit device_arrival_notification_t(const std::wstring&) {}
 bool wait(HANDLE, DWORD) {
  ++fixture->waits;
  if (fixture->waits > 3) throw std::runtime_error("worker did not finish after endpoint return");
  fixture->on_wait();
  return true;
 }
};
struct policy_t {
 HRESULT SetEndpointVisibility(const wchar_t*, bool visible) {
  if (visible) {
   ++fixture->shows;
   if (fixture->on_show) fixture->on_show();
   return fixture->show_fails ? E_FAIL : S_OK;
  }
  ++fixture->hides;
  if (fixture->hide_fails) return E_FAIL;
  for (auto& id: fixture->defaults) if (id == L"steam") id = L"controller";
  return S_OK;
 }
 HRESULT SetDefaultEndpoint(const wchar_t *id, ERole role) {
  if (std::wstring(id) == L"hdmi" && !fixture->preferred_present) return E_FAIL;
  if (fixture->fail_restore_once && std::wstring(id) == L"hdmi") { fixture->fail_restore_once = false; return E_FAIL; }
  fixture->defaults[role_index(role)] = id;
  ++fixture->writes;
  return S_OK;
 }
};
struct audio_control_t {
 device_enum_t device_enum = std::make_shared<enumerator_t>();
 std::shared_ptr<policy_t> policy = std::make_shared<policy_t>();
 bool pending_restore_worker_can_write(const std::stop_token& stop, const pending_restore_token_t& token, std::uint64_t) {
  return !stop.stop_requested() && token->load() && !fixture->newer_owner;
 }
 bool is_default_device(const std::wstring &id, ERole r) { return fixture->defaults[role_index(r)] == id; }
 bool adopt_current_policy_endpoint_for_worker(const pending_restore_token_t&, std::uint64_t, ERole) { return !fixture->newer_owner; }
 std::optional<std::wstring> preferred_device_match_list(const std::wstring& id) { return id; }
 std::optional<std::pair<int,std::wstring>> find_device_id(const std::wstring& id) {
  return fixture->preferred_present ? std::make_optional(std::make_pair(0,id)) : std::nullopt;
 }
 std::optional<HRESULT> set_default_endpoint_for_worker(const std::stop_token &stop, const pending_restore_token_t& token, std::uint64_t epoch, ERole role, const std::wstring &id) {
  if (!pending_restore_worker_can_write(stop, token, epoch)) return std::nullopt;
  return policy->SetDefaultEndpoint(id.c_str(),role);
 }
 bool process_shutdown_in_progress() { return fixture->shutdown; }
 bool write_visibility_marker(const std::wstring&) { return !fixture->marker_fails; }
 HRESULT show_steam_endpoint_with_retry(const std::wstring& id) { return policy->SetEndpointVisibility(id.c_str(),TRUE); }
 bool update_pending_role_restore_for_worker(const pending_role_restore_t& value, const pending_restore_token_t&, std::uint64_t, bool = true) {
  for (auto& pending: fixture->published) if (pending.role == value.role) { pending = value; return true; }
  return false;
 }
 void clear_pending_role_restore_for_worker(const pending_role_restore_t& value, const pending_restore_token_t&, std::uint64_t) {
  std::erase_if(fixture->published,[&](const auto& pending){ return pending.role == value.role; });
 }
 void clear_pending_role_restores_for_worker(const pending_role_restores_t&, const pending_restore_token_t&, std::uint64_t) { fixture->published.clear(); }
 void reassert_current_policy_assignment() {}
 void reassert_current_policy_assignment_role(ERole) {}
'''+methods+r'''
};
void check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
void run(const std::string &scenario) {
 fixture_t state;
 fixture = &state;
 audio_control_t control;
 auto token = std::make_shared<std::atomic_bool>(true);
 auto roles = pending_role_restores_t{{eConsole,L"hdmi",L"steam"},{eMultimedia,L"hdmi",L"steam"}};
 state.published = roles;
 state.on_wait = [&] {
  for (const auto& pending: state.published) {
   check(!pending.fallback_transition,"failed/finished visibility transition still owns arbitrary fallback");
   check(pending.expected_current_id==state.defaults[role_index(pending.role)],"published ownership lost exact fallback identity");
  }
  state.preferred_present = true;
 };
 if (scenario == "hide_failure") state.hide_fails = true;
 else if (scenario == "marker_failure") state.marker_fails = true;
 else if (scenario == "show_failure") state.show_fails = true;
 else if (scenario == "failed_fallback_user_choice") {
  state.show_fails = true;
  state.on_wait = [&] { state.defaults[eConsole]=L"user-speakers"; state.preferred_present=true; };
 }
 else if (scenario == "failed_show_user_choice") {
  state.show_fails = true;
  state.on_show = [&] { state.defaults[eConsole]=L"user-speakers"; };
  state.on_wait = [&] {
   check(state.published.size()==1 && state.published.front().role==eMultimedia,"retired console stopped surviving role recovery");
   check(state.published.front().expected_current_id==L"controller" && !state.published.front().fallback_transition,"surviving role lost exact fallback ownership");
   check(state.defaults[eConsole]==L"user-speakers","show failure replaced newer console choice");
   check(state.hides==1 && state.shows==1,"failed visibility fallback was retried");
   state.preferred_present=true;
  };
 }
 else if (scenario == "failed_fallback_new_owner") {
  state.hide_fails = true;
  state.on_wait = [&] { state.defaults[eConsole]=L"new-stream"; state.newer_owner=true; };
 }
 else if (scenario == "shutdown") state.shutdown = true;
 else if (scenario == "uncaptured") {
  state.marker_fails = true;
  for (auto& role: roles) role.preferred_id.clear();
  state.published = roles;
 }
 else if (scenario == "user_choice") state.on_wait = [&] { state.defaults[eConsole]=L"user-speakers"; state.preferred_present=true; };
 else if (scenario == "new_owner") state.on_wait = [&] { state.defaults[eConsole]=L"new-stream"; state.newer_owner=true; };
 else if (scenario == "restore_retry") { state.preferred_present=true; state.fail_restore_once=true; }
 else if (scenario == "handoff") {
  auto normalized = control.normalize_pending_role_restores(roles,L"steam",{L"controller",L"steam",L"user"});
  check(normalized.size()==1 && normalized.front().role==eMultimedia,"handoff adopted unowned role");
  roles.front().fallback_transition=true;
  normalized = control.normalize_pending_role_restores(roles,L"steam",{L"controller",L"steam",L"user"});
  check(normalized.size()==2 && normalized.front().preferred_id==L"hdmi" && normalized.front().expected_current_id==L"controller","handoff dropped published fallback transition");
  return;
 }
 control.run_pending_role_restore_task({},L"steam",roles,token,1);
 if (scenario == "shutdown" || scenario == "uncaptured") {
  check(state.writes==0 && state.published.empty(),"failed uncaptured/shutdown recovery was retained");
 }
 else if (scenario == "new_owner" || scenario == "failed_fallback_new_owner") check(state.defaults[eConsole]==L"new-stream","older worker overrode new owner");
 else {
  check(state.defaults[eConsole]==((scenario=="user_choice" || scenario=="failed_fallback_user_choice" || scenario=="failed_show_user_choice") ? L"user-speakers" : L"hdmi"),"preferred console endpoint not restored after return");
  check(state.defaults[eMultimedia]==L"hdmi","preferred multimedia endpoint not restored after return");
  check(state.defaults[eCommunications]==(state.hides && !state.hide_fails ? L"controller" : L"steam"),"uncaptured role was overwritten");
  check(state.published.empty(),"completed role recovery was not retired");
 }
}
int main() {
 int failed=0;
 for (const std::string scenario : {"normal_return","hide_failure","marker_failure","show_failure","user_choice","new_owner","restore_retry","handoff","failed_fallback_user_choice","failed_show_user_choice","failed_fallback_new_owner","shutdown","uncaptured"}) {
  try { run(scenario); std::cout<<scenario<<": PASS\n"; }
  catch (const std::exception &e) { ++failed; std::cout<<scenario<<": FAIL: "<<e.what()<<"\n"; }
 }
 return failed ? 1 : 0;
}
'''
with tempfile.TemporaryDirectory(prefix='audio-470-replay-') as temp:
    directory=Path(temp)
    unit=directory/'production.cpp'
    binary=directory/'production'
    unit.write_text(cpp)
    flags=['-std=c++23','-O2','-g','-pthread','-Wall','-Wextra']
    if args.sanitize: flags += ['-fsanitize=address,undefined','-fno-omit-frame-pointer']
    subprocess.run([args.compiler,*flags,str(unit),'-o',str(binary)],check=True)
    result=subprocess.run([str(binary)],check=False)
    raise SystemExit(result.returncode)
