#!/usr/bin/env python3
"""Compile exact production HDR capability and DXGI observation with fake APIs."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def compile_run(compiler, root, name, code):
    source = root / (name + '.cpp')
    binary = root / name
    source.write_text(code)
    subprocess.run([compiler, '-std=c++20', str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler', default='g++')
    parser.add_argument('--source-dir', type=Path, default=Path(__file__).resolve().parents[2] / 'src')
    args = parser.parse_args()
    support = (args.source_dir / 'platform/windows/display_helper_integration.cpp').read_text()
    dxgi = (args.source_dir / 'platform/windows/display_base.cpp').read_text()
    nvhttp = (args.source_dir / 'nvhttp.cpp').read_text()
    assert 'capture_output_hdr_state(*output)' in nvhttp
    support_code = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>
using UINT32 = std::uint32_t; using LONG = long;
constexpr LONG ERROR_SUCCESS=0, ERROR_INVALID_FUNCTION=1, ERROR_INVALID_PARAMETER=2, ERROR_NOT_SUPPORTED=3, ERROR_ACCESS_DENIED=4;
constexpr int DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2=1, DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO=2;
struct LUID { unsigned low, high; };
struct DISPLAYCONFIG_DEVICE_INFO_HEADER { int type; unsigned size; LUID adapterId; unsigned id; };
using DISPLAYCONFIG_COLOR_ENCODING = unsigned;
struct DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO { DISPLAYCONFIG_DEVICE_INFO_HEADER header; unsigned advancedColorSupported; };
struct DISPLAYCONFIG_PATH_INFO { struct { LUID adapterId; unsigned id; } targetInfo; };
int result2=0, result1=0; bool support2=false, support1=false, query_ok=true, path_ok=true;
using HANDLE = int; bool token_available=false, impersonation_ok=true, fix_query_after_impersonation=false;
void CloseHandle(HANDLE) {}
namespace platf {
 HANDLE retrieve_users_token(bool) { return token_available ? 1 : 0; }
 template<class F> int impersonate_current_user(HANDLE, F fn) { if (!impersonation_ok) return 1; if (fix_query_after_impersonation) result2=0; fn(); return 0; }
}
LONG DisplayConfigGetDeviceInfo(DISPLAYCONFIG_DEVICE_INFO_HEADER *header) {
 if (header->type == DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2) {
   if (result2 == 0) { const unsigned flags = support2 ? 16 : 0; std::memcpy(reinterpret_cast<char *>(header) + sizeof(*header), &flags, sizeof(flags)); }
   return result2;
 }
 if (result1 == 0) reinterpret_cast<DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO *>(header)->advancedColorSupported = support1;
 return result1;
}
namespace display_device {
 enum class QueryType { Active };
 enum class DisplayRecoveryBehavior { Skip };
 struct DisplayRecoveryBehaviorGuard { explicit DisplayRecoveryBehaviorGuard(DisplayRecoveryBehavior) {} };
 struct Data { std::vector<DISPLAYCONFIG_PATH_INFO> m_paths{{}}; };
 struct WinApiLayer { std::optional<Data> queryDisplayConfig(QueryType) { if (!query_ok) return std::nullopt; return Data{}; } };
 namespace win_utils { const DISPLAYCONFIG_PATH_INFO *getActivePath(WinApiLayer &, const std::string &, const std::vector<DISPLAYCONFIG_PATH_INFO> &paths) { return path_ok ? &paths.front() : nullptr; } }
}
''' + function(support, '  std::optional<bool> remote_monitor_hdr_unsupported_inner(') + '\n' + function(support, '  std::optional<bool> remote_monitor_hdr_unsupported(') + r'''
int main() {
 assert(remote_monitor_hdr_unsupported("x") == true); support2=true; assert(remote_monitor_hdr_unsupported("x") == false);
 result2=ERROR_NOT_SUPPORTED; support1=false; assert(remote_monitor_hdr_unsupported("x") == true);
 support1=true; assert(remote_monitor_hdr_unsupported("x") == false);
 result1=ERROR_ACCESS_DENIED; assert(!remote_monitor_hdr_unsupported("x"));
 result2=ERROR_ACCESS_DENIED; result1=0; assert(!remote_monitor_hdr_unsupported("x"));
 query_ok=false; assert(!remote_monitor_hdr_unsupported("x"));
 query_ok=true; path_ok=false; assert(!remote_monitor_hdr_unsupported("x"));
 path_ok=true; result2=ERROR_ACCESS_DENIED; token_available=true; fix_query_after_impersonation=true; support2=false;
 assert(remote_monitor_hdr_unsupported("x") == true);
}
'''
    dxgi_code = r'''
#include <cassert>
#include <cctype>
#include <optional>
#include <string>
#include <vector>
using UINT=unsigned; using HRESULT=int;
constexpr int DXGI_ERROR_NOT_FOUND=-1, ERROR_OTHER=-2, IID_IDXGIFactory1=1, IID_IDXGIOutput6=2;
constexpr int DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020=10;
#define FAILED(x) ((x)<0)
struct DXGI_OUTPUT_DESC { bool AttachedToDesktop=true; const wchar_t *DeviceName=L""; };
struct DXGI_OUTPUT_DESC1 { int ColorSpace=0; };
struct FakeOutput {
 const wchar_t *name; int color; bool attached=true, desc_ok=true, qi_ok=true, color_ok=true;
 HRESULT GetDesc(DXGI_OUTPUT_DESC *d) { if(!desc_ok)return ERROR_OTHER; d->DeviceName=name;d->AttachedToDesktop=attached;return 0; }
 HRESULT QueryInterface(int, void **p) { if(!qi_ok)return ERROR_OTHER; *p=this;return 0; }
 HRESULT GetDesc1(DXGI_OUTPUT_DESC1 *d) { if(!color_ok)return ERROR_OTHER; d->ColorSpace=color;return 0; }
};
struct FakeAdapter { std::vector<FakeOutput *> outputs; HRESULT EnumOutputs(UINT i, FakeOutput **p) { if(i>=outputs.size())return DXGI_ERROR_NOT_FOUND;*p=outputs[i];return 0; } };
struct FakeFactory { std::vector<FakeAdapter *> adapters; HRESULT EnumAdapters1(UINT i, FakeAdapter **p) { if(i>=adapters.size())return DXGI_ERROR_NOT_FOUND;*p=adapters[i];return 0; } };
FakeFactory factory; bool factory_ok=true;
HRESULT CreateDXGIFactory1(int, void **p) { if(!factory_ok)return ERROR_OTHER;*p=&factory;return 0; }
namespace boost { bool iequals(const std::string &a,const std::string &b) { if(a.size()!=b.size())return false;for(size_t i=0;i<a.size();++i)if(std::tolower(a[i])!=std::tolower(b[i]))return false;return true; } }
namespace utf_utils { std::string to_utf8(const wchar_t *s) { std::string out; while(*s)out.push_back(static_cast<char>(*s++));return out; } }
namespace platf { namespace dxgi {
 template<class T> struct Ptr { using pointer=T*; T *p=nullptr; Ptr()=default; Ptr(T *x):p(x){} T *operator->()const{return p;} explicit operator bool()const{return p;} T **operator&(){return &p;} };
 using factory1_t=Ptr<FakeFactory>; using adapter_t=Ptr<FakeAdapter>; using output_t=Ptr<FakeOutput>; using output6_t=Ptr<FakeOutput>;
 std::optional<bool> capture_output_hdr_state(const std::string &);
}
void syncThreadDesktop() {}
''' + function(dxgi, '  std::optional<bool> dxgi::capture_output_hdr_state(') + r'''
}
int main() {
 FakeOutput sdr{L"DISPLAY1",0}, hdr{L"DISPLAY2",10}; FakeAdapter adapter{{&sdr,&hdr}}; factory.adapters={&adapter};
 assert(platf::dxgi::capture_output_hdr_state("display1") == false);
 assert(platf::dxgi::capture_output_hdr_state("DISPLAY2") == true);
 assert(!platf::dxgi::capture_output_hdr_state("missing"));
 hdr.color=0; assert(platf::dxgi::capture_output_hdr_state("DISPLAY2") == false);
 hdr.qi_ok=false; assert(!platf::dxgi::capture_output_hdr_state("DISPLAY2")); hdr.qi_ok=true;
 hdr.color_ok=false; assert(!platf::dxgi::capture_output_hdr_state("DISPLAY2")); hdr.color_ok=true;
 FakeOutput duplicate{L"DISPLAY2",10}; adapter.outputs.push_back(&duplicate); assert(!platf::dxgi::capture_output_hdr_state("DISPLAY2"));
 factory_ok=false; assert(!platf::dxgi::capture_output_hdr_state("DISPLAY1"));
}
'''
    readiness_code = r'''
#include <algorithm>
#include <cassert>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>
namespace remote_display_topology { struct mode_t { int width,height,refresh_hz; bool hdr; }; }
struct Info { struct { unsigned m_width,m_height; } m_resolution; int m_refresh_rate; };
struct Device { std::string m_device_id,m_display_name; std::optional<Info> m_info; };
bool valid_device=true, output_found=true, duplicate_devices=false; int queries=0, hdr_after=100; bool observation_unknown=false;
namespace VDISPLAY { std::optional<std::string> resolveActiveVirtualDisplayDeviceIdForStableId(const std::string &, int, int, bool) { return valid_device ? std::optional<std::string>{"id"} : std::nullopt; } }
namespace display_helper_integration {
 enum class DeviceEnumerationDetail { Minimal };
 std::optional<std::vector<Device>> enumerate_devices(DeviceEnumerationDetail) { auto result=std::vector<Device>{{"id","DISPLAY1",Info{{3840,2160},120}}}; if(duplicate_devices)result.push_back(result.front());return result; }
}
namespace display_device { using DeviceEnumerationDetail=display_helper_integration::DeviceEnumerationDetail; }
namespace platf { enum class mem_type_e { dxgi }; std::vector<std::string> display_names(mem_type_e) { return output_found ? std::vector<std::string>{"DISPLAY1"} : std::vector<std::string>{}; }
 namespace dxgi { std::optional<bool> capture_output_hdr_state(const std::string &) { ++queries; if(observation_unknown)return std::nullopt; return queries >= hdr_after; } }
}
bool remote_device_id_equals(const std::string &a,const std::string &b) { return a==b; }
int remote_refresh_hz(int v) { return v; }
''' + function(nvhttp, '    std::optional<remote_display_topology::mode_t> remote_monitor_observed_geometry(') + '\n' + function(nvhttp, '    std::optional<std::string> remote_monitor_exact_capture_output(') + r'''
int main() {
 auto observed=remote_monitor_observed_geometry("client"); assert(observed && observed->width==3840 && observed->height==2160 && observed->refresh_hz==120);
 duplicate_devices=true; assert(!remote_monitor_observed_geometry("client")); duplicate_devices=false;
 remote_display_topology::mode_t hdr{3840,2160,120,true};
 hdr_after=3; assert(remote_monitor_exact_capture_output("client",hdr)=="DISPLAY1"); assert(queries==3);
 queries=0; hdr_after=100; assert(!remote_monitor_exact_capture_output("client",hdr));
 queries=0; observation_unknown=true; assert(!remote_monitor_exact_capture_output("client",hdr));
 observation_unknown=false; queries=0; hdr_after=100; hdr.hdr=false; assert(remote_monitor_exact_capture_output("client",hdr)=="DISPLAY1");
 valid_device=false; assert(!remote_monitor_exact_capture_output("client",hdr));
}
'''
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        compile_run(args.compiler, root, 'capability', support_code)
        compile_run(args.compiler, root, 'dxgi', dxgi_code)
        compile_run(args.compiler, root, 'readiness', readiness_code)
    print('Remote Monitor capability and effective DXGI color observation regressions passed')


if __name__ == '__main__':
    main()
