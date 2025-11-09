/**
 * @file src/platform/windows/ipc/display_settings_client.cpp
 */
#ifdef _WIN32

  // standard
  #include <algorithm>
  #include <array>
  #include <chrono>
  #include <cstdint>
  #include <mutex>
  #include <optional>
  #include <string>
  #include <vector>

  // local
  #include "display_settings_client.h"
  #include "src/globals.h"
  #include "src/logging.h"
  #include "src/platform/windows/ipc/pipes.h"

namespace platf::display_helper_client {

  namespace {
    constexpr int kConnectTimeoutMs = 2000;
    constexpr int kSendTimeoutMs = 5000;
    constexpr int kShutdownIpcTimeoutMs = 500;
    constexpr int kApplyResultTimeoutMs = 20000;

    bool shutdown_requested() {
      if (!mail::man) {
        return false;
      }
      try {
        auto shutdown_event = mail::man->event<bool>(mail::shutdown);
        return shutdown_event && shutdown_event->peek();
      } catch (...) {
        return false;
      }
    }

    int effective_connect_timeout() {
      return shutdown_requested() ? kShutdownIpcTimeoutMs : kConnectTimeoutMs;
    }

    int effective_send_timeout() {
      return shutdown_requested() ? kShutdownIpcTimeoutMs : kSendTimeoutMs;
    }

  }

  /**
   * @brief IPC message types used by the display settings helper protocol.
   */
  enum class MsgType : uint8_t {
    Apply = 1,  ///< Apply display settings from JSON payload.
    Revert = 2,  ///< Revert display settings to the previous state.
    Reset = 3,  ///< Reset helper persistence/state (if supported).
    ExportGolden = 4,  ///< Export current OS settings as golden snapshot
    Blacklist = 5,  ///< Blacklist a display device_id from topology exports (string payload).
    ApplyResult = 6,  ///< Helper acknowledgement for APPLY (payload: [u8 success][optional message...]).
    Ping = 0xFE,  ///< Health check message; expects a response.
    Stop = 0xFF  ///< Request helper process to terminate gracefully.
  };

  namespace {
    std::optional<bool> wait_for_apply_result_locked(platf::dxgi::INamedPipe &pipe) {
      using namespace std::chrono;

      const auto deadline = steady_clock::now() + milliseconds(kApplyResultTimeoutMs);
      std::array<uint8_t, 2048> buffer {};

      while (steady_clock::now() < deadline) {
        const auto now = steady_clock::now();
        auto remaining = duration_cast<milliseconds>(deadline - now);
        if (remaining.count() < 0) {
          remaining = milliseconds(0);
        }
        int timeout_ms = static_cast<int>(std::max<long long>(remaining.count(), 100LL));
        size_t bytes_read = 0;
        auto result = pipe.receive(buffer, bytes_read, timeout_ms);

        if (result == platf::dxgi::PipeResult::Timeout) {
          continue;
        }
        if (result != platf::dxgi::PipeResult::Success) {
          BOOST_LOG(error) << "Display helper IPC: failed waiting for APPLY result (pipe error)";
          return std::nullopt;
        }
        if (bytes_read == 0) {
          BOOST_LOG(error) << "Display helper IPC: connection closed while waiting for APPLY result";
          return std::nullopt;
        }

        const uint8_t msg_type = buffer[0];
        if (msg_type == static_cast<uint8_t>(MsgType::ApplyResult)) {
          bool success = bytes_read >= 2 && buffer[1] != 0;
          if (!success && bytes_read > 2) {
            std::string helper_msg(reinterpret_cast<const char *>(buffer.data() + 2), reinterpret_cast<const char *>(buffer.data() + bytes_read));
            BOOST_LOG(error) << "Display helper reported APPLY failure: " << helper_msg;
          }
          return success;
        }

        if (msg_type == static_cast<uint8_t>(MsgType::Ping)) {
          continue;
        }

        BOOST_LOG(debug) << "Display helper IPC: ignoring unexpected message type=" << static_cast<int>(msg_type)
                         << " while awaiting APPLY result";
      }

      BOOST_LOG(error) << "Display helper IPC: timed out waiting for APPLY result acknowledgement";
      return std::nullopt;
    }
  }  // namespace

  static bool send_message(platf::dxgi::INamedPipe &pipe, MsgType type, const std::vector<uint8_t> &payload) {
    const bool is_ping = (type == MsgType::Ping);
    if (!is_ping) {
      BOOST_LOG(info) << "Display helper IPC: sending frame type=" << static_cast<int>(type)
                      << ", payload_len=" << payload.size();
    }
    std::vector<uint8_t> out;
    out.reserve(1 + payload.size());
   out.push_back(static_cast<uint8_t>(type));
   out.insert(out.end(), payload.begin(), payload.end());
    const bool ok = pipe.send(out, effective_send_timeout());
    if (!is_ping) {
      BOOST_LOG(info) << "Display helper IPC: send result=" << (ok ? "true" : "false");
    }
    return ok;
  }

  // Persistent connection across a stream session. Helper stays alive until
  // successful revert; we reuse the data pipe for APPLY/REVERT.
  static std::unique_ptr<platf::dxgi::INamedPipe> &pipe_singleton() {
    static std::unique_ptr<platf::dxgi::INamedPipe> s_pipe;
    return s_pipe;
  }

  // Global mutex to serialize all access to the pipe (connect, reset, send)
  // and prevent interleaved writes on a BYTE-mode pipe.
  static std::mutex &pipe_mutex() {
    static std::mutex m;
    return m;
  }

  // Ensure connected while holding the pipe mutex. Returns true on success.
  static bool ensure_connected_locked() {
    auto &pipe = pipe_singleton();
    if (pipe && pipe->is_connected()) {
      return true;
    }
    BOOST_LOG(debug) << "Display helper IPC: connecting to server pipe 'sunshine_display_helper'";
    auto creator_anon = []() -> std::unique_ptr<platf::dxgi::INamedPipe> {
      platf::dxgi::FramedPipeFactory ff(std::make_unique<platf::dxgi::AnonymousPipeFactory>());
      return ff.create_client("sunshine_display_helper");
    };
    pipe = std::make_unique<platf::dxgi::SelfHealingPipe>(creator_anon);
    bool ok = false;
    if (pipe) {
      pipe->wait_for_client_connection(effective_connect_timeout());
      ok = pipe->is_connected();
    }
    if (!ok) {
      BOOST_LOG(debug) << "Display helper IPC: anonymous connect failed; trying named fallback";
      auto creator_named = []() -> std::unique_ptr<platf::dxgi::INamedPipe> {
        platf::dxgi::FramedPipeFactory ff(std::make_unique<platf::dxgi::NamedPipeFactory>());
        return ff.create_client("sunshine_display_helper");
      };
      pipe = std::make_unique<platf::dxgi::SelfHealingPipe>(creator_named);
      if (pipe) {
        pipe->wait_for_client_connection(effective_connect_timeout());
        ok = pipe->is_connected();
      } else {
        ok = false;
      }
    }
    BOOST_LOG(ok ? debug : warning) << "Display helper IPC: connection " << (ok ? "succeeded" : "failed");
    return ok;
  }

  void reset_connection() {
    std::lock_guard<std::mutex> lg(pipe_mutex());
    auto &pipe = pipe_singleton();
    if (pipe) {
      BOOST_LOG(debug) << "Display helper IPC: resetting cached connection";
    }
    pipe.reset();
  }

  bool send_apply_json(const std::string &json) {
    BOOST_LOG(debug) << "Display helper IPC: APPLY request queued (json_len=" << json.size() << ")";
    std::unique_lock<std::mutex> lk(pipe_mutex());
    if (!ensure_connected_locked()) {
      BOOST_LOG(warning) << "Display helper IPC: APPLY aborted - no connection";
      return false;
    }
    std::vector<uint8_t> payload(json.begin(), json.end());
    auto &pipe = pipe_singleton();
    if (!pipe) {
      BOOST_LOG(warning) << "Display helper IPC: APPLY aborted - no pipe instance";
      return false;
    }

    if (!send_message(*pipe, MsgType::Apply, payload)) {
      return false;
    }

    if (auto result = wait_for_apply_result_locked(*pipe)) {
      return *result;
    }

    return false;
  }

  bool send_revert() {
    BOOST_LOG(debug) << "Display helper IPC: REVERT request queued";
    std::unique_lock<std::mutex> lk(pipe_mutex());
    if (!ensure_connected_locked()) {
      BOOST_LOG(warning) << "Display helper IPC: REVERT aborted - no connection";
      return false;
    }
    std::vector<uint8_t> payload;
    auto &pipe = pipe_singleton();
    if (pipe && send_message(*pipe, MsgType::Revert, payload)) {
      return true;
    }
    return false;
  }

  bool send_export_golden() {
    BOOST_LOG(debug) << "Display helper IPC: EXPORT_GOLDEN request queued";
    std::unique_lock<std::mutex> lk(pipe_mutex());
    if (!ensure_connected_locked()) {
      BOOST_LOG(warning) << "Display helper IPC: EXPORT_GOLDEN aborted - no connection";
      return false;
    }
    std::vector<uint8_t> payload;
    auto &pipe = pipe_singleton();
    if (pipe && send_message(*pipe, MsgType::ExportGolden, payload)) {
      return true;
    }
    return false;
  }

  bool send_reset() {
    BOOST_LOG(debug) << "Display helper IPC: RESET request queued";
    std::unique_lock<std::mutex> lk(pipe_mutex());
    if (!ensure_connected_locked()) {
      BOOST_LOG(warning) << "Display helper IPC: RESET aborted - no connection";
      return false;
    }
    std::vector<uint8_t> payload;
    auto &pipe = pipe_singleton();
    if (pipe && send_message(*pipe, MsgType::Reset, payload)) {
      return true;
    }
    return false;
  }

  bool send_stop() {
    BOOST_LOG(info) << "Display helper IPC: STOP request queued";
    std::unique_lock<std::mutex> lk(pipe_mutex());
    if (!ensure_connected_locked()) {
      BOOST_LOG(warning) << "Display helper IPC: STOP aborted - no connection";
      return false;
    }
    std::vector<uint8_t> payload;
    auto &pipe = pipe_singleton();
    if (pipe && send_message(*pipe, MsgType::Stop, payload)) {
      return true;
    }
    return false;
  }

  bool send_blacklist(const std::string &device_id) {
    BOOST_LOG(debug) << "Display helper IPC: BLACKLIST request queued for device_id=" << device_id;
    std::unique_lock<std::mutex> lk(pipe_mutex());
    if (!ensure_connected_locked()) {
      BOOST_LOG(warning) << "Display helper IPC: BLACKLIST aborted - no connection";
      return false;
    }
    std::vector<uint8_t> payload(device_id.begin(), device_id.end());
    auto &pipe = pipe_singleton();
    if (pipe && send_message(*pipe, MsgType::Blacklist, payload)) {
      return true;
    }
    return false;
  }

  bool send_ping() {
    // No logging for ping path to reduce log spam
    std::unique_lock<std::mutex> lk(pipe_mutex());
    if (!ensure_connected_locked()) {
      return false;
    }
    std::vector<uint8_t> payload;
    auto &pipe = pipe_singleton();
    if (pipe && send_message(*pipe, MsgType::Ping, payload)) {
      return true;
    }
    return false;
  }
}  // namespace platf::display_helper_client

#endif
