
/**
 * @file process_handler.cpp
 * @brief Implements the ProcessHandler class for managing process creation and control on Windows.
 *
 * This file provides the implementation for starting, waiting, and terminating processes,
 * including support for attribute lists and impersonation as needed for the Sunshine project.
 */

// platform includes
#include <windows.h>

// standard includes
#include <algorithm>
#include <system_error>
#include <vector>

// local includes
#include "process_handler.h"
#include "src/platform/windows/misc.h"
#include "src/utility.h"

ProcessHandler::ProcessHandler():
    job_(create_kill_on_close_job()) {}

ProcessHandler::ProcessHandler(bool use_job):
    job_(use_job ? create_kill_on_close_job() : winrt::handle{}),
    use_job_(use_job) {}

bool ProcessHandler::start(const std::wstring &application_path, std::wstring_view arguments) {
  if (running_) {
    return false;
  }

  ZeroMemory(&pi_, sizeof(pi_));

  // Build command line: "app_path" [arguments]
  std::wstring cmd_line;
  cmd_line.reserve(application_path.size() + arguments.size() + 3);
  cmd_line.push_back(L'"');
  cmd_line += application_path;
  cmd_line.push_back(L'"');
  if (!arguments.empty()) {
    cmd_line.push_back(L' ');
    cmd_line.append(arguments);
  }

  STARTUPINFOEXW si = {};
  si.StartupInfo.cb = sizeof(si);

  DWORD creation_flags = CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW;
  BOOL ret = FALSE;

  if (platf::is_running_as_system()) {
    HANDLE user_token = platf::retrieve_users_token(true);
    if (!user_token) {
      return false;
    }
    auto close_token = util::fail_guard([&]() { CloseHandle(user_token); });
    ret = CreateProcessAsUserW(user_token, nullptr, (LPWSTR) cmd_line.c_str(), nullptr, nullptr, FALSE, creation_flags, nullptr, nullptr, (LPSTARTUPINFOW) &si, &pi_);
  } else {
    ret = CreateProcessW(nullptr, (LPWSTR) cmd_line.c_str(), nullptr, nullptr, FALSE, creation_flags, nullptr, nullptr, (LPSTARTUPINFOW) &si, &pi_);
  }

  if (ret && use_job_ && job_) {
    AssignProcessToJobObject(job_.get(), pi_.hProcess);
  }

  running_ = ret;
  if (!running_) {
    ZeroMemory(&pi_, sizeof(pi_));
  }
  return running_;
}

bool ProcessHandler::wait(DWORD &exit_code) {
  if (!running_ || pi_.hProcess == nullptr) {
    return false;
  }
  DWORD wait_result = WaitForSingleObject(pi_.hProcess, INFINITE);
  if (wait_result != WAIT_OBJECT_0) {
    return false;
  }
  BOOL got_code = GetExitCodeProcess(pi_.hProcess, &exit_code);
  running_ = false;
  return got_code != 0;
}

void ProcessHandler::terminate() {
  if (running_ && pi_.hProcess) {
    TerminateProcess(pi_.hProcess, 1);
    running_ = false;
  }
}

ProcessHandler::~ProcessHandler() {
  // Terminate process first if it's still running
  terminate();

  // Clean up handles
  if (pi_.hProcess) {
    CloseHandle(pi_.hProcess);
  }
  if (pi_.hThread) {
    CloseHandle(pi_.hThread);
  }
  // job_ is a winrt::handle and will auto-cleanup
}

HANDLE ProcessHandler::get_process_handle() const {
  return running_ ? pi_.hProcess : nullptr;
}

winrt::handle create_kill_on_close_job() {
  winrt::handle job_handle {CreateJobObjectW(nullptr, nullptr)};
  if (!job_handle) {
    return {};
  }
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_info = {};
  job_info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(job_handle.get(), JobObjectExtendedLimitInformation, &job_info, sizeof(job_info))) {
    // winrt::handle will auto-close on destruction
    return {};
  }
  return job_handle;
}
