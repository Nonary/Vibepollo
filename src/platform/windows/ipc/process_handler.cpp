
/**
 * @file process_handler.cpp
 * @brief Implements the ProcessHandler class for managing process creation and control on Windows.
 *
 * This file provides the implementation for starting, waiting, and terminating processes,
 * including support for attribute lists and impersonation as needed for the Sunshine project.
 */

// platform includes
#include <UserEnv.h>
#include <windows.h>

// standard includes
#include <algorithm>
#include <system_error>
#include <vector>

// local includes
#include "process_handler.h"
#include "src/logging.h"
#include "src/platform/windows/misc.h"
#include "src/utility.h"

ProcessHandler::ProcessHandler():
    job_(create_kill_on_close_job()),
    use_job_(true) {}

ProcessHandler::ProcessHandler(bool use_job):
    job_(use_job ? create_kill_on_close_job() : winrt::handle {}),
    use_job_(use_job) {}

bool ProcessHandler::start(const std::wstring &application_path, std::wstring_view arguments) {
  if (running_) {
    // Check if the previously started process has already exited. If so, clear stale state.
    if (pi_.hProcess != nullptr) {
      DWORD wait_result = WaitForSingleObject(pi_.hProcess, 0);
      if (wait_result == WAIT_TIMEOUT) {
        // Still running, don't start a new one
        return false;
      }

      // Process either exited or handle is invalid, clean up and allow restart
      if (pi_.hThread) {
        CloseHandle(pi_.hThread);
      }
      if (pi_.hProcess) {
        CloseHandle(pi_.hProcess);
      }
      ZeroMemory(&pi_, sizeof(pi_));
      running_ = false;
    } else {
      // No process handle but marked running; reset state to allow restart
      running_ = false;
    }
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

  BOOST_LOG(info) << "Launching process: " << platf::to_utf8(application_path)
                  << (arguments.empty() ? "" : " with arguments") << " (hidden, detached)";

  STARTUPINFOEXW si = {};
  si.StartupInfo.cb = sizeof(si);

  DWORD creation_flags = CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT;
  // When not using a job (keep-alive child), prefer to break away from any existing job to avoid kill-on-close
  if (!use_job_) {
    creation_flags |= CREATE_BREAKAWAY_FROM_JOB;
  }
  // Compute a sane working directory for the child: the directory of the target executable
  std::wstring working_dir;
  {
    size_t pos = application_path.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
      working_dir = application_path.substr(0, pos);
    }
  }

  BOOL ret = FALSE;

  if (platf::is_running_as_system()) {
    HANDLE user_token = platf::retrieve_users_token(false);
    if (!user_token) {
      BOOST_LOG(error) << "Failed to retrieve user token while launching: " << platf::to_utf8(application_path);
      return false;
    }
    auto close_token = util::fail_guard([&]() {
      CloseHandle(user_token);
    });

    // Build a user-specific environment block for the child process
    void *env_block = nullptr;
    if (!CreateEnvironmentBlock(&env_block, user_token, FALSE)) {
      BOOST_LOG(error) << "CreateEnvironmentBlock failed, error: " << GetLastError();
      env_block = nullptr;
    }
    auto destroy_env = util::fail_guard([&]() {
      if (env_block) {
        DestroyEnvironmentBlock(env_block);
      }
    });

    // Launch in the user's context with their environment and an explicit working directory
    ret = CreateProcessAsUserW(
      user_token,
      nullptr,
      (LPWSTR) cmd_line.c_str(),
      nullptr,
      nullptr,
      FALSE,
      creation_flags,
      env_block,
      working_dir.empty() ? nullptr : working_dir.c_str(),
      (LPSTARTUPINFOW) &si,
      &pi_
    );
  } else {
    // Non-SYSTEM: inherit our environment but still supply a sensible working directory
    ret = CreateProcessW(
      nullptr,
      (LPWSTR) cmd_line.c_str(),
      nullptr,
      nullptr,
      FALSE,
      creation_flags,
      nullptr,
      working_dir.empty() ? nullptr : working_dir.c_str(),
      (LPSTARTUPINFOW) &si,
      &pi_
    );
  }

  if (ret && use_job_ && job_) {
    AssignProcessToJobObject(job_.get(), pi_.hProcess);
  }

  running_ = ret;
  if (!running_) {
    auto winerr = GetLastError();
    BOOST_LOG(error) << "Failed to launch process: " << platf::to_utf8(application_path) << ", error: " << winerr;
    ZeroMemory(&pi_, sizeof(pi_));
  }
  if (running_) {
    DWORD pid = pi_.dwProcessId;
    BOOST_LOG(info) << "Process started successfully (pid=" << pid << ")";
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
