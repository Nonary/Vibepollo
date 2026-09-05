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
