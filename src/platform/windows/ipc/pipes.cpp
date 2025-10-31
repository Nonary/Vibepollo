

/**
 * @file pipes.cpp
 * @brief Implements Windows named and anonymous pipe IPC for Sunshine.
 *
 * Provides cross-process communication using Windows named pipes, including security descriptor setup,
 * overlapped I/O, and handshake logic for anonymous pipes. Used for secure and robust IPC between processes.
 */

// standard includes
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <format>
#include <functional>
#include <iomanip>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// platform includes
#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#include <AclAPI.h>
#include <combaseapi.h>
#include <sddl.h>
#include <Windows.h>
#include <winrt/base.h>
#include <winsock2.h>

// local includes
#include "misc_utils.h"
#include "pipes.h"
#include "src/utility.h"

namespace platf::dxgi {

  // Helper functions for proper string conversion

  bool init_sd_with_explicit_aces(SECURITY_DESCRIPTOR &desc, std::vector<EXPLICIT_ACCESS> &eaList, PACL *out_pacl) {
    if (!InitializeSecurityDescriptor(&desc, SECURITY_DESCRIPTOR_REVISION)) {
      return false;
    }
    PACL rawDacl = nullptr;
    if (DWORD err = SetEntriesInAcl(static_cast<ULONG>(eaList.size()), eaList.data(), nullptr, &rawDacl); err != ERROR_SUCCESS) {
      return false;
    }
    if (!SetSecurityDescriptorDacl(&desc, TRUE, rawDacl, FALSE)) {
      LocalFree(rawDacl);
      return false;
    }
    *out_pacl = rawDacl;
    return true;
  }

  bool NamedPipeFactory::create_security_descriptor(SECURITY_DESCRIPTOR &desc, PACL *out_pacl) const {
    bool isSystem = platf::dxgi::is_running_as_system();

    safe_token token;
    if (!obtain_access_token(isSystem, token)) {
      return false;
    }

    util::c_ptr<TOKEN_USER> tokenUser;
    PSID raw_user_sid = nullptr;
    if (!extract_user_sid_from_token(token, tokenUser, raw_user_sid)) {
      return false;
    }

    safe_sid system_sid;
    if (!create_system_sid(system_sid)) {
      return false;
    }

    if (!InitializeSecurityDescriptor(&desc, SECURITY_DESCRIPTOR_REVISION)) {
      BOOST_LOG(error) << "InitializeSecurityDescriptor failed in create_security_descriptor, error=" << GetLastError();
      return false;
    }

    return build_access_control_list(isSystem, desc, raw_user_sid, system_sid.get(), out_pacl);
  }

  bool NamedPipeFactory::obtain_access_token(bool isSystem, safe_token &token) const {
    if (isSystem) {
      token.reset(platf::dxgi::retrieve_users_token(false));
      if (!token) {
        BOOST_LOG(error) << "Failed to retrieve user token when running as SYSTEM";
        return false;
      }
    } else {
      HANDLE raw_token = nullptr;
      if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
        BOOST_LOG(error) << "OpenProcessToken failed in create_security_descriptor, error=" << GetLastError();
        return false;
      }
      token.reset(raw_token);
    }
    return true;
  }

  bool NamedPipeFactory::extract_user_sid_from_token(const safe_token &token, util::c_ptr<TOKEN_USER> &tokenUser, PSID &raw_user_sid) const {
    DWORD len = 0;
    auto tokenHandle = const_cast<HANDLE>(token.get());
    GetTokenInformation(tokenHandle, TokenUser, nullptr, 0, &len);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
      BOOST_LOG(error) << "GetTokenInformation (size query) failed in create_security_descriptor, error=" << GetLastError();
      return false;
    }

    // Allocate with malloc to match util::c_ptr<T>::c_free (free) deleter semantics.
    void *buf = std::malloc(len);
    if (!buf) {
      BOOST_LOG(error) << "malloc failed in extract_user_sid_from_token";
      return false;
    }
    tokenUser.reset(reinterpret_cast<TOKEN_USER *>(buf));

    if (!tokenUser || !GetTokenInformation(tokenHandle, TokenUser, tokenUser.get(), len, &len)) {
      BOOST_LOG(error) << "GetTokenInformation (fetch) failed in create_security_descriptor, error=" << GetLastError();
      return false;
    }

    raw_user_sid = tokenUser->User.Sid;
    if (!IsValidSid(raw_user_sid)) {
      BOOST_LOG(error) << "Invalid user SID in create_security_descriptor";
      return false;
    }

    return true;
  }

  bool NamedPipeFactory::create_system_sid(safe_sid &system_sid) const {
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    PSID raw_system_sid = nullptr;
    if (!AllocateAndInitializeSid(&ntAuthority, 1, SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0, 0, 0, 0, 0, &raw_system_sid)) {
      BOOST_LOG(error) << "AllocateAndInitializeSid failed in create_security_descriptor, error=" << GetLastError();
      return false;
    }
    system_sid.reset(raw_system_sid);

    if (!IsValidSid(system_sid.get())) {
      BOOST_LOG(error) << "Invalid system SID in create_security_descriptor";
      return false;
    }

    return true;
  }

  bool NamedPipeFactory::build_access_control_list(bool /*isSystem*/, SECURITY_DESCRIPTOR &desc, PSID raw_user_sid, PSID system_sid, PACL *out_pacl) const {
    std::vector<EXPLICIT_ACCESS> eaList;

    if (system_sid) {
      EXPLICIT_ACCESS eaSys {};
      eaSys.grfAccessPermissions = GENERIC_ALL;
      eaSys.grfAccessMode = SET_ACCESS;
      eaSys.grfInheritance = NO_INHERITANCE;
      eaSys.Trustee.TrusteeForm = TRUSTEE_IS_SID;
      eaSys.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
      eaSys.Trustee.ptstrName = (LPTSTR) system_sid;
      eaList.push_back(eaSys);
    }

    if (raw_user_sid) {
      EXPLICIT_ACCESS eaUser {};
      eaUser.grfAccessPermissions = GENERIC_ALL;
      eaUser.grfAccessMode = SET_ACCESS;
      eaUser.grfInheritance = NO_INHERITANCE;
      eaUser.Trustee.TrusteeForm = TRUSTEE_IS_SID;
      eaUser.Trustee.TrusteeType = TRUSTEE_IS_USER;
      eaUser.Trustee.ptstrName = (LPTSTR) raw_user_sid;
      eaList.push_back(eaUser);
    }

    if (!eaList.empty() && !init_sd_with_explicit_aces(desc, eaList, out_pacl)) {
      BOOST_LOG(error) << "init_sd_with_explicit_aces failed in create_security_descriptor";
      return false;
    }
    return true;
  }

  std::unique_ptr<INamedPipe> NamedPipeFactory::create_server(const std::string &pipeName) {
    auto wPipeBase = utf8_to_wide(pipeName);
    std::wstring fullPipeName = (wPipeBase.find(LR"(\\.\pipe\)") == 0) ? wPipeBase : LR"(\\.\pipe\)" + wPipeBase;

    SECURITY_ATTRIBUTES *pSecAttr = nullptr;
    SECURITY_ATTRIBUTES secAttr {};
    SECURITY_DESCRIPTOR secDesc {};
    PACL rawDacl = nullptr;

    auto fg = util::fail_guard([&rawDacl]() {
      if (rawDacl) {
        LocalFree(rawDacl);
      }
    });

    bool security_configured = false;

    if (_secdesc_builder) {
      if (_secdesc_builder(secDesc, &rawDacl)) {
        secAttr.nLength = static_cast<DWORD>(sizeof(secAttr));
        secAttr.lpSecurityDescriptor = &secDesc;
        secAttr.bInheritHandle = FALSE;
        pSecAttr = &secAttr;
        security_configured = true;
      } else {
        DWORD err = GetLastError();
        BOOST_LOG(warning) << "Custom security descriptor builder failed (error=" << err
                           << "); falling back to default pipe ACL";
        if (rawDacl) {
          LocalFree(rawDacl);
          rawDacl = nullptr;
        }
      }
    }

    if (!security_configured) {
      if (!create_security_descriptor(secDesc, &rawDacl)) {
        BOOST_LOG(error) << "Failed to init security descriptor";
        return nullptr;
      }
      secAttr.nLength = static_cast<DWORD>(sizeof(secAttr));
      secAttr.lpSecurityDescriptor = &secDesc;
      secAttr.bInheritHandle = FALSE;
      pSecAttr = &secAttr;
      security_configured = true;
    }

    winrt::file_handle hPipe {
      CreateNamedPipeW(
        fullPipeName.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,
        65536,
        65536,
        0,
        pSecAttr
      )
    };
    if (!hPipe) {
      DWORD err = GetLastError();
      BOOST_LOG(error) << "CreateNamedPipeW failed (" << err << ")";
      return nullptr;
    }

    auto pipeObj = std::make_unique<WinPipe>(std::move(hPipe), true);
    return pipeObj;
  }

  void NamedPipeFactory::set_security_descriptor_builder(SecurityDescriptorBuilder builder) {
    _secdesc_builder = std::move(builder);
  }

  std::unique_ptr<INamedPipe> NamedPipeFactory::create_client(const std::string &pipeName) {
    auto wPipeBase = utf8_to_wide(pipeName);
    std::wstring fullPipeName = (wPipeBase.find(LR"(\\.\pipe\)") == 0) ? wPipeBase : LR"(\\.\pipe\)" + wPipeBase;

    winrt::file_handle hPipe = create_client_pipe(fullPipeName);
    if (!hPipe) {
      DWORD err = GetLastError();
      BOOST_LOG(error) << "CreateFileW failed (" << err << ")";
      return nullptr;
    }

    auto pipeObj = std::make_unique<WinPipe>(std::move(hPipe), false);
    return pipeObj;
  }

  winrt::file_handle NamedPipeFactory::create_client_pipe(const std::wstring &fullPipeName) const {
    const ULONGLONG deadline = GetTickCount64() + 15000;  // 15s
    const ULONGLONG start_time = GetTickCount64();
    int retry_count = 0;
    DWORD last_error = 0;

    while (GetTickCount64() < deadline) {
      winrt::file_handle pipe {
        CreateFileW(fullPipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr)
      };

      if (pipe) {
        if (retry_count > 0) {
          BOOST_LOG(debug) << "CreateFileW succeeded after " << retry_count << " retries in " 
                          << (GetTickCount64() - start_time) << "ms";
        }
        return pipe;  // success
      }

      const DWORD err = GetLastError();
      last_error = err;
      retry_count++;
      
      if (err == ERROR_PIPE_BUSY) {
        if (retry_count == 1 || retry_count % 20 == 0) {
          BOOST_LOG(debug) << "Pipe busy, waiting... (retry " << retry_count << ")";
        }
        WaitNamedPipeW(fullPipeName.c_str(), 250);
        continue;
      }
      if (err == ERROR_FILE_NOT_FOUND) {
        if (retry_count == 1) {
          BOOST_LOG(debug) << "Pipe not found, waiting for server to create it...";
        } else if (retry_count % 40 == 0) {
          BOOST_LOG(warning) << "Still waiting for pipe after " << (GetTickCount64() - start_time) 
                           << "ms (" << retry_count << " retries)";
        }
        Sleep(50);
        continue;
      }

      BOOST_LOG(error) << "CreateFileW failed (" << err << ")";
      break;  // unrecoverable error
    }

    if (last_error == ERROR_FILE_NOT_FOUND) {
      BOOST_LOG(error) << "CreateFileW timed out after " << (GetTickCount64() - start_time) 
                      << "ms waiting for pipe server (ERROR_FILE_NOT_FOUND). "
                      << "The helper process may not be running or failed to create the pipe.";
    }

    return {};  // invalid handle
  }

  AnonymousPipeFactory::AnonymousPipeFactory() = default;

  std::unique_ptr<INamedPipe> AnonymousPipeFactory::create_server(const std::string &pipeName) {
    auto first_pipe = _pipe_factory.create_server(pipeName);
    if (!first_pipe) {
      return nullptr;
    }
    return handshake_server(std::move(first_pipe));
  }

  std::unique_ptr<INamedPipe> AnonymousPipeFactory::create_client(const std::string &pipeName) {
    auto first_pipe = _pipe_factory.create_client(pipeName);
    if (!first_pipe) {
      return nullptr;
    }
    return handshake_client(std::move(first_pipe));
  }

  void AnonymousPipeFactory::set_security_descriptor_builder(NamedPipeFactory::SecurityDescriptorBuilder builder) {
    _pipe_factory.set_security_descriptor_builder(std::move(builder));
  }

  class PrefetchedPipe: public INamedPipe {
  public:
    PrefetchedPipe(std::unique_ptr<INamedPipe> inner, std::vector<uint8_t> prebuffer):
        _inner(std::move(inner)),
        _buffer(std::move(prebuffer)) {}

    bool send(std::span<const uint8_t> bytes, int timeout_ms) override {
      return _inner && _inner->send(bytes, timeout_ms);
    }

    PipeResult receive(std::span<uint8_t> dst, size_t &bytesRead, int timeout_ms) override {
      if (!_inner) {
        bytesRead = 0;
        return PipeResult::Disconnected;
      }

      if (_cursor < _buffer.size()) {
        const size_t remaining = _buffer.size() - _cursor;
        const size_t to_copy = std::min(remaining, dst.size());
        std::memcpy(dst.data(), _buffer.data() + _cursor, to_copy);
        _cursor += to_copy;
        bytesRead = to_copy;
        if (_cursor < _buffer.size() || to_copy > 0) {
          return PipeResult::Success;
        }
      }

      return _inner->receive(dst, bytesRead, timeout_ms);
    }

    PipeResult receive_latest(std::span<uint8_t> dst, size_t &bytesRead, int timeout_ms) override {
      if (!_inner) {
        bytesRead = 0;
        return PipeResult::Disconnected;
      }

      if (_cursor < _buffer.size()) {
        const size_t remaining = _buffer.size() - _cursor;
        const size_t to_copy = std::min(remaining, dst.size());
        const size_t start = _buffer.size() - to_copy;
        std::memcpy(dst.data(), _buffer.data() + start, to_copy);
        _cursor = _buffer.size();
        bytesRead = to_copy;
        return PipeResult::Success;
      }

      return _inner->receive_latest(dst, bytesRead, timeout_ms);
    }

    void wait_for_client_connection(int milliseconds) override {
      if (_inner) {
        _inner->wait_for_client_connection(milliseconds);
      }
    }

    void disconnect() override {
      if (_inner) {
        _inner->disconnect();
      }
    }

    bool is_connected() override {
      return _inner && _inner->is_connected();
    }

  private:
    std::unique_ptr<INamedPipe> _inner;
    std::vector<uint8_t> _buffer;
    size_t _cursor {0};
  };

  std::unique_ptr<INamedPipe> AnonymousPipeFactory::handshake_server(std::unique_ptr<INamedPipe> pipe) {
    std::string pipe_name = generate_guid();

    if (!send_handshake_message(pipe, pipe_name)) {
      return nullptr;
    }

    std::vector<uint8_t> buffered;
    const auto ack_result = wait_for_handshake_ack(pipe, buffered);

    if (ack_result == HandshakeAckResult::Failed) {
      return nullptr;
    }

    if (ack_result == HandshakeAckResult::Fallback) {
      BOOST_LOG(warning)
        << "Anonymous handshake: ACK not received; falling back to legacy named pipe communication.";
      return std::make_unique<PrefetchedPipe>(std::move(pipe), std::move(buffered));
    }

    auto dataPipe = _pipe_factory.create_server(pipe_name);
    if (dataPipe) {
      dataPipe->wait_for_client_connection(0);
    }

    if (!buffered.empty()) {
      BOOST_LOG(warning) << "Discarding " << buffered.size()
                         << " byte(s) received alongside handshake ACK.";
    }

    pipe->disconnect();
    return dataPipe;
  }

  bool AnonymousPipeFactory::send_handshake_message(std::unique_ptr<INamedPipe> &pipe, const std::string &pipe_name) const {
    std::wstring wpipe_name = utf8_to_wide(pipe_name);

    AnonConnectMsg message {};
    wcsncpy_s(message.pipe_name, wpipe_name.c_str(), _TRUNCATE);

    auto bytes = std::span<const uint8_t>(
      reinterpret_cast<const uint8_t *>(&message),
      sizeof(message)
    );

    // Wait for control client to connect (match Playnite connector's 10s timeout with margin)
    pipe->wait_for_client_connection(15000);

    if (!pipe->is_connected()) {
      BOOST_LOG(error) << "Client did not connect to pipe instance within the specified timeout. Disconnecting server pipe.";
      pipe->disconnect();
      return false;
    }
    BOOST_LOG(debug) << "Anonymous handshake: control client connected; sending data-pipe name (" << sizeof(message) << " bytes)";
    if (!pipe->send(bytes, 5000)) {
      BOOST_LOG(error) << "Failed to send handshake message to client";
      pipe->disconnect();
      return false;
    }
    BOOST_LOG(debug) << "Anonymous handshake: control message sent";

    return true;
  }

  AnonymousPipeFactory::HandshakeAckResult AnonymousPipeFactory::wait_for_handshake_ack(
    std::unique_ptr<INamedPipe> &pipe,
    std::vector<uint8_t> &buffered
  ) const {
    using enum platf::dxgi::PipeResult;
    buffered.clear();

    std::array<uint8_t, 64> chunk {};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1200);

    while (std::chrono::steady_clock::now() < deadline) {
      size_t bytes_read = 0;
      const PipeResult result = pipe->receive(chunk, bytes_read, 1000);

      if (result == Success) {
        if (bytes_read == 0) {
          continue;
        }
        buffered.insert(buffered.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(bytes_read));
        auto it = std::find(buffered.begin(), buffered.end(), ACK_MSG);
        if (it != buffered.end()) {
          const auto offset = static_cast<size_t>(std::distance(buffered.begin(), it));
          BOOST_LOG(debug) << "Handshake ACK received (offset=" << offset
                           << ", buffered=" << buffered.size() << ')';
          buffered.erase(buffered.begin(), it + 1);
          return HandshakeAckResult::Acked;
        }

        if (buffered.size() >= sizeof(uint32_t)) {
          uint32_t framed_len = 0;
          std::memcpy(&framed_len, buffered.data(), sizeof(framed_len));
          constexpr uint32_t kMaxFrameLen = 2 * 1024 * 1024;  // matches FramedPipe guard
          if (framed_len > 0 && framed_len <= kMaxFrameLen) {
            BOOST_LOG(debug) << "Handshake ACK missing; framed payload detected (len=" << framed_len
                             << "). Falling back to control pipe.";
            return HandshakeAckResult::Fallback;
          }
        }

        if (buffered.size() > 65536) {
          BOOST_LOG(warning) << "Handshake ACK wait buffer exceeded 64KiB; assuming legacy fallback.";
          return HandshakeAckResult::Fallback;
        }
        continue;
      }

      if (result == BrokenPipe || result == Error || result == Disconnected) {
        BOOST_LOG(error) << "Pipe error during handshake ACK wait";
        return HandshakeAckResult::Failed;
      }
    }

    if (!buffered.empty()) {
      BOOST_LOG(warning) << "Handshake ACK not observed; treating " << buffered.size()
                         << " buffered byte(s) as legacy pipeline data.";
      return HandshakeAckResult::Fallback;
    }

    BOOST_LOG(debug) << "Handshake ACK not observed within deadline; using control pipe directly.";
    return HandshakeAckResult::Fallback;
  }

  std::unique_ptr<INamedPipe> AnonymousPipeFactory::handshake_client(std::unique_ptr<INamedPipe> pipe) {
    AnonConnectMsg msg {};
    std::vector<uint8_t> prefetched;

    const auto msg_result = receive_handshake_message(pipe, msg, prefetched);
    if (msg_result == HandshakeMessageResult::Failed) {
      return nullptr;
    }

    if (msg_result == HandshakeMessageResult::Inline) {
      BOOST_LOG(debug) << "Anonymous handshake: falling back to control pipe";
      return std::make_unique<PrefetchedPipe>(std::move(pipe), std::move(prefetched));
    }

    if (!send_handshake_ack(pipe)) {
      return nullptr;
    }

    std::wstring wpipeName(msg.pipe_name);
    std::string pipeNameStr = wide_to_utf8(wpipeName);
    pipe->disconnect();

    return connect_to_data_pipe(pipeNameStr);
  }

  AnonymousPipeFactory::HandshakeMessageResult AnonymousPipeFactory::receive_handshake_message(
    std::unique_ptr<INamedPipe> &pipe,
    AnonConnectMsg &msg,
    std::vector<uint8_t> &prefetched
  ) const {
    using enum platf::dxgi::PipeResult;
    prefetched.clear();

    std::array<uint8_t, 256> chunk {};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    int zero_reads = 0;

    constexpr uint32_t kMaxFrameLen = 2 * 1024 * 1024;

    while (std::chrono::steady_clock::now() < deadline) {
      size_t bytes_read = 0;
      const PipeResult result = pipe->receive(chunk, bytes_read, 200);

      if (result == Success) {
        if (bytes_read == 0) {
          ++zero_reads;
          if (zero_reads >= 5) {  // ~1s of empty reads at 200ms interval
            BOOST_LOG(info) << "Handshake message missing; assuming inline control pipe.";
            return HandshakeMessageResult::Inline;
          }
          continue;
        }

        zero_reads = 0;
        prefetched.insert(prefetched.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(bytes_read));

        if (prefetched.size() >= sizeof(AnonConnectMsg)) {
          std::memcpy(&msg, prefetched.data(), sizeof(AnonConnectMsg));
          prefetched.erase(prefetched.begin(), prefetched.begin() + static_cast<std::ptrdiff_t>(sizeof(AnonConnectMsg)));
          return HandshakeMessageResult::Message;
        }

        if (prefetched.size() >= sizeof(uint32_t)) {
          uint32_t framed_len = 0;
          std::memcpy(&framed_len, prefetched.data(), sizeof(framed_len));
          if (framed_len > 0 && framed_len <= kMaxFrameLen) {
            BOOST_LOG(info) << "Handshake message absent; detected framed payload len=" << framed_len << '.';
            return HandshakeMessageResult::Inline;
          }
        }
        continue;
      }

      if (result == Timeout) {
        continue;
      }

      if (result == BrokenPipe || result == Error || result == Disconnected) {
        BOOST_LOG(error) << "Pipe error during handshake message receive";
        return HandshakeMessageResult::Failed;
      }
    }

    if (!prefetched.empty()) {
      BOOST_LOG(info) << "Handshake message timed out with " << prefetched.size()
                      << " buffered byte(s); using inline control pipe.";
      return HandshakeMessageResult::Inline;
    }

    BOOST_LOG(error) << "Did not receive handshake message in time.";
    pipe->disconnect();
    return HandshakeMessageResult::Failed;
  }

  bool AnonymousPipeFactory::send_handshake_ack(std::unique_ptr<INamedPipe> &pipe) const {
    uint8_t ack = ACK_MSG;
    if (!pipe->send(std::span<const uint8_t>(&ack, 1), 5000)) {
      BOOST_LOG(error) << "Failed to send handshake ACK to server";
      pipe->disconnect();
      return false;
    }
    BOOST_LOG(debug) << "Anonymous handshake: client sent ACK";

    return true;
  }

  std::unique_ptr<INamedPipe> AnonymousPipeFactory::connect_to_data_pipe(const std::string &pipeNameStr) {
    std::unique_ptr<INamedPipe> data_pipe = nullptr;
    auto retry_start = std::chrono::steady_clock::now();
    const auto retry_timeout = std::chrono::seconds(5);

    while (std::chrono::steady_clock::now() - retry_start < retry_timeout) {
      data_pipe = _pipe_factory.create_client(pipeNameStr);
      if (data_pipe) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!data_pipe) {
      BOOST_LOG(error) << "Failed to connect to data pipe after retries";
      return nullptr;
    }

    return data_pipe;
  }

  WinPipe::WinPipe(winrt::file_handle pipe, bool isServer):
      _pipe(std::move(pipe)),
      _is_server(isServer) {
    if (_pipe) {
      _connected.store(!_is_server, std::memory_order_release);
    }
  }

  WinPipe::~WinPipe() {
    try {
      WinPipe::disconnect();
    } catch (const std::exception &ex) {
      BOOST_LOG(error) << "Exception in WinPipe destructor: " << ex.what();
    } catch (...) {
      BOOST_LOG(error) << "Unknown exception in WinPipe destructor.";
    }
  }

  bool WinPipe::send(std::span<const uint8_t> bytes, int timeout_ms) {
    if (!_connected.load(std::memory_order_acquire) || !_pipe) {
      return false;
    }

    io_context ctx;
    if (!ctx.is_valid()) {
      BOOST_LOG(error) << "Failed to create I/O context for send operation, error=" << GetLastError();
      return false;
    }

    DWORD bytesWritten = 0;
    if (BOOL result = WriteFile(_pipe.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &bytesWritten, ctx.get()); !result) {
      return handle_send_error(ctx, timeout_ms, bytesWritten);
    }

    if (bytesWritten != bytes.size()) {
      BOOST_LOG(error) << "WriteFile wrote " << bytesWritten << " bytes, expected " << bytes.size();
      return false;
    }
    return true;
  }

  bool WinPipe::handle_send_error(io_context &ctx, int timeout_ms, DWORD &bytesWritten) {
    DWORD err = GetLastError();
    if (err == ERROR_IO_PENDING) {
      return handle_pending_send_operation(ctx, timeout_ms, bytesWritten);
    } else if (err == ERROR_BROKEN_PIPE) {
      BOOST_LOG(warning) << "Pipe broken during WriteFile (ERROR_BROKEN_PIPE)";
      return false;
    } else {
      BOOST_LOG(error) << "WriteFile failed (" << err << ") in WinPipe::send";
      return false;
    }
  }

  bool WinPipe::handle_pending_send_operation(io_context &ctx, int timeout_ms, DWORD &bytesWritten) {
    DWORD waitResult = WaitForSingleObject(ctx.event(), timeout_ms);

    if (waitResult == WAIT_OBJECT_0) {
      if (!GetOverlappedResult(_pipe.get(), ctx.get(), &bytesWritten, FALSE)) {
        DWORD err = GetLastError();
        if (err != ERROR_OPERATION_ABORTED) {
          BOOST_LOG(error) << "GetOverlappedResult failed in send, error=" << err;
        }
        return false;
      }
      return true;
    } else if (waitResult == WAIT_TIMEOUT) {
      BOOST_LOG(warning) << "Send operation timed out after " << timeout_ms << "ms";
      CancelIoEx(_pipe.get(), ctx.get());
      DWORD transferred = 0;
      GetOverlappedResult(_pipe.get(), ctx.get(), &transferred, TRUE);
      return false;
    } else {
      BOOST_LOG(error) << "WaitForSingleObject failed in send, result=" << waitResult << ", error=" << GetLastError();
      return false;
    }
  }

  PipeResult WinPipe::receive(std::span<uint8_t> dst, size_t &bytesRead, int timeout_ms) {
    bytesRead = 0;
    if (!_connected.load(std::memory_order_acquire)) {
      return PipeResult::Disconnected;
    }

    io_context ctx;
    if (!ctx.is_valid()) {
      BOOST_LOG(error) << "Failed to create I/O context for receive operation, error=" << GetLastError();
      return PipeResult::Error;
    }

    DWORD bytesReadWin = 0;
    BOOL result = ReadFile(_pipe.get(), dst.data(), static_cast<DWORD>(dst.size()), &bytesReadWin, ctx.get());

    if (result) {
      bytesRead = static_cast<size_t>(bytesReadWin);
      return PipeResult::Success;
    } else {
      return handle_receive_error(ctx, timeout_ms, dst, bytesRead);
    }
  }

  PipeResult WinPipe::receive_latest(std::span<uint8_t> dst, size_t &bytesRead, int timeout_ms) {
    PipeResult result = receive(dst, bytesRead, timeout_ms);
    if (result != PipeResult::Success) {
      return result;
    }

    size_t lastBytesRead = bytesRead;
    PipeResult lastResult = result;
    while (true) {
      size_t tempBytesRead = 0;
      PipeResult next = receive(dst, tempBytesRead, 0);
      if (next == PipeResult::Success) {
        lastBytesRead = tempBytesRead;
        lastResult = next;
      } else if (next == PipeResult::Timeout) {
        break;
      } else {
        // If we get an error, broken pipe, or disconnected, return immediately
        return next;
      }
    }
    bytesRead = lastBytesRead;
    return lastResult;
  }

  PipeResult WinPipe::handle_receive_error(io_context &ctx, int timeout_ms, std::span<uint8_t> dst, size_t &bytesRead) {
    DWORD err = GetLastError();
    if (err == ERROR_IO_PENDING) {
      return handle_pending_receive_operation(ctx, timeout_ms, dst, bytesRead);
    } else if (err == ERROR_BROKEN_PIPE) {
      BOOST_LOG(warning) << "Pipe broken during ReadFile (ERROR_BROKEN_PIPE)";
      // Reflect disconnected state immediately so higher layers don't think we're still connected
      _connected.store(false, std::memory_order_release);
      return PipeResult::BrokenPipe;
    } else {
      BOOST_LOG(error) << "ReadFile failed in receive, error=" << err;
      return PipeResult::Error;
    }
  }

  PipeResult WinPipe::handle_pending_receive_operation(io_context &ctx, int timeout_ms, std::span<uint8_t> dst, size_t &bytesRead) {
    using enum platf::dxgi::PipeResult;
    DWORD waitResult = WaitForSingleObject(ctx.event(), timeout_ms);
    DWORD bytesReadWin = 0;

    if (waitResult == WAIT_OBJECT_0) {
      if (GetOverlappedResult(_pipe.get(), ctx.get(), &bytesReadWin, FALSE)) {
        bytesRead = static_cast<size_t>(bytesReadWin);
        return Success;
      } else {
        DWORD overlappedErr = GetLastError();
        if (overlappedErr == ERROR_BROKEN_PIPE) {
          BOOST_LOG(warning) << "IPC between Sunshine was severed, did the capture process crash?";
          _connected.store(false, std::memory_order_release);
          return BrokenPipe;
        }
        if (overlappedErr == ERROR_OPERATION_ABORTED) {
          return Disconnected;
        }
        BOOST_LOG(error) << "GetOverlappedResult failed in receive, error=" << overlappedErr;
        return Error;
      }
    } else if (waitResult == WAIT_TIMEOUT) {
      CancelIoEx(_pipe.get(), ctx.get());
      DWORD transferred = 0;
      GetOverlappedResult(_pipe.get(), ctx.get(), &transferred, TRUE);
      return Timeout;
    } else {
      BOOST_LOG(error) << "WinPipe::receive() wait failed, result=" << waitResult << ", error=" << GetLastError();
      return Error;
    }
  }

  void WinPipe::disconnect() {
    if (_pipe) {
      CancelIoEx(_pipe.get(), nullptr);
      if (_is_server) {
        DisconnectNamedPipe(_pipe.get());
      }
      _pipe.close();
    }
    _connected.store(false, std::memory_order_release);
  }

  void WinPipe::wait_for_client_connection(int milliseconds) {
    if (!_pipe) {
      return;
    }

    if (_is_server) {
      // For server pipes, use ConnectNamedPipe with proper overlapped I/O
      connect_server_pipe(milliseconds);
    } else {
      // For client handles created with CreateFileW, the connection already exists
      // _connected is set in constructor
    }
  }

  void WinPipe::connect_server_pipe(int milliseconds) {
    io_context ctx;
    if (!ctx.is_valid()) {
      BOOST_LOG(error) << "Failed to create I/O context for connection, error=" << GetLastError();
      return;
    }

    if (BOOL result = ConnectNamedPipe(_pipe.get(), ctx.get()); result) {
      _connected = true;
      BOOST_LOG(debug) << "NamedPipe server: ConnectNamedPipe completed synchronously";
      return;
    }

    DWORD err = GetLastError();
    if (err == ERROR_PIPE_CONNECTED) {
      // Client already connected
      _connected = true;
      BOOST_LOG(debug) << "NamedPipe server: client already connected (ERROR_PIPE_CONNECTED)";
      return;
    }

    if (err == ERROR_IO_PENDING) {
      handle_pending_connection(ctx, milliseconds);
      return;
    }

    BOOST_LOG(error) << "ConnectNamedPipe failed, error=" << err;
  }

  void WinPipe::handle_pending_connection(io_context &ctx, int milliseconds) {
    // Wait for the connection to complete
    DWORD waitResult = WaitForSingleObject(ctx.event(), milliseconds > 0 ? milliseconds : 5000);  // Use param or default 5s
    if (waitResult == WAIT_OBJECT_0) {
      DWORD transferred = 0;
      if (GetOverlappedResult(_pipe.get(), ctx.get(), &transferred, FALSE)) {
        _connected = true;
        BOOST_LOG(debug) << "NamedPipe server: overlapped ConnectNamedPipe completed";
      } else {
        DWORD err = GetLastError();
        if (err != ERROR_OPERATION_ABORTED) {
          BOOST_LOG(error) << "GetOverlappedResult failed in connect, error=" << err;
        }
      }
    } else if (waitResult == WAIT_TIMEOUT) {
      BOOST_LOG(error) << "ConnectNamedPipe timeout after " << (milliseconds > 0 ? milliseconds : 5000) << "ms";
      CancelIoEx(_pipe.get(), ctx.get());
      // Wait for cancellation to complete to ensure OVERLAPPED structure safety
      DWORD transferred = 0;
      GetOverlappedResult(_pipe.get(), ctx.get(), &transferred, TRUE);
    } else {
      BOOST_LOG(error) << "ConnectNamedPipe wait failed, waitResult=" << waitResult << ", error=" << GetLastError();
    }
  }

  bool WinPipe::is_connected() {
    return _connected.load(std::memory_order_acquire);
  }

  void WinPipe::flush_buffers() {
    if (_pipe) {
      FlushFileBuffers(_pipe.get());
    }
  }

  bool WinPipe::write_blocking(std::span<const uint8_t> bytes) {
    if (!_pipe || !_connected.load(std::memory_order_acquire)) {
      return false;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(_pipe.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    if (!ok) {
      DWORD err = GetLastError();
      BOOST_LOG(error) << "WinPipe::write_blocking failed, error=" << err;
      return false;
    }
    return written == bytes.size();
  }

  bool WinPipe::get_client_process_id(DWORD &pid) {
    pid = 0;
    if (!_is_server || !_pipe) {
      return false;
    }
    ULONG upid = 0;
    if (!GetNamedPipeClientProcessId(_pipe.get(), &upid)) {
      return false;
    }
    pid = static_cast<DWORD>(upid);
    return true;
  }

  bool WinPipe::get_client_user_sid_string(std::wstring &sid_str) {
    sid_str.clear();
    if (!_is_server || !_pipe) {
      return false;
    }

    ULONG upid = 0;
    if (!GetNamedPipeClientProcessId(_pipe.get(), &upid)) {
      return false;
    }

    winrt::handle hProc {OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(upid))};
    if (!hProc) {
      return false;
    }

    HANDLE hToken = nullptr;
    if (!OpenProcessToken(hProc.get(), TOKEN_QUERY, &hToken)) {
      return false;
    }
    auto token_guard = util::fail_guard([&]() {
      if (hToken) {
        CloseHandle(hToken);
      }
    });

    DWORD len = 0;
    GetTokenInformation(hToken, TokenUser, nullptr, 0, &len);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
      return false;
    }
    auto buf = std::make_unique<uint8_t[]>(len);
    auto tu = reinterpret_cast<TOKEN_USER *>(buf.get());
    if (!GetTokenInformation(hToken, TokenUser, tu, len, &len)) {
      return false;
    }
    LPWSTR sidW = nullptr;
    if (!ConvertSidToStringSidW(tu->User.Sid, &sidW)) {
      return false;
    }
    sid_str.assign(sidW);
    LocalFree(sidW);
    return true;
  }

  AsyncNamedPipe::AsyncNamedPipe(std::unique_ptr<INamedPipe> pipe):
      _pipe(std::move(pipe)) {
  }

  AsyncNamedPipe::~AsyncNamedPipe() {
    stop();
  }

  bool AsyncNamedPipe::start(const MessageCallback &onMessage, const ErrorCallback &onError, const BrokenPipeCallback &onBrokenPipe) {
    if (_running.load(std::memory_order_acquire)) {
      return false;  // Already running
    }

    if (!_pipe) {
      if (onError) {
        onError("No pipe available - failed to create pipe");
      }
      return false;
    }

    _onMessage = onMessage;
    _onError = onError;
    _onBrokenPipe = onBrokenPipe;

    _running.store(true, std::memory_order_release);
    _worker = std::jthread(&AsyncNamedPipe::worker_thread, this);
    return true;
  }

  void AsyncNamedPipe::stop() {
    _running.store(false, std::memory_order_release);

    // Cancel any pending I/O operations to unblock the worker thread
    if (_pipe) {
      _pipe->disconnect();
    }

    if (_worker.joinable()) {
      _worker.join();
    }
  }

  void AsyncNamedPipe::send(std::span<const uint8_t> message) {
    safe_execute_operation("send", [this, message]() {
      if (_pipe && _pipe->is_connected() && !_pipe->send(message, 5000)) {  // 5 second timeout for async sends
        BOOST_LOG(warning) << "Failed to send message through AsyncNamedPipe (timeout or error)";
      }
    });
  }

  void AsyncNamedPipe::wait_for_client_connection(int milliseconds) {
    _pipe->wait_for_client_connection(milliseconds);
  }

  bool AsyncNamedPipe::is_connected() const {
    return _pipe && _pipe->is_connected();
  }

  void AsyncNamedPipe::safe_execute_operation(const std::string &operation_name, const std::function<void()> &operation) const noexcept {
    if (!operation) {
      return;
    }

    try {
      operation();
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "AsyncNamedPipe: Exception in " << operation_name << ": " << e.what();
    } catch (...) {
      BOOST_LOG(error) << "AsyncNamedPipe: Unknown exception in " << operation_name;
    }
  }

  void AsyncNamedPipe::worker_thread() noexcept {
    safe_execute_operation("worker_thread", [this]() {
      if (!establish_connection()) {
        return;
      }

      run_message_loop();
    });
  }

  void AsyncNamedPipe::run_message_loop() {
    using namespace std::chrono_literals;
    using enum platf::dxgi::PipeResult;

    // No need to add a sleep here: _pipe->receive() blocks until data arrives or times out, so messages are delivered to callbacks as soon as they are available.
    while (_running.load(std::memory_order_acquire)) {
      size_t bytesRead = 0;
      PipeResult res = _pipe->receive(_buffer, bytesRead, 1000);

      // Fast cancel – bail out even before decoding res
      if (!_running.load(std::memory_order_acquire)) {
        break;
      }

      switch (res) {
        case Success:
          {
            if (bytesRead == 0) {  // remote closed
              // Ensure connection state reflects closure so outer loops can exit
              if (_pipe) {
                _pipe->disconnect();
              }
              return;
            }
            // Create span from only the valid portion of the buffer
            std::span<const uint8_t> message(_buffer.data(), bytesRead);
            process_message(message);
            break;  // keep looping
          }

        case Timeout:
          break;  // nothing to do

        case BrokenPipe:
          // Mark pipe as disconnected so the helper's outer loop can accept a new client
          if (_pipe) {
            _pipe->disconnect();
          }
          safe_execute_operation("brokenPipe callback", _onBrokenPipe);
          return;  // terminate

        case Error:
        case Disconnected:
        default:
          if (_pipe) {
            _pipe->disconnect();
          }
          return;  // terminate
      }
    }
  }

  bool AsyncNamedPipe::establish_connection() {
    // For server pipes, we need to wait for a client connection first
    if (!_pipe || _pipe->is_connected()) {
      return true;
    }

    _pipe->wait_for_client_connection(5000);  // Wait up to 5 seconds for connection
    if (!_pipe->is_connected()) {
      BOOST_LOG(error) << "AsyncNamedPipe: Failed to establish connection within timeout";
      safe_execute_operation("error callback", [this]() {
        if (_onError) {
          _onError("Failed to establish connection within timeout");
        }
      });
      return false;
    }
    return _pipe->is_connected();
  }

  void AsyncNamedPipe::process_message(std::span<const uint8_t> bytes) const {
    if (!_onMessage) {
      return;
    }

    safe_execute_operation("message callback", [this, bytes]() {
      _onMessage(bytes);
    });
  }

  FramedPipe::FramedPipe(std::unique_ptr<INamedPipe> inner):
      _inner(std::move(inner)) {}

  bool FramedPipe::send(std::span<const uint8_t> bytes, int timeout_ms) {
    const uint32_t len = static_cast<uint32_t>(bytes.size());
    std::array<uint8_t, 4> hdr {};
    std::memcpy(hdr.data(), &len, sizeof(len));
    std::vector<uint8_t> out;
    out.reserve(4 + bytes.size());
    out.insert(out.end(), hdr.begin(), hdr.end());
    out.insert(out.end(), bytes.begin(), bytes.end());
    return _inner && _inner->send(out, timeout_ms);
  }

  bool FramedPipe::try_decode_one_frame(std::span<uint8_t> dst, size_t &bytesRead) {
    bytesRead = 0;
    if (_rxbuf.size() < 4) {
      return false;
    }
    uint32_t len = 0;
    std::memcpy(&len, _rxbuf.data(), 4);
    if (len > 2 * 1024 * 1024) {
      return false;
    }
    if (_rxbuf.size() < 4u + len) {
      return false;
    }
    if (dst.size() < len) {
      return false;
    }
    std::memcpy(dst.data(), _rxbuf.data() + 4, len);
    bytesRead = len;
    _rxbuf.erase(_rxbuf.begin(), _rxbuf.begin() + 4 + len);
    return true;
  }

  PipeResult FramedPipe::receive(std::span<uint8_t> dst, size_t &bytesRead, int timeout_ms) {
    bytesRead = 0;
    if (!_inner) {
      return PipeResult::Disconnected;
    }
    if (try_decode_one_frame(dst, bytesRead)) {
      return PipeResult::Success;
    }

    while (true) {
      std::array<uint8_t, 65536> tmp {};
      size_t n = 0;
      auto res = _inner->receive(tmp, n, timeout_ms);
      if (res == PipeResult::Success) {
        if (n == 0) {
          return PipeResult::Disconnected;
        }
        _rxbuf.insert(_rxbuf.end(), tmp.begin(), tmp.begin() + static_cast<long>(n));
        if (try_decode_one_frame(dst, bytesRead)) {
          return PipeResult::Success;
        }
        timeout_ms = 0;
        continue;
      }
      if (res == PipeResult::Timeout) {
        if (try_decode_one_frame(dst, bytesRead)) {
          return PipeResult::Success;
        }
        return PipeResult::Timeout;
      }
      return res;
    }
  }

  PipeResult FramedPipe::receive_latest(std::span<uint8_t> dst, size_t &bytesRead, int timeout_ms) {
    size_t lastBytes = 0;
    PipeResult last = receive(dst, lastBytes, timeout_ms);
    if (last != PipeResult::Success) {
      bytesRead = 0;
      return last;
    }
    for (;;) {
      size_t n = 0;
      auto res = receive(dst, n, 0);
      if (res == PipeResult::Success) {
        lastBytes = n;
        continue;
      }
      if (res == PipeResult::Timeout) {
        break;
      }
      bytesRead = lastBytes;
      return res;
    }
    bytesRead = lastBytes;
    return PipeResult::Success;
  }

  void FramedPipe::wait_for_client_connection(int milliseconds) {
    if (_inner) {
      _inner->wait_for_client_connection(milliseconds);
    }
  }

  void FramedPipe::disconnect() {
    if (_inner) {
      _inner->disconnect();
    }
    _rxbuf.clear();
  }

  bool FramedPipe::is_connected() {
    return _inner && _inner->is_connected();
  }

  SelfHealingPipe::SelfHealingPipe(Creator creator):
      _creator(std::move(creator)) {}

  bool SelfHealingPipe::ensure_connected() {
    if (_inner && _inner->is_connected()) {
      return true;
    }
    reconnect();
    return _inner && _inner->is_connected();
  }

  void SelfHealingPipe::reconnect() {
    try {
      _inner = _creator ? _creator() : nullptr;
    } catch (...) {
      _inner.reset();
    }
  }

  bool SelfHealingPipe::send(std::span<const uint8_t> bytes, int timeout_ms) {
    if (!ensure_connected()) {
      return false;
    }
    if (_inner->send(bytes, timeout_ms)) {
      return true;
    }
    reconnect();
    return _inner && _inner->send(bytes, timeout_ms);
  }

  PipeResult SelfHealingPipe::receive(std::span<uint8_t> dst, size_t &bytesRead, int timeout_ms) {
    bytesRead = 0;
    if (!ensure_connected()) {
      return PipeResult::Disconnected;
    }
    auto r = _inner->receive(dst, bytesRead, timeout_ms);
    if (r == PipeResult::BrokenPipe || r == PipeResult::Disconnected || r == PipeResult::Error) {
      reconnect();
      if (!_inner) {
        return r;
      }
      return _inner->receive(dst, bytesRead, timeout_ms);
    }
    return r;
  }

  PipeResult SelfHealingPipe::receive_latest(std::span<uint8_t> dst, size_t &bytesRead, int timeout_ms) {
    bytesRead = 0;
    if (!ensure_connected()) {
      return PipeResult::Disconnected;
    }
    auto r = _inner->receive_latest(dst, bytesRead, timeout_ms);
    if (r == PipeResult::BrokenPipe || r == PipeResult::Disconnected || r == PipeResult::Error) {
      reconnect();
      if (!_inner) {
        return r;
      }
      return _inner->receive_latest(dst, bytesRead, timeout_ms);
    }
    return r;
  }

  void SelfHealingPipe::wait_for_client_connection(int milliseconds) {
    if (!ensure_connected()) {
      return;
    }
    _inner->wait_for_client_connection(milliseconds);
  }

  void SelfHealingPipe::disconnect() {
    if (_inner) {
      _inner->disconnect();
    }
  }

  bool SelfHealingPipe::is_connected() {
    return _inner && _inner->is_connected();
  }
}  // namespace platf::dxgi
