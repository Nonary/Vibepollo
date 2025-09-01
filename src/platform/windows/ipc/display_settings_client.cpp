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
#include "src/platform/windows/ipc/pipes.h"

namespace platf::display_helper_client {

  /**
   * @brief IPC message types used by the display settings helper protocol.
   */
  enum class MsgType : uint8_t {
    Apply = 1,   ///< Apply display settings from JSON payload.
    Revert = 2,  ///< Revert display settings to the previous state.
    Reset = 3,   ///< Reset helper persistence/state (if supported).
    Ping = 0xFE, ///< Health check message; expects a response.
    Stop = 0xFF  ///< Request helper process to terminate gracefully.
  };

  static bool send_frame(platf::dxgi::INamedPipe &pipe, MsgType type, const std::vector<uint8_t> &payload) {
    uint32_t len = static_cast<uint32_t>(1 + payload.size());
    std::vector<uint8_t> out;
    out.reserve(4 + len);
    out.insert(out.end(), reinterpret_cast<const uint8_t *>(&len), reinterpret_cast<const uint8_t *>(&len) + 4);
    out.push_back(static_cast<uint8_t>(type));
    out.insert(out.end(), payload.begin(), payload.end());
    return pipe.send(out, 5000);
  }

  static std::unique_ptr<platf::dxgi::INamedPipe> connect() {
    platf::dxgi::AnonymousPipeFactory f;
    return f.create_client("sunshine_display_helper");
  }

  bool send_apply_json(const std::string &json) {
    auto pipe = connect();
    if (!pipe) {
      return false;
    }
    std::vector<uint8_t> payload(json.begin(), json.end());
    return send_frame(*pipe, MsgType::Apply, payload);
  }

  bool send_revert() {
    auto pipe = connect();
    if (!pipe) {
      return false;
    }
    std::vector<uint8_t> payload;
    return send_frame(*pipe, MsgType::Revert, payload);
  }
}

#endif
