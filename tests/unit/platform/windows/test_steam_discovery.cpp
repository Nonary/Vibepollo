#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <gtest/gtest.h>
#include <map>
#include <src/platform/windows/steam_discovery.h>
#include <src/steam_integration.h>
#include <tuple>

namespace {
  struct user_t {
    std::wstring sid;
    bool system = false;
  };

  struct session_t {
    DWORD id;
    WTS_CONNECTSTATE_CLASS state = WTSActive;
    std::wstring name = L"desktop";
  };

  user_t process_user;
  std::map<DWORD, user_t> session_users;
  std::vector<session_t> sessions;
  using registry_key_t = std::tuple<HKEY, std::wstring, std::wstring, DWORD>;
  std::map<registry_key_t, std::wstring> registry;
  std::vector<registry_key_t> queries;
  std::vector<DWORD> token_queries;
  DWORD own_session, console_session;
  HANDLE thread_user;
  DWORD last_error;
  int tokens, allocations;
  bool enumerate_ok, process_token_ok, token_info_ok, sid_text_ok, user_token_ok, registry_read_ok;
  bool thread_query_ok, impersonate_ok, restore_ok;
  DWORD registry_size_override;

  void *allocate(size_t bytes) {
    ++allocations;
    return std::calloc(1, bytes);
  }

  void deallocate(void *data) {
    if (data) {
      --allocations;
      std::free(data);
    }
  }

  std::wstring user_key(const std::wstring &sid) {
    return sid + L"\\Software\\Valve\\Steam";
  }

  void user_path(const std::wstring &sid, const std::wstring &path) {
    registry[{HKEY_USERS, user_key(sid), L"SteamPath", 0}] = path;
  }

  void machine_path(DWORD view, const std::wstring &path) {
    registry[{HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath", view}] = path;
  }

  auto roots() {
    platf::steam::windows_discovery::discovery_scope scope;
    return platf::steam::windows_discovery::registry_roots(scope);
  }

  class SteamWindowsDiscovery: public testing::Test {
    void SetUp() override {
      process_user = {L"S-1-5-18", true};
      session_users = {{3, {L"S-1-5-21-3"}}, {7, {L"S-1-5-21-7"}}};
      sessions = {{3}};
      own_session = console_session = 3;
      tokens = allocations = 0;
      thread_user = nullptr;
      last_error = ERROR_NO_TOKEN;
      thread_query_ok = impersonate_ok = restore_ok = true;
      enumerate_ok = process_token_ok = token_info_ok = sid_text_ok = user_token_ok = registry_read_ok = true;
      registry_size_override = 0;
      registry.clear();
      queries.clear();
      token_queries.clear();
    }

    void TearDown() override {
      EXPECT_EQ(tokens, 0);
      EXPECT_EQ(allocations, 0);
      EXPECT_EQ(thread_user, nullptr);
      for (const auto &[hive, key, value, view] : queries) {
        if (hive == HKEY_USERS) {
          EXPECT_NE(key, user_key(L"S-1-5-18"));
          EXPECT_NE(key.find(L".Default"), 0U);
        }
      }
    }
  };
}  // namespace

HANDLE GetCurrentProcess() {
  return reinterpret_cast<HANDLE>(-1);
}

HANDLE GetCurrentThread() {
  return reinterpret_cast<HANDLE>(-2);
}

DWORD GetLastError() {
  return last_error;
}

BOOL OpenThreadToken(HANDLE thread, DWORD access, BOOL open_as_self, HANDLE *token) {
  EXPECT_EQ(thread, GetCurrentThread());
  EXPECT_EQ(access, TOKEN_QUERY | TOKEN_IMPERSONATE);
  EXPECT_EQ(open_as_self, TRUE);
  if (!thread_query_ok || !thread_user) {
    last_error = thread_query_ok ? ERROR_NO_TOKEN : 5;
    return 0;
  }
  ++tokens;
  *token = thread_user;
  return 1;
}

BOOL ImpersonateLoggedOnUser(HANDLE token) {
  if (!impersonate_ok) {
    return 0;
  }
  thread_user = token;
  return 1;
}

BOOL SetThreadToken(HANDLE *thread, HANDLE token) {
  EXPECT_EQ(thread, nullptr);
  if (!restore_ok) {
    return 0;
  }
  thread_user = token;
  return 1;
}

BOOL RevertToSelf() {
  if (!restore_ok) {
    return 0;
  }
  thread_user = nullptr;
  return 1;
}

DWORD GetCurrentProcessId() {
  return 42;
}

BOOL ProcessIdToSessionId(DWORD pid, DWORD *id) {
  EXPECT_EQ(pid, 42U);
  *id = own_session;
  return 1;
}

BOOL OpenProcessToken(HANDLE process, DWORD access, HANDLE *token) {
  EXPECT_EQ(process, GetCurrentProcess());
  EXPECT_EQ(access, TOKEN_QUERY);
  if (!process_token_ok) {
    return 0;
  }
  ++tokens;
  *token = &process_user;
  return 1;
}

BOOL CloseHandle(HANDLE) {
  --tokens;
  return 1;
}

BOOL GetTokenInformation(HANDLE token, TOKEN_INFORMATION_CLASS kind, void *buffer, DWORD bytes, DWORD *needed) {
  EXPECT_EQ(kind, TokenUser);
  *needed = sizeof(TOKEN_USER);
  if (!buffer || bytes < *needed || !token_info_ok) {
    return 0;
  }
  static_cast<TOKEN_USER *>(buffer)->User.Sid = token;
  return 1;
}

BOOL IsWellKnownSid(PSID sid, WELL_KNOWN_SID_TYPE kind) {
  EXPECT_EQ(kind, WinLocalSystemSid);
  return static_cast<user_t *>(sid)->system;
}

BOOL ConvertSidToStringSidW(PSID sid, LPWSTR *text) {
  if (!sid_text_ok) {
    return 0;
  }
  const auto &value = static_cast<user_t *>(sid)->sid;
  const auto bytes = (value.size() + 1) * sizeof(wchar_t);
  *text = static_cast<LPWSTR>(allocate(bytes));
  std::memcpy(*text, value.c_str(), bytes);
  return 1;
}

HLOCAL LocalFree(HLOCAL data) {
  deallocate(data);
  return nullptr;
}

BOOL WTSEnumerateSessionsW(HANDLE server, DWORD reserved, DWORD version, PWTS_SESSION_INFOW *data, DWORD *count) {
  EXPECT_EQ(server, WTS_CURRENT_SERVER_HANDLE);
  EXPECT_EQ(reserved, 0U);
  EXPECT_EQ(version, 1U);
  if (!enumerate_ok) {
    return 0;
  }
  *count = sessions.size();
  *data = static_cast<PWTS_SESSION_INFOW>(allocate(sizeof(WTS_SESSION_INFOW) * sessions.size()));
  for (size_t i = 0; i < sessions.size(); ++i) {
    (*data)[i] = {sessions[i].id, nullptr, sessions[i].state};
  }
  return 1;
}

BOOL WTSQuerySessionInformationW(HANDLE server, DWORD id, WTS_INFO_CLASS kind, LPWSTR *text, DWORD *bytes) {
  EXPECT_EQ(server, WTS_CURRENT_SERVER_HANDLE);
  EXPECT_EQ(kind, WTSUserName);
  for (const auto &session : sessions) {
    if (session.id != id) {
      continue;
    }
    *bytes = (session.name.size() + 1) * sizeof(wchar_t);
    *text = static_cast<LPWSTR>(allocate(*bytes));
    std::memcpy(*text, session.name.c_str(), *bytes);
    return 1;
  }
  return 0;
}

BOOL WTSQueryUserToken(DWORD id, HANDLE *token) {
  token_queries.push_back(id);
  const auto found = session_users.find(id);
  if (!user_token_ok || found == session_users.end()) {
    return 0;
  }
  ++tokens;
  *token = &found->second;
  return 1;
}

DWORD WTSGetActiveConsoleSessionId() {
  return console_session;
}

void WTSFreeMemory(void *data) {
  deallocate(data);
}

LSTATUS RegGetValueW(HKEY hive, LPCWSTR key, LPCWSTR value, DWORD flags, DWORD *type, void *buffer, DWORD *bytes) {
  EXPECT_EQ(type, nullptr);
  EXPECT_EQ(flags & RRF_RT_REG_SZ, RRF_RT_REG_SZ);
  registry_key_t query {hive, key, value, flags & ~RRF_RT_REG_SZ};
  if (hive == HKEY_USERS) {
    const auto *effective = thread_user ? static_cast<user_t *>(thread_user) : &process_user;
    EXPECT_FALSE(effective->system);
    EXPECT_EQ(key, user_key(effective->sid));
  }
  queries.push_back(query);
  const auto found = registry.find(query);
  if (found == registry.end()) {
    return ERROR_FILE_NOT_FOUND;
  }
  const DWORD needed = registry_size_override ? registry_size_override : (found->second.size() + 1) * sizeof(wchar_t);
  if (buffer) {
    if (!registry_read_ok || *bytes < needed) {
      return ERROR_MORE_DATA;
    }
    std::memcpy(buffer, found->second.c_str(), needed);
  }
  *bytes = needed;
  return ERROR_SUCCESS;
}

TEST_F(SteamWindowsDiscovery, SystemReadsSelectedInteractiveHiveAndBothMachineViews) {
  user_path(process_user.sid, L"/wrong/system/Steam");
  user_path(L"S-1-5-21-3", L"/D/Program Files (x86)/Steam");
  machine_path(RRF_SUBKEY_WOW6432KEY, L"/machine32/Steam");
  machine_path(RRF_SUBKEY_WOW6464KEY, L"/machine64/Steam");
  const auto found = roots();
  ASSERT_EQ(found.size(), 3U);
  EXPECT_EQ(found[0], std::filesystem::path("/D/Program Files (x86)/Steam"));
  EXPECT_EQ(found[1], std::filesystem::path("/machine32/Steam"));
  EXPECT_EQ(found[2], std::filesystem::path("/machine64/Steam"));
  EXPECT_EQ(token_queries, std::vector<DWORD> {3});
  EXPECT_TRUE(process_user.system);  // no process identity/token replacement
}

TEST_F(SteamWindowsDiscovery, NonSystemUsesOwnHiveWithoutQueryingOtherSessions) {
  process_user = {L"S-1-5-21-99"};
  user_path(process_user.sid, L"/own/Steam");
  user_path(L"S-1-5-21-3", L"/other/Steam");
  ASSERT_EQ(roots(), std::vector<std::filesystem::path> {"/own/Steam"});
  EXPECT_TRUE(token_queries.empty());
}

TEST_F(SteamWindowsDiscovery, OwnSessionWinsOverConsoleAndEnumerationOrder) {
  sessions = {{7}, {3}};
  console_session = 7;
  user_path(L"S-1-5-21-3", L"/own/Steam");
  user_path(L"S-1-5-21-7", L"/console/Steam");
  EXPECT_EQ(roots(), std::vector<std::filesystem::path> {"/own/Steam"});
}

TEST_F(SteamWindowsDiscovery, ConsoleWinsWhenServiceIsInSessionZero) {
  sessions = {{7}, {3}};
  own_session = 0;
  user_path(L"S-1-5-21-3", L"/console/Steam");
  EXPECT_EQ(roots(), std::vector<std::filesystem::path> {"/console/Steam"});
}

TEST_F(SteamWindowsDiscovery, SingleActiveRdpUserIsUnambiguous) {
  sessions = {{3, WTSDisconnected}, {7}};
  own_session = 0;
  user_path(L"S-1-5-21-7", L"/rdp/Steam");
  EXPECT_EQ(roots(), std::vector<std::filesystem::path> {"/rdp/Steam"});
}

TEST_F(SteamWindowsDiscovery, AmbiguousActiveUsersUseMachineRegistrationOnly) {
  sessions = {{7}, {3}};
  own_session = 0;
  console_session = 0xffffffff;
  machine_path(RRF_SUBKEY_WOW6432KEY, L"/machine/Steam");
  EXPECT_EQ(roots(), std::vector<std::filesystem::path> {"/machine/Steam"});
  EXPECT_TRUE(token_queries.empty());
}

TEST_F(SteamWindowsDiscovery, LoggedOutConsoleAndDisconnectedUsersAreNotSelected) {
  sessions = {{3, WTSActive, L""}, {7, WTSDisconnected}};
  machine_path(RRF_SUBKEY_WOW6464KEY, L"/machine/Steam");
  EXPECT_EQ(roots(), std::vector<std::filesystem::path> {"/machine/Steam"});
  EXPECT_TRUE(token_queries.empty());
}

TEST_F(SteamWindowsDiscovery, MissingProfileFallsBackToMachineRegistration) {
  machine_path(RRF_SUBKEY_WOW6432KEY, L"/machine/Steam");
  EXPECT_EQ(roots(), std::vector<std::filesystem::path> {"/machine/Steam"});
}

TEST_F(SteamWindowsDiscovery, FailedSessionOrTokenQueriesStillUseMachineRegistration) {
  machine_path(RRF_SUBKEY_WOW6432KEY, L"/machine/Steam");
  for (auto *failure : {&enumerate_ok, &process_token_ok, &token_info_ok, &sid_text_ok, &user_token_ok, &thread_query_ok, &impersonate_ok}) {
    *failure = false;
    EXPECT_EQ(roots(), std::vector<std::filesystem::path> {"/machine/Steam"});
    *failure = true;
  }
}

TEST_F(SteamWindowsDiscovery, MissingRegistryReturnsNoRootsForCallerFolderFallbacks) {
  sessions.clear();
  EXPECT_TRUE(roots().empty());
  EXPECT_TRUE(token_queries.empty());
}

TEST_F(SteamWindowsDiscovery, ChangingOrOversizedRegistryValuesAreIgnored) {
  user_path(L"S-1-5-21-3", L"/user/Steam");
  registry_read_ok = false;
  EXPECT_TRUE(roots().empty());
  registry_read_ok = true;
  for (const DWORD bytes : {1U, 65540U}) {
    registry_size_override = bytes;
    EXPECT_TRUE(roots().empty());
  }
}

TEST_F(SteamWindowsDiscovery, NestedScanRetainsChosenUserAndRestoresSystemThread) {
  using namespace platf::steam::windows_discovery;
  {
    discovery_scope outer;
    ASSERT_EQ(thread_user, &session_users.at(3));
    own_session = console_session = 7;
    {
      discovery_scope inner;
      EXPECT_EQ(inner.user_hive(), std::optional<std::wstring> {L"S-1-5-21-3"});
      EXPECT_EQ(thread_user, &session_users.at(3));
    }
    EXPECT_EQ(thread_user, &session_users.at(3));
  }
  EXPECT_EQ(thread_user, nullptr);
}

TEST_F(SteamWindowsDiscovery, ExistingThreadIdentityIsPreserved) {
  using namespace platf::steam::windows_discovery;
  thread_user = &session_users.at(7);
  {
    discovery_scope scope;
    EXPECT_EQ(scope.user_hive(), std::optional<std::wstring> {L"S-1-5-21-7"});
  }
  EXPECT_EQ(thread_user, &session_users.at(7));
  EXPECT_TRUE(token_queries.empty());
  thread_user = nullptr;
}

TEST_F(SteamWindowsDiscovery, FailedOuterAcquisitionCannotRetryUnderSystemDuringNestedScan) {
  using namespace platf::steam::windows_discovery;
  user_path(L"S-1-5-21-3", L"/user/Steam");
  impersonate_ok = false;
  {
    discovery_scope outer;
    EXPECT_FALSE(outer.user_hive());
    impersonate_ok = true;  // a transient failure/session transition is resolved
    discovery_scope inner;
    EXPECT_FALSE(inner.user_hive());
    EXPECT_TRUE(registry_roots(inner).empty());
    EXPECT_EQ(thread_user, nullptr);
    EXPECT_EQ(token_queries, std::vector<DWORD> {3});
  }
  // A later independent request is allowed to retry normally.
  EXPECT_EQ(roots(), std::vector<std::filesystem::path> {"/user/Steam"});
}

TEST_F(SteamWindowsDiscovery, ThrowingUserReadRestoresThreadIdentity) {
  EXPECT_THROW(platf::steam::windows_discovery::read_user_source([&]() -> bool {
                 EXPECT_EQ(thread_user, &session_users.at(3));
                 throw std::runtime_error("read failure");
               }),
               std::runtime_error);
  EXPECT_EQ(thread_user, nullptr);
}

TEST_F(SteamWindowsDiscovery, ExistingSystemThreadTokenIsRestored) {
  using namespace platf::steam::windows_discovery;
  thread_user = &process_user;
  {
    discovery_scope scope;
    EXPECT_EQ(thread_user, &session_users.at(3));
  }
  EXPECT_EQ(thread_user, &process_user);
  thread_user = nullptr;
}

TEST_F(SteamWindowsDiscovery, UserSourceReadRunsAsUserAndProtectedWritesRunAsSystem) {
  using namespace platf::steam::windows_discovery;
  const auto bytes = read_user_source([&]() {
    EXPECT_EQ(thread_user, &session_users.at(3));
    return std::vector<BYTE> {42};
  });
  EXPECT_EQ(bytes, std::vector<BYTE> {42});
  EXPECT_EQ(thread_user, nullptr);  // service-owned cache write occurs afterwards
  sessions.clear();
  bool read = false;
  EXPECT_TRUE(read_user_source([&]() {
                read = true;
                return std::vector<BYTE> {42};
              }).empty());
  EXPECT_FALSE(read);
}

TEST_F(SteamWindowsDiscovery, RestorationFailureTerminatesInsteadOfLeakingIdentity) {
  EXPECT_DEATH({
    platf::steam::windows_discovery::discovery_scope scope;
    restore_ok = false; }, "terminate");
}

TEST_F(SteamWindowsDiscovery, NonDefaultRegistrationFeedsRealInstalledGameDiscovery) {
  const auto base = std::filesystem::temp_directory_path() / ("steam-windows-discovery-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(base / "steamapps/common/Example");
  std::ofstream(base / "steamapps/appmanifest_42.acf") << R"("AppState" { "appid" "42" "name" "Example" "installdir" "Example" })";
  user_path(L"S-1-5-21-3", base.wstring());
  const auto games = platf::steam::discover(roots());
  ASSERT_EQ(games.size(), 1U);
  EXPECT_EQ(games[0].app_id, 42U);
  std::filesystem::remove_all(base);
}
