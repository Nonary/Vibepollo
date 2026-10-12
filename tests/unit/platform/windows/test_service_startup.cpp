#include <cwchar>
#include <gtest/gtest.h>
#include <src/platform/windows/service_startup.h>

namespace {
  DWORD mode = SERVICE_AUTO_START;
  DWORD last_error = ERROR_SUCCESS;
  bool missing = false;
  bool denied = false;
  bool query_failure = false;
  bool change_failure = false;
  bool ignore_change = false;
  int change_calls = 0;
  int open_handles = 0;
  DWORD changed_to = SERVICE_NO_CHANGE;

  class ServiceStartupTest: public testing::Test {
    void SetUp() override {
      mode = SERVICE_AUTO_START;
      last_error = ERROR_SUCCESS;
      missing = denied = query_failure = change_failure = ignore_change = false;
      change_calls = open_handles = 0;
      changed_to = SERVICE_NO_CHANGE;
    }

    void TearDown() override {
      EXPECT_EQ(open_handles, 0);
    }
  };
}  // namespace

SC_HANDLE OpenSCManagerW(LPCWSTR machine, LPCWSTR database, DWORD access) {
  EXPECT_EQ(machine, nullptr);
  EXPECT_EQ(database, nullptr);
  EXPECT_EQ(access, SC_MANAGER_CONNECT);
  ++open_handles;
  return reinterpret_cast<SC_HANDLE>(1);
}

SC_HANDLE OpenServiceW(SC_HANDLE manager, LPCWSTR name, DWORD access) {
  EXPECT_EQ(manager, reinterpret_cast<SC_HANDLE>(1));
  EXPECT_EQ(std::wcscmp(name, L"ApolloService"), 0);
  // Query/change configuration rights only; no start, stop, ACL, or delete rights.
  EXPECT_EQ(access & ~(SERVICE_QUERY_CONFIG | SERVICE_CHANGE_CONFIG), 0u);
  if (missing || (denied && (access & SERVICE_CHANGE_CONFIG))) {
    last_error = missing ? ERROR_SERVICE_DOES_NOT_EXIST : ERROR_ACCESS_DENIED;
    return nullptr;
  }
  ++open_handles;
  return reinterpret_cast<SC_HANDLE>(2);
}

BOOL CloseServiceHandle(SC_HANDLE) {
  --open_handles;
  return 1;
}

DWORD GetLastError() {
  return last_error;
}

BOOL QueryServiceConfigW(SC_HANDLE handle, QUERY_SERVICE_CONFIGW *config, DWORD bytes, LPDWORD needed) {
  EXPECT_EQ(handle, reinterpret_cast<SC_HANDLE>(2));
  *needed = sizeof(QUERY_SERVICE_CONFIGW);
  if (!config || bytes < *needed) {
    last_error = ERROR_INSUFFICIENT_BUFFER;
    return 0;
  }
  if (query_failure) {
    last_error = ERROR_INVALID_DATA;
    return 0;
  }
  config->dwStartType = mode;
  return 1;
}

BOOL ChangeServiceConfigW(SC_HANDLE handle, DWORD type, DWORD start, DWORD error, LPCWSTR binary, LPCWSTR group, LPDWORD tag, LPCWSTR dependencies, LPCWSTR account, LPCWSTR password, LPCWSTR display) {
  EXPECT_EQ(handle, reinterpret_cast<SC_HANDLE>(2));
  EXPECT_EQ(type, SERVICE_NO_CHANGE);
  EXPECT_EQ(error, SERVICE_NO_CHANGE);
  EXPECT_EQ(binary, nullptr);
  EXPECT_EQ(group, nullptr);
  EXPECT_EQ(tag, nullptr);
  EXPECT_EQ(dependencies, nullptr);
  EXPECT_EQ(account, nullptr);
  EXPECT_EQ(password, nullptr);
  EXPECT_EQ(display, nullptr);
  EXPECT_TRUE(start == SERVICE_AUTO_START || start == SERVICE_DEMAND_START);
  ++change_calls;
  changed_to = start;
  if (change_failure) {
    last_error = ERROR_ACCESS_DENIED;
    return 0;
  }
  if (!ignore_change) {
    mode = start;
  }
  return 1;
}

TEST_F(ServiceStartupTest, ReadsActualAutomaticAndManualWithoutChangingConfiguration) {
  auto state = service_ctrl::get_service_startup();
  EXPECT_TRUE(state.status);
  EXPECT_TRUE(state.installed);
  EXPECT_TRUE(state.can_change);
  EXPECT_TRUE(state.automatic);
  mode = SERVICE_DEMAND_START;
  state = service_ctrl::get_service_startup();
  EXPECT_TRUE(state.status);
  EXPECT_FALSE(state.automatic);
  EXPECT_EQ(state.start_type, SERVICE_DEMAND_START);
  EXPECT_EQ(change_calls, 0);
}

TEST_F(ServiceStartupTest, TurningOffSelectsManualAndPreservesAllOtherConfiguration) {
  const auto state = service_ctrl::set_service_startup(false);
  EXPECT_TRUE(state.status);
  EXPECT_FALSE(state.automatic);
  EXPECT_EQ(changed_to, SERVICE_DEMAND_START);
  EXPECT_EQ(change_calls, 1);
}

TEST_F(ServiceStartupTest, TurningOnSelectsAutomaticAndNoOpDoesNotWrite) {
  mode = SERVICE_DEMAND_START;
  EXPECT_TRUE(service_ctrl::set_service_startup(true).automatic);
  EXPECT_EQ(changed_to, SERVICE_AUTO_START);
  EXPECT_TRUE(service_ctrl::set_service_startup(true).status);
  EXPECT_EQ(change_calls, 1);
}

TEST_F(ServiceStartupTest, ManualPreferenceNeverDisablesRunPermission) {
  mode = SERVICE_DISABLED;
  EXPECT_TRUE(service_ctrl::set_service_startup(false).status);
  EXPECT_EQ(mode, SERVICE_DEMAND_START);
}

TEST_F(ServiceStartupTest, MissingServiceCannotBeChanged) {
  missing = true;
  const auto state = service_ctrl::set_service_startup(true);
  EXPECT_FALSE(state.status);
  EXPECT_FALSE(state.installed);
  EXPECT_EQ(state.error_code, ERROR_SERVICE_DOES_NOT_EXIST);
  EXPECT_EQ(change_calls, 0);
}

TEST_F(ServiceStartupTest, ReadOnlyAccessReportsActualModeAndMutationFailsTruthfully) {
  denied = true;
  const auto state = service_ctrl::get_service_startup();
  EXPECT_TRUE(state.status);
  EXPECT_TRUE(state.automatic);
  EXPECT_FALSE(state.can_change);
  EXPECT_EQ(state.error_code, ERROR_ACCESS_DENIED);
  EXPECT_FALSE(service_ctrl::set_service_startup(false).status);
  EXPECT_EQ(mode, SERVICE_AUTO_START);
  EXPECT_EQ(change_calls, 0);
}

TEST_F(ServiceStartupTest, FailedReadDoesNotModifyService) {
  query_failure = true;
  const auto state = service_ctrl::set_service_startup(false);
  EXPECT_FALSE(state.status);
  EXPECT_EQ(state.error_code, ERROR_INVALID_DATA);
  EXPECT_EQ(change_calls, 0);
}

TEST_F(ServiceStartupTest, FailedChangeKeepsActualPreferenceAndReturnsFailure) {
  change_failure = true;
  const auto state = service_ctrl::set_service_startup(false);
  EXPECT_FALSE(state.status);
  EXPECT_TRUE(state.automatic);
  EXPECT_EQ(state.error_code, ERROR_ACCESS_DENIED);
  EXPECT_EQ(mode, SERVICE_AUTO_START);
}

TEST_F(ServiceStartupTest, ChangeMustBeVerifiedAgainstActualReadback) {
  ignore_change = true;
  const auto state = service_ctrl::set_service_startup(false);
  EXPECT_FALSE(state.status);
  EXPECT_TRUE(state.automatic);
  EXPECT_EQ(state.error_code, ERROR_WRITE_FAULT);
}
