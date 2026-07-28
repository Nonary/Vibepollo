#include "src/platform/windows/display_helper_v2/win_scheduled_task_manager.h"

#include "src/logging.h"

#ifndef SECURITY_WIN32
  #define SECURITY_WIN32
#endif

#include <comdef.h>
#include <lmcons.h>
#include <sddl.h>
#include <secext.h>
#include <taskschd.h>
#include <wtsapi32.h>

#include <cstddef>
#include <memory>
#include <vector>

namespace display_helper::v2 {
  namespace {
    constexpr DWORD kInvalidSessionId = static_cast<DWORD>(-1);
    constexpr wchar_t kLocalSystemSid[] = L"S-1-5-18";
    constexpr wchar_t kLocalServiceSid[] = L"S-1-5-19";
    constexpr wchar_t kNetworkServiceSid[] = L"S-1-5-20";
    constexpr wchar_t kBuiltinUsersSid[] = L"S-1-5-32-545";

    struct HandleCloser {
      void operator()(void *handle) const noexcept {
        if (handle) {
          CloseHandle(handle);
        }
      }
    };

    struct LocalMemoryCloser {
      void operator()(void *memory) const noexcept {
        if (memory) {
          LocalFree(memory);
        }
      }
    };

    using unique_handle = std::unique_ptr<void, HandleCloser>;
    using unique_local_memory = std::unique_ptr<void, LocalMemoryCloser>;

    std::wstring sid_to_string(PSID sid) {
      if (!sid || !IsValidSid(sid)) {
        return {};
      }

      LPWSTR raw_sid = nullptr;
      if (!ConvertSidToStringSidW(sid, &raw_sid)) {
        return {};
      }
      unique_local_memory sid_guard(raw_sid);
      return raw_sid;
    }

    std::wstring canonicalize_sid_string(const std::wstring &candidate) {
      if (candidate.empty()) {
        return {};
      }

      PSID raw_sid = nullptr;
      if (!ConvertStringSidToSidW(candidate.c_str(), &raw_sid)) {
        return {};
      }
      unique_local_memory sid_guard(raw_sid);
      return sid_to_string(raw_sid);
    }

    std::wstring query_token_user_sid(HANDLE token) {
      if (!token) {
        return {};
      }

      DWORD required = 0;
      (void) GetTokenInformation(token, TokenUser, nullptr, 0, &required);
      if (required == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return {};
      }

      std::vector<std::byte> buffer(required);
      if (!GetTokenInformation(token, TokenUser, buffer.data(), required, &required)) {
        return {};
      }

      const auto *token_user = reinterpret_cast<const TOKEN_USER *>(buffer.data());
      return sid_to_string(token_user->User.Sid);
    }

    std::wstring query_session_user_sid(DWORD session_id) {
      if (session_id == kInvalidSessionId) {
        return {};
      }

      HANDLE raw_token = nullptr;
      if (!WTSQueryUserToken(session_id, &raw_token)) {
        return {};
      }
      const unique_handle token(raw_token);
      return query_token_user_sid(raw_token);
    }

    std::wstring query_process_user_sid() {
      HANDLE raw_token = nullptr;
      if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
        return {};
      }
      const unique_handle token(raw_token);
      return query_token_user_sid(raw_token);
    }

    std::wstring lookup_account_sid(const std::wstring &account_name) {
      if (account_name.empty()) {
        return {};
      }

      DWORD sid_size = 0;
      DWORD domain_size = 0;
      SID_NAME_USE sid_type = SidTypeUnknown;
      (void) LookupAccountNameW(
        nullptr,
        account_name.c_str(),
        nullptr,
        &sid_size,
        nullptr,
        &domain_size,
        &sid_type);
      if (sid_size == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return {};
      }

      std::vector<std::byte> sid_buffer(sid_size);
      std::vector<wchar_t> domain_buffer(domain_size == 0 ? 1 : domain_size);
      if (!LookupAccountNameW(
            nullptr,
            account_name.c_str(),
            sid_buffer.data(),
            &sid_size,
            domain_buffer.data(),
            &domain_size,
            &sid_type)) {
        return {};
      }
      return sid_to_string(sid_buffer.data());
    }

    std::wstring query_session_account(DWORD session_id) {
      if (session_id == kInvalidSessionId) {
        return {};
      }

      auto fetch_session_string = [&](WTS_INFO_CLASS info_class) -> std::wstring {
        LPWSTR buffer = nullptr;
        DWORD bytes = 0;
        if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session_id, info_class, &buffer, &bytes)) {
          return {};
        }

        std::wstring value;
        if (buffer && *buffer != L'\0') {
          value.assign(buffer);
        }

        if (buffer) {
          WTSFreeMemory(buffer);
        }

        return value;
      };

      std::wstring user = fetch_session_string(WTSUserName);
      if (user.empty()) {
        return {};
      }

      std::wstring domain = fetch_session_string(WTSDomainName);
      if (!domain.empty()) {
        return domain + L"\\" + user;
      }

      return user;
    }

    bool is_service_sid(const std::wstring &sid) {
      return _wcsicmp(sid.c_str(), kLocalSystemSid) == 0 ||
             _wcsicmp(sid.c_str(), kLocalServiceSid) == 0 ||
             _wcsicmp(sid.c_str(), kNetworkServiceSid) == 0;
    }
  }  // namespace

  std::wstring WinScheduledTaskManager::resolve_username(const std::wstring &username_hint) {
    if (!username_hint.empty()) {
      return username_hint;
    }

    std::wstring username = query_session_account(WTSGetActiveConsoleSessionId());

    if (username.empty()) {
      DWORD sam_required = 0;
      if (!GetUserNameExW(NameSamCompatible, nullptr, &sam_required) &&
          GetLastError() == ERROR_MORE_DATA && sam_required > 0) {
        std::wstring sam_name;
        sam_name.resize(sam_required);
        DWORD sam_size = sam_required;
        if (GetUserNameExW(NameSamCompatible, sam_name.data(), &sam_size)) {
          sam_name.resize(sam_size);
          username = std::move(sam_name);
        }
      }
    }

    if (username.empty()) {
      wchar_t fallback[UNLEN + 1] = {0};
      DWORD fallback_len = UNLEN + 1;
      if (GetUserNameW(fallback, &fallback_len) && fallback_len > 0) {
        username.assign(fallback);
      }
    }

    return username;
  }

  std::wstring WinScheduledTaskManager::resolve_user_sid(const std::wstring &username_hint) {
    // Resolve any explicit identity to a SID before crossing the Task Scheduler
    // COM boundary. Account strings can fail mapping there even when WTS was
    // able to return the same DOMAIN\\user text.
    if (!username_hint.empty()) {
      auto sid = canonicalize_sid_string(username_hint);
      if (sid.empty()) {
        sid = lookup_account_sid(username_hint);
      }
      if (!sid.empty() && !is_service_sid(sid)) {
        return sid;
      }
    }

    // The helper normally runs as SYSTEM. The active-session token is therefore
    // the authoritative identity of the user whose logon must trigger restore.
    auto sid = query_session_user_sid(WTSGetActiveConsoleSessionId());
    if (!sid.empty() && !is_service_sid(sid)) {
      return sid;
    }

    // This supports direct/manual helper launches under an interactive user.
    sid = query_process_user_sid();
    if (!sid.empty() && !is_service_sid(sid)) {
      return sid;
    }

    // Last chance for environments where token access is restricted but the
    // session account remains resolvable through LSA.
    sid = lookup_account_sid(resolve_username({}));
    if (!sid.empty() && !is_service_sid(sid)) {
      return sid;
    }
    return {};
  }

  std::wstring WinScheduledTaskManager::build_restore_task_name(const std::wstring &) {
    return L"VibeshineDisplayRestore";
  }

  bool WinScheduledTaskManager::create_restore_task(const std::wstring &username) {
    const std::wstring user_sid = resolve_user_sid(username);
    const bool has_user_sid = !user_sid.empty();
    const std::wstring task_name = build_restore_task_name({});

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
      BOOST_LOG(error) << "Display helper v2: failed to initialize COM for Task Scheduler: 0x" << std::hex << hr;
      return false;
    }

    ITaskService *service = nullptr;
    hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskService, (void **) &service);
    if (FAILED(hr)) {
      BOOST_LOG(error) << "Display helper v2: failed to create Task Scheduler service: 0x" << std::hex << hr;
      CoUninitialize();
      return false;
    }

    hr = service->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t());
    if (FAILED(hr)) {
      BOOST_LOG(error) << "Display helper v2: failed to connect to Task Scheduler: 0x" << std::hex << hr;
      service->Release();
      CoUninitialize();
      return false;
    }

    ITaskFolder *root_folder = nullptr;
    hr = service->GetFolder(_bstr_t(L"\\"), &root_folder);
    if (FAILED(hr)) {
      BOOST_LOG(error) << "Display helper v2: failed to open root task folder: 0x" << std::hex << hr;
      service->Release();
      CoUninitialize();
      return false;
    }

    ITaskDefinition *task = nullptr;
    hr = service->NewTask(0, &task);
    if (FAILED(hr)) {
      BOOST_LOG(error) << "Display helper v2: failed to create task definition: 0x" << std::hex << hr;
      root_folder->Release();
      service->Release();
      CoUninitialize();
      return false;
    }

    if (IRegistrationInfo *reg_info = nullptr; SUCCEEDED(task->get_RegistrationInfo(&reg_info))) {
      reg_info->put_Author(_bstr_t(L"Sunshine Display Helper"));
      reg_info->put_Description(_bstr_t(L"Automatically restores display settings after reboot"));
      reg_info->Release();
    }

    if (ITaskSettings *settings = nullptr; SUCCEEDED(task->get_Settings(&settings))) {
      settings->put_StartWhenAvailable(VARIANT_TRUE);
      settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
      settings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
      settings->put_ExecutionTimeLimit(_bstr_t(L"PT0S"));
      settings->put_Hidden(VARIANT_TRUE);
      settings->Release();
    }

    ITriggerCollection *trigger_collection = nullptr;
    hr = task->get_Triggers(&trigger_collection);
    if (FAILED(hr)) {
      task->Release();
      root_folder->Release();
      service->Release();
      CoUninitialize();
      return false;
    }

    ITrigger *trigger = nullptr;
    hr = trigger_collection->Create(TASK_TRIGGER_LOGON, &trigger);
    trigger_collection->Release();
    if (FAILED(hr)) {
      task->Release();
      root_folder->Release();
      service->Release();
      CoUninitialize();
      return false;
    }

    if (ILogonTrigger *logon_trigger = nullptr; SUCCEEDED(trigger->QueryInterface(IID_ILogonTrigger, (void **) &logon_trigger))) {
      logon_trigger->put_Id(_bstr_t(L"SunshineDisplayHelperLogonTrigger"));
      logon_trigger->put_Enabled(VARIANT_TRUE);
      if (has_user_sid) {
        logon_trigger->put_UserId(_bstr_t(user_sid.c_str()));
      }
      logon_trigger->Release();
    }
    trigger->Release();

    IActionCollection *action_collection = nullptr;
    hr = task->get_Actions(&action_collection);
    if (FAILED(hr)) {
      task->Release();
      root_folder->Release();
      service->Release();
      CoUninitialize();
      return false;
    }

    IAction *action = nullptr;
    hr = action_collection->Create(TASK_ACTION_EXEC, &action);
    action_collection->Release();
    if (FAILED(hr)) {
      task->Release();
      root_folder->Release();
      service->Release();
      CoUninitialize();
      return false;
    }

    IExecAction *exec_action = nullptr;
    hr = action->QueryInterface(IID_IExecAction, (void **) &exec_action);
    action->Release();
    if (FAILED(hr)) {
      task->Release();
      root_folder->Release();
      service->Release();
      CoUninitialize();
      return false;
    }

    wchar_t exe_path[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exe_path, MAX_PATH)) {
      exec_action->Release();
      task->Release();
      root_folder->Release();
      service->Release();
      CoUninitialize();
      return false;
    }

    exec_action->put_Path(_bstr_t(exe_path));
    exec_action->put_Arguments(_bstr_t(L"--restore"));
    exec_action->Release();

    // When no interactive token exists (for example, after sign-out), bind the
    // task to BUILTIN\Users by SID so registration never depends on localized
    // account-name mapping and still runs at the next user logon.
    const TASK_LOGON_TYPE logon_type = has_user_sid ? TASK_LOGON_INTERACTIVE_TOKEN : TASK_LOGON_GROUP;

    if (IPrincipal *principal = nullptr; SUCCEEDED(task->get_Principal(&principal))) {
      if (has_user_sid) {
        principal->put_UserId(_bstr_t(user_sid.c_str()));
      } else {
        principal->put_GroupId(_bstr_t(kBuiltinUsersSid));
      }
      principal->put_LogonType(logon_type);
      principal->put_RunLevel(TASK_RUNLEVEL_LUA);
      principal->Release();
    }

    IRegisteredTask *registered_task = nullptr;
    HRESULT registration_hr = root_folder->RegisterTaskDefinition(
      _bstr_t(task_name.c_str()),
      task,
      TASK_CREATE_OR_UPDATE,
      _variant_t(has_user_sid ? user_sid.c_str() : kBuiltinUsersSid),
      _variant_t(),
      logon_type,
      _variant_t(L""),
      &registered_task
    );

    if (registered_task) {
      registered_task->Release();
    }

    task->Release();
    root_folder->Release();
    service->Release();
    CoUninitialize();

    if (FAILED(registration_hr)) {
      BOOST_LOG(error) << "Display helper v2: failed to register scheduled task: 0x" << std::hex << registration_hr;
      return false;
    }

    return true;
  }

  bool WinScheduledTaskManager::delete_restore_task() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
      return false;
    }

    ITaskService *service = nullptr;
    hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskService, (void **) &service);
    if (FAILED(hr)) {
      CoUninitialize();
      return false;
    }

    hr = service->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t());
    if (FAILED(hr)) {
      service->Release();
      CoUninitialize();
      return false;
    }

    ITaskFolder *root_folder = nullptr;
    hr = service->GetFolder(_bstr_t(L"\\"), &root_folder);
    if (FAILED(hr)) {
      service->Release();
      CoUninitialize();
      return false;
    }

    bool success = true;
    const auto task_name = build_restore_task_name({});
    const HRESULT delete_hr = root_folder->DeleteTask(_bstr_t(task_name.c_str()), 0);
    if (FAILED(delete_hr) && delete_hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
      success = false;
    }

    root_folder->Release();
    service->Release();
    CoUninitialize();

    return success;
  }

  bool WinScheduledTaskManager::is_task_present() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
      return false;
    }

    ITaskService *service = nullptr;
    hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskService, (void **) &service);
    if (FAILED(hr)) {
      CoUninitialize();
      return false;
    }

    hr = service->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t());
    if (FAILED(hr)) {
      service->Release();
      CoUninitialize();
      return false;
    }

    ITaskFolder *root_folder = nullptr;
    hr = service->GetFolder(_bstr_t(L"\\"), &root_folder);
    if (FAILED(hr)) {
      service->Release();
      CoUninitialize();
      return false;
    }

    bool found = false;
    const std::wstring task_name = build_restore_task_name({});
    IRegisteredTask *task = nullptr;
    hr = root_folder->GetTask(_bstr_t(task_name.c_str()), &task);
    if (SUCCEEDED(hr) && task) {
      found = true;
      task->Release();
    }

    root_folder->Release();
    service->Release();
    CoUninitialize();

    return found;
  }
}  // namespace display_helper::v2
