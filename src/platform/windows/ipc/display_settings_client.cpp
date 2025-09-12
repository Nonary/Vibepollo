/**
 * @file src/platform/windows/ipc/display_settings_client.cpp
 */
#ifdef _WIN32

  // standard
  #include <cstdint>
  #include <mutex>
  #include <string>
  #include <vector>

  // local
  #include "display_settings_client.h"
  #include "src/logging.h"
  #include "src/platform/windows/ipc/pipes.h"

namespace platf::display_helper_client {

  /**
   * @brief IPC message types used by the display settings helper protocol.
   */
  enum class MsgType : uint8_t {
    Apply = 1,  ///< Apply display settings from JSON payload.
    Revert = 2,  ///< Revert display settings to the previous state.
    Reset = 3,  ///< Reset helper persistence/state (if supported).
    ExportGolden = 4,  ///< Export current OS settings as golden snapshot
    Ping = 0xFE,  ///< Health check message; expects a response.
    Stop = 0xFF  ///< Request helper process to terminate gracefully.
  };

  static bool send_frame(platf::dxgi::INamedPipe &pipe, MsgType type, const std::vector<uint8_t> &payload) {
    // Suppress logging for Ping to avoid log spam
    const bool is_ping = (type == MsgType::Ping);
    if (!is_ping) {
      BOOST_LOG(info) << "Display helper IPC: sending frame type=" << static_cast<int>(type)
                      << ", payload_len=" << payload.size();
    }
    uint32_t len = static_cast<uint32_t>(1 + payload.size());
    std::vector<uint8_t> out;
    out.reserve(4 + len);
    out.insert(out.end(), reinterpret_cast<const uint8_t *>(&len), reinterpret_cast<const uint8_t *>(&len) + 4);
    out.push_back(static_cast<uint8_t>(type));
    out.insert(out.end(), payload.begin(), payload.end());
    const bool ok = pipe.send(out, 5000);
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
    platf::dxgi::AnonymousPipeFactory f;
    BOOST_LOG(info) << "Display helper IPC: connecting to server pipe 'sunshine_display_helper'";
    pipe = f.create_client("sunshine_display_helper");
    BOOST_LOG(info) << "Display helper IPC: connection " << (pipe ? "succeeded" : "failed");
    return pipe != nullptr;
  }

  void reset_connection() {
    std::lock_guard<std::mutex> lg(pipe_mutex());
    auto &pipe = pipe_singleton();
    if (pipe) {
      BOOST_LOG(info) << "Display helper IPC: resetting cached connection";
    }
    pipe.reset();
  }

  bool send_apply_json(const std::string &json) {
    BOOST_LOG(info) << "Display helper IPC: APPLY request queued (json_len=" << json.size() << ")";
    std::unique_lock<std::mutex> lk(pipe_mutex());
    if (!ensure_connected_locked()) {
      BOOST_LOG(info) << "Display helper IPC: APPLY aborted - no connection";
      return false;
    }
    std::vector<uint8_t> payload(json.begin(), json.end());
    auto &pipe = pipe_singleton();
    if (pipe && send_frame(*pipe, MsgType::Apply, payload)) {
      return true;
    }
    // Retry once: reconnect + resend
    BOOST_LOG(warning) << "Display helper IPC: send failed; attempting reconnect";
    pipe.reset();
    if (!ensure_connected_locked()) {
      return false;
    }
    return pipe && send_frame(*pipe, MsgType::Apply, payload);
  }

  bool send_revert() {
    BOOST_LOG(info) << "Display helper IPC: REVERT request queued";
    std::unique_lock<std::mutex> lk(pipe_mutex());
    if (!ensure_connected_locked()) {
      BOOST_LOG(info) << "Display helper IPC: REVERT aborted - no connection";
      return false;
    }
    std::vector<uint8_t> payload;
    auto &pipe = pipe_singleton();
    if (pipe && send_frame(*pipe, MsgType::Revert, payload)) {
      return true;
    }
    BOOST_LOG(warning) << "Display helper IPC: send failed; attempting reconnect";
    pipe.reset();
    if (!ensure_connected_locked()) {
      return false;
    }
    return pipe && send_frame(*pipe, MsgType::Revert, payload);
  }

  bool send_export_golden() {
    BOOST_LOG(info) << "Display helper IPC: EXPORT_GOLDEN request queued";
    std::unique_lock<std::mutex> lk(pipe_mutex());
    if (!ensure_connected_locked()) {
      BOOST_LOG(info) << "Display helper IPC: EXPORT_GOLDEN aborted - no connection";
      return false;
    }
    std::vector<uint8_t> payload;
    auto &pipe = pipe_singleton();
    if (pipe && send_frame(*pipe, MsgType::ExportGolden, payload)) {
      return true;
    }
    BOOST_LOG(warning) << "Display helper IPC: send failed; attempting reconnect";
    pipe.reset();
    if (!ensure_connected_locked()) {
      return false;
    }
    return pipe && send_frame(*pipe, MsgType::ExportGolden, payload);
  }

  bool send_reset() {
    BOOST_LOG(info) << "Display helper IPC: RESET request queued";
    std::unique_lock<std::mutex> lk(pipe_mutex());
    if (!ensure_connected_locked()) {
      BOOST_LOG(info) << "Display helper IPC: RESET aborted - no connection";
      return false;
    }
    std::vector<uint8_t> payload;
    auto &pipe = pipe_singleton();
    if (pipe && send_frame(*pipe, MsgType::Reset, payload)) {
      return true;
    }
    BOOST_LOG(warning) << "Display helper IPC: send failed; attempting reconnect";
    pipe.reset();
    if (!ensure_connected_locked()) {
      return false;
    }
    return pipe && send_frame(*pipe, MsgType::Reset, payload);
  }

  bool send_ping() {
    // No logging for ping path to reduce log spam
    std::unique_lock<std::mutex> lk(pipe_mutex());
    if (!ensure_connected_locked()) {
      return false;
    }
    std::vector<uint8_t> payload;
    auto &pipe = pipe_singleton();
    if (pipe && send_frame(*pipe, MsgType::Ping, payload)) {
      return true;
    }
    pipe.reset();
    if (!ensure_connected_locked()) {
      return false;
    }
    return pipe && send_frame(*pipe, MsgType::Ping, payload);
  }
}  // namespace platf::display_helper_client

#endif
