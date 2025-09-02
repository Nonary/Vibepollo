/**
 * @file src/platform/windows/ipc/display_settings_client.cpp
 */
#ifdef _WIN32

  // standard
  #include <cstdint>
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
    Ping = 0xFE,  ///< Health check message; expects a response.
    Stop = 0xFF  ///< Request helper process to terminate gracefully.
  };

  static bool send_frame(platf::dxgi::INamedPipe &pipe, MsgType type, const std::vector<uint8_t> &payload) {
    BOOST_LOG(info) << "Display helper IPC: sending frame type=" << static_cast<int>(type)
                    << ", payload_len=" << payload.size();
    uint32_t len = static_cast<uint32_t>(1 + payload.size());
    std::vector<uint8_t> out;
    out.reserve(4 + len);
    out.insert(out.end(), reinterpret_cast<const uint8_t *>(&len), reinterpret_cast<const uint8_t *>(&len) + 4);
    out.push_back(static_cast<uint8_t>(type));
    out.insert(out.end(), payload.begin(), payload.end());
    const bool ok = pipe.send(out, 5000);
    BOOST_LOG(info) << "Display helper IPC: send result=" << (ok ? "true" : "false");
    return ok;
  }

  // Persistent connection across a stream session. Helper stays alive until
  // successful revert; we reuse the data pipe for APPLY/REVERT.
  static std::unique_ptr<platf::dxgi::INamedPipe> &pipe_singleton() {
    static std::unique_ptr<platf::dxgi::INamedPipe> s_pipe;
    return s_pipe;
  }

  static bool ensure_connected_once() {
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

  bool send_apply_json(const std::string &json) {
    BOOST_LOG(info) << "Display helper IPC: APPLY request queued (json_len=" << json.size() << ")";
    if (!ensure_connected_once()) {
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
    if (!ensure_connected_once()) {
      return false;
    }
    return pipe && send_frame(*pipe, MsgType::Apply, payload);
  }

  bool send_revert() {
    BOOST_LOG(info) << "Display helper IPC: REVERT request queued";
    if (!ensure_connected_once()) {
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
    if (!ensure_connected_once()) {
      return false;
    }
    return pipe && send_frame(*pipe, MsgType::Revert, payload);
  }
}  // namespace platf::display_helper_client

#endif
