"""Compile RTSS trust call sites against strict, inert Windows API fakes.

This exercises the production function bodies and API argument types without
loading modules, opening files, or performing real signature verification.
"""
import argparse
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[4]
parser = argparse.ArgumentParser()
parser.add_argument("source", nargs="?", type=pathlib.Path, default=ROOT / "src/platform/windows/rtss_integration.cpp")
parser.add_argument("--compiler", default="g++")
args = parser.parse_args()
SOURCE = args.source.read_text()


def extract(signature):
    start = SOURCE.index(signature)
    opening = SOURCE.index("{", SOURCE.index(")", start))
    depth = 1
    end = opening + 1
    while depth:
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        end += 1
    return SOURCE[start:end]


PROGRAM = r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <cwchar>
#include <optional>
#include <string>
#include <vector>

// Distinct handle types emulate STRICT Windows headers.
struct HWND__;
struct HMODULE__;
using HWND = HWND__ *;
using HMODULE = HMODULE__ *;
using HANDLE = void *;
using DWORD = unsigned long;
using LONG = long;
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
constexpr int FALSE = 0;
constexpr int ERROR_SUCCESS = 0;
constexpr DWORD WTD_UI_NONE = 2, WTD_REVOKE_NONE = 0, WTD_CHOICE_FILE = 1;
constexpr DWORD WTD_STATEACTION_VERIFY = 1, WTD_STATEACTION_CLOSE = 2;
constexpr DWORD WTD_REVOCATION_CHECK_NONE = 16, WTD_DISABLE_MD2_MD4 = 8192;
constexpr DWORD CERT_SHA256_HASH_PROP_ID = 107;
constexpr DWORD FILE_READ_ATTRIBUTES = 1, FILE_SHARE_READ = 2, FILE_SHARE_WRITE = 4;
constexpr DWORD FILE_SHARE_DELETE = 8, OPEN_EXISTING = 3;
constexpr DWORD FILE_ATTRIBUTE_NORMAL = 128, FILE_FLAG_OPEN_REPARSE_POINT = 0x200000;
struct GUID {};
#define WINTRUST_ACTION_GENERIC_VERIFY_V2 GUID{}
namespace fs { struct path { const wchar_t *c_str() const { return L"signed.exe"; } }; }
struct WINTRUST_FILE_INFO { DWORD cbStruct; const wchar_t *pcwszFilePath; HANDLE hFile; };
struct WINTRUST_DATA {
  DWORD cbStruct, dwUIChoice, fdwRevocationChecks, dwUnionChoice;
  WINTRUST_FILE_INFO *pFile;
  DWORD dwStateAction, dwProvFlags;
  HANDLE hWVTStateData;
};
struct Cert {};
struct CRYPT_PROVIDER_CERT { Cert *pCert; };
struct CRYPT_PROVIDER_SGNR { DWORD csCertChain; CRYPT_PROVIDER_CERT *pasCertChain; };
struct CRYPT_PROVIDER_DATA {};
using signer_fingerprint_t = std::array<unsigned char, 32>;
struct verified_code_file_t { HANDLE handle; };
int verify_calls = 0, close_calls = 0;
bool trust_ok = true, cert_ok = true;
LONG WinVerifyTrust(HWND hwnd, GUID *, WINTRUST_DATA *data) {
  assert(reinterpret_cast<HANDLE>(hwnd) == INVALID_HANDLE_VALUE);
  assert(data->dwUIChoice == WTD_UI_NONE && data->fdwRevocationChecks == WTD_REVOKE_NONE);
  assert(data->dwUnionChoice == WTD_CHOICE_FILE && data->pFile->hFile == reinterpret_cast<HANDLE>(42));
  assert(data->dwProvFlags == (WTD_REVOCATION_CHECK_NONE | WTD_DISABLE_MD2_MD4));
  if (data->dwStateAction == WTD_STATEACTION_VERIFY) { ++verify_calls; return trust_ok ? 0 : -1; }
  assert(data->dwStateAction == WTD_STATEACTION_CLOSE); ++close_calls; return 0;
}
CRYPT_PROVIDER_DATA *WTHelperProvDataFromStateData(HANDLE) { static CRYPT_PROVIDER_DATA d; return &d; }
CRYPT_PROVIDER_SGNR *WTHelperGetProvSignerFromChain(CRYPT_PROVIDER_DATA *, int, int, int) {
  static Cert c; static CRYPT_PROVIDER_CERT pc {&c}; static CRYPT_PROVIDER_SGNR s {1, &pc}; return &s;
}
bool CertGetCertificateContextProperty(Cert *, DWORD property, unsigned char *value, DWORD *size) {
  assert(property == CERT_SHA256_HASH_PROP_ID && *size == 32);
  if (!cert_ok) return false;
  value[0] = 42; return true;
}

DWORD reported_length = 0;
std::wstring expected_path;
int open_calls = 0, handle_closes = 0;
bool open_ok = true, identity_ok = true;
DWORD GetModuleFileNameW(HMODULE, wchar_t *buffer, DWORD capacity) {
  if (reported_length == 0 || reported_length >= capacity) return reported_length;
  assert(expected_path.size() == reported_length);
  for (DWORD i = 0; i < reported_length; ++i) buffer[i] = expected_path[i];
  buffer[reported_length] = L'\0';
  return reported_length;
}
HANDLE CreateFileW(const wchar_t *path, DWORD access, DWORD sharing, void *, DWORD disposition, DWORD flags, void *) {
  ++open_calls;
  assert(access == FILE_READ_ATTRIBUTES && sharing == (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE));
  assert(disposition == OPEN_EXISTING && flags == (FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT));
  assert(std::wstring(path) == expected_path);
  return open_ok ? reinterpret_cast<HANDLE>(17) : INVALID_HANDLE_VALUE;
}
bool same_file_identity(HANDLE a, HANDLE b) {
  assert(a == reinterpret_cast<HANDLE>(42) && b == reinterpret_cast<HANDLE>(17)); return identity_ok;
}
bool CloseHandle(HANDLE h) { assert(h == reinterpret_cast<HANDLE>(17)); ++handle_closes; return true; }
'''
PROGRAM += "\n" + extract("std::optional<signer_fingerprint_t> verify_authenticode_signature(")
PROGRAM += "\n" + extract("bool loaded_module_matches_verified_file(")
PROGRAM += r'''
int main() {
  fs::path p;
  auto signed_file = reinterpret_cast<HANDLE>(42);
  auto result = verify_authenticode_signature(p, signed_file);
  assert(result && (*result)[0] == 42 && verify_calls == 1 && close_calls == 1);
  trust_ok = false;
  assert(!verify_authenticode_signature(p, signed_file) && close_calls == 2);
  trust_ok = true; cert_ok = false;
  assert(!verify_authenticode_signature(p, signed_file) && close_calls == 3);

  verified_code_file_t verified {signed_file};
  assert(!loaded_module_matches_verified_file(nullptr, verified) && open_calls == 0);
  reported_length = 32768;
  assert(!loaded_module_matches_verified_file(nullptr, verified) && open_calls == 0);
  expected_path = L"C:\\Program Files\\RTSS\\RTSSHooks64.dll"; reported_length = expected_path.size();
  assert(loaded_module_matches_verified_file(nullptr, verified) && open_calls == 1 && handle_closes == 1);
  identity_ok = false;
  assert(!loaded_module_matches_verified_file(nullptr, verified) && handle_closes == 2);
  open_ok = false;
  assert(!loaded_module_matches_verified_file(nullptr, verified) && handle_closes == 2);
  open_ok = true; identity_ok = true;
  expected_path.assign(32767, L'x'); reported_length = expected_path.size();
  assert(loaded_module_matches_verified_file(nullptr, verified) && handle_closes == 3);
}
'''

with tempfile.TemporaryDirectory() as tmp:
    source = pathlib.Path(tmp) / "rtss_test.cpp"
    binary = pathlib.Path(tmp) / "rtss_test"
    source.write_text(PROGRAM)
    subprocess.run([args.compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("RTSS trust call-site fixture passed")
