"""Compile RTSS trust call sites against strict, inert Windows API fakes.

Exercise the production function bodies and API argument types without loading
RTSS modules or modifying a real installation. On Windows, also check Win32
ACL results on a temporary directory.
"""
import argparse
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[4]
parser = argparse.ArgumentParser()
parser.add_argument("source", nargs="?", type=pathlib.Path, default=ROOT / "src/platform/windows/rtss_integration.cpp")
parser.add_argument("--compiler", default="g++")
args = parser.parse_args()
SOURCE = args.source.read_text()


def extract(signature, offset=0):
    start = SOURCE.index(signature, offset)
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


IDENTITY = SOURCE[SOURCE.index("    enum class rtss_host_identity"):SOURCE.index("    struct pinned_directory_t")]
ACCESS = SOURCE[SOURCE.index("    bool token_matches_interactive_logon"):SOURCE.index("    void log_console_access_rejection")]
PROCESS_ABSENT = extract("bool rtss_process_confirmed_absent() {")

ACCESS_SIGNATURES = r'''
#include <array>
#include <cassert>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
'''

ACCESS_FAKES = r'''
using DWORD = unsigned int;
using BOOL = int;
using BYTE = unsigned char;
using HANDLE = void *;
struct station_t;
using HWINSTA = station_t *;
using PSECURITY_DESCRIPTOR = void *;
constexpr BOOL FALSE = 0;
constexpr DWORD ERROR_SUCCESS = 0;
constexpr DWORD TOKEN_QUERY = 8, TOKEN_DUPLICATE = 2;
constexpr DWORD MAXIMUM_ALLOWED = 0x02000000;
constexpr DWORD FILE_GENERIC_READ = 1, FILE_GENERIC_WRITE = 2, FILE_GENERIC_EXECUTE = 4, FILE_ALL_ACCESS = 7;
constexpr DWORD OWNER_SECURITY_INFORMATION = 1, GROUP_SECURITY_INFORMATION = 2, DACL_SECURITY_INFORMATION = 4;
constexpr DWORD SE_FILE_OBJECT = 1, SecurityIdentification = 1, WinLocalSystemSid = 1, WinBuiltinAdministratorsSid = 17;
constexpr DWORD WinLocalAccountAndAdministratorSid = 106;
constexpr DWORD SE_GROUP_USE_FOR_DENY_ONLY = 0x10, SE_GROUP_ENABLED_BY_DEFAULT = 2, SE_GROUP_ENABLED = 4;
constexpr DWORD SECURITY_MANDATORY_MEDIUM_RID = 0x2000;
constexpr DWORD WSF_VISIBLE = 1;
constexpr int UOI_FLAGS = 1, UOI_USER_SID = 4;
enum TOKEN_INFORMATION_CLASS {
  TokenSessionId, TokenElevation, TokenRestrictedSids, TokenIsAppContainer, TokenIntegrityLevel, TokenUser,
  TokenHasRestrictions, TokenLogonSid, TokenGroups, TokenElevationType
};
enum TOKEN_ELEVATION_TYPE { TokenElevationTypeDefault = 1, TokenElevationTypeFull, TokenElevationTypeLimited };
struct SID { DWORD value; BYTE authority_count = 1; };
using PSID = SID *;
constexpr DWORD SECURITY_MAX_SID_SIZE = 68;
struct SID_AND_ATTRIBUTES { PSID Sid; DWORD Attributes; };
struct TOKEN_ELEVATION { DWORD TokenIsElevated; };
struct TOKEN_GROUPS { DWORD GroupCount; SID_AND_ATTRIBUTES Groups[1]; };
struct USEROBJECTFLAGS { DWORD dwFlags; };
struct TOKEN_USER { SID_AND_ATTRIBUTES User; };
struct TOKEN_MANDATORY_LABEL { SID_AND_ATTRIBUTES Label; };
struct GENERIC_MAPPING { DWORD GenericRead, GenericWrite, GenericExecute, GenericAll; };
struct PRIVILEGE_SET { DWORD count; };
struct token_t {
  DWORD session = 5, elevated = 0, restricted = 0, app_container = 0, filtered = 0;
  TOKEN_ELEVATION_TYPE type = TokenElevationTypeDefault;
  DWORD group_attributes = SE_GROUP_ENABLED_BY_DEFAULT | SE_GROUP_ENABLED;
  SID group_sid {29};
  SID user {2};
  DWORD user_attributes = 0;
  SID logon {42};
  SID integrity {SECURITY_MANDATORY_MEDIUM_RID};
};
token_t process, console_user, duplicate_token;
DWORD active_session = 5;
unsigned service_queries = 0;
DWORD granted = FILE_GENERIC_READ;
bool fail_wts = false, fail_acl = false, fail_access = false, fail_token = false;
bool fail_duplicate = false, access_denied = false, switch_console_during_access = false;
bool station_visible = true, station_query_ok = true;
SID station_logon {42};
int handles = 0;

DWORD WTSGetActiveConsoleSessionId() { return active_session; }
HANDLE GetCurrentProcess() { return &process; }
BOOL OpenProcessToken(HANDLE proc, DWORD rights, HANDLE *out) {
  assert(proc == &process && rights == (TOKEN_QUERY | TOKEN_DUPLICATE));
  if (fail_token) return FALSE;
  *out = &process; ++handles; return true;
}
BOOL WTSQueryUserToken(DWORD session, HANDLE *out) {
  assert(session == active_session && process.user.value == WinLocalSystemSid);
  ++service_queries;
  if (fail_wts) return FALSE;
  *out = &console_user; ++handles; return true;
}
HWINSTA GetProcessWindowStation() { return reinterpret_cast<HWINSTA>(1); }
BOOL GetUserObjectInformationW(HWINSTA, int field, void *out, DWORD, DWORD *returned) {
  if (!station_query_ok) return FALSE;
  if (field == UOI_FLAGS) {
    static_cast<USEROBJECTFLAGS *>(out)->dwFlags = station_visible ? WSF_VISIBLE : 0;
    *returned = sizeof(USEROBJECTFLAGS);
  } else {
    assert(field == UOI_USER_SID);
    *static_cast<SID *>(out) = station_logon;
    *returned = sizeof(SID);
  }
  return true;
}
BOOL IsValidSid(void *sid) { return sid != nullptr; }
BOOL IsWellKnownSid(PSID sid, DWORD known) { return sid->value == known; }
BOOL EqualSid(PSID a, void *b) { return a->value == static_cast<SID *>(b)->value; }
BYTE *GetSidSubAuthorityCount(PSID sid) { return &sid->authority_count; }
DWORD *GetSidSubAuthority(PSID sid, DWORD index) { assert(index == 0); return &sid->value; }
BOOL GetTokenInformation(HANDLE handle, TOKEN_INFORMATION_CLASS field, void *out, DWORD size, DWORD *returned) {
  auto &token = *static_cast<token_t *>(handle);
  *returned = size;
  switch (field) {
    case TokenSessionId: *static_cast<DWORD *>(out) = token.session; break;
    case TokenElevation: static_cast<TOKEN_ELEVATION *>(out)->TokenIsElevated = token.elevated; break;
    case TokenRestrictedSids: static_cast<TOKEN_GROUPS *>(out)->GroupCount = token.restricted; break;
    case TokenIsAppContainer: *static_cast<DWORD *>(out) = token.app_container; break;
    case TokenIntegrityLevel: static_cast<TOKEN_MANDATORY_LABEL *>(out)->Label.Sid = &token.integrity; break;
    case TokenUser: static_cast<TOKEN_USER *>(out)->User = {&token.user, token.user_attributes}; break;
    case TokenHasRestrictions: *static_cast<DWORD *>(out) = token.filtered; break;
    case TokenElevationType: *static_cast<TOKEN_ELEVATION_TYPE *>(out) = token.type; break;
    case TokenGroups:
      *returned = sizeof(TOKEN_GROUPS);
      if (!out) return FALSE;
      *static_cast<TOKEN_GROUPS *>(out) = {1, {{&token.group_sid, token.group_attributes}}};
      break;
    case TokenLogonSid: *static_cast<TOKEN_GROUPS *>(out) = {1, {{&token.logon, 0}}}; break;
  }
  return true;
}
BOOL DuplicateToken(HANDLE handle, DWORD level, HANDLE *out) {
  assert(level == SecurityIdentification);
  if (fail_duplicate) return FALSE;
  duplicate_token = *static_cast<token_t *>(handle);
  *out = &duplicate_token; ++handles; return true;
}
BOOL CloseHandle(HANDLE token) { assert(token && handles > 0); --handles; return true; }
DWORD GetSecurityInfo(HANDLE object, DWORD type, DWORD info, PSID *, PSID *, void *, void *, PSECURITY_DESCRIPTOR *out) {
  assert(object == &granted && type == SE_FILE_OBJECT && info == 7);
  if (fail_acl) return 5;
  *out = object;
  return ERROR_SUCCESS;
}
void *LocalFree(void *) { return nullptr; }
BOOL AccessCheck(PSECURITY_DESCRIPTOR descriptor, HANDLE token, DWORD access, GENERIC_MAPPING *mapping,
                 PRIVILEGE_SET *, DWORD *, DWORD *rights, BOOL *okay) {
  assert(access == MAXIMUM_ALLOWED && mapping->GenericAll == FILE_ALL_ACCESS);
  assert(token == &duplicate_token && duplicate_token.user.value == 2);
  if (switch_console_during_access) ++active_session;
  if (fail_access) return FALSE;
  *okay = !access_denied;
  *rights = *static_cast<DWORD *>(descriptor);
  return true;
}
struct rtss_process_probe_t { bool known; std::optional<DWORD> pid; };
rtss_process_probe_t observation;
rtss_process_probe_t probe_rtss_process_id() { return observation; }
'''

ACCESS_TEST = r'''
int main() {
  auto verify = [](console_access expected, rtss_host_identity host) {
    auto context = get_console_access_context();
    assert(context && context->host == host);
    assert(active_console_user_access(*context, &granted, FILE_GENERIC_WRITE) == expected);
  };
  verify(console_access::protected_object, rtss_host_identity::interactive);
  assert(service_queries == 0 && handles == 0);
  granted |= FILE_GENERIC_WRITE;
  verify(console_access::writable, rtss_host_identity::interactive);
  granted = FILE_GENERIC_READ;
  process.user.value = WinLocalSystemSid; // The service host is still in session 5.
  verify(console_access::protected_object, rtss_host_identity::local_system);
  assert(service_queries == 1 && handles == 0);
  fail_wts = true;
  assert(!get_console_access_context() && handles == 0);
  fail_wts = false;
  console_user.session = 6;
  assert(!get_console_access_context() && handles == 0);
  console_user.session = 5;
  console_user.elevated = 1;
  assert(!get_console_access_context() && handles == 0);
  process.user.value = 2;
  // Same account and terminal session, different interactive logon.
  process.logon.value = 43;
  assert(!get_console_access_context() && handles == 0);
  process.logon.value = station_logon.value;
  station_visible = false;
  assert(!get_console_access_context() && handles == 0);
  station_visible = true;
  station_query_ok = false;
  assert(!get_console_access_context() && handles == 0);
  station_query_ok = true;
  process.filtered = 1;
  assert(!get_console_access_context() && handles == 0);
  process.type = TokenElevationTypeLimited;
  process.group_attributes = SE_GROUP_USE_FOR_DENY_ONLY;
  assert(!get_console_access_context() && handles == 0);
  for (const auto group : {WinBuiltinAdministratorsSid, WinLocalAccountAndAdministratorSid}) {
    process.group_sid.value = group;
    verify(console_access::protected_object, rtss_host_identity::interactive);
  }
  process.user_attributes = SE_GROUP_USE_FOR_DENY_ONLY;
  assert(!get_console_access_context() && handles == 0);
  process.user_attributes = 0;
  process.type = TokenElevationTypeDefault;
  process.filtered = 0;
  process.group_attributes = SE_GROUP_ENABLED_BY_DEFAULT;
  assert(!get_console_access_context() && handles == 0);
  process.group_attributes |= SE_GROUP_ENABLED;
  fail_duplicate = true;
  assert(!get_console_access_context() && handles == 0);
  fail_duplicate = false;
  fail_token = true;
  assert(!get_console_access_context() && handles == 0);
  fail_token = false;
  fail_acl = true;
  verify(console_access::unverifiable, rtss_host_identity::interactive);
  fail_acl = false;
  fail_access = true;
  verify(console_access::unverifiable, rtss_host_identity::interactive);
  fail_access = false;
  access_denied = true;
  verify(console_access::unverifiable, rtss_host_identity::interactive);
  access_denied = false;
  switch_console_during_access = true;
  verify(console_access::unverifiable, rtss_host_identity::interactive);
  assert(!get_console_access_context() && handles == 0);
  observation = {true, std::nullopt};
  assert(rtss_process_confirmed_absent());
  observation = {true, 23};
  assert(!rtss_process_confirmed_absent());
  observation = {false, std::nullopt};
  assert(!rtss_process_confirmed_absent());
}
'''

NATIVE_TEST = r'''
#include <winsock2.h>
#include <windows.h>
#include <aclapi.h>
#include <wtsapi32.h>
''' + IDENTITY + ACCESS + r'''
int main(int argc, char **argv) {
  assert(argc == 2);
  // The runner creates this directory normally and inherits the user's actual
  // writable ACL. Never modify ACLs or probe an installed product's files.
  const HANDLE directory = CreateFileA(argv[1], READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  assert(directory != INVALID_HANDLE_VALUE);
  const owned_handle pinned {directory, CloseHandle};

  HANDLE process = nullptr;
  assert(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &process));
  owned_handle primary {process, CloseHandle};
  HANDLE duplicate = nullptr;
  assert(DuplicateToken(primary.get(), SecurityIdentification, &duplicate));
  const console_access_context context {
    rtss_host_identity::interactive, WTSGetActiveConsoleSessionId(),
    std::move(primary), owned_handle {duplicate, CloseHandle}
  };
  assert(context.session != 0xFFFFFFFF);
  assert(active_console_user_access(context, directory, FILE_ADD_FILE) == console_access::writable);
  assert(active_console_user_access(context, nullptr, FILE_ADD_FILE) == console_access::unverifiable);
}
'''


RTSS_START = SOURCE.index("rtss_apply_result rtss_streaming_start(int numerator")
SNAPSHOT_FAILURE = extract("if (!g_recovery_file_owned)", RTSS_START)
MATCHING_PROFILE_RELOAD = extract("if (!g_flags_modified)", RTSS_START)
START_SAFETY = r'''
enum class rtss_apply_result { safe_to_fallback, retained_exclusive, applied };
bool g_flags_modified, g_settings_dirty, g_denominator_modified, g_limit_modified, g_sync_limiter_modified;
bool g_recovery_file_owned = false;
std::optional<int> g_original_limit;
bool reload_acknowledged = false;
bool reload_profiles_from_disk(bool, bool *scheduled) { *scheduled = true; return reload_acknowledged; }
struct null_log { template<class T> null_log &operator<<(const T &) { return *this; } };
#define BOOST_LOG(level) null_log{}
rtss_apply_result retain_rtss_exclusive(std::string_view) { return rtss_apply_result::retained_exclusive; }
rtss_apply_result mark_safe_to_fallback() { return rtss_apply_result::safe_to_fallback; }

rtss_apply_result snapshot_attempt(bool limiter_disabled, std::optional<int> on_disk) {
  g_flags_modified = limiter_disabled;
  g_original_limit = on_disk;
  g_settings_dirty = g_denominator_modified = g_limit_modified = g_sync_limiter_modified = true;
''' + SNAPSHOT_FAILURE + r'''
  return rtss_apply_result::applied;
}

rtss_apply_result matching_profile() {
  g_flags_modified = false;
''' + MATCHING_PROFILE_RELOAD + r'''
  return rtss_apply_result::applied;
}

int main() {
  for (auto on_disk : {std::optional<int>{}, std::optional<int>{0}, std::optional<int>{90}}) {
    assert(snapshot_attempt(false, on_disk) == rtss_apply_result::retained_exclusive);
    assert(snapshot_attempt(true, on_disk) == rtss_apply_result::safe_to_fallback);
    assert(!g_settings_dirty && !g_limit_modified && !g_denominator_modified && !g_sync_limiter_modified);
  }
  assert(matching_profile() == rtss_apply_result::retained_exclusive);
  reload_acknowledged = true;
  assert(matching_profile() == rtss_apply_result::applied);
}
'''

def compile_and_run(directory, stem, program, compile_args=(), run_args=()):
    source = directory / (stem + ".cpp")
    binary = directory / (stem + (".exe" if sys.platform == "win32" else ""))
    source.write_text(program)
    subprocess.run([args.compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                    str(source), "-o", str(binary), *compile_args], check=True)
    subprocess.run([str(binary), *run_args], check=True)


with tempfile.TemporaryDirectory() as tmp:
    directory = pathlib.Path(tmp)
    compile_and_run(directory, "rtss_signatures", PROGRAM)
    compile_and_run(directory, "rtss_console_access",
                    ACCESS_SIGNATURES + ACCESS_FAKES + IDENTITY + ACCESS + PROCESS_ABSENT + ACCESS_TEST)
    compile_and_run(directory, "rtss_start_safety", ACCESS_SIGNATURES + START_SAFETY)
    if sys.platform == "win32":
        writable = directory / "writable-installation"
        writable.mkdir()
        compile_and_run(directory, "rtss_native_access", ACCESS_SIGNATURES + NATIVE_TEST,
                        ("-ladvapi32", "-lwtsapi32", "-luser32"), (str(writable),))
        print("RTSS native ACL fixture passed")
print("RTSS trust call-site and console access fixtures passed")
