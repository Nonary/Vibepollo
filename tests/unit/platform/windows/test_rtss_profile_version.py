"""Run the production RTSS version gate and SDK transaction with fake hooks.

The official RTSS 7.3.7 build 28314's RTSS.exe has fixed file/product version
7.3.5.28314. Download inspected without execution:
https://ftp.nluug.nl/pub/games/PC/guru3d/afterburner/%5BGuru3D%5D-RTSSSetup737Build28314.zip
Archive SHA256: 9b084a8cb3e53ec1a673894d0b66e22b16c9fd8785636b020b2d422f3f2a820e
RTSS.exe SHA256: 84e6e439d313dcee0ba9549d8248d3923d9de6805d0380be5eab337446856736

Run with Python and g++. An optional source file supports before/after checks.
No Windows process, profile file, or installed RTSS settings are changed.
"""

import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parents[4]
source = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else root / 'src/platform/windows/rtss_integration.cpp'
text = source.read_text()

# The RTSS hooks run in the service-host process. Keep the security boundary
# contract beside the SDK/version transaction test so future refactors cannot
# silently return to loading a Web UI-selected DLL by pathname alone.
for required in (
    'canonical_trusted_rtss_root',
    'PROGRAMFILES(X86)',
    'PROGRAMW6432',
    'fs::weakly_canonical',
    'FILE_ATTRIBUTE_REPARSE_POINT',
    'pin_trusted_rtss_root',
    'GetFinalPathNameByHandleW',
    'FILE_FLAG_OPEN_REPARSE_POINT',
    'WinVerifyTrust',
    'CERT_SHA256_HASH_PROP_ID',
    'required_signer',
    'loaded_module_matches_verified_file',
    'LOAD_LIBRARY_SEARCH_SYSTEM32',
):
    assert required in text, f'missing RTSS trust-boundary guard: {required}'

assert text.count('AccessCheck(') == 1, 'RTSS ACL policy must use one handle-bound access helper'

access_source_start = text.index('bool active_console_user_has_mutating_access')
access_source_end = text.index('\n    std::optional<pinned_directory_t>', access_source_start)
access_source = text[access_source_start:access_source_end]
assert 'GetSecurityInfo(' in access_source
assert 'OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION' in access_source
assert 'MAXIMUM_ALLOWED' in access_source
assert 'retrieve_users_token(false)' in access_source

trusted_root_source_start = text.index('std::optional<fs::path> canonical_trusted_rtss_root')
trusted_root_source_end = text.index('\n    std::optional<fs::path> final_dos_path', trusted_root_source_start)
trusted_root_source = text[trusted_root_source_start:trusted_root_source_end]
assert 'is_strict_path_descendant(absolute, program_files)' in trusted_root_source
assert 'is_strict_path_descendant(canonical, program_files)' in trusted_root_source
assert trusted_root_source.count('existing_path_has_reparse_component') >= 2

resolve_root_source_start = text.index('fs::path resolve_rtss_root() {')
resolve_root_source_end = text.index('\n    }\n  }  // namespace', resolve_root_source_start)
resolve_root_source = text[resolve_root_source_start:resolve_root_source_end]
assert 'canonical_trusted_rtss_root(configured)' in resolve_root_source
assert 'using the default installation path instead' in resolve_root_source

pin_root_source_start = text.index('std::optional<pinned_directory_t> pin_trusted_rtss_root')
pin_root_source_end = text.index('\n    std::optional<signer_fingerprint_t>', pin_root_source_start)
pin_root_source = text[pin_root_source_start:pin_root_source_end]
assert 'FILE_SHARE_READ | FILE_SHARE_WRITE,' in pin_root_source
assert 'FILE_SHARE_DELETE' not in pin_root_source
assert 'normalized_path_key(*opened_trusted) != normalized_path_key(*canonical)' in pin_root_source
assert 'FILE_ADD_FILE' in pin_root_source
assert 'FILE_ADD_SUBDIRECTORY' in pin_root_source
assert 'FILE_DELETE_CHILD' in pin_root_source
assert 'active_console_user_has_mutating_access(handle, root_mutation_rights)' in pin_root_source

verified_file_source_start = text.index('std::optional<verified_code_file_t> open_verified_code_file')
verified_file_source_end = text.index('\n    bool same_file_identity', verified_file_source_start)
verified_file_source = text[verified_file_source_start:verified_file_source_end]
assert 'FILE_SHARE_READ,' in verified_file_source
assert 'FILE_SHARE_DELETE' not in verified_file_source
assert 'FILE_WRITE_DATA' in verified_file_source
assert 'DELETE' in verified_file_source
assert 'WRITE_DAC' in verified_file_source
assert 'WRITE_OWNER' in verified_file_source
assert 'active_console_user_has_mutating_access(file, file_mutation_rights)' in verified_file_source
assert verified_file_source.index('verify_authenticode_signature') < len(verified_file_source)

load_hooks_source_start = text.index('bool load_hooks(const fs::path &root) {')
load_hooks_source_end = text.index('\n    // Resolve RTSS only', load_hooks_source_start)
load_hooks_source = text[load_hooks_source_start:load_hooks_source_end]
assert 'pin_trusted_rtss_root(root)' in load_hooks_source
assert 'find_verified_rtss_executable(pinned_root->path)' in load_hooks_source
assert 'open_verified_code_file(p, trusted_executable->signer)' in load_hooks_source
assert 'loaded_module_matches_verified_file(m, *trusted_dll)' in load_hooks_source
assert 'LoadLibraryW(' not in load_hooks_source
assert 'LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR' not in load_hooks_source
load_call = load_hooks_source.index('HMODULE m = LoadLibraryExW')
assert load_hooks_source.index('open_verified_code_file') < load_call
assert load_call < load_hooks_source.index('loaded_module_matches_verified_file')


def function(signature):
    start = text.index(signature)
    body = text.index('{', start)
    depth = 1
    end = body + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


harness = r'''
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>
namespace fs = std::filesystem;
using DWORD = unsigned long;
constexpr bool FALSE = false;
struct null_log { template<class T> null_log& operator<<(const T&) { return *this; } };
#define BOOST_LOG(level) null_log{}
std::string version;
bool version_available = true;
bool hooks_available = true;
bool reject_property = false;
std::vector<std::string> calls;
std::map<std::string,int> values;
bool getFileVersionInfo(const fs::path&, std::string& out) { out=version; return version_available; }
bool profile_hooks_available() { return hooks_available; }
template<class T, class F> std::optional<T> call_rtss_hooks(const char*, F fn, bool) { return fn(); }
void load(const char*) { calls.push_back("load"); }
void save(const char*) { calls.push_back("save"); }
void update() { calls.push_back("update"); }
bool set(const char* name, void* value, DWORD size) {
 assert(size==sizeof(int)); calls.push_back(std::string("set:")+name);
 if (reject_property) return false;
 values[name]=*static_cast<int*>(value); return true;
}
bool get(const char* name, void* value, DWORD size) {
 assert(size==sizeof(int)); calls.push_back(std::string("get:")+name);
 if (!values.count(name)) return false;
 *static_cast<int*>(value)=values[name]; return true;
}
struct hooks {
 decltype(&load) LoadProfile=load;
 decltype(&save) SaveProfile=save;
 decltype(&get) GetProfileProperty=get;
 decltype(&set) SetProfileProperty=set;
 decltype(&update) UpdateProfiles=update;
} g_hooks;
'''
harness += function('bool profile_hooks_support_fractional_limits(')
harness += '\n' + function('std::optional<bool> write_framerate_values_via_hooks(')
harness += r'''
int main() {
 const fs::path root="unused";
 std::optional<int> limit=120, denominator=1, sync=2;
 int scenarios=0;
 for (const auto& [reported,supported] : std::vector<std::pair<std::string,bool>>{
  {"7.3.5.28314",true}, {"7.3.7.28314",true}, {"7.3.8.1",true},
  {"7.4.0.1",true}, {"8.0.0.1",true}, {"7.3.5.28000",false},
  {"7.3.5.28313",false}, {"7.3.5.28315",false}, {"7.3.6.28010",false},
  {"6.9.9.28314",false}, {"",false}, {"7.3.7",false}, {"unknown",false}}) {
  version=reported; calls.clear(); values.clear();
  const auto result=write_framerate_values_via_hooks(root,&limit,&denominator,&sync);
  if (supported) {
   if (!result.value_or(false)) { std::cerr<<"SDK unexpectedly skipped for "<<version<<'\n'; return 1; }
   assert(values["FramerateLimit"]==120 && values["FramerateLimitDenominator"]==1);
   assert(values["SyncLimiter"]==2);
   assert((calls==std::vector<std::string>{"load","set:FramerateLimit","set:FramerateLimitDenominator",
     "set:SyncLimiter","save","update","get:FramerateLimit","get:FramerateLimitDenominator","get:SyncLimiter"}));
  } else { assert(!result.has_value() && calls.empty()); }
  ++scenarios;
 }
 version="7.3.5.28314";
 limit=60000; denominator=1001;
 assert(write_framerate_values_via_hooks(root,&limit,&denominator,&sync).value_or(false));
 assert(values["FramerateLimit"]==60000 && values["FramerateLimitDenominator"]==1001);
 ++scenarios;
 // Unknown versions and unavailable SDKs retain the disk fallback.
 version_available=false; calls.clear();
 assert(!write_framerate_values_via_hooks(root,&limit,&denominator,&sync).has_value() && calls.empty());
 version_available=true; hooks_available=false;
 assert(!write_framerate_values_via_hooks(root,&limit,&denominator,&sync).has_value() && calls.empty());
 hooks_available=true; scenarios+=2;
 // Restoration of absent keys must still use the file path; the SDK cannot delete them.
 for (auto* absent : {&limit,&denominator,&sync}) {
  const auto original=*absent; absent->reset(); calls.clear();
  assert(!write_framerate_values_via_hooks(root,&limit,&denominator,&sync).has_value() && calls.empty());
  *absent=original; ++scenarios;
 }
 reject_property=true; calls.clear();
 const auto rejected=write_framerate_values_via_hooks(root,&limit,&denominator,&sync);
 assert(rejected.has_value() && !*rejected);
 assert((calls==std::vector<std::string>{"load","set:FramerateLimit"}));
 ++scenarios;
 std::cout<<scenarios<<" RTSS version/SDK scenarios passed\n";
}
'''
with tempfile.TemporaryDirectory(prefix='rtss-profile-version-test-') as directory:
    path = pathlib.Path(directory)
    (path / 'test.cpp').write_text(harness)
    subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror', str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
