/** @file src/platform/windows/end_stream_hotkey.cpp
 * @brief Owns a console-user helper and authenticates its local hotkey IPC.
 */
#ifdef _WIN32
  #include "end_stream_hotkey_protocol.h"
  #include "hotkey_manager.h"
  #include "misc.h"
  #include "src/logging.h"
  #include "src/nvhttp.h"
  #include "src/utility.h"

  #include <array>
  #include <condition_variable>
  #include <filesystem>
  #include <mutex>
  #include <objbase.h>
  #include <sddl.h>
  #include <stop_token>
  #include <thread>
  #include <vector>
  #include <wtsapi32.h>

namespace {
  using namespace std::chrono_literals;
  using namespace platf::hotkey::end_stream;

  struct configuration {
    int key = 0;
    unsigned int modifiers = 0;
    std::uint64_t generation = 0;
  };

  struct manager {
    std::mutex mutex;
    std::condition_variable_any changed;
    configuration config;
    std::jthread worker;
  };

  manager &state() {
    static manager instance;
    return instance;
  }

  configuration snapshot() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    return s.config;
  }

  void pause(std::stop_token stop, std::uint64_t generation, std::chrono::milliseconds duration) {
    auto &s = state();
    std::unique_lock lock(s.mutex);
    s.changed.wait_for(lock, stop, duration, [&] {
      return s.config.generation != generation;
    });
  }

  bool current(std::stop_token stop, const configuration &config, DWORD session) {
    return !stop.stop_requested() && snapshot().generation == config.generation && WTSGetActiveConsoleSessionId() == session;
  }

  bool run_helper(std::stop_token stop, const configuration &config, DWORD session) {
    // This selects the console identity, not an unconditional limited token.
    // Non-SYSTEM CreateProcess inherits host elevation; retrieve_users_token(false)
    // does not strip a Full token (for example, when UAC is disabled).
    HANDLE token = nullptr;
    const bool system = platf::is_running_as_system();
    if (system) {
      token = platf::retrieve_users_token(false);
    } else if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
      return false;
    }
    if (!token) {
      return false;
    }
    auto close_token = util::fail_guard([&] {
      CloseHandle(token);
    });
    DWORD token_session = 0, needed = 0;
    if (!GetTokenInformation(token, TokenSessionId, &token_session, sizeof(token_session), &needed) || token_session != session) {
      return false;
    }
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    std::vector<std::uint8_t> token_user(needed);
    if (!GetTokenInformation(token, TokenUser, token_user.data(), needed, &needed)) {
      return false;
    }
    auto sid = reinterpret_cast<TOKEN_USER *>(token_user.data())->User.Sid;
    LPWSTR sid_text = nullptr;
    if (!ConvertSidToStringSidW(sid, &sid_text)) {
      return false;
    }
    auto free_sid = util::fail_guard([&] {
      LocalFree(sid_text);
    });
    std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GW;;;" + std::wstring(sid_text) + L")S:(ML;;NW;;;ME)";
    PSECURITY_DESCRIPTOR security = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &security, nullptr)) {
      return false;
    }
    auto free_security = util::fail_guard([&] {
      LocalFree(security);
    });
    SECURITY_ATTRIBUTES attributes {sizeof(attributes), security, FALSE};
    GUID guid {};
    if (FAILED(CoCreateGuid(&guid))) {
      return false;
    }
    wchar_t guid_text[40] {};
    if (StringFromGUID2(guid, guid_text, 40) == 0) {
      return false;
    }
    const std::wstring pipe_name = L"\\\\.\\pipe\\vibepollo_end_stream_" + std::wstring(guid_text);
    HANDLE pipe = CreateNamedPipeW(pipe_name.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 0, 4096, 0, &attributes);
    if (pipe == INVALID_HANDLE_VALUE) {
      return false;
    }
    auto close_pipe = util::fail_guard([&] {
      CancelIoEx(pipe, nullptr);
      CloseHandle(pipe);
    });
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) {
      return false;
    }
    auto close_event = util::fail_guard([&] {
      CloseHandle(event);
    });
    OVERLAPPED connection {};
    connection.hEvent = event;
    const bool immediate = ConnectNamedPipe(pipe, &connection) != FALSE;
    const DWORD connection_error = immediate ? ERROR_SUCCESS : GetLastError();
    if (!immediate && connection_error != ERROR_IO_PENDING && connection_error != ERROR_PIPE_CONNECTED) {
      return false;
    }
    auto cancel_connection = util::fail_guard([&] {
      if (connection_error == ERROR_IO_PENDING) {
        CancelIoEx(pipe, &connection);
        DWORD ignored = 0;
        GetOverlappedResult(pipe, &connection, &ignored, TRUE);
      }
    });
    std::array<wchar_t, 32768> executable {};
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size()) {
      return false;
    }
    const auto directory = std::filesystem::path(executable.data()).parent_path();
    const auto helper = directory / L"tools" / L"sunshine_end_stream_hotkey.exe";
    const std::wstring command = L"\"" + helper.wstring() + L"\" \"" + pipe_name + L"\" " +
                                 std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(config.key) + L" " + std::to_wstring(config.modifiers);
    std::error_code error;
    auto startup = platf::create_startup_info(nullptr, nullptr, error);
    if (error) {
      return false;
    }
    auto free_startup = util::fail_guard([&] {
      platf::free_proc_thread_attr_list(startup.lpAttributeList);
    });
    wchar_t desktop[] = L"winsta0\\default";
    startup.StartupInfo.lpDesktop = desktop;
    PROCESS_INFORMATION child {};
    constexpr DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW;
    const bool launched = system ?
                            platf::launch_process_with_impersonation(false, platf::to_utf8(command), directory.wstring(), flags, startup, child, error) :
                            platf::launch_process_without_impersonation(platf::to_utf8(command), directory.wstring(), flags, startup, child, error);
    if (!launched) {
      BOOST_LOG(warning) << "End-stream hotkey helper could not be launched: " << error.message();
      return false;
    }
    CloseHandle(child.hThread);
    auto retire_child = util::fail_guard([&] {
      // Retire only the exact process we launched, without a name-based kill.
      DisconnectNamedPipe(pipe);
      if (WaitForSingleObject(child.hProcess, 1500) == WAIT_TIMEOUT) {
        TerminateProcess(child.hProcess, 0);
        WaitForSingleObject(child.hProcess, 1500);
      }
      CloseHandle(child.hProcess);
    });
    DWORD child_session = 0;
    if (!ProcessIdToSessionId(child.dwProcessId, &child_session) || child_session != session) {
      return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    if (connection_error == ERROR_IO_PENDING) {
      while (WaitForSingleObject(event, 100) == WAIT_TIMEOUT) {
        if (!current(stop, config, session) || std::chrono::steady_clock::now() >= deadline || WaitForSingleObject(child.hProcess, 0) != WAIT_TIMEOUT) {
          return false;
        }
      }
      DWORD ignored = 0;
      if (!GetOverlappedResult(pipe, &connection, &ignored, FALSE)) {
        return false;
      }
    }
    cancel_connection.disable();
    ULONG client_pid = 0, client_session = 0;
    if (!GetNamedPipeClientProcessId(pipe, &client_pid) || !GetNamedPipeClientSessionId(pipe, &client_session) || !authorized_sender(child.dwProcessId, client_pid, session, client_session, WTSGetActiveConsoleSessionId())) {
      return false;
    }
    bool ready = false;
    while (current(stop, config, session) && WaitForSingleObject(child.hProcess, 0) == WAIT_TIMEOUT) {
      ResetEvent(event);
      OVERLAPPED read {};
      read.hEvent = event;
      std::uint8_t byte = 0;
      DWORD bytes = 0;
      const bool read_now = ReadFile(pipe, &byte, 1, &bytes, &read) != FALSE;
      if (!read_now) {
        if (GetLastError() != ERROR_IO_PENDING) {
          break;
        }
        // Drain every canceled I/O before its OVERLAPPED storage goes away.
        auto cancel_read = util::fail_guard([&] {
          CancelIoEx(pipe, &read);
          GetOverlappedResult(pipe, &read, &bytes, TRUE);
        });
        const auto read_deadline = std::chrono::steady_clock::now() + 3s;
        DWORD wait = WAIT_TIMEOUT;
        while ((wait = WaitForSingleObject(event, 100)) == WAIT_TIMEOUT) {
          if (!current(stop, config, session) || std::chrono::steady_clock::now() >= read_deadline) {
            break;
          }
        }
        if (wait != WAIT_OBJECT_0 || !current(stop, config, session) || !GetOverlappedResult(pipe, &read, &bytes, FALSE)) {
          break;
        }
        cancel_read.disable();
      }
      if (bytes != 1 || !current(stop, config, session)) {
        break;
      }
      const auto type = static_cast<message>(byte);
      if (type == message::ready && !ready) {
        ready = true;
        BOOST_LOG(info) << "End-stream hotkey registered in interactive console session " << session << '.';
      } else if (type == message::registration_failed) {
        BOOST_LOG(warning) << "End-stream shortcut registration failed; another application may own the combination.";
        break;
      } else if (type == message::terminate && ready) {
        if (authorized_sender(child.dwProcessId, client_pid, session, child_session, WTSGetActiveConsoleSessionId())) {
          nvhttp::terminate_all_streams_from_host([&] {
            return current(stop, config, session) && authorized_sender(child.dwProcessId, client_pid, session, child_session, WTSGetActiveConsoleSessionId());
          });
        }
      } else if (type != message::heartbeat) {
        break;
      }
    }
    return ready;
  }

  void supervise(std::stop_token stop) {
    while (!stop.stop_requested()) {
      const auto config = snapshot();
      const DWORD session = WTSGetActiveConsoleSessionId();
      if (!valid_binding(config.key, config.modifiers) || session == 0 || session == 0xFFFFFFFF) {
        pause(stop, config.generation, 500ms);
        continue;
      }
      try {
        if (!run_helper(stop, config, session) && current(stop, config, session)) {
          BOOST_LOG(warning) << "End-stream hotkey unavailable; retrying the interactive helper.";
        }
      } catch (const std::exception &exception) {
        BOOST_LOG(error) << "End-stream hotkey operation failed: " << exception.what();
      } catch (...) {
        BOOST_LOG(error) << "End-stream hotkey operation failed.";
      }
      pause(stop, config.generation, 5s);
    }
  }
}  // namespace

namespace platf::hotkey {
  void update_end_stream_hotkey(int key, unsigned int modifiers) {
    if (key != 0 && !end_stream::valid_binding(key, modifiers)) {
      BOOST_LOG(warning) << "End-stream hotkey disabled: choose a letter, digit, or F1-F24 and at least one modifier.";
      key = 0;
    }
    auto &s = state();
    {
      std::lock_guard lock(s.mutex);
      if (s.config.key == key && s.config.modifiers == modifiers) {
        return;
      }
      s.config = {key, modifiers, s.config.generation + 1};
    }
    s.changed.notify_all();
  }

  void start_end_stream_hotkey() {
    // Owned by nvhttp after initialization, rather than by config parsing.
    state().worker = std::jthread(supervise);
  }

  void stop_end_stream_hotkey() {
    auto &s = state();
    s.worker.request_stop();
    s.changed.notify_all();
    if (s.worker.joinable()) {
      s.worker.join();
    }
  }
}  // namespace platf::hotkey
#endif
