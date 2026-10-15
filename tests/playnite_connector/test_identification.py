"""Compile production pipe-open logic with Windows syscall boundaries mocked."""
from pathlib import Path
import subprocess
import tempfile
import re

root = Path(__file__).resolve().parents[2]
source = (root / 'src/platform/windows/ipc/pipes.cpp').read_text()
header = (root / 'src/platform/windows/ipc/pipes.h').read_text()
ipc = (root / 'src/platform/windows/playnite_ipc.cpp').read_text()

def function(text, marker):
    start = text.index(marker)
    opening = text.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

named_setter = function(source, '  void NamedPipeFactory::set_client_identification_only()')
anonymous_setter = function(source, '  void AnonymousPipeFactory::set_client_identification_only()')
pipe_open = function(source, '  winrt::file_handle NamedPipeFactory::create_client_pipe(')
connect = function(ipc, '  std::unique_ptr<platf::dxgi::INamedPipe> IpcClient::connect_to_plugin()')
field = re.search(r'DWORD _client_security_flags\s*=\s*[^;]+;', header).group()
fixture = r'''
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <iostream>
#include <cassert>
using DWORD = uint32_t;
using ULONGLONG = uint64_t;
constexpr DWORD SECURITY_SQOS_PRESENT = 0x100000;
constexpr DWORD SECURITY_IDENTIFICATION = 0x10000;
constexpr DWORD FILE_FLAG_OVERLAPPED = 0x40000000;
constexpr DWORD GENERIC_READ = 0x80000000, GENERIC_WRITE = 0x40000000, OPEN_EXISTING = 3;
constexpr DWORD ERROR_PIPE_BUSY = 231, ERROR_FILE_NOT_FOUND = 2;
#define BOOST_LOG(level) std::cout
namespace winrt { struct file_handle { uintptr_t value = 0; explicit operator bool() const { return value != 0; } }; }
static std::vector<DWORD> opens;
static DWORD failure = 0;
ULONGLONG GetTickCount64() { static ULONGLONG tick = 0; return ++tick; }
DWORD GetLastError() { return failure; }
void WaitNamedPipeW(const wchar_t *, int) {}
void Sleep(int) {}
uintptr_t CreateFileW(const wchar_t *, DWORD, DWORD, void *, DWORD, DWORD flags, void *) {
    opens.push_back(flags);
    return failure ? 0 : 1;
}
namespace platf::dxgi {
struct INamedPipe {};
class NamedPipeFactory {
public:
    FIELD
    void set_client_identification_only();
    winrt::file_handle create_client_pipe(const std::wstring &) const;
};
class AnonymousPipeFactory {
public:
    NamedPipeFactory _pipe_factory;
    void set_client_identification_only();
    std::unique_ptr<INamedPipe> create_client(const char *) {
        if (!_pipe_factory.create_client_pipe(L"control")) return nullptr;
        if (!_pipe_factory.create_client_pipe(L"data")) return nullptr;
        return std::make_unique<INamedPipe>();
    }
};
NAMED_SETTER
ANONYMOUS_SETTER
PIPE_OPEN
}
class IpcClient {
public:
    std::string control_name_;
    static constexpr auto kControlPipeName = "Sunshine.PlayniteExtension";
    std::unique_ptr<platf::dxgi::INamedPipe> connect_to_plugin();
};
CONNECT
int main() {
    platf::dxgi::NamedPipeFactory other;
    assert(other.create_client_pipe(L"other"));
    assert(opens.back() == FILE_FLAG_OVERLAPPED);
    IpcClient playnite;
    assert(playnite.connect_to_plugin());
    assert(opens.size() == 3);
    assert(opens[1] == (FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION));
    assert(opens[2] == opens[1]);
    assert(other.create_client_pipe(L"other-again"));
    assert(opens.back() == FILE_FLAG_OVERLAPPED);
    failure = 5;
    assert(!playnite.connect_to_plugin());
    assert(opens.back() == (FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION));
    std::cout << "\nPASS: Playnite control/data identification-only, helper default preserved, open failure rejected\n";
}
'''
for marker, value in [('FIELD', field), ('NAMED_SETTER', named_setter),
                      ('ANONYMOUS_SETTER', anonymous_setter), ('PIPE_OPEN', pipe_open),
                      ('CONNECT', connect)]:
    fixture = fixture.replace(marker, value)
with tempfile.TemporaryDirectory(prefix='playnite-identification-') as directory:
    cpp = Path(directory) / 'test.cpp'
    exe = Path(directory) / 'test'
    cpp.write_text(fixture)
    subprocess.run(['c++', '-std=c++23', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
